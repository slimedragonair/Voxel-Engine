#include <voxel/world/WorldDelta.hpp>

namespace voxel::world {

namespace {

void applyBlockDelta(ChunkManager& chunks, const BlockDelta& delta)
{
    auto& chunk = chunks.createOrGet(delta.position.chunk);
    chunk.setBlock(delta.position.block.x, delta.position.block.y, delta.position.block.z, delta.next);
}

} // namespace

void WorldDeltaApplier::apply(ChunkManager& chunks, const WorldDeltaBatch& batch) const
{
    for (const auto& delta : batch) {
        std::visit([&chunks](const auto& typedDelta) {
            applyBlockDelta(chunks, typedDelta);
        }, delta);
    }
}

} // namespace voxel::world

