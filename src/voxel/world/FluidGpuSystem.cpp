#include <voxel/world/FluidGpuSystem.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <unordered_set>
#include <vector>

#include <vulkan/vulkan.h>

#include <voxel/core/Logger.hpp>
#include <voxel/render/VulkanRenderer.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/Chunk.hpp>
#include <voxel/world/ChunkFluidData.hpp>
#include <voxel/world/CoordinateUtils.hpp>

#ifndef VOXEL_SHADER_DIR
#define VOXEL_SHADER_DIR "shaders"
#endif

// Phase 4 — full integration. End-to-end flow:
//
//   tick N    : main thread waits on fence (frame N-2's compute done),
//               reads frame N-2's eventReadback, applies events to chunks.
//               Records frame N's dispatch (clear events -> dispatch per
//               active chunk -> copy events to readback), submits with fence.
//   tick N+1  : same, one frame later. So GPU work is one frame pipelined
//               behind the main thread — no stall.
//
// Constraints in this v1:
//   - One full slot upload happens when a chunk is first observed with fluid;
//     after that the slot is read-only on the GPU until the chunk re-enters
//     the active set. Per-edit incremental uploads are a future optimization.
//   - Neighbour slots for cross-chunk falls aren't resolved yet; a cell at
//     y=0 in chunk C only falls if C's -Y neighbour is already slot-resident.
//     Otherwise we let the CPU FluidSystem handle that cell next tick.

namespace voxel::world {

namespace {

constexpr std::uint32_t kFluidSlotCount = 128;
constexpr std::uint32_t kFluidSlotBytes = 32U * 32U * 32U;
constexpr std::uint32_t kBlockBitsSlotBytes = 32U * 32U * 32U * 2U / 8U; // 8 KB
constexpr std::uint32_t kEventEntryBytes = 8U;
constexpr std::uint32_t kEventEntryCount = 16384U;
constexpr std::uint32_t kEventHeaderBytes = 16U;

constexpr VkDeviceSize kSlotPoolBytes =
    static_cast<VkDeviceSize>(kFluidSlotCount) * kFluidSlotBytes;
constexpr VkDeviceSize kBlockBitsBytes =
    static_cast<VkDeviceSize>(kFluidSlotCount) * kBlockBitsSlotBytes;
constexpr VkDeviceSize kEventBufferBytes =
    kEventHeaderBytes + static_cast<VkDeviceSize>(kEventEntryCount) * kEventEntryBytes;

constexpr std::uint32_t kReadbackFrames = 2;
constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

// Block-bit encoding (matches the shader):
constexpr std::uint32_t kBlockBitAir = 0U;
constexpr std::uint32_t kBlockBitWater = 1U;
constexpr std::uint32_t kBlockBitSolid = 2U;

// Event types (must match the shader):
constexpr std::uint32_t kEventDrained = 0U;
constexpr std::uint32_t kEventCarvedFalling = 1U;
constexpr std::uint32_t kEventLevelChanged = 2U;

// Push constants — must match `layout(push_constant)` in fluid_sim.comp.
struct FluidPushConstants {
    std::uint32_t currentSlot;
    std::uint32_t negXSlot;
    std::uint32_t posXSlot;
    std::uint32_t negYSlot;
    std::uint32_t posYSlot;
    std::uint32_t negZSlot;
    std::uint32_t posZSlot;
    std::uint32_t maxEvents;
};
static_assert(sizeof(FluidPushConstants) == 32U, "Push constants must be 32 bytes");

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

// Classify a block as air / water / solid for the 2-bit block-bits buffer.
[[nodiscard]] std::uint32_t classifyBlock(BlockStateId state, std::uint32_t waterValue) noexcept
{
    if (state.value == AirBlockState.value) {
        return kBlockBitAir;
    }
    // Same matching rule as FluidSystem::isWaterBlock — compare the high 16
    // bits (the block type) to allow waterlogged variants.
    if (waterValue != 0U && (state.value >> 16) == (waterValue >> 16)) {
        return kBlockBitWater;
    }
    return kBlockBitSolid;
}

} // namespace

struct FluidGpuSystem::GpuResources {
    bool initialized{false};

    render::VulkanRenderer::ComputeBuffer slotPool{};
    render::VulkanRenderer::ComputeBuffer blockBits{};
    render::VulkanRenderer::ComputeBuffer eventBuffer{};
    std::array<render::VulkanRenderer::ComputeBuffer, kReadbackFrames> eventReadback{};

    VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};

    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};

    // Per-frame command resources. One command buffer + fence per in-flight
    // frame; the fence is what lets us safely reuse the command buffer and
    // read back the previous frame's event buffer without blocking.
    VkCommandPool commandPool{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, kReadbackFrames> commandBuffers{};
    std::array<VkFence, kReadbackFrames> fences{};
    // Track which frames have actually been submitted at least once, so we
    // don't try to wait on an unsignaled fence the first time round.
    std::array<bool, kReadbackFrames> frameSubmitted{};
};

FluidGpuSystem::FluidGpuSystem(render::VulkanRenderer& renderer,
                                FluidSystemSettings settings)
    : renderer_(renderer),
      settings_(settings),
      gpu_(std::make_unique<GpuResources>())
{
}

FluidGpuSystem::~FluidGpuSystem()
{
    shutdown();
}

bool FluidGpuSystem::initialize()
{
    if (initialized_) {
        return true;
    }
    if (!renderer_.initialized()) {
        Logger::error("FluidGpuSystem::initialize: VulkanRenderer not initialized yet");
        return false;
    }

    // ---- 1. Allocate the three device-local storage buffers ----------
    gpu_->slotPool    = renderer_.createComputeBuffer(kSlotPoolBytes,    /*hostVisible=*/false);
    gpu_->blockBits   = renderer_.createComputeBuffer(kBlockBitsBytes,   /*hostVisible=*/false);
    gpu_->eventBuffer = renderer_.createComputeBuffer(kEventBufferBytes, /*hostVisible=*/false);

    // ---- 2. Allocate host-visible readback buffers (double) ----------
    for (auto& rb : gpu_->eventReadback) {
        rb = renderer_.createComputeBuffer(kEventBufferBytes, /*hostVisible=*/true);
    }

    // ---- 3. Descriptor set layout (3 storage buffers) ----------------
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
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
        Logger::error("FluidGpuSystem::initialize: failed to create descriptor set layout");
        shutdown();
        return false;
    }

    // ---- 4. Allocate the descriptor set ------------------------------
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = renderer_.descriptorPool();
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &gpu_->descriptorSetLayout;
    if (vkAllocateDescriptorSets(renderer_.device(), &allocInfo, &gpu_->descriptorSet) != VK_SUCCESS) {
        Logger::error("FluidGpuSystem::initialize: failed to allocate descriptor set");
        shutdown();
        return false;
    }

    // ---- 5. Write descriptor bindings --------------------------------
    std::array<VkDescriptorBufferInfo, 3> bufferInfos{};
    bufferInfos[0] = {gpu_->slotPool.buffer,    0, VK_WHOLE_SIZE};
    bufferInfos[1] = {gpu_->blockBits.buffer,   0, VK_WHOLE_SIZE};
    bufferInfos[2] = {gpu_->eventBuffer.buffer, 0, VK_WHOLE_SIZE};

    std::array<VkWriteDescriptorSet, 3> writes{};
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

    // ---- 6. Pipeline layout ------------------------------------------
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(FluidPushConstants);

    VkPipelineLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCreateInfo.setLayoutCount = 1;
    layoutCreateInfo.pSetLayouts = &gpu_->descriptorSetLayout;
    layoutCreateInfo.pushConstantRangeCount = 1;
    layoutCreateInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(renderer_.device(), &layoutCreateInfo, nullptr,
                               &gpu_->pipelineLayout) != VK_SUCCESS) {
        Logger::error("FluidGpuSystem::initialize: failed to create pipeline layout");
        shutdown();
        return false;
    }

    // ---- 7. Load fluid_sim.comp.spv and build the compute pipeline ---
    const std::filesystem::path spvPath =
        std::filesystem::path(VOXEL_SHADER_DIR) / "fluid_sim.comp.spv";
    auto spvBytes = readSpv(spvPath);
    if (spvBytes.empty()) {
        Logger::error("FluidGpuSystem::initialize: failed to read fluid_sim.comp.spv "
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
        Logger::error("FluidGpuSystem::initialize: failed to create shader module");
        shutdown();
        return false;
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineCreateInfo{};
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.stage = stageInfo;
    pipelineCreateInfo.layout = gpu_->pipelineLayout;

    const auto pipelineResult = vkCreateComputePipelines(
        renderer_.device(), VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr,
        &gpu_->pipeline);
    vkDestroyShaderModule(renderer_.device(), shaderModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
        Logger::error("FluidGpuSystem::initialize: failed to create compute pipeline");
        shutdown();
        return false;
    }

    // ---- 8. Command pool + per-frame command buffers + fences --------
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = renderer_.graphicsQueueFamily();
    if (vkCreateCommandPool(renderer_.device(), &poolInfo, nullptr,
                            &gpu_->commandPool) != VK_SUCCESS) {
        Logger::error("FluidGpuSystem::initialize: failed to create command pool");
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
        Logger::error("FluidGpuSystem::initialize: failed to allocate command buffers");
        shutdown();
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (std::uint32_t i = 0; i < kReadbackFrames; ++i) {
        if (vkCreateFence(renderer_.device(), &fenceInfo, nullptr, &gpu_->fences[i]) != VK_SUCCESS) {
            Logger::error("FluidGpuSystem::initialize: failed to create fence");
            shutdown();
            return false;
        }
        gpu_->frameSubmitted[i] = false;
    }

    // ---- 9. Populate the free-slot list ------------------------------
    slotPoolCapacity_ = kFluidSlotCount;
    freeSlots_.resize(kFluidSlotCount);
    std::iota(freeSlots_.begin(), freeSlots_.end(), 0U);
    std::reverse(freeSlots_.begin(), freeSlots_.end());

    gpu_->initialized = true;
    initialized_ = true;
    Logger::info("FluidGpuSystem::initialize: Phase 4 complete — full GPU "
                 "fluid sim pipeline ready. Pool: 4 MB slot + 1 MB blocks + "
                 "128 KB events. CPU FluidSystem remains authoritative when "
                 "useGpuFluidSim=false.");
    return true;
}

void FluidGpuSystem::shutdown() noexcept
{
    if (!gpu_) {
        return;
    }
    if (renderer_.device() != VK_NULL_HANDLE) {
        // Wait on any in-flight compute before destroying its resources.
        for (std::uint32_t i = 0; i < kReadbackFrames; ++i) {
            if (gpu_->frameSubmitted[i] && gpu_->fences[i] != VK_NULL_HANDLE) {
                vkWaitForFences(renderer_.device(), 1, &gpu_->fences[i], VK_TRUE,
                                /*timeout_ns=*/UINT64_MAX);
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
        // Destroying the pool frees the command buffers.
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

    renderer_.destroyComputeBuffer(gpu_->eventBuffer);
    renderer_.destroyComputeBuffer(gpu_->blockBits);
    renderer_.destroyComputeBuffer(gpu_->slotPool);
    for (auto& rb : gpu_->eventReadback) {
        renderer_.destroyComputeBuffer(rb);
    }
    gpu_->initialized = false;
    initialized_ = false;
    slotForChunk_.clear();
    freeSlots_.clear();
    slotPoolCapacity_ = 0;
    currentFrameIdx_ = 0;
}

void FluidGpuSystem::wake(ChunkCoord chunkCoord, BlockCoord local, float priority)
{
    const auto world = toWorldBlock(chunkCoord, local);
    if (settings_.maxActiveWorldY.has_value() && world.y > *settings_.maxActiveWorldY) {
        return;
    }
    queue_.enqueue(FluidQueueKey{chunkCoord, local}, priority);
}

std::size_t FluidGpuSystem::activateOceanEdge(ChunkManager& /*chunks*/,
                                              ChunkCoord /*originChunk*/,
                                              BlockCoord /*originLocal*/,
                                              int /*radius*/)
{
    // Stays CPU-side per design (rare, large BFS batches don't fit the
    // per-frame GPU dispatch model). Phase 5 will factor this out of
    // FluidSystem and call it here.
    return 0;
}

// Walk a chunk's 32^3 cells, pack the 2-bit block classification into a
// host buffer, then upload to the GPU slot via vkCmdUpdateBuffer through a
// one-shot command buffer. Slot pool gets zero-initialised (no fluid bytes
// yet — every water block looks like a non-falling source until proven
// otherwise, which matches the CPU sim's initial behaviour).
bool FluidGpuSystem::uploadChunkToSlot(const Chunk& chunk, std::uint32_t slot)
{
    if (slot == kInvalidSlot || slot >= slotPoolCapacity_) {
        return false;
    }

    // Pack block bits.
    std::array<std::uint32_t, kBlockBitsSlotBytes / sizeof(std::uint32_t)> blockBitsBuf{};
    const auto waterValue = settings_.waterBlockValue;
    for (int z = 0; z < 32; ++z) {
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
                const auto block = chunk.blockAtUnchecked(x, y, z);
                const auto bits = classifyBlock(block, waterValue);
                const std::uint32_t localIdx =
                    static_cast<std::uint32_t>(x + y * 32 + z * 32 * 32);
                const std::uint32_t wordIdx = localIdx >> 4U;
                const std::uint32_t shift = (localIdx & 15U) * 2U;
                blockBitsBuf[wordIdx] |= (bits << shift);
            }
        }
    }

    // Zero-init fluid bytes for the slot. Pack into uint32 array to match
    // vkCmdUpdateBuffer alignment (4 bytes).
    std::array<std::uint32_t, kFluidSlotBytes / sizeof(std::uint32_t)> fluidBuf{};

    // Allocate a one-shot command buffer to record the two updates.
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = gpu_->commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(renderer_.device(), &allocInfo, &cb) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &beginInfo);
    const VkDeviceSize fluidOffset = static_cast<VkDeviceSize>(slot) * kFluidSlotBytes;
    const VkDeviceSize blockOffset = static_cast<VkDeviceSize>(slot) * kBlockBitsSlotBytes;
    vkCmdUpdateBuffer(cb, gpu_->slotPool.buffer,  fluidOffset, kFluidSlotBytes,    fluidBuf.data());
    vkCmdUpdateBuffer(cb, gpu_->blockBits.buffer, blockOffset, kBlockBitsSlotBytes, blockBitsBuf.data());
    vkEndCommandBuffer(cb);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(renderer_.graphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(renderer_.graphicsQueue());
    vkFreeCommandBuffers(renderer_.device(), gpu_->commandPool, 1, &cb);
    return true;
}

std::uint32_t FluidGpuSystem::ensureSlot(ChunkManager& chunks, ChunkCoord coord)
{
    if (const auto it = slotForChunk_.find(coord); it != slotForChunk_.end()) {
        return it->second;
    }
    if (freeSlots_.empty()) {
        return kInvalidSlot; // pool exhausted; cell will be retried next tick
    }
    const auto* chunk = chunks.find(coord);
    if (chunk == nullptr) {
        return kInvalidSlot;
    }
    const std::uint32_t slot = freeSlots_.back();
    freeSlots_.pop_back();
    if (!uploadChunkToSlot(*chunk, slot)) {
        // Failed upload — return slot to free list, give up for this tick.
        freeSlots_.push_back(slot);
        return kInvalidSlot;
    }
    slotForChunk_.emplace(coord, slot);
    return slot;
}

// Walk the host-visible readback buffer, apply each event to the chunk
// manager. Returns the count of events processed.
std::size_t FluidGpuSystem::applyEvents(ChunkManager& chunks,
                                        const void* readbackData,
                                        FluidSimStats& stats)
{
    if (readbackData == nullptr) {
        return 0;
    }
    // Header: [count:u32, pad:u32x3]. Then `count` event entries follow.
    std::uint32_t count = 0;
    std::memcpy(&count, readbackData, sizeof(count));
    if (count == 0) {
        return 0;
    }
    if (count > kEventEntryCount) {
        // Overflow: shader's atomicAdd kept incrementing past capacity. The
        // entries beyond [count, kEventEntryCount) are garbage. Clamp.
        Logger::warn("FluidGpuSystem::applyEvents: event buffer overflow "
                     "(" + std::to_string(count) + " > " +
                     std::to_string(kEventEntryCount) + "); dropping excess");
        count = kEventEntryCount;
    }

    // Build a reverse map from slot index -> chunk coord so we can resolve
    // events without scanning slotForChunk_ for every entry.
    std::vector<ChunkCoord> slotToCoord(slotPoolCapacity_, ChunkCoord{});
    std::vector<bool> slotValid(slotPoolCapacity_, false);
    for (const auto& [coord, slot] : slotForChunk_) {
        if (slot < slotPoolCapacity_) {
            slotToCoord[slot] = coord;
            slotValid[slot] = true;
        }
    }

    const auto* entries = reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::byte*>(readbackData) + kEventHeaderBytes);

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t packed = entries[i * 2 + 0];
        const std::uint32_t newCellByte = entries[i * 2 + 1];
        (void)newCellByte; // currently only used informally; the CPU re-derives state

        const std::uint32_t type = (packed >> 28) & 0xFu;
        const std::uint32_t slot = (packed >> 16) & 0xFFFu;
        const std::uint32_t localIdx = packed & 0xFFFFu;

        if (slot >= slotPoolCapacity_ || !slotValid[slot]) {
            continue;
        }
        const auto coord = slotToCoord[slot];
        auto* chunk = chunks.find(coord);
        if (chunk == nullptr) {
            continue; // chunk evicted between dispatch and apply
        }

        const int x = static_cast<int>(localIdx & 31U);
        const int y = static_cast<int>((localIdx >> 5U) & 31U);
        const int z = static_cast<int>((localIdx >> 10U) & 31U);
        const auto world = toWorldBlock(coord, {x, y, z});
        if (settings_.maxActiveWorldY.has_value() && world.y > *settings_.maxActiveWorldY) {
            continue;
        }

        if (type == kEventCarvedFalling) {
            // Same path as FluidSystem: carve water into the cell, mark
            // the chunk mesh-dirty (no revision bump — block content
            // changed but neighbour topology didn't).
            const auto waterState = BlockStateId{settings_.waterBlockValue};
            chunk->setBlockSilently(x, y, z, waterState);
            chunk->markMeshDirtyNoRevision();
            ++stats.cellsCarved;
        }
        // kEventDrained / kEventLevelChanged are reserved for future passes
        // that emit them. The current shader only emits kEventCarvedFalling.
        ++stats.cellsProcessed;
    }
    return count;
}

FluidSimStats FluidGpuSystem::tick(ChunkManager& chunks, ChunkCoord center)
{
    FluidSimStats stats{};
    if (!initialized_ || gpu_->pipeline == VK_NULL_HANDLE) {
        return stats;
    }
    if (settings_.waterBlockValue == 0U || queue_.size() == 0U) {
        return stats;
    }

    const std::uint32_t frameIdx = currentFrameIdx_;

    // ---- 1. Wait on this frame's fence (so we can reuse cmd buffer) --
    // The fence was signaled by THIS slot's previous dispatch (kReadbackFrames
    // ago). By the time we cycle back to it, the GPU has long finished — the
    // wait is almost always non-blocking. That's the point of double-buffer.
    if (gpu_->frameSubmitted[frameIdx]) {
        vkWaitForFences(renderer_.device(), 1, &gpu_->fences[frameIdx], VK_TRUE, UINT64_MAX);
        vkResetFences(renderer_.device(), 1, &gpu_->fences[frameIdx]);
        // Apply the events from the readback corresponding to the dispatch
        // that THIS slot performed last cycle. With kReadbackFrames=2, that's
        // 2 ticks ago — early enough that the GPU has finished, but late
        // enough that we never block waiting.
        applyEvents(chunks, gpu_->eventReadback[frameIdx].mapped, stats);
    }

    // ---- 2. Discover unique active chunks from the cell queue --------
    auto batch = queue_.popClosest(center, settings_.maxCellsPerTick);
    std::unordered_set<ChunkCoord, ChunkCoordHash> activeChunks;
    activeChunks.reserve(batch.size());
    for (const auto& item : batch) {
        const auto world = toWorldBlock(item.key.chunk, item.key.local);
        if (settings_.maxActiveWorldY.has_value() && world.y > *settings_.maxActiveWorldY) {
            continue;
        }
        activeChunks.insert(item.key.chunk);
    }
    if (activeChunks.empty()) {
        // No work this tick — bail without recording an empty command buffer.
        return stats;
    }

    // Cap dispatches per frame: 16 chunks * 64 workgroups * 512 threads =
    // 524k threads. Most GPUs handle this in <100µs. Bigger batches risk
    // long dispatch latency.
    constexpr std::size_t kMaxDispatchesPerTick = 16;
    std::vector<std::pair<ChunkCoord, std::uint32_t>> toDispatch;
    toDispatch.reserve(std::min<std::size_t>(activeChunks.size(), kMaxDispatchesPerTick));
    for (const auto& coord : activeChunks) {
        if (toDispatch.size() >= kMaxDispatchesPerTick) break;
        const std::uint32_t slot = ensureSlot(chunks, coord);
        if (slot != kInvalidSlot) {
            toDispatch.emplace_back(coord, slot);
        } else {
            // Slot couldn't be allocated; re-queue the cells from this chunk.
            for (const auto& item : batch) {
                if (item.key.chunk == coord) {
                    queue_.enqueue(item.key, item.priority);
                    ++stats.cellsRequeued;
                }
            }
        }
    }
    if (toDispatch.empty()) {
        return stats;
    }

    // ---- 3. Record command buffer ------------------------------------
    VkCommandBuffer cb = gpu_->commandBuffers[frameIdx];
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    // 3a. Clear the event count to 0.
    vkCmdFillBuffer(cb, gpu_->eventBuffer.buffer, 0, sizeof(std::uint32_t), 0U);

    // 3b. Barrier: transfer-write (fill) -> shader-read/write (dispatch).
    VkBufferMemoryBarrier toShader{};
    toShader.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.buffer = gpu_->eventBuffer.buffer;
    toShader.offset = 0;
    toShader.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &toShader, 0, nullptr);

    // 3c. Bind pipeline + descriptor set once.
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gpu_->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gpu_->pipelineLayout,
                            0, 1, &gpu_->descriptorSet, 0, nullptr);

    // 3d. Per-chunk dispatch.
    for (const auto& [coord, slot] : toDispatch) {
        FluidPushConstants pc{};
        pc.currentSlot = slot;
        // Neighbour resolution v1: look them up in slotForChunk_. If absent,
        // the shader treats the neighbour as kInvalidSlot (all air, no fluid).
        const auto lookupNeighbour = [&](ChunkCoord nc) -> std::uint32_t {
            const auto it = slotForChunk_.find(nc);
            return it != slotForChunk_.end() ? it->second : kInvalidSlot;
        };
        pc.negXSlot = lookupNeighbour({coord.x - 1, coord.y,     coord.z    });
        pc.posXSlot = lookupNeighbour({coord.x + 1, coord.y,     coord.z    });
        pc.negYSlot = lookupNeighbour({coord.x,     coord.y - 1, coord.z    });
        pc.posYSlot = lookupNeighbour({coord.x,     coord.y + 1, coord.z    });
        pc.negZSlot = lookupNeighbour({coord.x,     coord.y,     coord.z - 1});
        pc.posZSlot = lookupNeighbour({coord.x,     coord.y,     coord.z + 1});
        pc.maxEvents = kEventEntryCount;
        vkCmdPushConstants(cb, gpu_->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(pc), &pc);
        // 4x4x4 workgroups, each 8x8x8 = covers the 32^3 chunk.
        vkCmdDispatch(cb, 4, 4, 4);
    }

    // 3e. Barrier: shader-write -> transfer-read (for the copy to readback).
    VkBufferMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.buffer = gpu_->eventBuffer.buffer;
    toTransfer.offset = 0;
    toTransfer.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &toTransfer, 0, nullptr);

    // 3f. Copy events to the host-visible readback buffer.
    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = kEventBufferBytes;
    vkCmdCopyBuffer(cb, gpu_->eventBuffer.buffer, gpu_->eventReadback[frameIdx].buffer,
                    1, &region);

    // 3g. Barrier: transfer-write -> host-read (so CPU can read next tick).
    VkBufferMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = gpu_->eventReadback[frameIdx].buffer;
    toHost.offset = 0;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &toHost, 0, nullptr);

    vkEndCommandBuffer(cb);

    // ---- 4. Submit + fence -------------------------------------------
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(renderer_.graphicsQueue(), 1, &submit, gpu_->fences[frameIdx]);
    gpu_->frameSubmitted[frameIdx] = true;

    currentFrameIdx_ = (frameIdx + 1U) % kReadbackFrames;
    return stats;
}

std::size_t FluidGpuSystem::residentSlotCount() const noexcept
{
    return slotForChunk_.size();
}

std::size_t FluidGpuSystem::freeSlotCount() const noexcept
{
    return freeSlots_.size();
}

std::size_t FluidGpuSystem::pendingEventCount() const noexcept
{
    if (!initialized_ || gpu_->eventReadback[0].mapped == nullptr) {
        return 0;
    }
    std::uint32_t count = 0;
    std::memcpy(&count, gpu_->eventReadback[0].mapped, sizeof(count));
    return static_cast<std::size_t>(count);
}

} // namespace voxel::world
