#include <voxel/data/ItemRegistry.hpp>

namespace voxel::data {

ItemTypeId ItemRegistry::registerItem(ItemDefinition definition)
{
    return ItemTypeId{registry_.add(std::move(definition))};
}

const ItemDefinition* ItemRegistry::find(const Identifier& id) const
{
    return registry_.find(id);
}

ItemTypeId ItemRegistry::findRuntimeId(const Identifier& id) const
{
    return ItemTypeId{registry_.runtimeId(id)};
}

const Registry<ItemDefinition>& ItemRegistry::registry() const noexcept
{
    return registry_;
}

} // namespace voxel::data
