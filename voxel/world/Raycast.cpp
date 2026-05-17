#include <voxel/world/Raycast.hpp>

#include <cmath>
#include <limits>

#include <voxel/world/CoordinateUtils.hpp>

namespace voxel::world {

namespace {

constexpr float Epsilon = 0.00001F;

float axisTDelta(float direction) noexcept
{
    if (std::abs(direction) < Epsilon) {
        return std::numeric_limits<float>::infinity();
    }
    return std::abs(1.0F / direction);
}

float axisTMax(float origin, int block, float direction) noexcept
{
    if (std::abs(direction) < Epsilon) {
        return std::numeric_limits<float>::infinity();
    }

    const float nextBoundary = direction > 0.0F ? static_cast<float>(block + 1) : static_cast<float>(block);
    return (nextBoundary - origin) / direction;
}

} // namespace

std::optional<VoxelRaycastHit> VoxelRaycaster::cast(const ChunkManager& chunks, const VoxelRay& ray) const
{
    const auto direction = core::normalize(ray.direction);
    if (core::dot(direction, direction) <= Epsilon) {
        return std::nullopt;
    }

    int x = static_cast<int>(std::floor(ray.origin.x));
    int y = static_cast<int>(std::floor(ray.origin.y));
    int z = static_cast<int>(std::floor(ray.origin.z));

    const int stepX = direction.x > 0.0F ? 1 : -1;
    const int stepY = direction.y > 0.0F ? 1 : -1;
    const int stepZ = direction.z > 0.0F ? 1 : -1;

    float tMaxX = axisTMax(ray.origin.x, x, direction.x);
    float tMaxY = axisTMax(ray.origin.y, y, direction.y);
    float tMaxZ = axisTMax(ray.origin.z, z, direction.z);
    const float tDeltaX = axisTDelta(direction.x);
    const float tDeltaY = axisTDelta(direction.y);
    const float tDeltaZ = axisTDelta(direction.z);

    BlockCoord normal{};
    float distance = 0.0F;

    while (distance <= ray.maxDistance) {
        const auto chunkLocal = toChunkLocal(x, y, z);
        if (const auto* chunk = chunks.find(chunkLocal.chunk)) {
            const auto block = chunk->blockAt(chunkLocal.local.x, chunkLocal.local.y, chunkLocal.local.z);
            if (block.value != AirBlockState.value) {
                return VoxelRaycastHit{
                    {ray.planetId, {}, chunkLocal.chunk, chunkLocal.local},
                    normal,
                    block,
                    distance
                };
            }
        }

        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX;
            distance = tMaxX;
            tMaxX += tDeltaX;
            normal = {-stepX, 0, 0};
        } else if (tMaxY < tMaxZ) {
            y += stepY;
            distance = tMaxY;
            tMaxY += tDeltaY;
            normal = {0, -stepY, 0};
        } else {
            z += stepZ;
            distance = tMaxZ;
            tMaxZ += tDeltaZ;
            normal = {0, 0, -stepZ};
        }
    }

    return std::nullopt;
}

} // namespace voxel::world

