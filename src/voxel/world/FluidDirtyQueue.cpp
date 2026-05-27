#include <voxel/world/FluidDirtyQueue.hpp>

#include <algorithm>

namespace voxel::world {

namespace {

// Same distance metric used by ChunkDirtyQueue: chunk-space squared distance
// with vertical weight ×4 (player movement is mostly horizontal — keep
// vertical-but-near chunks higher priority than horizontal-but-far ones).
[[nodiscard]] std::int64_t chunkDistanceScore(ChunkCoord coord, ChunkCoord center) noexcept
{
    const std::int64_t dx = coord.x - center.x;
    const std::int64_t dy = coord.y - center.y;
    const std::int64_t dz = coord.z - center.z;
    return dx * dx + dy * dy * 4 + dz * dz;
}

} // namespace

bool FluidDirtyQueue::enqueue(FluidQueueKey key, float priority)
{
    const auto [it, inserted] = items_.try_emplace(key, FluidQueueItem{key, 0u, priority});
    if (!inserted) {
        // Coalesce: keep the LOWER priority (closer to player) and the
        // YOUNGER age (the new push resets the wait timer so we don't
        // think the cell has been sitting forever).
        it->second.priority = std::min(it->second.priority, priority);
        it->second.age = 0u;
    }
    return inserted;
}

bool FluidDirtyQueue::contains(FluidQueueKey key) const noexcept
{
    return items_.find(key) != items_.end();
}

void FluidDirtyQueue::remove(FluidQueueKey key)
{
    items_.erase(key);
}

void FluidDirtyQueue::clear() noexcept
{
    items_.clear();
}

std::vector<FluidQueueItem> FluidDirtyQueue::popClosest(ChunkCoord center, std::size_t maxItems)
{
    if (maxItems == 0 || items_.empty()) {
        return {};
    }

    // Collect into a vector keyed by (distanceScore, priority) so the
    // closest cells in the hottest priority come first.
    std::vector<FluidQueueItem> all;
    all.reserve(items_.size());
    for (const auto& [_, item] : items_) {
        all.push_back(item);
    }

    const std::size_t target = std::min(maxItems, all.size());
    std::partial_sort(
        all.begin(),
        all.begin() + static_cast<std::ptrdiff_t>(target),
        all.end(),
        [center](const FluidQueueItem& a, const FluidQueueItem& b) {
            const auto da = chunkDistanceScore(a.key.chunk, center);
            const auto db = chunkDistanceScore(b.key.chunk, center);
            if (da != db) return da < db;
            return a.priority < b.priority;
        });

    std::vector<FluidQueueItem> result(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(target));
    for (const auto& item : result) {
        items_.erase(item.key);
    }
    // Age the remaining items so cells that never get popped don't stay
    // queued forever — the simulation can drop entries above an age threshold.
    for (auto& [_, item] : items_) {
        ++item.age;
    }
    return result;
}

} // namespace voxel::world
