#pragma once

#include <vector>

#include <voxel/data/BlockRegistry.hpp>
#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/WorldDelta.hpp>

namespace voxel::world {

struct BlockEditResult {
    bool changed{};
    WorldDeltaBatch deltas;
    std::vector<ChunkCoord> dirtiedChunks;
};

class BlockEditor {
public:
    explicit BlockEditor(const data::BlockRegistry& registry) : registry_(registry) {}

    [[nodiscard]] BlockEditResult breakBlock(ChunkManager& chunks, PlanetCoord position) const;
    [[nodiscard]] BlockEditResult placeBlock(ChunkManager& chunks, PlanetCoord position, BlockStateId next) const;

private:
    const data::BlockRegistry& registry_;
    static void markBorderNeighbors(ChunkManager& chunks, PlanetCoord position, BlockEditResult& result);
};

} // namespace voxel::world

