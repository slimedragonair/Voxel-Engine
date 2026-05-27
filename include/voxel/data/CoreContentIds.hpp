#pragma once

#include <voxel/core/Types.hpp>
#include <voxel/data/BlockRegistry.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/NoiseTerrainGenerator.hpp>

namespace voxel::data {

// Resolved runtime IDs for the engine-owned core content. Defaults mirror the
// append-only built-in/JSON order for save compatibility, then initialize()
// replaces them with registry lookups by stable string identifier.
struct CoreBlockIds {
    BlockTypeId airType{0};
    BlockTypeId stoneType{2};
    BlockTypeId glassType{3};
    BlockTypeId creativeMotorType{5};
    BlockTypeId woodenGearType{6};
    BlockTypeId mechanicalPressType{7};
    BlockTypeId dirtType{8};
    BlockTypeId grassType{9};
    BlockTypeId coalOreType{10};
    BlockTypeId ironOreType{11};
    BlockTypeId waterType{12};
    BlockTypeId oakLogType{13};
    BlockTypeId beltType{16};
    BlockTypeId gearboxType{17};
    BlockTypeId clutchType{18};
    BlockTypeId millstoneType{19};
    BlockTypeId sandType{20};
    BlockTypeId sandstoneType{21};
    BlockTypeId snowType{22};
    BlockTypeId redSandType{23};
    BlockTypeId terracottaType{24};
    BlockTypeId gravelType{25};
    BlockTypeId basaltType{26};
    BlockTypeId podzolType{27};
    BlockTypeId mossyStoneType{28};
    BlockTypeId iceType{29};
    BlockTypeId leavesType{30};
    BlockTypeId spaceStoneType{31};
    BlockTypeId spaceMetalOreType{32};
    BlockTypeId spaceCrystalType{33};
    BlockTypeId spaceIceType{34};

    BlockStateId air{world::AirBlockState};
    BlockStateId stone{world::makeBlockState(stoneType)};
    BlockStateId glass{world::makeBlockState(glassType)};
    BlockStateId creativeMotor{world::makeBlockState(creativeMotorType)};
    BlockStateId woodenGear{world::makeBlockState(woodenGearType)};
    BlockStateId mechanicalPress{world::makeBlockState(mechanicalPressType)};
    BlockStateId dirt{world::makeBlockState(dirtType)};
    BlockStateId grass{world::makeBlockState(grassType)};
    BlockStateId coalOre{world::makeBlockState(coalOreType)};
    BlockStateId ironOre{world::makeBlockState(ironOreType)};
    BlockStateId water{world::makeBlockState(waterType)};
    BlockStateId oakLog{world::makeBlockState(oakLogType)};
    BlockStateId belt{world::makeBlockState(beltType)};
    BlockStateId gearbox{world::makeBlockState(gearboxType)};
    BlockStateId clutch{world::makeBlockState(clutchType)};
    BlockStateId millstone{world::makeBlockState(millstoneType)};
    BlockStateId sand{world::makeBlockState(sandType)};
    BlockStateId sandstone{world::makeBlockState(sandstoneType)};
    BlockStateId snow{world::makeBlockState(snowType)};
    BlockStateId redSand{world::makeBlockState(redSandType)};
    BlockStateId terracotta{world::makeBlockState(terracottaType)};
    BlockStateId gravel{world::makeBlockState(gravelType)};
    BlockStateId basalt{world::makeBlockState(basaltType)};
    BlockStateId podzol{world::makeBlockState(podzolType)};
    BlockStateId mossyStone{world::makeBlockState(mossyStoneType)};
    BlockStateId ice{world::makeBlockState(iceType)};
    BlockStateId leaves{world::makeBlockState(leavesType)};
    BlockStateId spaceStone{world::makeBlockState(spaceStoneType)};
    BlockStateId spaceMetalOre{world::makeBlockState(spaceMetalOreType)};
    BlockStateId spaceCrystal{world::makeBlockState(spaceCrystalType)};
    BlockStateId spaceIce{world::makeBlockState(spaceIceType)};

    bool allRequiredResolved{true};

    [[nodiscard]] bool isWater(BlockStateId state) const noexcept
    {
        return waterType.value != 0U && world::blockTypeOf(state).value == waterType.value;
    }
};

[[nodiscard]] CoreBlockIds resolveCoreBlockIds(const BlockRegistry& registry);
void applyCoreBlockIds(world::NoiseTerrainSettings& settings, const CoreBlockIds& ids) noexcept;

} // namespace voxel::data
