#pragma once

#include <cstdint>
#include <cstdlib>

namespace voxel::world {

struct ChunkCoord {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};

    [[nodiscard]] friend bool operator==(ChunkCoord lhs, ChunkCoord rhs) noexcept
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }
};

struct BlockCoord {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};

    [[nodiscard]] friend bool operator==(BlockCoord lhs, BlockCoord rhs) noexcept
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }
};

struct RegionCoord {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};

    [[nodiscard]] friend bool operator==(RegionCoord lhs, RegionCoord rhs) noexcept
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }
};

struct PlanetCoord {
    std::uint64_t planetId{};
    RegionCoord region{};
    ChunkCoord chunk{};
    BlockCoord block{};

    [[nodiscard]] friend bool operator==(const PlanetCoord& lhs, const PlanetCoord& rhs) noexcept
    {
        return lhs.planetId == rhs.planetId
            && lhs.region == rhs.region
            && lhs.chunk == rhs.chunk
            && lhs.block == rhs.block;
    }
};

struct RenderPos {
    float x{};
    float y{};
    float z{};
};

struct ChunkCoordHash {
    [[nodiscard]] std::size_t operator()(ChunkCoord coord) const noexcept
    {
        const auto x = static_cast<std::uint64_t>(coord.x);
        const auto y = static_cast<std::uint64_t>(coord.y);
        const auto z = static_cast<std::uint64_t>(coord.z);
        return static_cast<std::size_t>((x * 73856093ULL) ^ (y * 19349663ULL) ^ (z * 83492791ULL));
    }
};

} // namespace voxel::world
