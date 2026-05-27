#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/automation/KineticNetwork.hpp>
#include <voxel/data/Identifier.hpp>
#include <voxel/data/Registry.hpp>
#include <voxel/render/meshing/BlockRenderCatalog.hpp>
#include <voxel/world/BlockCollisionCatalog.hpp>
#include <voxel/world/BlockLightCatalog.hpp>

namespace voxel::data {

struct BlockPropertyDef {
    std::string name;
    std::vector<std::string> values;
};

struct BlockStateSchema {
    std::vector<BlockPropertyDef> properties;
    bool hasAxis{false};
    bool hasPowered{false};
    bool hasWaterlogged{false};
};

struct BlockDefinition {
    Identifier id;
    std::string displayName;
    bool solid{true};
    bool opaque{true};
    render::meshing::MeshSurface renderSurface{render::meshing::MeshSurface::Opaque};
    automation::KineticBlockDefinition kinetic{};
    std::vector<std::string> tags;
    BlockStateSchema stateSchema;
    bool hasBlockEntity{false};
    std::string blockEntityType;
    std::size_t inventorySlots{0};
    std::string recipeCategory;

    std::uint8_t lightEmission{0};
    std::uint8_t lightAttenuation{15};
};

class BlockRegistry {
public:
    BlockTypeId registerBlock(BlockDefinition definition);
    [[nodiscard]] const BlockDefinition* find(const Identifier& id) const;
    [[nodiscard]] const Registry<BlockDefinition>& registry() const noexcept;
    [[nodiscard]] render::meshing::BlockRenderCatalog buildRenderCatalog() const;
    [[nodiscard]] world::BlockCollisionCatalog buildCollisionCatalog() const;
    [[nodiscard]] automation::KineticBlockCatalog buildKineticCatalog() const;
    [[nodiscard]] world::BlockLightCatalog buildLightCatalog() const;

    void registerCoreBlocks();

private:
    Registry<BlockDefinition> registry_;
};

} // namespace voxel::data
