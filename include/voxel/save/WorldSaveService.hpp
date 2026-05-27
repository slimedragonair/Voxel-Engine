#pragma once

#include <cstddef>

#include <voxel/save/ISaveStore.hpp>
#include <voxel/world/ChunkManager.hpp>

namespace voxel::save {

class WorldSaveService {
public:
    [[nodiscard]] std::size_t saveDirtyChunks(world::ChunkManager& chunks, ISaveStore& store) const;
    [[nodiscard]] std::size_t saveDirtyChunks(world::ChunkManager& chunks, ISaveStore& store, std::size_t maxChunks) const;
    [[nodiscard]] std::size_t dirtyChunkCount(const world::ChunkManager& chunks) const;
};

} // namespace voxel::save
