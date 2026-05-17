#include <voxel/world/ChunkLightData.hpp>

namespace voxel::world {

namespace {

std::size_t lightIndex(int x, int y, int z) noexcept
{
    return static_cast<std::size_t>(x + (y * ChunkSize) + (z * ChunkSize * ChunkSize));
}

} // namespace

std::uint8_t ChunkLightData::skyLight(int x, int y, int z) const noexcept
{
    return packed[lightIndex(x, y, z)] & 0x0FU;
}

std::uint8_t ChunkLightData::blockLight(int x, int y, int z) const noexcept
{
    return (packed[lightIndex(x, y, z)] >> 4U) & 0x0FU;
}

void ChunkLightData::setSkyLight(int x, int y, int z, std::uint8_t value) noexcept
{
    auto& cell = packed[lightIndex(x, y, z)];
    cell = static_cast<std::uint8_t>((cell & 0xF0U) | (value & 0x0FU));
}

void ChunkLightData::setBlockLight(int x, int y, int z, std::uint8_t value) noexcept
{
    auto& cell = packed[lightIndex(x, y, z)];
    cell = static_cast<std::uint8_t>((cell & 0x0FU) | ((value & 0x0FU) << 4U));
}

void ChunkLightData::clear() noexcept
{
    packed.fill(0U);
}

} // namespace voxel::world
