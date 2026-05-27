#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/data/ItemRegistry.hpp>

namespace voxel::inventory {

struct ItemStack {
    ItemTypeId itemId{0};
    std::uint32_t count{0};
    std::uint16_t durability{0};

    [[nodiscard]] bool empty() const noexcept { return count == 0 || itemId.value == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return !empty(); }

    void clear()
    {
        itemId = ItemTypeId{0};
        count = 0;
        durability = 0;
    }
};

enum class SlotKind : std::uint8_t {
    Generic,
    Hotbar,
    Helmet,
    Chest,
    Legs,
    Boots,
    Gloves,
    Back,
    Accessory,
    MainHand,
    OffHand,
    Fuel,
    MachineInput,
    MachineOutput,
    Spell,
};

struct InventorySlot {
    ItemStack stack{};
    SlotKind kind{SlotKind::Generic};
    bool locked{false};
    bool favorite{false};
};

class Inventory {
public:
    explicit Inventory(std::size_t slotCount);
    Inventory(std::size_t slotCount, SlotKind kind);

    [[nodiscard]] std::size_t slotCount() const noexcept { return slots_.size(); }
    [[nodiscard]] const ItemStack& slot(std::size_t index) const;
    [[nodiscard]] const InventorySlot& slotInfo(std::size_t index) const;
    [[nodiscard]] std::size_t itemCount(ItemTypeId itemId) const;

    ItemStack insert(ItemStack stack, const data::ItemRegistry& itemRegistry);
    ItemStack insertAt(std::size_t index, ItemStack stack, const data::ItemRegistry& itemRegistry);
    [[nodiscard]] bool canInsertAt(std::size_t index, const ItemStack& stack, const data::ItemRegistry& itemRegistry) const;
    [[nodiscard]] bool swapSlots(std::size_t lhs, std::size_t rhs, const data::ItemRegistry& itemRegistry);
    [[nodiscard]] bool tryMergeIntoSlot(std::size_t index, ItemStack& carried, const data::ItemRegistry& itemRegistry);
    [[nodiscard]] bool placeOneIntoSlot(std::size_t index, ItemStack& carried, const data::ItemRegistry& itemRegistry);
    [[nodiscard]] bool moveSlotTo(std::size_t sourceIndex, Inventory& destination, const data::ItemRegistry& itemRegistry);
    [[nodiscard]] std::optional<ItemStack> takeHalf(std::size_t index);

    std::optional<ItemStack> extract(std::size_t index, std::uint32_t count);
    std::optional<ItemStack> extractItem(ItemTypeId itemId, std::uint32_t count);

    [[nodiscard]] bool canInsert(const ItemStack& stack, const data::ItemRegistry& itemRegistry) const;
    [[nodiscard]] SlotKind slotKind(std::size_t index) const;
    void setSlotKind(std::size_t index, SlotKind kind);
    void setSlotLocked(std::size_t index, bool locked);
    void setSlotFavorite(std::size_t index, bool favorite);
    [[nodiscard]] bool slotLocked(std::size_t index) const;
    [[nodiscard]] bool slotFavorite(std::size_t index) const;

    void setDirty(bool dirty) noexcept { dirty_ = dirty; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void clearDirty() noexcept { dirty_ = false; }

    using SlotCallback = std::function<void(std::size_t)>;
    void setOnSlotChanged(SlotCallback callback) { onSlotChanged_ = std::move(callback); }

private:
    void notifySlotChanged(std::size_t index);

    std::vector<InventorySlot> slots_;
    bool dirty_{false};
    SlotCallback onSlotChanged_;
};

class PlayerInventory {
public:
    static constexpr std::size_t kBackpackColumns = 12;
    static constexpr std::size_t kBackpackRows = 10;
    static constexpr std::size_t kMainSlots = kBackpackColumns * kBackpackRows;
    static constexpr std::size_t kHotbarSlots = 12;
    static constexpr std::size_t kArmorSlots = 4;
    static constexpr std::size_t kEquipmentSlots = 8;
    static constexpr std::size_t kAccessorySlots = 6;
    static constexpr std::size_t kOffhandSlots = 1;

    PlayerInventory();

    Inventory& mainInventory() noexcept { return main_; }
    const Inventory& mainInventory() const noexcept { return main_; }

    Inventory& hotbarInventory() noexcept { return hotbar_; }
    const Inventory& hotbarInventory() const noexcept { return hotbar_; }

    [[nodiscard]] std::size_t selectedHotbarSlot() const noexcept { return selectedHotbarSlot_; }
    void selectHotbarSlot(std::size_t slot);
    [[nodiscard]] const ItemStack& selectedHotbarItem() const;

    Inventory& equipmentInventory() noexcept { return equipment_; }
    const Inventory& equipmentInventory() const noexcept { return equipment_; }

    Inventory& armorInventory() noexcept { return armor_; }
    const Inventory& armorInventory() const noexcept { return armor_; }

    Inventory& accessoryInventory() noexcept { return accessories_; }
    const Inventory& accessoryInventory() const noexcept { return accessories_; }

    Inventory& offhandInventory() noexcept { return offhand_; }
    const Inventory& offhandInventory() const noexcept { return offhand_; }

    ItemStack insert(ItemStack stack, const data::ItemRegistry& itemRegistry);

private:
    Inventory main_;
    Inventory hotbar_;
    Inventory equipment_;
    Inventory armor_;
    Inventory accessories_;
    Inventory offhand_;
    std::size_t selectedHotbarSlot_{0};
};

class InventoryManager {
public:
    using InventoryKey = std::uint64_t;

    Inventory& createInventory(InventoryKey key, std::size_t slotCount);
    [[nodiscard]] Inventory* find(InventoryKey key);
    [[nodiscard]] const Inventory* find(InventoryKey key) const;
    void remove(InventoryKey key);

    [[nodiscard]] std::size_t inventoryCount() const noexcept;

private:
    std::unordered_map<InventoryKey, std::unique_ptr<Inventory>> inventories_;
};

} // namespace voxel::inventory
