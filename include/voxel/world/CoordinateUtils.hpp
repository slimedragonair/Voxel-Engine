#pragma once

#include <cstdint>

#include <voxel/world/Chunk.hpp>

namespace voxel::world {

struct ChunkLocalBlock {
    ChunkCoord chunk{};
    BlockCoord local{};
};

[[nodiscard]] constexpr std::int64_t floorDiv(std::int64_t value, std::int64_t divisor) noexcept
{
    std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

[[nodiscard]] constexpr std::int32_t floorMod(std::int64_t value, std::int64_t divisor) noexcept
{
    std::int64_t remainder = value % divisor;
    if (remainder < 0) {
        remainder += divisor;
    }
    return static_cast<std::int32_t>(remainder);
}

[[nodiscard]] constexpr ChunkLocalBlock toChunkLocal(std::int64_t x, std::int64_t y, std::int64_t z) noexcept
{
    return {
        {floorDiv(x, ChunkSize), floorDiv(y, ChunkSize), floorDiv(z, ChunkSize)},
        {floorMod(x, ChunkSize), floorMod(y, ChunkSize), floorMod(z, ChunkSize)}
    };
}

[[nodiscard]] constexpr BlockCoord toWorldBlock(ChunkCoord chunk, BlockCoord local) noexcept
{
    return {
        static_cast<std::int32_t>(chunk.x * ChunkSize + local.x),
        static_cast<std::int32_t>(chunk.y * ChunkSize + local.y),
        static_cast<std::int32_t>(chunk.z * ChunkSize + local.z)
    };
}

} // namespace voxel::world

