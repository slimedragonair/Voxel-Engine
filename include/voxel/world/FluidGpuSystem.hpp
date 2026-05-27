#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/FluidDirtyQueue.hpp>
#include <voxel/world/FluidSystem.hpp>

// Forward declarations — the GPU resources live in voxel::render but this
// system is conceptually part of the world simulation, so we keep its main
// interface in voxel::world. The implementation file pulls in Vulkan headers.
namespace voxel::render {
class VulkanRenderer;
}

namespace voxel::world {

// GPU-accelerated fluid simulation. Drop-in replacement for `FluidSystem`
// when `ApplicationConfig::useGpuFluidSim` is true.
//
// Architecture: see `docs/gpu_fluid_sim_design.md`.
//
// Lifecycle:
//   1. Construct with a reference to VulkanRenderer (which owns the device,
//      allocator, command pool, etc.).
//   2. Call `initialize()` once after the renderer is up.
//   3. Each frame, call `tick(chunks, center)` from the same point in the
//      main loop that calls `FluidSystem::tick()` today.
//   4. Internally the system pipelines GPU work one frame: tick N dispatches
//      the compute pass and at the same time reads back tick N-1's results,
//      applying any cell changes via the standard
//      `Chunk::setBlockSilently` / `markMeshDirtyNoRevision` paths.
//
// Thread safety: this object is owned by the Application and called only from
// the main thread. The GPU itself is the parallelism source — no internal
// worker threads.
class FluidGpuSystem {
public:
    explicit FluidGpuSystem(render::VulkanRenderer& renderer,
                            FluidSystemSettings settings = {});
    ~FluidGpuSystem();

    FluidGpuSystem(const FluidGpuSystem&) = delete;
    FluidGpuSystem& operator=(const FluidGpuSystem&) = delete;
    FluidGpuSystem(FluidGpuSystem&&) = delete;
    FluidGpuSystem& operator=(FluidGpuSystem&&) = delete;

    // Allocates Vulkan resources (storage buffers, descriptor sets, compute
    // pipeline, etc.). Must be called once after the VulkanRenderer is
    // initialized. Returns false if any resource creation fails — caller
    // should fall back to the CPU `FluidSystem` in that case.
    [[nodiscard]] bool initialize();

    // Releases all Vulkan resources. Idempotent.
    void shutdown() noexcept;

    // Mirror of FluidSystem::wake — push a cell into the dirty queue.
    void wake(ChunkCoord chunkCoord, BlockCoord local, float priority = 0.0F);

    // Mirror of FluidSystem::activateOceanEdge. Currently stays CPU-side
    // (rare, large batches don't fit the per-frame GPU dispatch model).
    [[nodiscard]] std::size_t activateOceanEdge(ChunkManager& chunks,
                                                ChunkCoord originChunk,
                                                BlockCoord originLocal,
                                                int radius);

    // Run one simulation tick. Returns stats compatible with the CPU sim
    // for telemetry continuity.
    [[nodiscard]] FluidSimStats tick(ChunkManager& chunks, ChunkCoord center);

    // Diagnostic / inspector access (used by the debug overlay).
    [[nodiscard]] std::size_t residentSlotCount() const noexcept;
    [[nodiscard]] std::size_t freeSlotCount() const noexcept;
    [[nodiscard]] std::size_t pendingEventCount() const noexcept;

    [[nodiscard]] FluidDirtyQueue& queue() noexcept { return queue_; }
    [[nodiscard]] const FluidDirtyQueue& queue() const noexcept { return queue_; }
    [[nodiscard]] const FluidSystemSettings& settings() const noexcept { return settings_; }
    void setSettings(FluidSystemSettings s) noexcept { settings_ = s; }

private:
    // Opaque PIMPL for the Vulkan / shader resources. Keeps Vulkan headers
    // out of this public include — only the .cpp pulls them in.
    struct GpuResources;

    // Allocate a slot for `coord` if one isn't already assigned, uploading
    // the chunk's current block classification (air/water/solid) to the GPU.
    // Returns the slot index, or 0xFFFFFFFF on failure (pool exhausted /
    // chunk evicted / upload failed).
    std::uint32_t ensureSlot(ChunkManager& chunks, ChunkCoord coord);

    // Upload one chunk's block-bits + zero-init fluid bytes to the given
    // slot. Used by `ensureSlot()` on first observation.
    bool uploadChunkToSlot(const Chunk& chunk, std::uint32_t slot);

    // Walk the host-visible readback buffer, applying each event to chunks
    // via setBlockSilently + markMeshDirty. Returns events processed.
    std::size_t applyEvents(ChunkManager& chunks,
                            const void* readbackData,
                            FluidSimStats& stats);

    render::VulkanRenderer& renderer_;
    FluidSystemSettings settings_{};
    FluidDirtyQueue queue_;
    std::unique_ptr<GpuResources> gpu_;

    // CPU-side slot directory: ChunkCoord -> slot index. Slots are 32 KB
    // segments of the GPU fluid storage buffer. `freeSlots_` is a LIFO free
    // list maintained alongside the directory.
    std::unordered_map<ChunkCoord, std::uint32_t, ChunkCoordHash> slotForChunk_;
    std::vector<std::uint32_t> freeSlots_;
    std::uint32_t slotPoolCapacity_{0};

    // Round-robin index into the per-frame command buffer / fence /
    // readback-buffer arrays. Range [0, kReadbackFrames).
    std::uint32_t currentFrameIdx_{0};

    bool initialized_{false};
};

} // namespace voxel::world
