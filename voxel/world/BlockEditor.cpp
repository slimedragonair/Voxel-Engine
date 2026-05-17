#include <voxel/world/BlockEditor.hpp>

#include <algorithm>

#include <voxel/world/CoordinateUtils.hpp>

namespace voxel::world {

namespace {

void pushUnique(std::vector<ChunkCoord>& coords, ChunkCoord coord)
{
    const auto found = std::find(coords.begin(), coords.end(), coord);
    if (found == coords.end()) {
        coords.push_back(coord);
    }
}

PlanetCoord normalized(PlanetCoord position)
{
    const auto world = toWorldBlock(position.chunk, position.block);
    const auto local = toChunkLocal(world.x, world.y, world.z);
    position.chunk = local.chunk;
    position.block = local.local;
    return position;
}

} // namespace

BlockEditResult BlockEditor::breakBlock(ChunkManager& chunks, PlanetCoord position) const
{
    position = normalized(position);
    BlockEditResult result;

    auto* chunk = chunks.find(position.chunk);
    if (chunk == nullptr) {
        return result;
    }

    const auto previous = chunk->blockAt(position.block.x, position.block.y, position.block.z);
    if (previous.value == AirBlockState.value) {
        return result;
    }

    const auto prevType = blockTypeOf(previous);
    const auto* prevDef = registry_.registry().byRuntimeId(prevType.value);
    if (prevDef != nullptr && prevDef->hasBlockEntity) {
        chunk->removeBlockEntity(Chunk::index(position.block.x, position.block.y, position.block.z));
    }

    chunk->setBlock(position.block.x, position.block.y, position.block.z, AirBlockState);
    result.changed = true;
    result.deltas.push_back(BlockDelta{position, previous, AirBlockState});
    pushUnique(result.dirtiedChunks, position.chunk);
    markBorderNeighbors(chunks, position, result);
    return result;
}

BlockEditResult BlockEditor::placeBlock(ChunkManager& chunks, PlanetCoord position, BlockStateId next) const
{
    position = normalized(position);
    BlockEditResult result;

    auto& chunk = chunks.createOrGet(position.chunk);
    const auto previous = chunk.blockAt(position.block.x, position.block.y, position.block.z);
    if (previous.value != AirBlockState.value || next.value == AirBlockState.value) {
        return result;
    }

    chunk.setBlock(position.block.x, position.block.y, position.block.z, next);
    if (chunk.state() == ChunkState::Empty || chunk.state() == ChunkState::Requested) {
        chunk.setState(ChunkState::Resident);
    }

    const auto nextType = blockTypeOf(next);
    const auto* nextDef = registry_.registry().byRuntimeId(nextType.value);
    if (nextDef != nullptr && nextDef->hasBlockEntity) {
        chunk.createBlockEntity(Chunk::index(position.block.x, position.block.y, position.block.z));
    }

    result.changed = true;
    result.deltas.push_back(BlockDelta{position, previous, next});
    pushUnique(result.dirtiedChunks, position.chunk);
    markBorderNeighbors(chunks, position, result);
    return result;
}

void BlockEditor::markBorderNeighbors(ChunkManager& chunks, PlanetCoord position, BlockEditResult& result)
{
    const auto mark = [&chunks, &result](ChunkCoord coord) {
        if (auto* neighbor = chunks.find(coord)) {
            neighbor->markMeshDirtyNoRevision();
            neighbor->markLightingDirtyNoRevision();
            pushUnique(result.dirtiedChunks, coord);
        }
    };

    if (position.block.x == 0) {
        mark({position.chunk.x - 1, position.chunk.y, position.chunk.z});
    } else if (position.block.x == ChunkSize - 1) {
        mark({position.chunk.x + 1, position.chunk.y, position.chunk.z});
    }

    if (position.block.y == 0) {
        mark({position.chunk.x, position.chunk.y - 1, position.chunk.z});
    } else if (position.block.y == ChunkSize - 1) {
        mark({position.chunk.x, position.chunk.y + 1, position.chunk.z});
    }

    if (position.block.z == 0) {
        mark({position.chunk.x, position.chunk.y, position.chunk.z - 1});
    } else if (position.block.z == ChunkSize - 1) {
        mark({position.chunk.x, position.chunk.y, position.chunk.z + 1});
    }
}

} // namespace voxel::world
