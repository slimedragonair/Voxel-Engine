#include <voxel/render/meshing/BlockRenderCatalog.hpp>

namespace voxel::render::meshing {

void BlockRenderCatalog::set(BlockTypeId type, BlockRenderInfo info)
{
    if (entries_.size() <= type.value) {
        entries_.resize(static_cast<std::size_t>(type.value) + 1U);
    }
    entries_[type.value] = info;
}

BlockRenderInfo BlockRenderCatalog::get(BlockStateId state) const noexcept
{
    return getByType(world::blockTypeOf(state));
}

BlockRenderInfo BlockRenderCatalog::getByType(BlockTypeId type) const noexcept
{
    if (type.value < entries_.size()) {
        return entries_[type.value];
    }
    return {};
}

} // namespace voxel::render::meshing
