#pragma once

#include <voxel/world/IChunkGenerator.hpp>

namespace voxel::world {

struct FlatTerrainSettings {
    int surfaceBlockY{0};
     BlockStateId fillState{makeBlockState(BlockTypeId{2})};
};

class FlatTerrainGenerator final : public IChunkGenerator {
public:
    explicit FlatTerrainGenerator(FlatTerrainSettings settings = {});

    void generate(Chunk& chunk) override;

    // TODO(world): Replace with biome/structure-aware terrain stages and deterministic seeded noise.

private:
    FlatTerrainSettings settings_;
};

} // namespace voxel::world

