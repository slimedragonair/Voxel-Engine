#pragma once

#include <cstdint>
#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/world/BlockState.hpp>

namespace voxel::world {

struct BlockLightInfo {
    std::uint8_t emission{0};
    std::uint8_t attenuation{15};
};

class BlockLightCatalog {
public:
    void set(BlockTypeId type, BlockLightInfo info);
    [[nodiscard]] BlockLightInfo get(BlockStateId state) const noexcept;
    [[nodiscard]] BlockLightInfo getByType(BlockTypeId type) const noexcept;

private:
    std::vector<BlockLightInfo> entries_;
};

} // namespace voxel::world
