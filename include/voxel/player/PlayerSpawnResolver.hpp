#pragma once

#include <optional>

#include <voxel/core/Math.hpp>
#include <voxel/world/BlockCollisionCatalog.hpp>
#include <voxel/world/ChunkManager.hpp>

namespace voxel::player {

struct PlayerSpawnResolverConfig {
    int minWorldY{-1024};
    int maxWorldY{768};
    float feetClearance{1.0F};
};

class PlayerSpawnResolver {
public:
    explicit PlayerSpawnResolver(PlayerSpawnResolverConfig config = {});
    void setCollisionCatalog(world::BlockCollisionCatalog catalog);

    [[nodiscard]] std::optional<core::Vec3> resolve(
        const world::ChunkManager& chunks,
        float worldX,
        float worldZ) const;

private:
    PlayerSpawnResolverConfig config_{};
    world::BlockCollisionCatalog collisionCatalog_{};
};

} // namespace voxel::player
