#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/FluidDirtyQueue.hpp>

namespace voxel::world {

struct FluidSystemSettings {
    // How many cells can flow / equalize per tick. Cells that don't fit are
    // re-queued for the next tick.
    std::size_t maxCellsPerTick{2048};

    // Cells whose age exceeds this number of ticks without being popped are
    // dropped from the queue (assumed settled). 0 = never drop.
    std::uint16_t maxIdleTicks{300};

    // BlockStateId that represents water. Cached so the system doesn't have
    // to look it up via the registry every cell. Set by the caller on init.
    std::uint32_t waterBlockValue{0};

    // Optional hard ceiling for active fluid. Cells above this world-block Y
    // are ignored and new water is never carved there. Space uses this to
    // prevent an exposed water source from flooding infinite empty chunks.
    std::optional<std::int32_t> maxActiveWorldY{};
};

struct FluidSimStats {
    std::size_t cellsProcessed{};
    std::size_t cellsCarved{};       // newly-created flowing water cells
    std::size_t cellsDrained{};      // cells whose level fell to 0
    std::size_t cellsRequeued{};
};

// W2 active-cell fluid simulation. The system processes a fixed budget of
// dirty cells per tick; cells that change push their neighbours back into
// the queue, so an edit ripples outward until everything stabilizes.
class FluidSystem {
public:
    explicit FluidSystem(FluidSystemSettings settings = {});

    // Push a single cell into the dirty queue. Use this when:
    //  - Player breaks a block adjacent to water (call once per neighbour).
    //  - A pump places new water.
    //  - Rain accumulates (future).
    // The `center` is the player's chunk; cells closer to it run first.
    void wake(ChunkCoord chunkCoord, BlockCoord local, float priority = 0.0F);

    // BFS from `origin` outward, marking adjacent oceanLocked water cells
    // as active (not-locked). Bounded by `radius` to keep cost predictable
    // when an edit exposes the ocean. Returns # cells unlocked.
    std::size_t activateOceanEdge(ChunkManager& chunks, ChunkCoord originChunk,
                                  BlockCoord originLocal, int radius);

    // Run one tick of the simulation against the chunk manager. Pops at most
    // `settings.maxCellsPerTick` cells closest to `center`.
    [[nodiscard]] FluidSimStats tick(ChunkManager& chunks, ChunkCoord center);

    [[nodiscard]] FluidDirtyQueue& queue() noexcept { return queue_; }
    [[nodiscard]] const FluidDirtyQueue& queue() const noexcept { return queue_; }
    [[nodiscard]] const FluidSystemSettings& settings() const noexcept { return settings_; }
    void setSettings(FluidSystemSettings s) noexcept { settings_ = s; }

private:
    FluidSystemSettings settings_{};
    FluidDirtyQueue queue_;
};

} // namespace voxel::world
