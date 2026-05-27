#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::world {

struct ChunkDirtyQueueItem {
    ChunkCoord coord{};
    Revision revision{};
    Revision meshRevision{};
    float priority{};
};

class ChunkDirtyQueue {
public:
    // Returns true when this coord was newly queued. Existing queued coords
    // are updated in place so repeated dirty notifications coalesce.
    bool enqueue(ChunkCoord coord, Revision revision, Revision meshRevision, float priority = 0.0F);
    bool contains(ChunkCoord coord) const;
    void remove(ChunkCoord coord);
    void clear();
    void reserve(std::size_t itemCount);
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::vector<ChunkDirtyQueueItem> popClosest(ChunkCoord center, std::size_t maxItems);

private:
    std::unordered_map<ChunkCoord, ChunkDirtyQueueItem, ChunkCoordHash> items_;
};

} // namespace voxel::world
