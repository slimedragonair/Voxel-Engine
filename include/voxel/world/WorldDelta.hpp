#pragma once

#include <variant>
#include <vector>

#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::world {

struct BlockDelta {
    PlanetCoord position{};
    BlockStateId previous{};
    BlockStateId next{};
};

using WorldDelta = std::variant<BlockDelta>;
using WorldDeltaBatch = std::vector<WorldDelta>;

class WorldDeltaApplier {
public:
    void apply(ChunkManager& chunks, const WorldDeltaBatch& batch) const;
};

// TODO(world): Route mining, building, machines, spells, explosions, saves, and networking through validated deltas.

} // namespace voxel::world
