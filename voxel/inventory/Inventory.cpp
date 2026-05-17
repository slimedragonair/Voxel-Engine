#include <voxel/inventory/Inventory.hpp>

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace voxel::inventory {

namespace {

[[nodiscard]] bool itemHasTag(const data::ItemDefinition& itemDef, std::string_view tag) noexcept
{
    return std::any_of(itemDef.tags.begin(), itemDef.tags.end(), [tag](const std::string& candidate) {
        return candidate == tag;
    });
}

[[nodiscard]] bool itemAllowedInSlot(SlotKind kind, const data::ItemDefinition* itemDef) noexcept
{
    if (kind == SlotKind::Generic || kind == SlotKind::Hotbar || kind == SlotKind::MachineInput
        || kind == SlotKind::MachineOutput || kind == SlotKind::MainHand || kind == SlotKind::OffHand) {
        return true;
    }

    if (itemDef == nullptr) {
        return false;
    }

    switch (kind) {
    case SlotKind::Helmet:
        return itemHasTag(*itemDef, "helmet");
    case SlotKind::Chest:
        return itemHasTag(*itemDef, "chest") || itemHasTag(*itemDef, "chestplate");
    case SlotKind::Legs:
        return itemHasTag(*itemDef, "legs") || itemHasTag(*itemDef, "leggings");
    case SlotKind::Boots:
        return itemHasTag(*itemDef, "boots");
    case SlotKind::Gloves:
        return itemHasTag(*itemDef, "gloves");
    case SlotKind::Back:
        return itemHasTag(*itemDef, "back") || itemHasTag(*itemDef, "backpack");
    case SlotKind::Accessory:
        return itemHasTag(*itemDef, "accessory") || itemHasTag(*itemDef, "relic") || itemHasTag(*itemDef, "charm");
    case SlotKind::Fuel:
        return itemHasTag(*itemDef, "fuel");
    case SlotKind::Spell:
        return itemHasTag(*itemDef, "spell") || itemHasTag(*itemDef, "magic");
    case SlotKind::Generic:
    case SlotKind::Hotbar:
    case SlotKind::MainHand:
    case SlotKind::OffHand:
    case SlotKind::MachineInput:
    case SlotKind::MachineOutput:
        return true;
    }

    return false;
}

[[nodiscard]] std::uint32_t maxStackFor(ItemTypeId itemId, const data::ItemRegistry& itemRegistry) noexcept
{
    const auto* itemDef = itemRegistry.registry().byRuntimeId(itemId.value);
    return itemDef != nullptr ? itemDef->maxStackSize : 64U;
}

} // namespace

Inventory::Inventory(std::size_t slotCount)
    : slots_(slotCount)
{
}

Inventory::Inventory(std::size_t slotCount, SlotKind kind)
    : slots_(slotCount)
{
    for (auto& slot : slots_) {
        slot.kind = kind;
    }
}

const ItemStack& Inventory::slot(std::size_t index) const
{
    return slots_[index].stack;
}

const InventorySlot& Inventory::slotInfo(std::size_t index) const
{
    return slots_[index];
}

std::size_t Inventory::itemCount(ItemTypeId itemId) const
{
    std::size_t total = 0;
    for (const auto& slot : slots_) {
        if (slot.stack.itemId == itemId) {
            total += slot.stack.count;
        }
    }
    return total;
}

ItemStack Inventory::insert(ItemStack stack, const data::ItemRegistry& itemRegistry)
{
    if (stack.empty()) {
        return {};
    }

    const std::uint32_t maxStack = maxStackFor(stack.itemId, itemRegistry);

    for (std::size_t i = 0; i < slots_.size() && !stack.empty(); ++i) {
        auto& slot = slots_[i];
        if (slot.locked || !canInsertAt(i, stack, itemRegistry)) {
            continue;
        }
        if (slot.stack.itemId == stack.itemId && slot.stack.count < maxStack) {
            const auto space = maxStack - slot.stack.count;
            const auto toAdd = std::min(space, stack.count);
            slot.stack.count += toAdd;
            stack.count -= toAdd;
            notifySlotChanged(i);
        }
    }

    for (std::size_t i = 0; i < slots_.size() && !stack.empty(); ++i) {
        auto& slot = slots_[i];
        if (slot.locked || !canInsertAt(i, stack, itemRegistry)) {
            continue;
        }
        if (slot.stack.empty()) {
            const auto toAdd = std::min(maxStack, stack.count);
            slot.stack = ItemStack{stack.itemId, toAdd, stack.durability};
            stack.count -= toAdd;
            notifySlotChanged(i);
        }
    }

    dirty_ = true;
    return stack;
}

ItemStack Inventory::insertAt(std::size_t index, ItemStack stack, const data::ItemRegistry& itemRegistry)
{
    if (index >= slots_.size() || stack.empty()) {
        return stack;
    }

    auto& slot = slots_[index];
    if (!canInsertAt(index, stack, itemRegistry)) {
        return stack;
    }

    const std::uint32_t maxStack = maxStackFor(stack.itemId, itemRegistry);

    if (slot.stack.empty()) {
        const auto toAdd = std::min(maxStack, stack.count);
        slot.stack = ItemStack{stack.itemId, toAdd, stack.durability};
        stack.count -= toAdd;
    } else if (slot.stack.itemId == stack.itemId && slot.stack.count < maxStack) {
        const auto space = maxStack - slot.stack.count;
        const auto toAdd = std::min(space, stack.count);
        slot.stack.count += toAdd;
        stack.count -= toAdd;
    }

    notifySlotChanged(index);
    dirty_ = true;
    return stack;
}

bool Inventory::canInsertAt(std::size_t index, const ItemStack& stack, const data::ItemRegistry& itemRegistry) const
{
    if (index >= slots_.size()) {
        return false;
    }
    if (stack.empty()) {
        return true;
    }

    const auto& slot = slots_[index];
    const auto* itemDef = itemRegistry.registry().byRuntimeId(stack.itemId.value);
    if (!itemAllowedInSlot(slot.kind, itemDef)) {
        return false;
    }
    if (slot.locked) {
        return false;
    }
    if (slot.stack.empty()) {
        return true;
    }
    return slot.stack.itemId == stack.itemId && slot.stack.count < maxStackFor(stack.itemId, itemRegistry);
}

bool Inventory::swapSlots(std::size_t lhs, std::size_t rhs, const data::ItemRegistry& itemRegistry)
{
    if (lhs >= slots_.size() || rhs >= slots_.size()) {
        return false;
    }
    if (lhs == rhs) {
        return true;
    }

    auto& left = slots_[lhs];
    auto& right = slots_[rhs];
    if (left.locked || right.locked) {
        return false;
    }

    const auto leftStack = left.stack;
    const auto rightStack = right.stack;
    if (!leftStack.empty()) {
        const auto* leftDef = itemRegistry.registry().byRuntimeId(leftStack.itemId.value);
        if (!itemAllowedInSlot(right.kind, leftDef)) {
            return false;
        }
    }
    if (!rightStack.empty()) {
        const auto* rightDef = itemRegistry.registry().byRuntimeId(rightStack.itemId.value);
        if (!itemAllowedInSlot(left.kind, rightDef)) {
            return false;
        }
    }

    std::swap(left.stack, right.stack);
    notifySlotChanged(lhs);
    notifySlotChanged(rhs);
    dirty_ = true;
    return true;
}

bool Inventory::tryMergeIntoSlot(std::size_t index, ItemStack& carried, const data::ItemRegistry& itemRegistry)
{
    if (carried.empty()) {
        return false;
    }

    const auto before = carried.count;
    carried = insertAt(index, carried, itemRegistry);
    return carried.count != before;
}

bool Inventory::placeOneIntoSlot(std::size_t index, ItemStack& carried, const data::ItemRegistry& itemRegistry)
{
    if (carried.empty()) {
        return false;
    }

    ItemStack one{carried.itemId, 1, carried.durability};
    one = insertAt(index, one, itemRegistry);
    if (!one.empty()) {
        return false;
    }

    --carried.count;
    if (carried.count == 0) {
        carried.clear();
    }
    return true;
}

bool Inventory::moveSlotTo(std::size_t sourceIndex, Inventory& destination, const data::ItemRegistry& itemRegistry)
{
    if (sourceIndex >= slots_.size()) {
        return false;
    }

    auto& source = slots_[sourceIndex];
    if (source.locked || source.stack.empty() || !destination.canInsert(source.stack, itemRegistry)) {
        return false;
    }

    const auto moving = source.stack;
    source.stack.clear();
    notifySlotChanged(sourceIndex);
    dirty_ = true;

    const auto remainder = destination.insert(moving, itemRegistry);
    if (!remainder.empty()) {
        source.stack = remainder;
        notifySlotChanged(sourceIndex);
        return false;
    }

    return true;
}

std::optional<ItemStack> Inventory::takeHalf(std::size_t index)
{
    if (index >= slots_.size()) {
        return std::nullopt;
    }

    const auto& slot = slots_[index];
    if (slot.locked || slot.stack.empty()) {
        return std::nullopt;
    }

    const auto count = (slot.stack.count + 1U) / 2U;
    return extract(index, count);
}

std::optional<ItemStack> Inventory::extract(std::size_t index, std::uint32_t count)
{
    if (index >= slots_.size()) {
        return std::nullopt;
    }

    auto& slot = slots_[index];
    if (slot.locked || slot.stack.empty()) {
        return std::nullopt;
    }

    const auto toExtract = std::min(count, slot.stack.count);
    ItemStack result{slot.stack.itemId, toExtract, slot.stack.durability};
    slot.stack.count -= toExtract;
    if (slot.stack.count == 0) {
        slot.stack.clear();
    }

    notifySlotChanged(index);
    dirty_ = true;
    return result;
}

std::optional<ItemStack> Inventory::extractItem(ItemTypeId itemId, std::uint32_t count)
{
    std::uint32_t remaining = count;
    ItemStack result{itemId, 0, 0};

    for (std::size_t i = 0; i < slots_.size() && remaining > 0; ++i) {
        auto& slot = slots_[i];
        if (slot.locked || slot.stack.itemId != itemId) {
            continue;
        }
        const auto toExtract = std::min(remaining, slot.stack.count);
        result.count += toExtract;
        result.durability = slot.stack.durability;
        remaining -= toExtract;
        slot.stack.count -= toExtract;
        if (slot.stack.count == 0) {
            slot.stack.clear();
        }
        notifySlotChanged(i);
    }

    if (result.count == 0) {
        return std::nullopt;
    }

    dirty_ = true;
    return result;
}

bool Inventory::canInsert(const ItemStack& stack, const data::ItemRegistry& itemRegistry) const
{
    if (stack.empty()) {
        return true;
    }

    const std::uint32_t maxStack = maxStackFor(stack.itemId, itemRegistry);
    std::uint32_t remaining = stack.count;

    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto& slot = slots_[i];
        if (slot.locked || !canInsertAt(i, stack, itemRegistry)) {
            continue;
        }
        if (slot.stack.empty()) {
            remaining -= std::min(remaining, maxStack);
        } else if (slot.stack.itemId == stack.itemId && slot.stack.count < maxStack) {
            const auto space = maxStack - slot.stack.count;
            remaining -= std::min(remaining, space);
        }
        if (remaining == 0) {
            return true;
        }
    }

    return remaining == 0;
}

SlotKind Inventory::slotKind(std::size_t index) const
{
    return slots_[index].kind;
}

void Inventory::setSlotKind(std::size_t index, SlotKind kind)
{
    if (index >= slots_.size()) {
        return;
    }
    slots_[index].kind = kind;
}

void Inventory::setSlotLocked(std::size_t index, bool locked)
{
    if (index >= slots_.size()) {
        return;
    }
    slots_[index].locked = locked;
}

void Inventory::setSlotFavorite(std::size_t index, bool favorite)
{
    if (index >= slots_.size()) {
        return;
    }
    slots_[index].favorite = favorite;
}

bool Inventory::slotLocked(std::size_t index) const
{
    return index < slots_.size() && slots_[index].locked;
}

bool Inventory::slotFavorite(std::size_t index) const
{
    return index < slots_.size() && slots_[index].favorite;
}

void Inventory::notifySlotChanged(std::size_t index)
{
    if (onSlotChanged_) {
        onSlotChanged_(index);
    }
}

PlayerInventory::PlayerInventory()
    : main_(kMainSlots, SlotKind::Generic)
    , hotbar_(kHotbarSlots, SlotKind::Hotbar)
    , equipment_(kEquipmentSlots)
    , armor_(kArmorSlots)
    , accessories_(kAccessorySlots, SlotKind::Accessory)
    , offhand_(kOffhandSlots)
{
    armor_.setSlotKind(0, SlotKind::Helmet);
    armor_.setSlotKind(1, SlotKind::Chest);
    armor_.setSlotKind(2, SlotKind::Legs);
    armor_.setSlotKind(3, SlotKind::Boots);

    equipment_.setSlotKind(0, SlotKind::Helmet);
    equipment_.setSlotKind(1, SlotKind::Chest);
    equipment_.setSlotKind(2, SlotKind::Legs);
    equipment_.setSlotKind(3, SlotKind::Boots);
    equipment_.setSlotKind(4, SlotKind::Gloves);
    equipment_.setSlotKind(5, SlotKind::Back);
    equipment_.setSlotKind(6, SlotKind::MainHand);
    equipment_.setSlotKind(7, SlotKind::OffHand);

    offhand_.setSlotKind(0, SlotKind::OffHand);
}

void PlayerInventory::selectHotbarSlot(std::size_t slot)
{
    selectedHotbarSlot_ = std::min(slot, kHotbarSlots - 1);
}

const ItemStack& PlayerInventory::selectedHotbarItem() const
{
    return hotbar_.slot(selectedHotbarSlot_);
}

ItemStack PlayerInventory::insert(ItemStack stack, const data::ItemRegistry& itemRegistry)
{
    stack = hotbar_.insert(stack, itemRegistry);
    if (!stack.empty()) {
        stack = main_.insert(stack, itemRegistry);
    }
    return stack;
}

Inventory& InventoryManager::createInventory(InventoryKey key, std::size_t slotCount)
{
    auto inventory = std::make_unique<Inventory>(slotCount);
    auto& ref = *inventory;
    inventories_.emplace(key, std::move(inventory));
    return ref;
}

Inventory* InventoryManager::find(InventoryKey key)
{
    const auto it = inventories_.find(key);
    return it != inventories_.end() ? it->second.get() : nullptr;
}

const Inventory* InventoryManager::find(InventoryKey key) const
{
    const auto it = inventories_.find(key);
    return it != inventories_.end() ? it->second.get() : nullptr;
}

void InventoryManager::remove(InventoryKey key)
{
    inventories_.erase(key);
}

std::size_t InventoryManager::inventoryCount() const noexcept
{
    return inventories_.size();
}

} // namespace voxel::inventory
