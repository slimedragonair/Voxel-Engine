#include <voxel/player/CreativeHotbar.hpp>

using namespace voxel::world;

namespace voxel::player {

CreativeHotbar::CreativeHotbar()
    : CreativeHotbar(data::CoreBlockIds{})
{
}

CreativeHotbar::CreativeHotbar(const data::CoreBlockIds& coreBlocks)
{
    reset(coreBlocks);
}

void CreativeHotbar::reset(const data::CoreBlockIds& coreBlocks)
{
    slots_ = {{
        {"stone", coreBlocks.stone},
        {"dirt", coreBlocks.dirt},
        {"grass", coreBlocks.grass},
        {"glass", coreBlocks.glass},
        {"water", coreBlocks.water},
        {"creative motor", coreBlocks.creativeMotor},
        {"wooden gear", coreBlocks.woodenGear},
        {"belt", coreBlocks.belt},
        {"gearbox", coreBlocks.gearbox},
        {"mechanical press", coreBlocks.mechanicalPress},
        {"clutch", coreBlocks.clutch},
        {"millstone", coreBlocks.millstone},
    }};
    if (!validSlot(selected_)) {
        selected_ = 0;
    }
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
