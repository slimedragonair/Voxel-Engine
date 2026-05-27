#include <voxel/world/ChunkFluidData.hpp>

namespace voxel::world {

namespace {

[[nodiscard]] std::size_t cellIndex(int x, int y, int z) noexcept
{
    return static_cast<std::size_t>(x)
         + static_cast<std::size_t>(y) * ChunkSize
         + static_cast<std::size_t>(z) * ChunkSize * ChunkSize;
}

} // namespace

std::uint8_t ChunkFluidData::levelAt(int x, int y, int z) const noexcept
{
    return fluidLevel(packed[cellIndex(x, y, z)]);
}

bool ChunkFluidData::fallingAt(int x, int y, int z) const noexcept
{
    return fluidFalling(packed[cellIndex(x, y, z)]);
}

bool ChunkFluidData::oceanLockedAt(int x, int y, int z) const noexcept
{
    return fluidOceanLocked(packed[cellIndex(x, y, z)]);
}

void ChunkFluidData::setCell(int x, int y, int z, std::uint8_t value) noexcept
{
    packed[cellIndex(x, y, z)] = value;
}

void ChunkFluidData::setLevel(int x, int y, int z, std::uint8_t level) noexcept
{
    auto& cell = packed[cellIndex(x, y, z)];
    cell = static_cast<std::uint8_t>((cell & ~kLevelMask) | (level & kLevelMask));
}

void ChunkFluidData::setFalling(int x, int y, int z, bool falling) noexcept
{
    auto& cell = packed[cellIndex(x, y, z)];
    if (falling) {
        cell = static_cast<std::uint8_t>(cell | kFallingBit);
    } else {
        cell = static_cast<std::uint8_t>(cell & ~kFallingBit);
    }
}

void ChunkFluidData::setOceanLocked(int x, int y, int z, bool locked) noexcept
{
    auto& cell = packed[cellIndex(x, y, z)];
    if (locked) {
        cell = static_cast<std::uint8_t>(cell | kOceanLockedBit);
    } else {
        cell = static_cast<std::uint8_t>(cell & ~kOceanLockedBit);
    }
}

void ChunkFluidData::clear() noexcept
{
    packed.fill(0);
}

} // namespace voxel::world
