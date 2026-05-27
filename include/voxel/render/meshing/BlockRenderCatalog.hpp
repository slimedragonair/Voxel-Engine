#pragma once

#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/render/meshing/GreedyMesher.hpp>

namespace voxel::render::meshing {

struct BlockRenderInfo {
    MeshSurface surface{MeshSurface::Opaque};
    bool occludesNeighborFaces{true};
    bool isFluid{false};
};

class BlockRenderCatalog {
public:
    void set(BlockTypeId type, BlockRenderInfo info);
    [[nodiscard]] BlockRenderInfo get(BlockStateId state) const noexcept;
    [[nodiscard]] BlockRenderInfo getByType(BlockTypeId type) const noexcept;

private:
    std::vector<BlockRenderInfo> entries_;
};

} // namespace voxel::render::meshing
