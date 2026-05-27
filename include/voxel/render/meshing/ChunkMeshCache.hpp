#pragma once

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include <voxel/render/meshing/GreedyMesher.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::render::meshing {

class ChunkMeshCache {
public:
    void store(world::ChunkCoord coord, ChunkMesh mesh);
    [[nodiscard]] const ChunkMesh* find(world::ChunkCoord coord) const;
    [[nodiscard]] bool isCurrent(world::ChunkCoord coord, Revision revision) const;
    void remove(world::ChunkCoord coord);
    [[nodiscard]] std::vector<world::ChunkCoord> removeOutsideRadius(world::ChunkCoord center, int horizontalRadius, int verticalRadius);
    void clear();
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<world::ChunkCoord, ChunkMesh, world::ChunkCoordHash> meshes_;
};

} // namespace voxel::render::meshing
