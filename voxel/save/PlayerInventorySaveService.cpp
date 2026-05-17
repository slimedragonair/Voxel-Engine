#include <voxel/save/PlayerInventorySaveService.hpp>

#include <fstream>
#include <sstream>

#include <voxel/inventory/InventorySerialization.hpp>

namespace voxel::save {

bool PlayerInventorySaveService::save(const std::filesystem::path& worldRoot,
                                      const inventory::PlayerInventory& inventory,
                                      const data::ItemRegistry& itemRegistry) const
{
    const auto path = inventoryPath(worldRoot);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    out << inventory::serializePlayerInventoryJson(inventory, itemRegistry);
    return static_cast<bool>(out);
}

bool PlayerInventorySaveService::load(const std::filesystem::path& worldRoot,
                                      inventory::PlayerInventory& inventory,
                                      const data::ItemRegistry& itemRegistry) const
{
    const auto path = inventoryPath(worldRoot);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return inventory::deserializePlayerInventoryJson(buffer.str(), inventory, itemRegistry);
}

std::filesystem::path PlayerInventorySaveService::inventoryPath(const std::filesystem::path& worldRoot) const
{
    return worldRoot / "player" / "inventory.json";
}

} // namespace voxel::save
