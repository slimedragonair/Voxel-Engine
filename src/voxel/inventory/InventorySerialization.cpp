#include <voxel/inventory/InventorySerialization.hpp>

#include <nlohmann/json.hpp>

namespace voxel::inventory {

namespace {

[[nodiscard]] data::Identifier parseIdentifier(std::string_view text)
{
    const auto separator = text.find(':');
    if (separator == std::string_view::npos) {
        return {"core", std::string{text}};
    }
    return {std::string{text.substr(0, separator)}, std::string{text.substr(separator + 1)}};
}

void serializeInventory(nlohmann::json& out, const char* key, const Inventory& inventory, const data::ItemRegistry& itemRegistry)
{
    auto slots = nlohmann::json::array();
    for (std::size_t i = 0; i < inventory.slotCount(); ++i) {
        const auto& info = inventory.slotInfo(i);
        if (info.stack.empty() && !info.locked && !info.favorite) {
            continue;
        }

        nlohmann::json slot;
        slot["slot"] = i;
        if (!info.stack.empty()) {
            const auto* item = itemRegistry.registry().byRuntimeId(info.stack.itemId.value);
            if (item == nullptr) {
                continue;
            }
            slot["item"] = item->id.str();
            slot["count"] = info.stack.count;
            if (info.stack.durability != 0) {
                slot["durability"] = info.stack.durability;
            }
        }
        if (info.locked) {
            slot["locked"] = true;
        }
        if (info.favorite) {
            slot["favorite"] = true;
        }
        slots.push_back(std::move(slot));
    }
    out[key] = std::move(slots);
}

void deserializeInventory(const nlohmann::json& root,
                          const char* key,
                          Inventory& inventory,
                          const data::ItemRegistry& itemRegistry)
{
    if (!root.contains(key) || !root[key].is_array()) {
        return;
    }

    for (const auto& entry : root[key]) {
        const auto index = entry.value("slot", static_cast<std::size_t>(inventory.slotCount()));
        if (index >= inventory.slotCount()) {
            continue;
        }

        if (!entry.contains("item") || !entry["item"].is_string()) {
            inventory.setSlotLocked(index, entry.value("locked", false));
            inventory.setSlotFavorite(index, entry.value("favorite", false));
            continue;
        }

        const auto itemId = itemRegistry.findRuntimeId(parseIdentifier(entry["item"].get<std::string>()));
        const auto count = entry.value("count", 0U);
        if (itemId.value == 0 || count == 0) {
            continue;
        }

        const auto durability = static_cast<std::uint16_t>(entry.value("durability", 0));
        inventory.insertAt(index, ItemStack{itemId, count, durability}, itemRegistry);
        inventory.setSlotLocked(index, entry.value("locked", false));
        inventory.setSlotFavorite(index, entry.value("favorite", false));
    }
}

} // namespace

std::string serializePlayerInventoryJson(const PlayerInventory& inventory, const data::ItemRegistry& itemRegistry)
{
    nlohmann::json root;
    root["version"] = 1;
    root["selected_hotbar"] = inventory.selectedHotbarSlot();
    serializeInventory(root, "hotbar", inventory.hotbarInventory(), itemRegistry);
    serializeInventory(root, "main", inventory.mainInventory(), itemRegistry);
    serializeInventory(root, "equipment", inventory.equipmentInventory(), itemRegistry);
    serializeInventory(root, "armor", inventory.armorInventory(), itemRegistry);
    serializeInventory(root, "accessories", inventory.accessoryInventory(), itemRegistry);
    serializeInventory(root, "offhand", inventory.offhandInventory(), itemRegistry);
    return root.dump(2);
}

bool deserializePlayerInventoryJson(std::string_view jsonText,
                                    PlayerInventory& inventory,
                                    const data::ItemRegistry& itemRegistry)
{
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(jsonText);
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }

    if (!root.is_object() || root.value("version", 0) != 1) {
        return false;
    }

    PlayerInventory loaded;
    loaded.selectHotbarSlot(root.value("selected_hotbar", 0U));
    deserializeInventory(root, "hotbar", loaded.hotbarInventory(), itemRegistry);
    deserializeInventory(root, "main", loaded.mainInventory(), itemRegistry);
    deserializeInventory(root, "equipment", loaded.equipmentInventory(), itemRegistry);
    deserializeInventory(root, "armor", loaded.armorInventory(), itemRegistry);
    deserializeInventory(root, "accessories", loaded.accessoryInventory(), itemRegistry);
    deserializeInventory(root, "offhand", loaded.offhandInventory(), itemRegistry);
    inventory = std::move(loaded);
    return true;
}

} // namespace voxel::inventory
