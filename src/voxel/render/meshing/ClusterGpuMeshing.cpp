#include <voxel/render/meshing/ClusterGpuMeshing.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <vulkan/vulkan.h>

#include <voxel/core/Logger.hpp>
#include <voxel/render/VulkanRenderer.hpp>

#ifndef VOXEL_SHADER_DIR
#define VOXEL_SHADER_DIR "shaders"
#endif

namespace voxel::render::meshing {

namespace {

// Padded supervoxel grid: 64 interior + 1 border each side = 66³.
constexpr std::uint32_t kPaddedExtent = 66U;
constexpr std::size_t kCellCount =
    static_cast<std::size_t>(kPaddedExtent) * kPaddedExtent * kPaddedExtent;
constexpr VkDeviceSize kCellInfoBytes = 16U;
constexpr VkDeviceSize kInputBufferBytes =
    static_cast<VkDeviceSize>(kCellCount) * kCellInfoBytes;

// Theoretical max faces is 64³ * 6 = 1.57M, but realistic clusters emit
// far fewer (overhangs/cave walls only). 500k is generous and bounds
// the readback buffer at 8 MB.
constexpr std::uint32_t kMaxFacesPerCluster = 500'000U;
constexpr VkDeviceSize kFaceRecordBytes = 16U;
constexpr VkDeviceSize kFaceBufferHeaderBytes = 16U;
constexpr VkDeviceSize kFaceBufferBytes =
    kFaceBufferHeaderBytes + static_cast<VkDeviceSize>(kMaxFacesPerCluster) * kFaceRecordBytes;

// 64 / local_size = 8 / 8 = 8 workgroups per axis.
constexpr std::uint32_t kDispatchGroups = 8U;

struct GpuFaceRecord {
    std::uint32_t localFace{};   // (cellIdx & 0xFFFFFF) | (face << 24)
    std::uint32_t materialId{};
    std::uint32_t packedLight{};
    std::uint32_t surface{};
};
static_assert(sizeof(GpuFaceRecord) == kFaceRecordBytes,
              "GpuFaceRecord must match cluster_mesh_classify.comp");
static_assert(sizeof(ClusterGpuMeshing::GpuCellInfo) == kCellInfoBytes,
              "GpuCellInfo must match cluster_mesh_classify.comp");

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

[[nodiscard]] ClusterGpuMeshing::ClassificationResult parseReadback(const void* mapped)
{
    ClusterGpuMeshing::ClassificationResult parsed;
    if (mapped == nullptr) {
        return parsed;
    }
    const auto* readback = static_cast<const std::byte*>(mapped);
    std::uint32_t count = 0;
    std::memcpy(&count, readback, sizeof(count));
    parsed.rawFaceCount = count;
    if (count > kMaxFacesPerCluster) {
        parsed.overflow = true;
        count = kMaxFacesPerCluster;
    }
    const auto* gpuFaces = reinterpret_cast<const GpuFaceRecord*>(
        readback + kFaceBufferHeaderBytes);
    parsed.faces.reserve(count);
    constexpr std::uint32_t kCellMask = 0x00FFFFFFu;  // bits 0..23
    constexpr std::uint32_t kFaceShift = 24u;
    constexpr std::uint32_t kSupervoxelVolume = 64U * 64U * 64U;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& face = gpuFaces[i];
        const std::uint32_t cellIdx = face.localFace & kCellMask;
        const std::uint32_t faceIndex = (face.localFace >> kFaceShift) & 7U;
        if (cellIdx >= kSupervoxelVolume || faceIndex >= 6U || face.materialId == 0U) {
            continue;
        }
        parsed.faces.push_back({
            cellIdx,
            static_cast<std::uint8_t>(faceIndex),
            static_cast<MeshSurface>(face.surface & 3U),
            face.materialId,
            face.packedLight,
        });
    }
    return parsed;
}

} // namespace

struct ClusterGpuMeshing::GpuResources {
    render::VulkanRenderer::ComputeBuffer inputUpload{};   // host-visible
    render::VulkanRenderer::ComputeBuffer inputCells{};    // device-local
    render::VulkanRenderer::ComputeBuffer faceBuffer{};    // device-local
    render::VulkanRenderer::ComputeBuffer faceReadback{};  // host-visible

    VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet       descriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout      pipelineLayout{VK_NULL_HANDLE};
    VkPipeline            pipeline{VK_NULL_HANDLE};

    VkCommandPool   commandPool{VK_NULL_HANDLE};
    VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
    VkFence         fence{VK_NULL_HANDLE};
    bool            submitted{false};
};

ClusterGpuMeshing::ClusterGpuMeshing(render::VulkanRenderer& renderer)
    : renderer_(renderer),
      gpu_(std::make_unique<GpuResources>())
{
}

ClusterGpuMeshing::~ClusterGpuMeshing()
{
    shutdown();
}

bool ClusterGpuMeshing::initialize()
{
    if (initialized_) {
        return true;
    }
    if (!renderer_.initialized()) {
        Logger::error("ClusterGpuMeshing::initialize: VulkanRenderer not initialized yet");
        return false;
    }

    gpu_->inputUpload   = renderer_.createComputeBuffer(kInputBufferBytes, /*hostVisible=*/true);
    gpu_->inputCells    = renderer_.createComputeBuffer(kInputBufferBytes, /*hostVisible=*/false);
    gpu_->faceBuffer    = renderer_.createComputeBuffer(kFaceBufferBytes, /*hostVisible=*/false);
    gpu_->faceReadback  = renderer_.createComputeBuffer(kFaceBufferBytes, /*hostVisible=*/true);
    if (gpu_->inputUpload.buffer == VK_NULL_HANDLE
        || gpu_->inputCells.buffer == VK_NULL_HANDLE
        || gpu_->faceBuffer.buffer == VK_NULL_HANDLE
        || gpu_->faceReadback.buffer == VK_NULL_HANDLE) {
        Logger::error("ClusterGpuMeshing::initialize: buffer allocation failed");
        shutdown();
        return false;
    }

    // Descriptor set layout: 2 storage buffers (cells in, faces out).
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
        Logger::error("ClusterGpuMeshing::initialize: descriptor set layout creation failed");
        shutdown();
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = renderer_.descriptorPool();
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &gpu_->descriptorSetLayout;
    if (vkAllocateDescriptorSets(renderer_.device(), &allocInfo,
                                  &gpu_->descriptorSet) != VK_SUCCESS) {
        Logger::error("ClusterGpuMeshing::initialize: descriptor set allocation failed");
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

    // Pipeline layout — no push constants, layout is just the one set.
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &gpu_->descriptorSetLayout;
    if (vkCreatePipelineLayout(renderer_.device(), &pipelineLayoutInfo, nullptr,
                               &gpu_->pipelineLayout) != VK_SUCCESS) {
        Logger::error("ClusterGpuMeshing::initialize: pipeline layout failed");
        shutdown();
        return false;
    }

    const std::filesystem::path spvPath =
        std::filesystem::path(VOXEL_SHADER_DIR) / "cluster_mesh_classify.comp.spv";
    const auto spvBytes = readSpv(spvPath);
    if (spvBytes.empty()) {
        Logger::error("ClusterGpuMeshing::initialize: failed to read cluster_mesh_classify.comp.spv "
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
        Logger::error("ClusterGpuMeshing::initialize: failed to create shader module");
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
        Logger::error("ClusterGpuMeshing::initialize: failed to create compute pipeline");
        shutdown();
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = renderer_.graphicsQueueFamily();
    if (vkCreateCommandPool(renderer_.device(), &poolInfo, nullptr,
                            &gpu_->commandPool) != VK_SUCCESS) {
        Logger::error("ClusterGpuMeshing::initialize: failed to create command pool");
        shutdown();
        return false;
    }

    VkCommandBufferAllocateInfo cbAllocInfo{};
    cbAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAllocInfo.commandPool = gpu_->commandPool;
    cbAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAllocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(renderer_.device(), &cbAllocInfo,
                                 &gpu_->commandBuffer) != VK_SUCCESS) {
        Logger::error("ClusterGpuMeshing::initialize: failed to allocate command buffer");
        shutdown();
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(renderer_.device(), &fenceInfo, nullptr, &gpu_->fence) != VK_SUCCESS) {
        Logger::error("ClusterGpuMeshing::initialize: failed to create fence");
        shutdown();
        return false;
    }

    initialized_ = true;
    Logger::info("ClusterGpuMeshing::initialize: GPU cluster face classifier ready. "
                 "Input=" + std::to_string(static_cast<unsigned long long>(kInputBufferBytes / 1024U))
                 + " KB faces=" + std::to_string(kMaxFacesPerCluster)
                 + " readback=" + std::to_string(static_cast<unsigned long long>(kFaceBufferBytes / 1024U))
                 + " KB.");
    return true;
}

void ClusterGpuMeshing::shutdown() noexcept
{
    if (!gpu_) {
        return;
    }
    if (renderer_.device() != VK_NULL_HANDLE && gpu_->submitted && gpu_->fence != VK_NULL_HANDLE) {
        vkWaitForFences(renderer_.device(), 1, &gpu_->fence, VK_TRUE, UINT64_MAX);
    }
    if (gpu_->fence != VK_NULL_HANDLE) {
        vkDestroyFence(renderer_.device(), gpu_->fence, nullptr);
        gpu_->fence = VK_NULL_HANDLE;
    }
    if (gpu_->commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(renderer_.device(), gpu_->commandPool, nullptr);
        gpu_->commandPool = VK_NULL_HANDLE;
        gpu_->commandBuffer = VK_NULL_HANDLE;
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
    renderer_.destroyComputeBuffer(gpu_->faceReadback);
    gpu_->submitted = false;
    initialized_ = false;
}

bool ClusterGpuMeshing::busy() const noexcept
{
    return gpu_ && gpu_->submitted;
}

std::size_t ClusterGpuMeshing::maxFacesPerCluster() const noexcept
{
    return static_cast<std::size_t>(kMaxFacesPerCluster);
}

bool ClusterGpuMeshing::submit(const std::vector<GpuCellInfo>& paddedCells)
{
    if (!initialized_ || gpu_->pipeline == VK_NULL_HANDLE || gpu_->inputUpload.mapped == nullptr) {
        return false;
    }
    if (gpu_->submitted) {
        return false; // already an in-flight job; caller retries next frame
    }
    if (paddedCells.size() != kCellCount) {
        Logger::error("ClusterGpuMeshing::submit: paddedCells.size()="
                      + std::to_string(paddedCells.size())
                      + " expected " + std::to_string(kCellCount));
        return false;
    }

    std::memcpy(gpu_->inputUpload.mapped, paddedCells.data(),
                paddedCells.size() * sizeof(GpuCellInfo));

    VkCommandBuffer cb = gpu_->commandBuffer;
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) {
        return false;
    }

    // 1. Host upload → device copy.
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

    // 2. Reset face buffer header (count=0, maxFaces=kMaxFacesPerCluster).
    const std::array<std::uint32_t, 4> faceHeader{
        0U, kMaxFacesPerCluster, 0U, 0U};
    vkCmdUpdateBuffer(cb, gpu_->faceBuffer.buffer, 0,
                      static_cast<VkDeviceSize>(faceHeader.size() * sizeof(faceHeader[0])),
                      faceHeader.data());

    // 3. Barriers transfer-write → shader-read/write.
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

    // 4. Dispatch the classifier.
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gpu_->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gpu_->pipelineLayout,
                            0, 1, &gpu_->descriptorSet, 0, nullptr);
    vkCmdDispatch(cb, kDispatchGroups, kDispatchGroups, kDispatchGroups);

    // 5. Compute-write → transfer-read → host-read.
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
    vkCmdCopyBuffer(cb, gpu_->faceBuffer.buffer, gpu_->faceReadback.buffer, 1, &faceCopy);

    VkBufferMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = gpu_->faceReadback.buffer;
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
    if (vkQueueSubmit(renderer_.graphicsQueue(), 1, &submit, gpu_->fence) != VK_SUCCESS) {
        return false;
    }
    gpu_->submitted = true;
    return true;
}

std::optional<ClusterGpuMeshing::ClassificationResult> ClusterGpuMeshing::poll()
{
    if (!initialized_ || !gpu_->submitted) {
        return std::nullopt;
    }
    const auto status = vkGetFenceStatus(renderer_.device(), gpu_->fence);
    if (status == VK_NOT_READY) {
        return std::nullopt;
    }
    if (status != VK_SUCCESS) {
        gpu_->submitted = false;
        return ClassificationResult{};
    }
    vkResetFences(renderer_.device(), 1, &gpu_->fence);
    gpu_->submitted = false;

    auto parsed = parseReadback(gpu_->faceReadback.mapped);
    if (parsed.overflow) {
        Logger::warn("ClusterGpuMeshing::poll: face buffer overflow ("
                     + std::to_string(parsed.rawFaceCount) + " > "
                     + std::to_string(kMaxFacesPerCluster) + ")");
    }
    return parsed;
}

} // namespace voxel::render::meshing
