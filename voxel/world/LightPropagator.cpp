#include <voxel/world/LightPropagator.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace voxel::world {

namespace {

bool inBounds(int x, int y, int z) noexcept
{
    return x >= 0 && y >= 0 && z >= 0
        && x < ChunkSize && y < ChunkSize && z < ChunkSize;
}

constexpr std::array<std::array<int, 3>, 6> kNeighbours{{
    {-1, 0, 0}, {1, 0, 0},
    {0, -1, 0}, {0, 1, 0},
    {0, 0, -1}, {0, 0, 1},
}};

// Read sky light from a neighbour chunk at a local coordinate.
// Returns 15 (open sky) if the neighbour is above and not loaded, 0 otherwise.
std::uint8_t sampleNeighbourSky(const ChunkManager& chunks, ChunkCoord neighbourCoord,
                                int nx, int ny, int nz, int stepDirY) noexcept
{
    const Chunk* neighbour = chunks.find(neighbourCoord);
    if (neighbour == nullptr || neighbour->lightData() == nullptr) {
        // No data: assume open sky only when looking upward through a missing +Y neighbour.
        return (stepDirY > 0) ? std::uint8_t{15} : std::uint8_t{0};
    }
    return neighbour->lightData()->skyLight(nx, ny, nz);
}

std::uint8_t sampleNeighbourBlock(const ChunkManager& chunks, ChunkCoord neighbourCoord,
                                  int nx, int ny, int nz) noexcept
{
    const Chunk* neighbour = chunks.find(neighbourCoord);
    if (neighbour == nullptr || neighbour->lightData() == nullptr) {
        return 0U;
    }
    return neighbour->lightData()->blockLight(nx, ny, nz);
}

bool isUniformChunk(const Chunk& chunk, BlockStateId& state) noexcept
{
    const auto& data = chunk.blockData();
    if (data.palette.size() != 1) {
        return false;
    }
    state = data.palette.at(0);
    return true;
}

bool aboveFaceIsFullSky(const Chunk& target, const ChunkManager* chunks) noexcept
{
    if (chunks == nullptr) {
        return true;
    }
    const ChunkCoord above{target.coord().x, target.coord().y + 1, target.coord().z};
    const auto* chunk = chunks->find(above);
    if (chunk == nullptr) {
        return true;
    }
    const auto* light = chunk->lightData();
    if (light == nullptr) {
        return false;
    }
    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            if (light->skyLight(x, 0, z) != 15U) {
                return false;
            }
        }
    }
    return true;
}

bool hasIncomingNeighbourBlockLight(const Chunk& target, const ChunkManager* chunks) noexcept
{
    if (chunks == nullptr) {
        return false;
    }
    const auto cx = target.coord().x;
    const auto cy = target.coord().y;
    const auto cz = target.coord().z;

    const auto faceHasLight = [](const Chunk* chunk, auto&& sampler) noexcept {
        if (chunk == nullptr || chunk->lightData() == nullptr) {
            return false;
        }
        const auto* light = chunk->lightData();
        for (int b = 0; b < ChunkSize; ++b) {
            for (int a = 0; a < ChunkSize; ++a) {
                if (sampler(*light, a, b) > 1U) {
                    return true;
                }
            }
        }
        return false;
    };

    if (faceHasLight(chunks->find({cx - 1, cy, cz}), [](const ChunkLightData& l, int y, int z) { return l.blockLight(ChunkSize - 1, y, z); })) return true;
    if (faceHasLight(chunks->find({cx + 1, cy, cz}), [](const ChunkLightData& l, int y, int z) { return l.blockLight(0, y, z); })) return true;
    if (faceHasLight(chunks->find({cx, cy - 1, cz}), [](const ChunkLightData& l, int x, int z) { return l.blockLight(x, ChunkSize - 1, z); })) return true;
    if (faceHasLight(chunks->find({cx, cy + 1, cz}), [](const ChunkLightData& l, int x, int z) { return l.blockLight(x, 0, z); })) return true;
    if (faceHasLight(chunks->find({cx, cy, cz - 1}), [](const ChunkLightData& l, int x, int y) { return l.blockLight(x, y, ChunkSize - 1); })) return true;
    if (faceHasLight(chunks->find({cx, cy, cz + 1}), [](const ChunkLightData& l, int x, int y) { return l.blockLight(x, y, 0); })) return true;
    return false;
}

std::uint32_t packCell(int x, int y, int z, std::uint8_t level) noexcept
{
    return static_cast<std::uint32_t>(x)
        | (static_cast<std::uint32_t>(y) << 5U)
        | (static_cast<std::uint32_t>(z) << 10U)
        | (static_cast<std::uint32_t>(level) << 15U);
}

void runPropagation(const Chunk& target, const ChunkManager* chunks,
                    const BlockLightCatalog& catalog, ChunkLightData& out)
{
    out.clear();

    BlockStateId uniformState{};
    if (isUniformChunk(target, uniformState)) {
        const auto info = catalog.get(uniformState);
        if (info.emission == 0U && info.attenuation >= 15U) {
            return;
        }
        if (info.emission == 0U && info.attenuation == 0U
            && aboveFaceIsFullSky(target, chunks)
            && !hasIncomingNeighbourBlockLight(target, chunks)) {
            out.packed.fill(0x0FU);
            return;
        }
    }

    // --- Sky light --------------------------------------------------------
    // Top-down column seeded from the +Y neighbour (if present) or open sky.
    std::vector<std::uint32_t> skyFrontier;
    skyFrontier.reserve(static_cast<std::size_t>(ChunkVolume) / 2U);

    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            std::uint8_t level = 15;
            if (chunks != nullptr) {
                const ChunkCoord above{target.coord().x, target.coord().y + 1, target.coord().z};
                level = sampleNeighbourSky(*chunks, above, x, 0, z, 1);
            }
            for (int y = ChunkSize - 1; y >= 0; --y) {
                const auto info = catalog.get(target.blockAt(x, y, z));
                if (info.attenuation == 0U) {
                    out.setSkyLight(x, y, z, level);
                    if (level > 1U) {
                        skyFrontier.push_back(packCell(x, y, z, level));
                    }
                } else {
                    level = 0U;
                }
            }
        }
    }

    // Seed sky from the other five neighbour faces (-X, +X, -Y, -Z, +Z).
    if (chunks != nullptr) {
        const auto trySeedSky = [&](int x, int y, int z, std::uint8_t incoming) {
            if (incoming <= 1U) {
                return;
            }
            const auto info = catalog.get(target.blockAt(x, y, z));
            if (info.attenuation > 0U) {
                return;
            }
            const auto seeded = static_cast<std::uint8_t>(incoming - 1U);
            if (seeded > out.skyLight(x, y, z)) {
                out.setSkyLight(x, y, z, seeded);
                if (seeded > 1U) {
                    skyFrontier.push_back(packCell(x, y, z, seeded));
                }
            }
        };

        // -X face (target's x=0 cells receive from neighbour at (cx-1) cell (31, y, z))
        const ChunkCoord negX{target.coord().x - 1, target.coord().y, target.coord().z};
        for (int z = 0; z < ChunkSize; ++z) {
            for (int y = 0; y < ChunkSize; ++y) {
                trySeedSky(0, y, z, sampleNeighbourSky(*chunks, negX, ChunkSize - 1, y, z, 0));
            }
        }
        const ChunkCoord posX{target.coord().x + 1, target.coord().y, target.coord().z};
        for (int z = 0; z < ChunkSize; ++z) {
            for (int y = 0; y < ChunkSize; ++y) {
                trySeedSky(ChunkSize - 1, y, z, sampleNeighbourSky(*chunks, posX, 0, y, z, 0));
            }
        }
        const ChunkCoord negY{target.coord().x, target.coord().y - 1, target.coord().z};
        for (int z = 0; z < ChunkSize; ++z) {
            for (int x = 0; x < ChunkSize; ++x) {
                trySeedSky(x, 0, z, sampleNeighbourSky(*chunks, negY, x, ChunkSize - 1, z, -1));
            }
        }
        const ChunkCoord negZ{target.coord().x, target.coord().y, target.coord().z - 1};
        for (int y = 0; y < ChunkSize; ++y) {
            for (int x = 0; x < ChunkSize; ++x) {
                trySeedSky(x, y, 0, sampleNeighbourSky(*chunks, negZ, x, y, ChunkSize - 1, 0));
            }
        }
        const ChunkCoord posZ{target.coord().x, target.coord().y, target.coord().z + 1};
        for (int y = 0; y < ChunkSize; ++y) {
            for (int x = 0; x < ChunkSize; ++x) {
                trySeedSky(x, y, ChunkSize - 1, sampleNeighbourSky(*chunks, posZ, x, y, 0, 0));
            }
        }
    }

    for (std::size_t cursor = 0; cursor < skyFrontier.size(); ++cursor) {
        const auto cell = skyFrontier[cursor];
        const int x = static_cast<int>(cell & 31U);
        const int y = static_cast<int>((cell >> 5U) & 31U);
        const int z = static_cast<int>((cell >> 10U) & 31U);
        const auto level = static_cast<std::uint8_t>((cell >> 15U) & 15U);
        if (level <= 1U) {
            continue;
        }
        for (const auto& delta : kNeighbours) {
            const int nx = x + delta[0];
            const int ny = y + delta[1];
            const int nz = z + delta[2];
            if (!inBounds(nx, ny, nz)) {
                continue; // Cross-chunk neighbours were seeded upfront.
            }
            const auto info = catalog.get(target.blockAt(nx, ny, nz));
            if (info.attenuation > 0U) {
                continue;
            }
            const auto next = static_cast<std::uint8_t>(level - 1U);
            if (next > out.skyLight(nx, ny, nz)) {
                out.setSkyLight(nx, ny, nz, next);
                if (next > 1U) {
                    skyFrontier.push_back(packCell(nx, ny, nz, next));
                }
            }
        }
    }

    // --- Block light ------------------------------------------------------
    std::vector<std::uint32_t> blockFrontier;
    blockFrontier.reserve(256U);
    for (int z = 0; z < ChunkSize; ++z) {
        for (int y = 0; y < ChunkSize; ++y) {
            for (int x = 0; x < ChunkSize; ++x) {
                const auto info = catalog.get(target.blockAt(x, y, z));
                if (info.emission > 0U) {
                    out.setBlockLight(x, y, z, info.emission);
                    if (info.emission > 1U) {
                        blockFrontier.push_back(packCell(x, y, z, info.emission));
                    }
                }
            }
        }
    }

    // Seed block light from neighbours.
    if (chunks != nullptr) {
        const auto trySeedBlock = [&](int x, int y, int z, std::uint8_t incoming) {
            if (incoming <= 1U) {
                return;
            }
            const auto info = catalog.get(target.blockAt(x, y, z));
            if (info.attenuation >= 15U) {
                return;
            }
            const auto seeded = static_cast<std::uint8_t>(incoming - 1U);
            if (seeded > out.blockLight(x, y, z)) {
                out.setBlockLight(x, y, z, seeded);
                if (seeded > 1U) {
                    blockFrontier.push_back(packCell(x, y, z, seeded));
                }
            }
        };
        const auto cx = target.coord().x;
        const auto cy = target.coord().y;
        const auto cz = target.coord().z;
        for (int z = 0; z < ChunkSize; ++z) {
            for (int y = 0; y < ChunkSize; ++y) {
                trySeedBlock(0, y, z, sampleNeighbourBlock(*chunks, {cx - 1, cy, cz}, ChunkSize - 1, y, z));
                trySeedBlock(ChunkSize - 1, y, z, sampleNeighbourBlock(*chunks, {cx + 1, cy, cz}, 0, y, z));
            }
        }
        for (int z = 0; z < ChunkSize; ++z) {
            for (int x = 0; x < ChunkSize; ++x) {
                trySeedBlock(x, 0, z, sampleNeighbourBlock(*chunks, {cx, cy - 1, cz}, x, ChunkSize - 1, z));
                trySeedBlock(x, ChunkSize - 1, z, sampleNeighbourBlock(*chunks, {cx, cy + 1, cz}, x, 0, z));
            }
        }
        for (int y = 0; y < ChunkSize; ++y) {
            for (int x = 0; x < ChunkSize; ++x) {
                trySeedBlock(x, y, 0, sampleNeighbourBlock(*chunks, {cx, cy, cz - 1}, x, y, ChunkSize - 1));
                trySeedBlock(x, y, ChunkSize - 1, sampleNeighbourBlock(*chunks, {cx, cy, cz + 1}, x, y, 0));
            }
        }
    }

    for (std::size_t cursor = 0; cursor < blockFrontier.size(); ++cursor) {
        const auto cell = blockFrontier[cursor];
        const int x = static_cast<int>(cell & 31U);
        const int y = static_cast<int>((cell >> 5U) & 31U);
        const int z = static_cast<int>((cell >> 10U) & 31U);
        const auto level = static_cast<std::uint8_t>((cell >> 15U) & 15U);
        if (level <= 1U) {
            continue;
        }
        for (const auto& delta : kNeighbours) {
            const int nx = x + delta[0];
            const int ny = y + delta[1];
            const int nz = z + delta[2];
            if (!inBounds(nx, ny, nz)) {
                continue;
            }
            const auto info = catalog.get(target.blockAt(nx, ny, nz));
            if (info.attenuation >= 15U) {
                continue;
            }
            const auto next = static_cast<std::uint8_t>(level - 1U);
            if (next > out.blockLight(nx, ny, nz)) {
                out.setBlockLight(nx, ny, nz, next);
                if (next > 1U) {
                    blockFrontier.push_back(packCell(nx, ny, nz, next));
                }
            }
        }
    }

}

} // namespace

void LightPropagator::propagate(const Chunk& target, const ChunkManager& chunks,
                                const BlockLightCatalog& catalog, ChunkLightData& out) const
{
    runPropagation(target, &chunks, catalog, out);
}

void LightPropagator::propagateIsolated(const Chunk& target, const BlockLightCatalog& catalog,
                                        ChunkLightData& out) const
{
    runPropagation(target, nullptr, catalog, out);
}

} // namespace voxel::world
