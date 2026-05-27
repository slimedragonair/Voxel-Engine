#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/data/Identifier.hpp>
#include <voxel/data/Registry.hpp>

namespace voxel::data {

struct ItemDefinition {
    Identifier id;
    std::string displayName;
    std::uint32_t maxStackSize{64};
    std::vector<std::string> tags;
    std::string toolType;
    std::uint16_t durability{0};
    Identifier blockPlacementId;
    bool hasBlockPlacement() const noexcept { return !blockPlacementId.path.empty(); }
};

class ItemRegistry {
public:
    ItemTypeId registerItem(ItemDefinition definition);
    [[nodiscard]] const ItemDefinition* find(const Identifier& id) const;
    [[nodiscard]] ItemTypeId findRuntimeId(const Identifier& id) const;
    [[nodiscard]] const Registry<ItemDefinition>& registry() const noexcept;

private:
    Registry<ItemDefinition> registry_;
};

} // namespace voxel::data
