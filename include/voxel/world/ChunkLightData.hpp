#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <voxel/world/ChunkConstants.hpp>

namespace voxel::world {

// Per-block packed lighting:
//   low nibble  = skyLight  (0..15)
//   high nibble = blockLight (0..15)
//
// One byte per voxel = 32 KB for a 32^3 chunk. Allocated only when the chunk
// has had its light propagated at least once; until then, the mesher should
// treat all blocks as fully lit so the placeholder render isn't pitch-black.
struct ChunkLightData {
    std::array<std::uint8_t, static_cast<std::size_t>(ChunkVolume)> packed{};

    [[nodiscard]] inline std::uint8_t skyLight(int x, int y, int z) const noexcept
    {
        return packed[static_cast<std::size_t>(x + (y * ChunkSize) + (z * ChunkSize * ChunkSize))] & 0x0FU;
    }

    [[nodiscard]] inline std::uint8_t blockLight(int x, int y, int z) const noexcept
    {
        return (packed[static_cast<std::size_t>(x + (y * ChunkSize) + (z * ChunkSize * ChunkSize))] >> 4U) & 0x0FU;
    }

    inline void setSkyLight(int x, int y, int z, std::uint8_t value) noexcept
    {
        auto& cell = packed[static_cast<std::size_t>(x + (y * ChunkSize) + (z * ChunkSize * ChunkSize))];
        cell = static_cast<std::uint8_t>((cell & 0xF0U) | (value & 0x0FU));
    }

    inline void setBlockLight(int x, int y, int z, std::uint8_t value) noexcept
    {
        auto& cell = packed[static_cast<std::size_t>(x + (y * ChunkSize) + (z * ChunkSize * ChunkSize))];
        cell = static_cast<std::uint8_t>((cell & 0x0FU) | ((value & 0x0FU) << 4U));
    }

    inline void clear() noexcept
    {
        packed.fill(0U);
    }
};

} // namespace voxel::world
