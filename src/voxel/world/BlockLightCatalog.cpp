#include <voxel/world/BlockLightCatalog.hpp>

namespace voxel::world {

void BlockLightCatalog::set(BlockTypeId type, BlockLightInfo info)
{
    if (type.value >= entries_.size()) {
        entries_.resize(type.value + 1U);
    }
    entries_[type.value] = info;
}

BlockLightInfo BlockLightCatalog::get(BlockStateId state) const noexcept
{
    return getByType(blockTypeOf(state));
}

BlockLightInfo BlockLightCatalog::getByType(BlockTypeId type) const noexcept
{
    if (type.value < entries_.size()) {
        return entries_[type.value];
    }
    return BlockLightInfo{};
}

} // namespace voxel::world
