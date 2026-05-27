#pragma once

#include <filesystem>

#include <voxel/data/ItemRegistry.hpp>
#include <voxel/inventory/Inventory.hpp>

namespace voxel::save {

class PlayerInventorySaveService {
public:
    [[nodiscard]] bool save(const std::filesystem::path& worldRoot,
                            const inventory::PlayerInventory& inventory,
                            const data::ItemRegistry& itemRegistry) const;

    [[nodiscard]] bool load(const std::filesystem::path& worldRoot,
                            inventory::PlayerInventory& inventory,
                            const data::ItemRegistry& itemRegistry) const;

    [[nodiscard]] std::filesystem::path inventoryPath(const std::filesystem::path& worldRoot) const;
};

} // namespace voxel::save
