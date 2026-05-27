#include <voxel/world/ChunkDirtyQueue.hpp>

#include <algorithm>

namespace voxel::world {

namespace {

std::int64_t distanceScore(ChunkCoord coord, ChunkCoord center) noexcept
{
    const auto dx = coord.x - center.x;
    const auto dy = coord.y - center.y;
    const auto dz = coord.z - center.z;
    return (dx * dx) + (dy * dy * 4) + (dz * dz);
}

} // namespace

bool ChunkDirtyQueue::enqueue(ChunkCoord coord, Revision revision, Revision meshRevision, float priority)
{
    const ChunkDirtyQueueItem item{coord, revision, meshRevision, priority};
    auto [it, inserted] = items_.emplace(coord, item);
    if (!inserted) {
        it->second.revision = revision;
        it->second.meshRevision = meshRevision;
        it->second.priority = std::min(it->second.priority, priority);
    }
    return inserted;
}

bool ChunkDirtyQueue::contains(ChunkCoord coord) const
{
    return items_.find(coord) != items_.end();
}

void ChunkDirtyQueue::remove(ChunkCoord coord)
{
    items_.erase(coord);
}

void ChunkDirtyQueue::clear()
{
    items_.clear();
}

void ChunkDirtyQueue::reserve(std::size_t itemCount)
{
    items_.reserve(itemCount);
}

std::size_t ChunkDirtyQueue::size() const noexcept
{
    return items_.size();
}

std::vector<ChunkDirtyQueueItem> ChunkDirtyQueue::popClosest(ChunkCoord center, std::size_t maxItems)
{
    // Hot path: during cold-start streaming the queue can hold 1500-2000+
    // entries while `maxItems` is ~32. A full O(N log N) sort here was the
    // single biggest contributor to mesh_dispatch_ms p99 (~10 ms in Debug at
    // N=1649). We only need the K smallest items, in order — so partition
    // first (O(N) average via `nth_element`), then sort just the K-prefix
    // (O(K log K)). For N=1649, K=32 that's ~1800 comparisons vs ~18000
    // for the previous full sort.
    if (items_.empty()) {
        return {};
    }

    std::vector<ChunkDirtyQueueItem> sorted;
    sorted.reserve(items_.size());
    for (const auto& [coord, item] : items_) {
        (void)coord;
        sorted.push_back(item);
    }

    auto cmp = [center](const ChunkDirtyQueueItem& lhs, const ChunkDirtyQueueItem& rhs) {
        if (lhs.priority != rhs.priority) {
            return lhs.priority < rhs.priority;
        }
        const auto l = distanceScore(lhs.coord, center);
        const auto r = distanceScore(rhs.coord, center);
        if (l != r) {
            return l < r;
        }
        if (lhs.coord.y != rhs.coord.y) {
            return lhs.coord.y < rhs.coord.y;
        }
        if (lhs.coord.x != rhs.coord.x) {
            return lhs.coord.x < rhs.coord.x;
        }
        return lhs.coord.z < rhs.coord.z;
    };

    const auto keepCount = std::min(maxItems, sorted.size());
    if (keepCount < sorted.size()) {
        // Partial-sort: nth_element guarantees the first `keepCount` items
        // are the K smallest (in unspecified order). We then sort just the
        // K-prefix to give callers a stable ordering of the K returned
        // items. The remaining N-K items end up in unspecified order, but
        // we discard the vector after extracting [0, keepCount) anyway.
        std::nth_element(sorted.begin(), sorted.begin() + keepCount, sorted.end(), cmp);
        std::sort(sorted.begin(), sorted.begin() + keepCount, cmp);
        sorted.resize(keepCount);
    } else {
        // We're taking the entire queue — just sort the whole thing once.
        std::sort(sorted.begin(), sorted.end(), cmp);
    }

    for (const auto& item : sorted) {
        items_.erase(item.coord);
    }
    return sorted;
}

} // namespace voxel::world
