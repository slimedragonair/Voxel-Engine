#pragma once

#include <cstdint>

#include <voxel/core/Types.hpp>

namespace voxel::world {

struct BlockStateIdHash {
    [[nodiscard]] std::size_t operator()(BlockStateId id) const noexcept
    {
        return std::hash<std::uint32_t>{}(id.value);
    }
};

constexpr BlockStateId AirBlockState{0};

[[nodiscard]] constexpr BlockTypeId blockTypeOf(BlockStateId state) noexcept
{
    return BlockTypeId{static_cast<std::uint32_t>(state.value >> 16)};
}

[[nodiscard]] constexpr std::uint16_t propertyBitsOf(BlockStateId state) noexcept
{
    return static_cast<std::uint16_t>(state.value & 0xFFFF);
}

[[nodiscard]] constexpr BlockStateId makeBlockState(BlockTypeId type, std::uint16_t properties = 0) noexcept
{
    return BlockStateId{(static_cast<std::uint32_t>(type.value) << 16) | static_cast<std::uint32_t>(properties)};
}

enum class BlockAxis : std::uint8_t {
    X,
    Y,
    Z
};

[[nodiscard]] constexpr std::uint16_t encodeAxisProperty(BlockAxis axis) noexcept
{
    return static_cast<std::uint16_t>(axis);
}

[[nodiscard]] constexpr BlockAxis decodeAxisProperty(std::uint16_t bits) noexcept
{
    return static_cast<BlockAxis>(bits & 0x3);
}

[[nodiscard]] constexpr std::uint16_t setBoolProperty(std::uint16_t bits, std::uint8_t bitIndex, bool value) noexcept
{
    if (value) {
        return bits | static_cast<std::uint16_t>(1U << bitIndex);
    }
    return bits & ~static_cast<std::uint16_t>(1U << bitIndex);
}

[[nodiscard]] constexpr bool getBoolProperty(std::uint16_t bits, std::uint8_t bitIndex) noexcept
{
    return (bits & static_cast<std::uint16_t>(1U << bitIndex)) != 0;
}

constexpr std::uint8_t kAxisPropertyBitOffset = 0;
constexpr std::uint8_t kAxisPropertyBitCount = 2;
constexpr std::uint8_t kPoweredPropertyBitOffset = 2;
constexpr std::uint8_t kWaterloggedPropertyBitOffset = 3;

[[nodiscard]] constexpr BlockAxis axisOf(BlockStateId state) noexcept
{
    return decodeAxisProperty((propertyBitsOf(state) >> kAxisPropertyBitOffset) & 0x3);
}

[[nodiscard]] constexpr bool isPowered(BlockStateId state) noexcept
{
    return getBoolProperty(propertyBitsOf(state), kPoweredPropertyBitOffset);
}

[[nodiscard]] constexpr bool isWaterlogged(BlockStateId state) noexcept
{
    return getBoolProperty(propertyBitsOf(state), kWaterloggedPropertyBitOffset);
}

[[nodiscard]] constexpr BlockStateId withAxis(BlockStateId state, BlockAxis axis) noexcept
{
    auto bits = propertyBitsOf(state);
    bits &= ~(0x3 << kAxisPropertyBitOffset);
    bits |= static_cast<std::uint16_t>(encodeAxisProperty(axis) << kAxisPropertyBitOffset);
    return makeBlockState(blockTypeOf(state), bits);
}

[[nodiscard]] constexpr BlockStateId withPowered(BlockStateId state, bool powered) noexcept
{
    return makeBlockState(blockTypeOf(state), setBoolProperty(propertyBitsOf(state), kPoweredPropertyBitOffset, powered));
}

[[nodiscard]] constexpr BlockStateId withWaterlogged(BlockStateId state, bool waterlogged) noexcept
{
    return makeBlockState(blockTypeOf(state), setBoolProperty(propertyBitsOf(state), kWaterloggedPropertyBitOffset, waterlogged));
}

} // namespace voxel::world
