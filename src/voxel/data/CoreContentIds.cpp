#include <voxel/data/CoreContentIds.hpp>

#include <voxel/data/Identifier.hpp>

namespace voxel::data {

namespace {

[[nodiscard]] BlockTypeId resolveType(const BlockRegistry& registry,
                                      const Identifier& id,
                                      BlockTypeId fallback,
                                      bool& allRequiredResolved)
{
    const auto runtimeId = registry.registry().runtimeId(id);
    if (runtimeId == 0U) {
        allRequiredResolved = false;
        return fallback;
    }
    return BlockTypeId{runtimeId};
}

void refreshStates(CoreBlockIds& ids) noexcept
{
    ids.air = world::AirBlockState;
    ids.stone = world::makeBlockState(ids.stoneType);
    ids.glass = world::makeBlockState(ids.glassType);
    ids.creativeMotor = world::makeBlockState(ids.creativeMotorType);
    ids.woodenGear = world::makeBlockState(ids.woodenGearType);
    ids.mechanicalPress = world::makeBlockState(ids.mechanicalPressType);
    ids.dirt = world::makeBlockState(ids.dirtType);
    ids.grass = world::makeBlockState(ids.grassType);
    ids.coalOre = world::makeBlockState(ids.coalOreType);
    ids.ironOre = world::makeBlockState(ids.ironOreType);
    ids.water = world::makeBlockState(ids.waterType);
    ids.oakLog = world::makeBlockState(ids.oakLogType);
    ids.belt = world::makeBlockState(ids.beltType);
    ids.gearbox = world::makeBlockState(ids.gearboxType);
    ids.clutch = world::makeBlockState(ids.clutchType);
    ids.millstone = world::makeBlockState(ids.millstoneType);
    ids.sand = world::makeBlockState(ids.sandType);
    ids.sandstone = world::makeBlockState(ids.sandstoneType);
    ids.snow = world::makeBlockState(ids.snowType);
    ids.redSand = world::makeBlockState(ids.redSandType);
    ids.terracotta = world::makeBlockState(ids.terracottaType);
    ids.gravel = world::makeBlockState(ids.gravelType);
    ids.basalt = world::makeBlockState(ids.basaltType);
    ids.podzol = world::makeBlockState(ids.podzolType);
    ids.mossyStone = world::makeBlockState(ids.mossyStoneType);
    ids.ice = world::makeBlockState(ids.iceType);
    ids.leaves = world::makeBlockState(ids.leavesType);
    ids.spaceStone = world::makeBlockState(ids.spaceStoneType);
    ids.spaceMetalOre = world::makeBlockState(ids.spaceMetalOreType);
    ids.spaceCrystal = world::makeBlockState(ids.spaceCrystalType);
    ids.spaceIce = world::makeBlockState(ids.spaceIceType);
}

} // namespace

CoreBlockIds resolveCoreBlockIds(const BlockRegistry& registry)
{
    CoreBlockIds ids{};
    bool ok = true;
    ids.airType = resolveType(registry, {"core", "air"}, ids.airType, ok);
    ids.stoneType = resolveType(registry, {"core", "stone"}, ids.stoneType, ok);
    ids.glassType = resolveType(registry, {"core", "glass"}, ids.glassType, ok);
    ids.creativeMotorType = resolveType(registry, {"core", "creative_motor"}, ids.creativeMotorType, ok);
    ids.woodenGearType = resolveType(registry, {"core", "wooden_gear"}, ids.woodenGearType, ok);
    ids.mechanicalPressType = resolveType(registry, {"core", "mechanical_press"}, ids.mechanicalPressType, ok);
    ids.dirtType = resolveType(registry, {"core", "dirt"}, ids.dirtType, ok);
    ids.grassType = resolveType(registry, {"core", "grass"}, ids.grassType, ok);
    ids.coalOreType = resolveType(registry, {"core", "coal_ore"}, ids.coalOreType, ok);
    ids.ironOreType = resolveType(registry, {"core", "iron_ore"}, ids.ironOreType, ok);
    ids.waterType = resolveType(registry, {"core", "water"}, ids.waterType, ok);
    ids.oakLogType = resolveType(registry, {"core", "oak_log"}, ids.oakLogType, ok);
    ids.beltType = resolveType(registry, {"core", "belt"}, ids.beltType, ok);
    ids.gearboxType = resolveType(registry, {"core", "gearbox"}, ids.gearboxType, ok);
    ids.clutchType = resolveType(registry, {"core", "clutch"}, ids.clutchType, ok);
    ids.millstoneType = resolveType(registry, {"core", "millstone"}, ids.millstoneType, ok);
    ids.sandType = resolveType(registry, {"core", "sand"}, ids.sandType, ok);
    ids.sandstoneType = resolveType(registry, {"core", "sandstone"}, ids.sandstoneType, ok);
    ids.snowType = resolveType(registry, {"core", "snow"}, ids.snowType, ok);
    ids.redSandType = resolveType(registry, {"core", "red_sand"}, ids.redSandType, ok);
    ids.terracottaType = resolveType(registry, {"core", "terracotta"}, ids.terracottaType, ok);
    ids.gravelType = resolveType(registry, {"core", "gravel"}, ids.gravelType, ok);
    ids.basaltType = resolveType(registry, {"core", "basalt"}, ids.basaltType, ok);
    ids.podzolType = resolveType(registry, {"core", "podzol"}, ids.podzolType, ok);
    ids.mossyStoneType = resolveType(registry, {"core", "mossy_stone"}, ids.mossyStoneType, ok);
    ids.iceType = resolveType(registry, {"core", "ice"}, ids.iceType, ok);
    ids.leavesType = resolveType(registry, {"core", "leaves"}, ids.leavesType, ok);
    ids.spaceStoneType = resolveType(registry, {"core", "space_rock"}, ids.spaceStoneType, ok);
    ids.spaceMetalOreType = resolveType(registry, {"core", "rich_metal_ore"}, ids.spaceMetalOreType, ok);
    ids.spaceCrystalType = resolveType(registry, {"core", "aether_crystal_ore"}, ids.spaceCrystalType, ok);
    ids.spaceIceType = resolveType(registry, {"core", "compressed_ice"}, ids.spaceIceType, ok);
    ids.allRequiredResolved = ok;
    refreshStates(ids);
    return ids;
}

void applyCoreBlockIds(world::NoiseTerrainSettings& settings, const CoreBlockIds& ids) noexcept
{
    settings.stoneBlock = ids.stone;
    settings.dirtBlock = ids.dirt;
    settings.grassBlock = ids.grass;
    settings.coalOreBlock = ids.coalOre;
    settings.ironOreBlock = ids.ironOre;
    settings.waterBlock = ids.water;
    settings.sandBlock = ids.sand;
    settings.sandstoneBlock = ids.sandstone;
    settings.snowBlock = ids.snow;
    settings.redSandBlock = ids.redSand;
    settings.terracottaBlock = ids.terracotta;
    settings.gravelBlock = ids.gravel;
    settings.basaltBlock = ids.basalt;
    settings.podzolBlock = ids.podzol;
    settings.mossyStoneBlock = ids.mossyStone;
    settings.iceBlock = ids.ice;
    settings.oakLogBlock = ids.oakLog;
    settings.leavesBlock = ids.leaves;
    settings.spaceStoneBlock = ids.spaceStone;
    settings.spaceMetalOreBlock = ids.spaceMetalOre;
    settings.spaceCrystalBlock = ids.spaceCrystal;
    settings.spaceIceBlock = ids.spaceIce;
}

} // namespace voxel::data
