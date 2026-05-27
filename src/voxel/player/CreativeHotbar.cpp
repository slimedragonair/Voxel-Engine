#include <voxel/player/CreativeHotbar.hpp>

using namespace voxel::world;

namespace voxel::player {

CreativeHotbar::CreativeHotbar()
    : slots_{{
        {"stone", makeBlockState(BlockTypeId{2})},
        {"dirt", makeBlockState(BlockTypeId{8})},
        {"grass", makeBlockState(BlockTypeId{9})},
        {"glass", makeBlockState(BlockTypeId{3})},
        {"water", makeBlockState(BlockTypeId{12})},
        {"creative motor", makeBlockState(BlockTypeId{5})},
        {"wooden gear", makeBlockState(BlockTypeId{6})},
        {"belt", makeBlockState(BlockTypeId{16})},
        {"gearbox", makeBlockState(BlockTypeId{17})},
        {"mechanical press", makeBlockState(BlockTypeId{7})},
        {"clutch", makeBlockState(BlockTypeId{18})},
        {"millstone", makeBlockState(BlockTypeId{19})},
    }}
{
}

bool CreativeHotbar::select(std::size_t slot) noexcept
{
    if (!validSlot(slot)) {
        return false;
    }
    selected_ = slot;
    return true;
}

std::size_t CreativeHotbar::selectedSlot() const noexcept
{
    return selected_;
}

HotbarSlot CreativeHotbar::selected() const noexcept
{
    return slots_[selected_];
}

HotbarSlot CreativeHotbar::slot(std::size_t index) const noexcept
{
    return validSlot(index) ? slots_[index] : HotbarSlot{};
}

bool CreativeHotbar::validSlot(std::size_t index) const noexcept
{
    return index < slots_.size() && slots_[index].block.value != 0;
}

} // namespace voxel::player
