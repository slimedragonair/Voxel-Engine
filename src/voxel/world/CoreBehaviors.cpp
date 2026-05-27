#include <voxel/world/CoreBehaviors.hpp>

#include <voxel/core/Logger.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/CoordinateUtils.hpp>

namespace voxel::world {

void KineticSourceBehavior::tick(BlockEntityTickContext& context)
{
    (void)context;
}

MachineBehavior::MachineBehavior(std::string recipeCategory, std::size_t inventorySlots)
    : recipeCategory_(std::move(recipeCategory))
    , inventorySlots_(inventorySlots)
    , internalInventory_(inventorySlots)
{
}

void MachineBehavior::tick(BlockEntityTickContext& context)
{
    if (currentRecipeId_.path.empty()) {
        const auto recipes = context.recipeRegistry.recipesForMachineCategory(recipeCategory_);
        for (const auto* recipe : recipes) {
            bool canStart = true;
            for (const auto& input : recipe->inputs) {
                const auto inputTypeId = context.itemRegistry.findRuntimeId(input.itemId);
                if (inputTypeId.value == 0 || internalInventory_.itemCount(inputTypeId) < input.count) {
                    canStart = false;
                    break;
                }
            }
            if (canStart) {
                for (const auto& input : recipe->inputs) {
                    const auto inputTypeId = context.itemRegistry.findRuntimeId(input.itemId);
                    if (inputTypeId.value != 0) {
                        internalInventory_.extractItem(inputTypeId, input.count);
                    }
                }
                currentRecipeId_ = recipe->id;
                totalRecipeTicks_ = recipe->durationTicks;
                currentRecipeTicks_ = 0;
                progress_ = 0.0F;
                break;
            }
        }
    }

    if (!currentRecipeId_.path.empty()) {
        ++currentRecipeTicks_;
        progress_ = totalRecipeTicks_ > 0
            ? static_cast<float>(currentRecipeTicks_) / static_cast<float>(totalRecipeTicks_)
            : 1.0F;

        if (currentRecipeTicks_ >= totalRecipeTicks_) {
            const auto* recipe = context.recipeRegistry.find(currentRecipeId_);
            if (recipe != nullptr) {
                for (const auto& output : recipe->outputs) {
                    const auto outputTypeId = context.itemRegistry.findRuntimeId(output.itemId);
                    if (outputTypeId.value != 0) {
                        inventory::ItemStack stack{outputTypeId, output.count, 0};
                        auto remainder = internalInventory_.insert(stack, context.itemRegistry);
                        if (!remainder.empty()) {
                            Logger::warn("Machine output inventory full, items lost");
                        }
                    }
                }
            }
            currentRecipeId_ = {};
            currentRecipeTicks_ = 0;
            totalRecipeTicks_ = 0;
            progress_ = 0.0F;
        }
    }
}

std::vector<std::uint8_t> MachineBehavior::serialize() const
{
    std::vector<std::uint8_t> data;
    data.reserve(sizeof(std::uint32_t) * 3);
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        data.push_back(static_cast<std::uint8_t>((currentRecipeTicks_ >> (i * 8)) & 0xFF));
    }
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        data.push_back(static_cast<std::uint8_t>((totalRecipeTicks_ >> (i * 8)) & 0xFF));
    }
    const auto progressPacked = static_cast<std::uint32_t>(progress_ * 10000.0F);
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        data.push_back(static_cast<std::uint8_t>((progressPacked >> (i * 8)) & 0xFF));
    }
    return data;
}

bool MachineBehavior::deserialize(const std::vector<std::uint8_t>& data)
{
    if (data.size() < sizeof(std::uint32_t) * 3) {
        return false;
    }
    currentRecipeTicks_ = 0;
    totalRecipeTicks_ = 0;
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        currentRecipeTicks_ |= static_cast<std::uint32_t>(data[i]) << (i * 8);
    }
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        totalRecipeTicks_ |= static_cast<std::uint32_t>(data[sizeof(std::uint32_t) + i]) << (i * 8);
    }
    std::uint32_t progressPacked = 0;
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        progressPacked |= static_cast<std::uint32_t>(data[sizeof(std::uint32_t) * 2 + i]) << (i * 8);
    }
    progress_ = static_cast<float>(progressPacked) / 10000.0F;
    return true;
}

void CraftingStationBehavior::tick(BlockEntityTickContext& context)
{
    (void)context;
}

namespace {

std::uint64_t inventoryKeyFor(world::PlanetCoord pos)
{
    const auto cx = static_cast<std::uint64_t>(pos.chunk.x);
    const auto cy = static_cast<std::uint64_t>(pos.chunk.y);
    const auto cz = static_cast<std::uint64_t>(pos.chunk.z);
    const auto bx = static_cast<std::uint64_t>(pos.block.x);
    const auto by = static_cast<std::uint64_t>(pos.block.y);
    const auto bz = static_cast<std::uint64_t>(pos.block.z);
    return (cx & 0xFFFFULL) << 48 | (cy & 0xFFFFULL) << 32 | (cz & 0xFFFFULL) << 16
         | (bx & 0x3ULL) << 14 | (by & 0x3ULL) << 12 | (bz & 0x3ULL) << 10;
}

world::PlanetCoord neighborPosition(world::PlanetCoord pos, world::BlockCoord normal)
{
    pos.block.x += normal.x;
    pos.block.y += normal.y;
    pos.block.z += normal.z;
    return pos;
}

}

void BeltBehavior::tick(BlockEntityTickContext& context)
{
    if (hasItem()) {
        itemPosition_ += speed_ * context.dt;
        if (itemPosition_ >= 1.0F) {
            const auto blockState = context.chunks.find(context.position.chunk)
                ? context.chunks.find(context.position.chunk)->blockAt(
                    context.position.block.x, context.position.block.y, context.position.block.z)
                : BlockStateId{0};
            const auto axis = world::axisOf(blockState);
            world::BlockCoord forward{};
            if (axis == world::BlockAxis::X) forward = {1, 0, 0};
            else if (axis == world::BlockAxis::Z) forward = {0, 0, 1};
            else forward = {0, 1, 0};

            const auto target = neighborPosition(context.position, forward);
            const auto targetKey = inventoryKeyFor(target);
            auto* targetInv = context.inventoryManager.find(targetKey);
            if (targetInv != nullptr) {
                auto remainder = targetInv->insert(carriedItem_, context.itemRegistry);
                if (remainder.empty()) {
                    carriedItem_.clear();
                    itemPosition_ = 0.0F;
                }
            }
        }
    } else {
        const auto blockState = context.chunks.find(context.position.chunk)
            ? context.chunks.find(context.position.chunk)->blockAt(
                context.position.block.x, context.position.block.y, context.position.block.z)
            : BlockStateId{0};
        const auto axis = world::axisOf(blockState);
        world::BlockCoord backward{};
        if (axis == world::BlockAxis::X) backward = {-1, 0, 0};
        else if (axis == world::BlockAxis::Z) backward = {0, 0, -1};
        else backward = {0, -1, 0};

        const auto source = neighborPosition(context.position, backward);
        const auto sourceKey = inventoryKeyFor(source);
        auto* sourceInv = context.inventoryManager.find(sourceKey);
        if (sourceInv != nullptr) {
            for (std::size_t i = 0; i < sourceInv->slotCount(); ++i) {
                const auto& slot = sourceInv->slot(i);
                if (!slot.empty()) {
                    auto extracted = sourceInv->extract(i, 1);
                    if (extracted.has_value()) {
                        carriedItem_ = *extracted;
                        itemPosition_ = 0.0F;
                        break;
                    }
                }
            }
        }
    }
}

void BeltBehavior::onNeighborChange(BlockEntityTickContext& context, BlockCoord normal)
{
    (void)context;
    (void)normal;
}

void GearboxBehavior::tick(BlockEntityTickContext& context)
{
    (void)context;
}

void ClutchBehavior::tick(BlockEntityTickContext& context)
{
    const bool wasEngaged = engaged_;
    if (!engaged_) {
        context.kineticSolver.setBlockDisabled(context.position, true);
    } else {
        context.kineticSolver.setBlockDisabled(context.position, false);
    }
    if (wasEngaged != engaged_) {
        context.kineticSolver.markDirty(context.position);
    }
}

std::vector<std::uint8_t> ClutchBehavior::serialize() const
{
    return {engaged_ ? std::uint8_t{1} : std::uint8_t{0}};
}

bool ClutchBehavior::deserialize(const std::vector<std::uint8_t>& data)
{
    if (data.empty()) {
        return false;
    }
    engaged_ = data[0] != 0;
    return true;
}

void registerCoreBlockEntityTypes(BlockEntityTypeRegistry& registry)
{
    registry.registerType("kinetic_source", []() -> std::unique_ptr<IBlockEntityBehavior> {
        return std::make_unique<KineticSourceBehavior>();
    });

    registry.registerType("machine", []() -> std::unique_ptr<IBlockEntityBehavior> {
        return std::make_unique<MachineBehavior>("", 2);
    });

    registry.registerType("crafting_station", []() -> std::unique_ptr<IBlockEntityBehavior> {
        return std::make_unique<CraftingStationBehavior>();
    });

    registry.registerType("pipe", []() -> std::unique_ptr<IBlockEntityBehavior> {
        return std::make_unique<KineticSourceBehavior>();
    });

    registry.registerType("belt", []() -> std::unique_ptr<IBlockEntityBehavior> {
        return std::make_unique<BeltBehavior>();
    });

    registry.registerType("gearbox", []() -> std::unique_ptr<IBlockEntityBehavior> {
        return std::make_unique<GearboxBehavior>();
    });

    registry.registerType("clutch", []() -> std::unique_ptr<IBlockEntityBehavior> {
        return std::make_unique<ClutchBehavior>();
    });
}

} // namespace voxel::world
