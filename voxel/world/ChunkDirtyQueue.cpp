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
    auto [it, inserted] = items_.insert_or_assign(coord, item);
    (void)it;
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

std::size_t ChunkDirtyQueue::size() const noexcept
{
    return items_.size();
}

std::vector<ChunkDirtyQueueItem> ChunkDirtyQueue::popClosest(ChunkCoord center, std::size_t maxItems)
{
    std::vector<ChunkDirtyQueueItem> sorted;
    sorted.reserve(items_.size());
    for (const auto& [coord, item] : items_) {
        (void)coord;
        sorted.push_back(item);
    }
    std::sort(sorted.begin(), sorted.end(), [center](const ChunkDirtyQueueItem& lhs, const ChunkDirtyQueueItem& rhs) {
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
    });

    if (sorted.size() > maxItems) {
        sorted.resize(maxItems);
    }
    for (const auto& item : sorted) {
        items_.erase(item.coord);
    }
    return sorted;
}

} // namespace voxel::world
