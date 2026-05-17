#include <voxel/data/BlockRegistry.hpp>

namespace voxel::data {

BlockTypeId BlockRegistry::registerBlock(BlockDefinition definition)
{
    return BlockTypeId{registry_.add(std::move(definition))};
}

const BlockDefinition* BlockRegistry::find(const Identifier& id) const
{
    return registry_.find(id);
}

const Registry<BlockDefinition>& BlockRegistry::registry() const noexcept
{
    return registry_;
}

render::meshing::BlockRenderCatalog BlockRegistry::buildRenderCatalog() const
{
    render::meshing::BlockRenderCatalog catalog;
    for (Registry<BlockDefinition>::RuntimeId id = 1; id <= registry_.size(); ++id) {
        const auto* block = registry_.byRuntimeId(id);
        if (block == nullptr) {
            continue;
        }

        catalog.set(
            BlockTypeId{id},
            {
                block->renderSurface,
                block->opaque && block->renderSurface == render::meshing::MeshSurface::Opaque
            });
    }
    return catalog;
}

automation::KineticBlockCatalog BlockRegistry::buildKineticCatalog() const
{
    automation::KineticBlockCatalog catalog;
    for (Registry<BlockDefinition>::RuntimeId id = 1; id <= registry_.size(); ++id) {
        const auto* block = registry_.byRuntimeId(id);
        if (block == nullptr) {
            continue;
        }
        catalog.set(BlockTypeId{id}, block->kinetic);
    }
    return catalog;
}

world::BlockLightCatalog BlockRegistry::buildLightCatalog() const
{
    world::BlockLightCatalog catalog;
    catalog.set(BlockTypeId{0}, {0U, 0U});
    for (Registry<BlockDefinition>::RuntimeId id = 1; id <= registry_.size(); ++id) {
        const auto* block = registry_.byRuntimeId(id);
        if (block == nullptr) {
            continue;
        }
        std::uint8_t attenuation = block->lightAttenuation;
        if (attenuation == 15U && !block->opaque) {
            attenuation = 0U;
        }
        catalog.set(BlockTypeId{id}, {block->lightEmission, attenuation});
    }
    return catalog;
}

void BlockRegistry::registerCoreBlocks()
{
    registerBlock({Identifier{"core", "air"}, "Air", false, false, render::meshing::MeshSurface::Transparent, {}, {"air"}, {}, false, "", 0, ""});
    registerBlock({Identifier{"core", "stone"}, "Stone", true, true, render::meshing::MeshSurface::Opaque, {}, {"terrain", "solid"}, {}, false, "", 0, ""});
    registerBlock({Identifier{"core", "glass"}, "Glass", true, false, render::meshing::MeshSurface::Transparent, {}, {"glass", "transparent"}, {{}, false, false, true}, false, "", 0, ""});
    registerBlock({Identifier{"core", "copper_pipe"}, "Copper Pipe", true, false, render::meshing::MeshSurface::Cutout, {}, {"pipe", "automation"}, {{}, true, true, false}, true, "pipe", 0, ""});
    registerBlock({Identifier{"core", "creative_motor"}, "Creative Motor", true, true, render::meshing::MeshSurface::Opaque, {true, automation::KineticRole::Source, 32.0F, 64.0F, 0.0F, 1.0F, false, true}, {"kinetic", "source"}, {{}, true, false, false}, true, "kinetic_source", 0, ""});
    registerBlock({Identifier{"core", "wooden_gear"}, "Wooden Gear", true, true, render::meshing::MeshSurface::Opaque, {true, automation::KineticRole::Transfer, 0.0F, 16.0F, 0.0F, 1.0F, true, true}, {"kinetic", "gear"}, {{}, true, false, false}, false, "", 0, ""});
    registerBlock({Identifier{"core", "mechanical_press"}, "Mechanical Press", true, true, render::meshing::MeshSurface::Opaque, {true, automation::KineticRole::Consumer, 0.0F, 0.0F, 24.0F, 1.0F, true, true}, {"kinetic", "consumer"}, {{}, true, true, false}, true, "machine", 2, "press"});
    registerBlock({Identifier{"core", "dirt"}, "Dirt", true, true, render::meshing::MeshSurface::Opaque, {}, {"terrain", "soil"}, {}, false, "", 0, ""});
    registerBlock({Identifier{"core", "grass"}, "Grass Block", true, true, render::meshing::MeshSurface::Opaque, {}, {"terrain", "surface"}, {}, false, "", 0, ""});
    registerBlock({Identifier{"core", "coal_ore"}, "Coal Ore", true, true, render::meshing::MeshSurface::Opaque, {}, {"terrain", "ore", "coal"}, {}, false, "", 0, ""});
    registerBlock({Identifier{"core", "iron_ore"}, "Iron Ore", true, true, render::meshing::MeshSurface::Opaque, {}, {"terrain", "ore", "iron"}, {}, false, "", 0, ""});
    registerBlock({Identifier{"core", "water"}, "Water", false, false, render::meshing::MeshSurface::Transparent, {}, {"fluid", "water"}, {}, false, "", 0, ""});
    registerBlock({Identifier{"core", "oak_log"}, "Oak Log", true, true, render::meshing::MeshSurface::Opaque, {}, {"terrain", "wood"}, {{}, true, false, false}, false, "", 0, ""});
    registerBlock({Identifier{"core", "crafting_table"}, "Crafting Table", true, true, render::meshing::MeshSurface::Opaque, {}, {"crafting", "workstation"}, {}, true, "crafting_station", 9, "hand"});
    registerBlock({Identifier{"core", "torch"}, "Torch", false, false, render::meshing::MeshSurface::Cutout, {}, {"light", "decoration"}, {}, false, "", 0, "", 14, 0});
    registerBlock({Identifier{"core", "belt"}, "Belt", true, true, render::meshing::MeshSurface::Opaque, {}, {"logistics", "belt"}, {{}, true, false, false}, true, "belt", 0, ""});
    registerBlock({Identifier{"core", "gearbox"}, "Gearbox", true, true, render::meshing::MeshSurface::Opaque, {true, automation::KineticRole::Transfer, 0.0F, 32.0F, 0.0F, 2.0F, true, true}, {"kinetic", "gear"}, {{}, true, false, false}, true, "gearbox", 0, ""});
    registerBlock({Identifier{"core", "clutch"}, "Clutch", true, true, render::meshing::MeshSurface::Opaque, {true, automation::KineticRole::Transfer, 0.0F, 48.0F, 0.0F, 1.0F, false, true}, {"kinetic", "transfer"}, {{}, true, true, false}, true, "clutch", 0, ""});
    registerBlock({Identifier{"core", "millstone"}, "Millstone", true, true, render::meshing::MeshSurface::Opaque, {true, automation::KineticRole::Consumer, 0.0F, 0.0F, 16.0F, 1.0F, true, true}, {"kinetic", "consumer"}, {{}, true, false, false}, true, "machine", 2, "mill"});
}

} // namespace voxel::data
