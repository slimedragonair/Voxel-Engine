#pragma once

#include <cstdint>

namespace voxel {

using Tick = std::uint64_t;
using Revision = std::uint64_t;

template <typename Tag>
struct Id {
    std::uint32_t value{};

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] friend bool operator==(Id lhs, Id rhs) noexcept { return lhs.value == rhs.value; }
};

struct BlockTypeTag;
struct BlockStateTag;
struct EntityTag;
struct BlockEntityTag;
struct NetworkTag;
struct SpellGraphTag;
struct SpellTag;
struct ItemTypeTag;
struct RecipeTag;

using BlockTypeId = Id<BlockTypeTag>;
using BlockStateId = Id<BlockStateTag>;
using EntityId = Id<EntityTag>;
using BlockEntityId = Id<BlockEntityTag>;
using NetworkId = Id<NetworkTag>;
using SpellGraphId = Id<SpellGraphTag>;
using SpellId = Id<SpellTag>;
using ItemTypeId = Id<ItemTypeTag>;
using RecipeId = Id<RecipeTag>;

} // namespace voxel

