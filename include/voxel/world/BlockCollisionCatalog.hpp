#pragma once

#include <cstddef>
#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/world/BlockState.hpp>

namespace voxel::world {

class BlockCollisionCatalog {
public:
    void set(BlockTypeId type, bool solid)
    {
        if (type.value >= entries_.size()) {
            entries_.resize(static_cast<std::size_t>(type.value) + 1U, true);
        }
        entries_[type.value] = solid;
    }

    [[nodiscard]] bool isSolid(BlockStateId state) const noexcept
    {
        if (state.value == AirBlockState.value) {
            return false;
        }
        const auto type = blockTypeOf(state);
        if (type.value < entries_.size()) {
            return entries_[type.value];
        }
        return true;
    }

private:
    std::vector<bool> entries_{false};
};

} // namespace voxel::world
