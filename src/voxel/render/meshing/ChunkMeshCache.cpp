#include <voxel/render/meshing/ChunkMeshCache.hpp>

#include <utility>

namespace voxel::render::meshing {

void ChunkMeshCache::store(world::ChunkCoord coord, ChunkMesh mesh)
{
    meshes_[coord] = std::move(mesh);
}

const ChunkMesh* ChunkMeshCache::find(world::ChunkCoord coord) const
{
    const auto found = meshes_.find(coord);
    if (found == meshes_.end()) {
        return nullptr;
    }
    return &found->second;
}

bool ChunkMeshCache::isCurrent(world::ChunkCoord coord, Revision revision) const
{
    const auto* mesh = find(coord);
    return mesh != nullptr && mesh->sourceRevision == revision;
}

void ChunkMeshCache::remove(world::ChunkCoord coord)
{
    meshes_.erase(coord);
}

std::vector<world::ChunkCoord> ChunkMeshCache::removeOutsideRadius(
    world::ChunkCoord center,
    int horizontalRadius,
    int verticalRadius,
    const std::function<bool(world::ChunkCoord)>& keep)
{
    std::vector<world::ChunkCoord> removed;
    for (auto it = meshes_.begin(); it != meshes_.end();) {
        const auto coord = it->first;
        if (keep && keep(coord)) {
            ++it;
            continue;
        }
        const auto dx = coord.x - center.x;
        const auto dy = coord.y - center.y;
        const auto dz = coord.z - center.z;
        if (dx < -horizontalRadius || dx > horizontalRadius
            || dz < -horizontalRadius || dz > horizontalRadius
            || dy < -verticalRadius || dy > verticalRadius) {
            removed.push_back(coord);
            it = meshes_.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

void ChunkMeshCache::clear()
{
    meshes_.clear();
}

std::size_t ChunkMeshCache::size() const noexcept
{
    return meshes_.size();
}

} // namespace voxel::render::meshing
