#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <voxel/world/Coordinates.hpp>

namespace voxel::world {

// W2: queue of fluid cells that need simulation. Keyed by (chunk, local block)
// rather than by block-coord-only so the dedup is O(1) per push and the
// per-tick drain can pull the cells closest to the player first.
struct FluidQueueKey {
    ChunkCoord chunk{};
    BlockCoord local{};

    [[nodiscard]] friend bool operator==(const FluidQueueKey& lhs, const FluidQueueKey& rhs) noexcept
    {
        return lhs.chunk == rhs.chunk
            && lhs.local.x == rhs.local.x
            && lhs.local.y == rhs.local.y
            && lhs.local.z == rhs.local.z;
    }
};

struct FluidQueueKeyHash {
    [[nodiscard]] std::size_t operator()(const FluidQueueKey& k) const noexcept
    {
        std::size_t seed = ChunkCoordHash{}(k.chunk);
        // Pack the 3 local coords (each in [0,31]) into one 15-bit value.
        const auto local = static_cast<std::size_t>(k.local.x)
                         | (static_cast<std::size_t>(k.local.y) << 5)
                         | (static_cast<std::size_t>(k.local.z) << 10);
        seed ^= local + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        return seed;
    }
};

struct FluidQueueItem {
    FluidQueueKey key{};
    std::uint16_t age{0};   // # ticks waiting (used to evict stagnant cells)
    float priority{0.0F};
};

// Dirty queue of fluid cells needing simulation. Mirrors ChunkDirtyQueue's
// design: hash-set for dedup, popClosest for distance-based prioritization.
class FluidDirtyQueue {
public:
    // Returns true if newly inserted. Existing entries get their priority
    // updated (taking the LOWER value = closer = sooner).
    bool enqueue(FluidQueueKey key, float priority = 0.0F);
    bool contains(FluidQueueKey key) const noexcept;
    void remove(FluidQueueKey key);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

    // Pop up to `maxItems` cells, prioritizing those nearest to `center`.
    // `center` is a chunk coord; cells in farther chunks come later.
    [[nodiscard]] std::vector<FluidQueueItem> popClosest(ChunkCoord center, std::size_t maxItems);

private:
    std::unordered_map<FluidQueueKey, FluidQueueItem, FluidQueueKeyHash> items_;
};

} // namespace voxel::world
