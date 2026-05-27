#pragma once

#include <voxel/world/BlockEntityBehavior.hpp>

namespace voxel::world {

class KineticSourceBehavior : public IBlockEntityBehavior {
public:
    void tick(BlockEntityTickContext& context) override;
};

class MachineBehavior : public IBlockEntityBehavior {
public:
    explicit MachineBehavior(std::string recipeCategory, std::size_t inventorySlots);

    void tick(BlockEntityTickContext& context) override;
    std::vector<std::uint8_t> serialize() const override;
    bool deserialize(const std::vector<std::uint8_t>& data) override;

    [[nodiscard]] float progress() const noexcept { return progress_; }
    [[nodiscard]] const std::string& recipeCategory() const noexcept { return recipeCategory_; }

private:
    std::string recipeCategory_;
    std::size_t inventorySlots_;
    std::uint32_t currentRecipeTicks_{0};
    std::uint32_t totalRecipeTicks_{0};
    float progress_{0.0F};
    data::Identifier currentRecipeId_;
    inventory::Inventory internalInventory_;
};

class CraftingStationBehavior : public IBlockEntityBehavior {
public:
    void tick(BlockEntityTickContext& context) override;
};

class BeltBehavior : public IBlockEntityBehavior {
public:
    void tick(BlockEntityTickContext& context) override;
    void onNeighborChange(BlockEntityTickContext& context, BlockCoord normal) override;

    [[nodiscard]] bool hasItem() const noexcept { return carriedItem_.count > 0; }
    [[nodiscard]] float itemPosition() const noexcept { return itemPosition_; }

private:
    inventory::ItemStack carriedItem_{};
    float itemPosition_{0.0F};
    float speed_{1.0F};
};

class GearboxBehavior : public IBlockEntityBehavior {
public:
    void tick(BlockEntityTickContext& context) override;
};

class ClutchBehavior : public IBlockEntityBehavior {
public:
    void tick(BlockEntityTickContext& context) override;
    std::vector<std::uint8_t> serialize() const override;
    bool deserialize(const std::vector<std::uint8_t>& data) override;

    [[nodiscard]] bool engaged() const noexcept { return engaged_; }
    void setEngaged(bool engaged) noexcept { engaged_ = engaged; }

private:
    bool engaged_{true};
};

void registerCoreBlockEntityTypes(BlockEntityTypeRegistry& registry);

} // namespace voxel::world
