#include <voxel/player/PlayerSpawnResolver.hpp>

#include <algorithm>
#include <cmath>

#include <voxel/player/PlayerController.hpp>
#include <voxel/world/CoordinateUtils.hpp>

namespace voxel::player {

PlayerSpawnResolver::PlayerSpawnResolver(PlayerSpawnResolverConfig config)
    : config_(config)
{
}

std::optional<core::Vec3> PlayerSpawnResolver::resolve(
    const world::ChunkManager& chunks,
    float worldX,
    float worldZ) const
{
    const auto blockX = static_cast<std::int64_t>(std::floor(worldX));
    const auto blockZ = static_cast<std::int64_t>(std::floor(worldZ));
    const auto top = std::max(config_.minWorldY, config_.maxWorldY);
    const auto bottom = std::min(config_.minWorldY, config_.maxWorldY);
    const auto chunkX = world::floorDiv(blockX, world::ChunkSize);
    const auto chunkZ = world::floorDiv(blockZ, world::ChunkSize);
    const auto localX = world::floorMod(blockX, world::ChunkSize);
    const auto localZ = world::floorMod(blockZ, world::ChunkSize);
    const auto topChunkY = world::floorDiv(top, world::ChunkSize);
    const auto bottomChunkY = world::floorDiv(bottom, world::ChunkSize);

    bool sawAnyColumnChunk = false;
    for (auto chunkY = topChunkY; chunkY >= bottomChunkY; --chunkY) {
        const auto* chunk = chunks.find({chunkX, chunkY, chunkZ});
        if (chunk == nullptr) {
            continue;
        }
        sawAnyColumnChunk = true;

        for (int localY = world::ChunkSize - 1; localY >= 0; --localY) {
            const auto y = static_cast<int>(chunkY * world::ChunkSize + localY);
            if (y > top || y < bottom) {
                continue;
            }

            const auto block = chunk->blockAt(localX, localY, localZ);
            if (!PlayerController::blockIsSolid(block)) {
                continue;
            }

            const auto above = world::toChunkLocal(blockX, static_cast<std::int64_t>(y) + 1, blockZ);
            const auto* aboveChunk = chunks.find(above.chunk);
            if (aboveChunk != nullptr
                && PlayerController::blockIsSolid(aboveChunk->blockAt(above.local.x, above.local.y, above.local.z))) {
                continue;
            }
            return core::Vec3{worldX, static_cast<float>(y) + config_.feetClearance, worldZ};
        }
    }

    if (!sawAnyColumnChunk) {
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace voxel::player
