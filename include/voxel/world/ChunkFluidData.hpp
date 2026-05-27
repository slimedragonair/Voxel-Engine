#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <voxel/world/ChunkConstants.hpp>

namespace voxel::world {

// W2: per-voxel fluid state, parallel to ChunkLightData. One byte per voxel:
//
//   bits 0..3: level (0 = source / full, 1..7 = decaying flow, 8..15 reserved)
//   bit  4   : falling  (water in mid-air, with empty space above the source)
//   bit  5   : oceanLocked (static terrain water — not simulated until disturbed)
//   bit  6   : reserved (planned: salt vs fresh, hot)
//   bit  7   : reserved (planned: dirty flag for sim coalescing)
//
// 32^3 bytes = 32 KB per chunk. Allocated on demand the first time a chunk
// gets any non-air fluid block — non-fluid chunks pay zero overhead.
struct ChunkFluidData {
    static constexpr std::uint8_t kMaxLevel = 7;
    static constexpr std::uint8_t kLevelMask = 0x0F;
    static constexpr std::uint8_t kFallingBit = 0x10;
    static constexpr std::uint8_t kOceanLockedBit = 0x20;

    std::array<std::uint8_t, static_cast<std::size_t>(ChunkVolume)> packed{};

    [[nodiscard]] std::uint8_t levelAt(int x, int y, int z) const noexcept;
    [[nodiscard]] bool fallingAt(int x, int y, int z) const noexcept;
    [[nodiscard]] bool oceanLockedAt(int x, int y, int z) const noexcept;

    // Single-byte mutator. Replaces all flags + level at the cell.
    void setCell(int x, int y, int z, std::uint8_t value) noexcept;
    void setLevel(int x, int y, int z, std::uint8_t level) noexcept;
    void setFalling(int x, int y, int z, bool falling) noexcept;
    void setOceanLocked(int x, int y, int z, bool locked) noexcept;

    void clear() noexcept;
};

// Compose / inspect a raw cell byte without going through ChunkFluidData.
// Useful for the simulation step which works directly on byte arrays.
[[nodiscard]] constexpr std::uint8_t makeFluidCell(
    std::uint8_t level, bool falling, bool oceanLocked) noexcept
{
    std::uint8_t value = level & ChunkFluidData::kLevelMask;
    if (falling)     value |= ChunkFluidData::kFallingBit;
    if (oceanLocked) value |= ChunkFluidData::kOceanLockedBit;
    return value;
}

[[nodiscard]] constexpr std::uint8_t fluidLevel(std::uint8_t cell) noexcept
{
    return cell & ChunkFluidData::kLevelMask;
}

[[nodiscard]] constexpr bool fluidFalling(std::uint8_t cell) noexcept
{
    return (cell & ChunkFluidData::kFallingBit) != 0;
}

[[nodiscard]] constexpr bool fluidOceanLocked(std::uint8_t cell) noexcept
{
    return (cell & ChunkFluidData::kOceanLockedBit) != 0;
}

} // namespace voxel::world
