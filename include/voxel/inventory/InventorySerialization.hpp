#pragma once

#include <string>
#include <string_view>

#include <voxel/data/ItemRegistry.hpp>
#include <voxel/inventory/Inventory.hpp>

namespace voxel::inventory {

[[nodiscard]] std::string serializePlayerInventoryJson(const PlayerInventory& inventory,
                                                       const data::ItemRegistry& itemRegistry);

[[nodiscard]] bool deserializePlayerInventoryJson(std::string_view jsonText,
                                                  PlayerInventory& inventory,
                                                  const data::ItemRegistry& itemRegistry);

} // namespace voxel::inventory
