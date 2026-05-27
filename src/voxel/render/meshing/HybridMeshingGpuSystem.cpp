#include <voxel/render/meshing/HybridMeshingGpuSystem.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

#include <vulkan/vulkan.h>

#include <voxel/core/Logger.hpp>
#include <voxel/render/VulkanRenderer.hpp>
#include <voxel/render/meshing/BlockRenderCatalog.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/Chunk.hpp>
#include <voxel/world/ChunkConstants.hpp>

#ifndef VOXEL_SHADER_DIR
#define VOXEL_SHADER_DIR "shaders"
#endif

namespace voxel::render::meshing {

namespace {

constexpr std::uint32_t kReadbackFrames = 2;
constexpr std::uint32_t kPaddedChunkSize = 34;
constexpr std::uint32_t kCellInfoBytes = 16;
constexpr std::uint32_t kFaceRecordBytes = 16;
constexpr std::uint32_t kFaceBufferHeaderBytes = 16;
constexpr std::uint32_t kMaxFacesPerChunk =
    static_cast<std::uint32_t>(world::ChunkVolume) * 6U;

constexpr VkDeviceSize kInputBufferBytes =
    static_cast<VkDeviceSize>(kPaddedChunkSize) * kPaddedChunkSize * kPaddedChunkSize * kCellInfoBytes;
constexpr VkDeviceSize kFaceBufferBytes =
    kFaceBufferHeaderBytes + static_cast<VkDeviceSize>(kMaxFacesPerChunk) * kFaceRecordBytes;

struct GpuCellInfo {
    std::uint32_t materialId{};
    std::uint32_t flags{};
    std::uint32_t packedLight{};
    std::uint32_t pad{};
};
static_assert(sizeof(GpuCellInfo) == kCellInfoBytes, "GpuCellInfo must match mesh_face_classify.comp");

struct GpuFaceRecord {
    std::uint32_t localFace{};
    std::uint32_t materialId{};
    std::uint32_t packedLight{};
    std::uint32_t surface{};
};
static_assert(sizeof(GpuFaceRecord) == kFaceRecordBytes, "GpuFaceRecord must match mesh_face_classify.comp");

struct HybridMeshPushConstants {
    std::int32_t chunkBaseY;
    std::int32_t staticWaterSurfaceY;
    std::uint32_t suppressStaticWater;
    std::uint32_t reserved0;
};
static_assert(sizeof(HybridMeshPushConstants) == 16U, "Push constants must match mesh_face_classify.comp");

[[nodiscard]] constexpr std::size_t paddedIndex(int x, int y, int z) noexcept
{
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(y) * kPaddedChunkSize
        + static_cast<std::size_t>(z) * kPaddedChunkSize * kPaddedChunkSize;
}

[[nodiscard]] constexpr std::uint32_t localIndex(int x, int y, int z) noexcept
{
    return static_cast<std::uint32_t>(x + y * world::ChunkSize + z * world::ChunkSize * world::ChunkSize);
}

[[nodiscard]] std::uint32_t packFlags(const BlockRenderInfo& info) noexcept
{
    std::uint32_t flags = 0;
    if (info.occludesNeighborFaces) {
        flags |= 1U;
    }
    if (info.isFluid) {
        flags |= 2U;
    }
    flags |= (static_cast<std::uint32_t>(info.surface) & 3U) << 8U;
    return flags;
}

[[nodiscard]] GpuCellInfo makeCell(BlockStateId state, const BlockRenderCatalog& catalog) noexcept
{
    if (state.value == world::AirBlockState.value) {
        return {};
    }
    const auto info = catalog.get(state);
    return {state.value, packFlags(info), 0U, 0U};
}

void packChunkInterior(std::vector<GpuCellInfo>& cells,
                       const world::Chunk& chunk,
                       const BlockRenderCatalog& catalog)
{
    for (int z = 0; z < world::ChunkSize; ++z) {
        for (int y = 0; y < world::ChunkSize; ++y) {
            for (int x = 0; x < world::ChunkSize; ++x) {
                cells[paddedIndex(x + 1, y + 1, z + 1)] =
                    makeCell(chunk.blockAtUnchecked(x, y, z), catalog);
            }
        }
    }
}

void packNeighbourHalo(std::vector<GpuCellInfo>& cells,
                       const world::Chunk& chunk,
                       const ChunkNeighborhood& neighborhood,
                       const BlockRenderCatalog& catalog,
                       MeshingOptions options)
{
    const auto copyMissingStaticWater = [&](int dstX, int dstY, int dstZ, int srcX, int srcY, int srcZ) {
        if (!options.staticWaterSurfaceY.has_value()) {
            return false;
        }
        const auto src = cells[paddedIndex(srcX, srcY, srcZ)];
        if ((src.flags & 2U) == 0U) {
            return false;
        }
        const int localY = srcY - 1;
        const std::int64_t worldBlockY =
            chunk.coord().y * static_cast<std::int64_t>(world::ChunkSize) + localY;
        if (worldBlockY > static_cast<std::int64_t>(*options.staticWaterSurfaceY)) {
            return false;
        }
        cells[paddedIndex(dstX, dstY, dstZ)] = src;
        return true;
    };

    for (int z = 0; z < world::ChunkSize; ++z) {
        for (int y = 0; y < world::ChunkSize; ++y) {
            cells[paddedIndex(0, y + 1, z + 1)] = neighborhood.negX != nullptr
                ? makeCell(neighborhood.negX->blockAtUnchecked(world::ChunkSize - 1, y, z), catalog)
                : GpuCellInfo{};
            if (neighborhood.negX == nullptr) {
                (void)copyMissingStaticWater(0, y + 1, z + 1, 1, y + 1, z + 1);
            }
            cells[paddedIndex(world::ChunkSize + 1, y + 1, z + 1)] = neighborhood.posX != nullptr
                ? makeCell(neighborhood.posX->blockAtUnchecked(0, y, z), catalog)
                : GpuCellInfo{};
            if (neighborhood.posX == nullptr) {
                (void)copyMissingStaticWater(world::ChunkSize + 1, y + 1, z + 1,
                                             world::ChunkSize, y + 1, z + 1);
            }
        }
    }
    for (int z = 0; z < world::ChunkSize; ++z) {
        for (int x = 0; x < world::ChunkSize; ++x) {
            cells[paddedIndex(x + 1, 0, z + 1)] = neighborhood.negY != nullptr
                ? makeCell(neighborhood.negY->blockAtUnchecked(x, world::ChunkSize - 1, z), catalog)
                : GpuCellInfo{};
            if (neighborhood.negY == nullptr) {
                (void)copyMissingStaticWater(x + 1, 0, z + 1, x + 1, 1, z + 1);
            }
            cells[paddedIndex(x + 1, world::ChunkSize + 1, z + 1)] = neighborhood.posY != nullptr
                ? makeCell(neighborhood.posY->blockAtUnchecked(x, 0, z), catalog)
                : GpuCellInfo{};
            if (neighborhood.posY == nullptr) {
                (void)copyMissingStaticWater(x + 1, world::ChunkSize + 1, z + 1,
                                             x + 1, world::ChunkSize, z + 1);
            }
        }
    }
    for (int y = 0; y < world::ChunkSize; ++y) {
        for (int x = 0; x < world::ChunkSize; ++x) {
            cells[paddedIndex(x + 1, y + 1, 0)] = neighborhood.negZ != nullptr
                ? makeCell(neighborhood.negZ->blockAtUnchecked(x, y, world::ChunkSize - 1), catalog)
                : GpuCellInfo{};
            if (neighborhood.negZ == nullptr) {
                (void)copyMissingStaticWater(x + 1, y + 1, 0, x + 1, y + 1, 1);
            }
            cells[paddedIndex(x + 1, y + 1, world::ChunkSize + 1)] = neighborhood.posZ != nullptr
                ? makeCell(neighborhood.posZ->blockAtUnchecked(x, y, 0), catalog)
                : GpuCellInfo{};
            if (neighborhood.posZ == nullptr) {
                (void)copyMissingStaticWater(x + 1, y + 1, world::ChunkSize + 1,
                                             x + 1, y + 1, world::ChunkSize);
            }
        }
    }
}

[[nodiscard]] std::vector<char> readSpv(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    const auto size = static_cast<std::size_t>(file.tellg());
    std::vector<char> buf(size);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

[[nodiscard]] HybridMeshingGpuSystem::ClassificationResult parseClassificationReadback(const void* mapped)
{
    HybridMeshingGpuSystem::ClassificationResult parsed;
    if (mapped == nullptr) {
        return parsed;
    }

    const auto* readback = static_cast<const std::byte*>(mapped);
    std::uint32_t count = 0;
    std::memcpy(&count, readback, sizeof(count));
    parsed.rawFaceCount = count;
    if (count > kMaxFacesPerChunk) {
        parsed.overflow = true;
        count = kMaxFacesPerChunk;
    }

    const auto* gpuFaces = reinterpret_cast<const GpuFaceRecord*>(readback + kFaceBufferHeaderBytes);
    parsed.faces.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& face = gpuFaces[i];
        const std::uint32_t local = face.localFace & 0xFFFFU;
        const std::uint32_t faceIndex = (face.localFace >> 16U) & 7U;
        if (local >= world::ChunkVolume || faceIndex >= 6U || face.materialId == 0U) {
            continue;
        }
        parsed.faces.push_back({
            static_cast<std::uint16_t>(local),
            static_cast<std::uint8_t>(faceIndex),
            static_cast<MeshSurface>(face.surface & 3U),
            face.materialId,
            face.packedLight
        });
    }
    return parsed;
}

} // namespace

struct HybridMeshingGpuSystem::GpuResources {
    render::VulkanRenderer::ComputeBuffer inputUpload{};
    render::VulkanRenderer::ComputeBuffer inputCells{};
    render::VulkanRenderer::ComputeBuffer faceBuffer{};
    std::array<render::VulkanRenderer::ComputeBuffer, kReadbackFrames> faceReadback{};

    VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};

    VkCommandPool commandPool{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, kReadbackFrames> commandBuffers{};
    std::array<VkFence, kReadbackFrames> fences{};
    std::array<bool, kReadbackFrames> frameSubmitted{};
};

HybridMeshingGpuSystem::HybridMeshingGpuSystem(render::VulkanRenderer& renderer)
    : renderer_(renderer),
      gpu_(std::make_unique<GpuResources>())
{
}

HybridMeshingGpuSystem::~HybridMeshingGpuSystem()
{
    shutdown();
}

bool HybridMeshingGpuSystem::initialize()
{
    if (initialized_) {
        return true;
    }
    if (!renderer_.initialized()) {
        Logger::error("HybridMeshingGpuSystem::initialize: VulkanRenderer not initialized yet");
        return false;
    }

    gpu_->inputUpload = renderer_.createComputeBuffer(kInputBufferBytes, /*hostVisible=*/true);
    gpu_->inputCells = renderer_.createComputeBuffer(kInputBufferBytes, /*hostVisible=*/false);
    gpu_->faceBuffer = renderer_.createComputeBuffer(kFaceBufferBytes, /*hostVisible=*/false);
    for (auto& rb : gpu_->faceReadback) {
        rb = renderer_.createComputeBuffer(kFaceBufferBytes, /*hostVisible=*/true);
    }

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (std::uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(renderer_.device(), &layoutInfo, nullptr,
                                    &gpu_->descriptorSetLayout) != VK_SUCCESS) {
        Logger::error("HybridMeshingGpuSystem::initialize: failed to create descriptor set layout");
        shutdown();
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = renderer_.descriptorPool();
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &gpu_->descriptorSetLayout;
    if (vkAllocateDescriptorSets(renderer_.device(), &allocInfo, &gpu_->descriptorSet) != VK_SUCCESS) {
        Logger::error("HybridMeshingGpuSystem::initialize: failed to allocate descriptor set");
        shutdown();
        return false;
    }

    std::array<VkDescriptorBufferInfo, 2> bufferInfos{};
    bufferInfos[0] = {gpu_->inputCells.buffer, 0, VK_WHOLE_SIZE};
    bufferInfos[1] = {gpu_->faceBuffer.buffer, 0, VK_WHOLE_SIZE};

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = gpu_->descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(renderer_.device(),
                           static_cast<std::uint32_t>(writes.size()), writes.data(),
                           0, nullptr);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(HybridMeshPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &gpu_->descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(renderer_.device(), &pipelineLayoutInfo, nullptr,
                               &gpu_->pipelineLayout) != VK_SUCCESS) {
        Logger::error("HybridMeshingGpuSystem::initialize: failed to create pipeline layout");
        shutdown();
        return false;
    }

    const std::filesystem::path spvPath =
        std::filesystem::path(VOXEL_SHADER_DIR) / "mesh_face_classify.comp.spv";
    const auto spvBytes = readSpv(spvPath);
    if (spvBytes.empty()) {
        Logger::error("HybridMeshingGpuSystem::initialize: failed to read mesh_face_classify.comp.spv "
                            "(expected at " + spvPath.string() + ")");
        shutdown();
        return false;
    }

    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = spvBytes.size();
    shaderInfo.pCode = reinterpret_cast<const std::uint32_t*>(spvBytes.data());

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(renderer_.device(), &shaderInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        Logger::error("HybridMeshingGpuSystem::initialize: failed to create shader module");
        shutdown();
        return false;
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = gpu_->pipelineLayout;

    const auto pipelineResult = vkCreateComputePipelines(
        renderer_.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu_->pipeline);
    vkDestroyShaderModule(renderer_.device(), shaderModule, nullptr);
    if (pipelineResult != VK_SUCCESS) {
        Logger::error("HybridMeshingGpuSystem::initialize: failed to create compute pipeline");
        shutdown();
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = renderer_.graphicsQueueFamily();
    if (vkCreateCommandPool(renderer_.device(), &poolInfo, nullptr,
                            &gpu_->commandPool) != VK_SUCCESS) {
        Logger::error("HybridMeshingGpuSystem::initialize: failed to create command pool");
        shutdown();
        return false;
    }

    VkCommandBufferAllocateInfo cbAllocInfo{};
    cbAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAllocInfo.commandPool = gpu_->commandPool;
    cbAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAllocInfo.commandBufferCount = kReadbackFrames;
    if (vkAllocateCommandBuffers(renderer_.device(), &cbAllocInfo,
                                 gpu_->commandBuffers.data()) != VK_SUCCESS) {
        Logger::error("HybridMeshingGpuSystem::initialize: failed to allocate command buffers");
        shutdown();
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (std::uint32_t i = 0; i < kReadbackFrames; ++i) {
        if (vkCreateFence(renderer_.device(), &fenceInfo, nullptr, &gpu_->fences[i]) != VK_SUCCESS) {
            Logger::error("HybridMeshingGpuSystem::initialize: failed to create fence");
            shutdown();
            return false;
        }
        gpu_->frameSubmitted[i] = false;
    }

    initialized_ = true;
    Logger::info("HybridMeshingGpuSystem::initialize: GPU face classifier pipeline ready. "
                       "Input=" + std::to_string(static_cast<unsigned long long>(kInputBufferBytes / 1024U)) +
                       " KB faces=" + std::to_string(kMaxFacesPerChunk) +
                       " readback=" + std::to_string(static_cast<unsigned long long>(kFaceBufferBytes / 1024U)) +
                       " KB.");
    return true;
}

bool HybridMeshingGpuSystem::busy() const noexcept
{
    return gpu_ && gpu_->frameSubmitted[0];
}

bool HybridMeshingGpuSystem::submitClassification(
    const world::Chunk& chunk,
    const BlockRenderCatalog& catalog,
    const world::ChunkLightData* light,
    const ChunkNeighborhood& neighborhood,
    MeshingOptions options)
{
    if (!initialized_ || gpu_->pipeline == VK_NULL_HANDLE || gpu_->inputUpload.mapped == nullptr) {
        return false;
    }
    if (light != nullptr) {
        Logger::warn("HybridMeshingGpuSystem::submitClassification: baked-light path is not implemented; "
                     "falling back to CPU meshing for this chunk");
        return false;
    }

    constexpr std::uint32_t kFrameIdx = 0;
    if (gpu_->frameSubmitted[kFrameIdx]) {
        return false;
    }

    std::vector<GpuCellInfo> cells(
        static_cast<std::size_t>(kPaddedChunkSize) * kPaddedChunkSize * kPaddedChunkSize);
    packChunkInterior(cells, chunk, catalog);
    packNeighbourHalo(cells, chunk, neighborhood, catalog, options);
    std::memcpy(gpu_->inputUpload.mapped, cells.data(), cells.size() * sizeof(GpuCellInfo));

    VkCommandBuffer cb = gpu_->commandBuffers[kFrameIdx];
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) {
        return false;
    }

    VkBufferMemoryBarrier hostToTransfer{};
    hostToTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    hostToTransfer.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    hostToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostToTransfer.buffer = gpu_->inputUpload.buffer;
    hostToTransfer.offset = 0;
    hostToTransfer.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &hostToTransfer, 0, nullptr);

    VkBufferCopy inputCopy{};
    inputCopy.srcOffset = 0;
    inputCopy.dstOffset = 0;
    inputCopy.size = kInputBufferBytes;
    vkCmdCopyBuffer(cb, gpu_->inputUpload.buffer, gpu_->inputCells.buffer, 1, &inputCopy);

    const std::array<std::uint32_t, 4> faceHeader{0U, kMaxFacesPerChunk, 0U, 0U};
    vkCmdUpdateBuffer(cb, gpu_->faceBuffer.buffer, 0,
                      static_cast<VkDeviceSize>(faceHeader.size() * sizeof(faceHeader[0])),
                      faceHeader.data());

    std::array<VkBufferMemoryBarrier, 2> toShader{};
    toShader[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toShader[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShader[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader[0].buffer = gpu_->inputCells.buffer;
    toShader[0].offset = 0;
    toShader[0].size = VK_WHOLE_SIZE;
    toShader[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toShader[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toShader[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader[1].buffer = gpu_->faceBuffer.buffer;
    toShader[1].offset = 0;
    toShader[1].size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr,
                         static_cast<std::uint32_t>(toShader.size()), toShader.data(),
                         0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gpu_->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gpu_->pipelineLayout,
                            0, 1, &gpu_->descriptorSet, 0, nullptr);

    HybridMeshPushConstants pc{};
    pc.chunkBaseY = static_cast<std::int32_t>(
        chunk.coord().y * static_cast<std::int64_t>(world::ChunkSize));
    pc.staticWaterSurfaceY = options.staticWaterSurfaceY.value_or(0);
    pc.suppressStaticWater = options.staticWaterSurfaceY.has_value() ? 1U : 0U;
    vkCmdPushConstants(cb, gpu_->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDispatch(cb, 4, 4, 4);

    VkBufferMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.buffer = gpu_->faceBuffer.buffer;
    toTransfer.offset = 0;
    toTransfer.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &toTransfer, 0, nullptr);

    VkBufferCopy faceCopy{};
    faceCopy.srcOffset = 0;
    faceCopy.dstOffset = 0;
    faceCopy.size = kFaceBufferBytes;
    vkCmdCopyBuffer(cb, gpu_->faceBuffer.buffer, gpu_->faceReadback[kFrameIdx].buffer, 1, &faceCopy);

    VkBufferMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = gpu_->faceReadback[kFrameIdx].buffer;
    toHost.offset = 0;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &toHost, 0, nullptr);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        return false;
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (vkQueueSubmit(renderer_.graphicsQueue(), 1, &submit, gpu_->fences[kFrameIdx]) != VK_SUCCESS) {
        return false;
    }
    gpu_->frameSubmitted[kFrameIdx] = true;
    return true;
}

std::optional<HybridMeshingGpuSystem::ClassificationResult> HybridMeshingGpuSystem::pollClassification()
{
    constexpr std::uint32_t kFrameIdx = 0;
    if (!initialized_ || !gpu_->frameSubmitted[kFrameIdx]) {
        return std::nullopt;
    }
    const auto status = vkGetFenceStatus(renderer_.device(), gpu_->fences[kFrameIdx]);
    if (status == VK_NOT_READY) {
        return std::nullopt;
    }
    if (status != VK_SUCCESS) {
        gpu_->frameSubmitted[kFrameIdx] = false;
        return ClassificationResult{};
    }
    vkResetFences(renderer_.device(), 1, &gpu_->fences[kFrameIdx]);
    gpu_->frameSubmitted[kFrameIdx] = false;

    auto parsed = parseClassificationReadback(gpu_->faceReadback[kFrameIdx].mapped);
    if (parsed.overflow) {
        Logger::warn("HybridMeshingGpuSystem::pollClassification: face buffer overflow "
                     "(" + std::to_string(parsed.rawFaceCount) + " > "
                     + std::to_string(kMaxFacesPerChunk) + ")");
    }
    return parsed;
}

std::vector<VisibleFaceRecord> HybridMeshingGpuSystem::classifyBlocking(
    const world::Chunk& chunk,
    const BlockRenderCatalog& catalog,
    const world::ChunkLightData* light,
    const ChunkNeighborhood& neighborhood,
    MeshingOptions options)
{
    constexpr std::uint32_t kFrameIdx = 0;
    if (gpu_ && gpu_->frameSubmitted[kFrameIdx]) {
        vkWaitForFences(renderer_.device(), 1, &gpu_->fences[kFrameIdx], VK_TRUE, UINT64_MAX);
        vkResetFences(renderer_.device(), 1, &gpu_->fences[kFrameIdx]);
        gpu_->frameSubmitted[kFrameIdx] = false;
    }
    if (!submitClassification(chunk, catalog, light, neighborhood, options)) {
        return {};
    }
    vkWaitForFences(renderer_.device(), 1, &gpu_->fences[kFrameIdx], VK_TRUE, UINT64_MAX);
    auto parsed = pollClassification();
    return parsed.has_value() ? std::move(parsed->faces) : std::vector<VisibleFaceRecord>{};
}

void HybridMeshingGpuSystem::shutdown() noexcept
{
    if (!gpu_) {
        return;
    }
    if (renderer_.device() != VK_NULL_HANDLE) {
        for (std::uint32_t i = 0; i < kReadbackFrames; ++i) {
            if (gpu_->frameSubmitted[i] && gpu_->fences[i] != VK_NULL_HANDLE) {
                vkWaitForFences(renderer_.device(), 1, &gpu_->fences[i], VK_TRUE, UINT64_MAX);
            }
        }
    }
    for (auto& fence : gpu_->fences) {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(renderer_.device(), fence, nullptr);
            fence = VK_NULL_HANDLE;
        }
    }
    if (gpu_->commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(renderer_.device(), gpu_->commandPool, nullptr);
        gpu_->commandPool = VK_NULL_HANDLE;
        gpu_->commandBuffers.fill(VK_NULL_HANDLE);
    }
    if (gpu_->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(renderer_.device(), gpu_->pipeline, nullptr);
        gpu_->pipeline = VK_NULL_HANDLE;
    }
    if (gpu_->pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(renderer_.device(), gpu_->pipelineLayout, nullptr);
        gpu_->pipelineLayout = VK_NULL_HANDLE;
    }
    if (gpu_->descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(renderer_.device(), gpu_->descriptorSetLayout, nullptr);
        gpu_->descriptorSetLayout = VK_NULL_HANDLE;
    }
    gpu_->descriptorSet = VK_NULL_HANDLE;

    renderer_.destroyComputeBuffer(gpu_->inputUpload);
    renderer_.destroyComputeBuffer(gpu_->inputCells);
    renderer_.destroyComputeBuffer(gpu_->faceBuffer);
    for (auto& rb : gpu_->faceReadback) {
        renderer_.destroyComputeBuffer(rb);
    }
    initialized_ = false;
}

std::size_t HybridMeshingGpuSystem::maxFacesPerChunk() const noexcept
{
    return kMaxFacesPerChunk;
}

std::size_t HybridMeshingGpuSystem::inputBufferBytes() const noexcept
{
    return static_cast<std::size_t>(kInputBufferBytes);
}

std::size_t HybridMeshingGpuSystem::faceBufferBytes() const noexcept
{
    return static_cast<std::size_t>(kFaceBufferBytes);
}

} // namespace voxel::render::meshing
