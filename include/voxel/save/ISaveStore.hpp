#pragma once

#include <optional>

#include <voxel/world/Chunk.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::save {

class ISaveStore {
public:
    virtual ~ISaveStore() = default;
    virtual void saveChunk(const world::Chunk& chunk) = 0;
    virtual std::optional<world::Chunk> loadChunk(world::ChunkCoord coord) = 0;
};

} // namespace voxel::save

