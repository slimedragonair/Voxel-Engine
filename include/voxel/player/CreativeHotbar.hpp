#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include <voxel/data/CoreContentIds.hpp>
#include <voxel/world/BlockState.hpp>

namespace voxel::player {

struct HotbarSlot {
    std::string_view name{};
    BlockStateId block{};
};

class CreativeHotbar {
public:
    static constexpr std::size_t SlotCount = 12;

    CreativeHotbar();
    explicit CreativeHotbar(const data::CoreBlockIds& coreBlocks);
    void reset(const data::CoreBlockIds& coreBlocks);

    [[nodiscard]] bool select(std::size_t slot) noexcept;
    [[nodiscard]] std::size_t selectedSlot() const noexcept;
    [[nodiscard]] HotbarSlot selected() const noexcept;
    [[nodiscard]] HotbarSlot slot(std::size_t index) const noexcept;
    [[nodiscard]] bool validSlot(std::size_t index) const noexcept;

private:
    std::array<HotbarSlot, SlotCount> slots_{};
    std::size_t selected_{0};
};

} // namespace voxel::player
