#include <voxel/world/NoiseTerrainGenerator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

#include <voxel/core/Math.hpp>

namespace voxel::world {

namespace {

thread_local TerrainGenerationMode LastGenerationMode = TerrainGenerationMode::Direct;

std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    return seed;
}

std::uint32_t floatBits(float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::size_t columnIndex(int x, int z) noexcept
{
    return static_cast<std::size_t>(x + (z * ChunkSize));
}

struct ChunkTerrainBounds {
    int maxSurfaceBlockY{std::numeric_limits<int>::min()};
    int maxSolidTop{-1};
    int maxStoneTop{-1};
    int maxSeaTop{-1};
    bool needsCaveSamples{false};
};

struct WorldShapeSignals {
    float continentalness{};
    float erosion{};
    float peaksValleys{};
    float temperature{};
    float humidity{};
    float weirdness{};
    float detail{};
};

float clamp01(float value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

float remap(float value, float inMin, float inMax, float outMin, float outMax) noexcept
{
    const float t = clamp01((value - inMin) / (inMax - inMin));
    return outMin + ((outMax - outMin) * t);
}

float smooth01(float value) noexcept
{
    const float t = clamp01(value);
    return t * t * (3.0F - 2.0F * t);
}

WorldShapeSignals sampleWorldShapeSignals(float worldX, float worldZ, const NoiseTerrainSettings& settings) noexcept
{
    WorldShapeSignals signals{};
    signals.continentalness = std::clamp((core::fbm2D(
        worldX * settings.continentFrequency,
        worldZ * settings.continentFrequency,
        settings.seed ^ 0xA511E9B3U, 4, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.85F, -1.0F, 1.0F);
    signals.erosion = std::clamp((core::fbm2D(
        worldX * settings.erosionFrequency,
        worldZ * settings.erosionFrequency,
        settings.seed ^ 0x6C8E9CF5U, 3, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.4F, -1.0F, 1.0F);
    signals.peaksValleys = std::clamp((core::fbm2D(
        worldX * settings.peaksFrequency,
        worldZ * settings.peaksFrequency,
        settings.seed ^ 0xB5297A4DU, 4, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.5F, -1.0F, 1.0F);
    signals.temperature = std::clamp((core::fbm2D(
        worldX * settings.climateFrequency,
        worldZ * settings.climateFrequency,
        settings.seed ^ 0x1F123BB5U, 3, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.35F, -1.0F, 1.0F);
    signals.humidity = std::clamp((core::fbm2D(
        worldX * settings.climateFrequency,
        worldZ * settings.climateFrequency,
        settings.seed ^ 0x7D2B4A91U, 3, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.35F, -1.0F, 1.0F);
    signals.weirdness = std::clamp((core::fbm2D(
        worldX * settings.weirdnessFrequency,
        worldZ * settings.weirdnessFrequency,
        settings.seed ^ 0xDD4B2F27U, 3, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.45F, -1.0F, 1.0F);
    signals.detail = core::fbm2D(
        worldX * settings.surfaceFrequency,
        worldZ * settings.surfaceFrequency,
        settings.seed, settings.surfaceOctaves, 2.0F, 0.5F) * 2.0F - 1.0F;
    return signals;
}

float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

WorldShapeSignals lerpSignals(const WorldShapeSignals& a, const WorldShapeSignals& b, float t) noexcept
{
    return {
        lerp(a.continentalness, b.continentalness, t),
        lerp(a.erosion, b.erosion, t),
        lerp(a.peaksValleys, b.peaksValleys, t),
        lerp(a.temperature, b.temperature, t),
        lerp(a.humidity, b.humidity, t),
        lerp(a.weirdness, b.weirdness, t),
        lerp(a.detail, b.detail, t),
    };
}

TerrainBiomeId resolveBiome(float continentalness, float elevation, float erosion, float temperature,
                            float humidity, float weirdness, TerrainSurfaceKind surfaceKind) noexcept
{
    if (surfaceKind == TerrainSurfaceKind::DeepOcean) {
        return temperature < -0.25F ? TerrainBiomeId::ColdOcean : TerrainBiomeId::DeepOcean;
    }
    if (surfaceKind == TerrainSurfaceKind::Ocean || surfaceKind == TerrainSurfaceKind::ShallowOcean) {
        if (temperature > 0.35F) return TerrainBiomeId::WarmOcean;
        if (temperature < -0.35F) return TerrainBiomeId::ColdOcean;
        return TerrainBiomeId::Ocean;
    }
    if (surfaceKind == TerrainSurfaceKind::Beach) {
        return TerrainBiomeId::Beach;
    }
    if (weirdness > 0.72F && humidity > 0.15F) {
        return TerrainBiomeId::MagicalGrove;
    }
    if (weirdness > 0.78F && humidity < -0.35F && temperature > 0.25F) {
        return TerrainBiomeId::VolcanicWastes;
    }
    if (elevation > 42.0F || continentalness > 0.65F) {
        return temperature < -0.25F ? TerrainBiomeId::SnowyMountains : TerrainBiomeId::Mountains;
    }
    if (temperature < -0.35F) {
        return TerrainBiomeId::Taiga;
    }
    if (humidity > 0.55F && elevation < 8.0F) {
        return TerrainBiomeId::Swamp;
    }
    if (humidity < -0.52F && temperature > 0.25F) {
        return erosion > 0.35F ? TerrainBiomeId::Badlands : TerrainBiomeId::Desert;
    }
    if (temperature > 0.35F && humidity < -0.1F) {
        return TerrainBiomeId::Savanna;
    }
    if (humidity > 0.42F) {
        return TerrainBiomeId::DenseForest;
    }
    if (humidity > 0.12F) {
        return TerrainBiomeId::Forest;
    }
    return TerrainBiomeId::Plains;
}

ColumnWorldgenData buildColumnDataFromSignals(const WorldShapeSignals& signals, const NoiseTerrainSettings& settings) noexcept
{
    const float continentalness = std::clamp(signals.continentalness, -1.0F, 1.0F);
    const float erosion = std::clamp(signals.erosion, -1.0F, 1.0F);
    const float peaksValleys = std::clamp(signals.peaksValleys, -1.0F, 1.0F);
    const float temperature = std::clamp(signals.temperature, -1.0F, 1.0F);
    const float humidity = std::clamp(signals.humidity, -1.0F, 1.0F);
    const float weirdness = std::clamp(signals.weirdness, -1.0F, 1.0F);
    const float detail = std::clamp(signals.detail, -1.0F, 1.0F);

    TerrainSurfaceKind surfaceKind = TerrainSurfaceKind::Land;
    float surfaceY = settings.seaLevel;
    float oceanDepth = 0.0F;

    if (continentalness < -0.65F) {
        surfaceKind = TerrainSurfaceKind::DeepOcean;
        oceanDepth = remap(continentalness, -1.0F, -0.65F, settings.deepOceanMaxDepth, settings.deepOceanMinDepth);
        surfaceY = settings.seaLevel - oceanDepth + detail * 5.0F;
    } else if (continentalness < -0.35F) {
        surfaceKind = TerrainSurfaceKind::Ocean;
        oceanDepth = remap(continentalness, -0.65F, -0.35F, settings.deepOceanMinDepth, settings.shelfMaxDepth);
        surfaceY = settings.seaLevel - oceanDepth + detail * 4.0F;
    } else if (continentalness < -0.15F) {
        surfaceKind = TerrainSurfaceKind::ShallowOcean;
        oceanDepth = remap(continentalness, -0.35F, -0.15F, settings.shelfMaxDepth, settings.shelfMinDepth);
        surfaceY = settings.seaLevel - oceanDepth + detail * 3.0F;
    } else if (continentalness < 0.05F) {
        surfaceKind = TerrainSurfaceKind::Beach;
        const float coastT = remap(continentalness, -0.15F, 0.05F, 0.0F, 1.0F);
        surfaceY = settings.seaLevel - 2.0F + coastT * 5.0F + detail * 1.5F;
        oceanDepth = std::max(0.0F, settings.seaLevel - surfaceY);
    } else {
        const float landT = remap(continentalness, 0.05F, 1.0F, 0.0F, 1.0F);
        const float roughness = (1.0F - clamp01((erosion + 1.0F) * 0.5F)) * settings.hillBoost;
        const float peak = smooth01((peaksValleys + 1.0F) * 0.5F) * settings.mountainBoost * landT;
        surfaceY = settings.seaLevel + landT * settings.surfaceAmplitude + roughness + peak + detail * settings.surfaceAmplitude;
    }

    ColumnWorldgenData data{};
    data.continentalness = continentalness;
    data.erosion = erosion;
    data.peaksValleys = peaksValleys;
    data.temperature = temperature;
    data.humidity = humidity;
    data.weirdness = weirdness;
    data.oceanDepth = std::max(0.0F, oceanDepth);
    data.biomeBlend = smooth01(1.0F - std::abs(continentalness - 0.05F) * 5.0F);
    data.surfaceY = static_cast<std::int32_t>(std::floor(surfaceY));
    data.seaLevel = static_cast<std::int32_t>(std::floor(settings.seaLevel));
    data.surfaceKind = surfaceKind;
    data.isOcean = surfaceKind == TerrainSurfaceKind::DeepOcean
        || surfaceKind == TerrainSurfaceKind::Ocean
        || surfaceKind == TerrainSurfaceKind::ShallowOcean;
    data.isBeach = surfaceKind == TerrainSurfaceKind::Beach;
    data.isRiverCandidate = !data.isOcean && humidity > 0.45F && erosion < -0.15F && std::abs(weirdness) < 0.35F;
    data.biome = resolveBiome(continentalness, surfaceY - settings.seaLevel, erosion, temperature, humidity, weirdness, surfaceKind);
    return data;
}

ColumnWorldgenData buildColumnData(float worldX, float worldZ, const NoiseTerrainSettings& settings) noexcept
{
    return buildColumnDataFromSignals(sampleWorldShapeSignals(worldX, worldZ, settings), settings);
}

} // namespace

NoiseTerrainGenerator::NoiseTerrainGenerator(NoiseTerrainSettings settings)
    : settings_(settings)
{
}

TerrainGenerationMode NoiseTerrainGenerator::lastGenerationMode() const noexcept
{
    return LastGenerationMode;
}

void NoiseTerrainGenerator::setPrepassCache(std::shared_ptr<TerrainColumnPrepassCache> cache) noexcept
{
    prepassCache_ = std::move(cache);
}

std::uint64_t NoiseTerrainGenerator::terrainVersion() const noexcept
{
    std::uint64_t version = 0x1d6f8b2d4c3a0197ULL;
    version = hashCombine(version, floatBits(settings_.surfaceFrequency));
    version = hashCombine(version, floatBits(settings_.surfaceAmplitude));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.surfaceOctaves));
    version = hashCombine(version, floatBits(settings_.seaLevel));
    version = hashCombine(version, floatBits(settings_.continentFrequency));
    version = hashCombine(version, floatBits(settings_.erosionFrequency));
    version = hashCombine(version, floatBits(settings_.peaksFrequency));
    version = hashCombine(version, floatBits(settings_.climateFrequency));
    version = hashCombine(version, floatBits(settings_.weirdnessFrequency));
    version = hashCombine(version, floatBits(settings_.deepOceanMinDepth));
    version = hashCombine(version, floatBits(settings_.deepOceanMaxDepth));
    version = hashCombine(version, floatBits(settings_.shelfMinDepth));
    version = hashCombine(version, floatBits(settings_.shelfMaxDepth));
    version = hashCombine(version, floatBits(settings_.mountainBoost));
    version = hashCombine(version, floatBits(settings_.hillBoost));
    version = hashCombine(version, floatBits(settings_.caveFrequency));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.caveOctaves));
    version = hashCombine(version, floatBits(settings_.caveThreshold));
    version = hashCombine(version, floatBits(settings_.caveCeilingOffset));
    version = hashCombine(version, floatBits(settings_.coalFrequency));
    version = hashCombine(version, floatBits(settings_.coalThreshold));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.coalMinDepth));
    version = hashCombine(version, floatBits(settings_.ironFrequency));
    version = hashCombine(version, floatBits(settings_.ironThreshold));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.ironMinDepth));
    version = hashCombine(version, settings_.stoneBlock.value);
    version = hashCombine(version, settings_.dirtBlock.value);
    version = hashCombine(version, settings_.grassBlock.value);
    version = hashCombine(version, settings_.coalOreBlock.value);
    version = hashCombine(version, settings_.ironOreBlock.value);
    version = hashCombine(version, settings_.waterBlock.value);
    return version;
}

TerrainColumnKey NoiseTerrainGenerator::prepassKey(TerrainColumnCoord coord) const noexcept
{
    return {coord, settings_.seed, terrainVersion()};
}

TerrainColumnPrepass NoiseTerrainGenerator::buildColumnPrepass(TerrainColumnCoord coord) const
{
    TerrainColumnPrepass prepass;
    prepass.key = prepassKey(coord);
    const auto worldBaseX = static_cast<float>(coord.x * ChunkSize);
    const auto worldBaseZ = static_cast<float>(coord.z * ChunkSize);

    constexpr int kWorldShapeStep = 4;
    constexpr int kWorldShapeGrid = (ChunkSize / kWorldShapeStep) + 1;
    std::array<WorldShapeSignals, kWorldShapeGrid * kWorldShapeGrid> signalGrid{};
    const auto signalIndex = [](int x, int z) constexpr {
        return static_cast<std::size_t>(x + z * kWorldShapeGrid);
    };

    for (int gz = 0; gz < kWorldShapeGrid; ++gz) {
        const float worldZ = worldBaseZ + static_cast<float>(gz * kWorldShapeStep);
        for (int gx = 0; gx < kWorldShapeGrid; ++gx) {
            const float worldX = worldBaseX + static_cast<float>(gx * kWorldShapeStep);
            signalGrid[signalIndex(gx, gz)] = sampleWorldShapeSignals(worldX, worldZ, settings_);
        }
    }

    for (int z = 0; z < ChunkSize; ++z) {
        const int gz = std::clamp(z / kWorldShapeStep, 0, kWorldShapeGrid - 2);
        const float tz = static_cast<float>(z - (gz * kWorldShapeStep)) / static_cast<float>(kWorldShapeStep);
        for (int x = 0; x < ChunkSize; ++x) {
            const int gx = std::clamp(x / kWorldShapeStep, 0, kWorldShapeGrid - 2);
            const float tx = static_cast<float>(x - (gx * kWorldShapeStep)) / static_cast<float>(kWorldShapeStep);
            const auto x0 = lerpSignals(signalGrid[signalIndex(gx, gz)], signalGrid[signalIndex(gx + 1, gz)], tx);
            const auto x1 = lerpSignals(signalGrid[signalIndex(gx, gz + 1)], signalGrid[signalIndex(gx + 1, gz + 1)], tx);
            const auto data = buildColumnDataFromSignals(lerpSignals(x0, x1, tz), settings_);
            const auto idx = columnIndex(x, z);
            prepass.surfaceY[idx] = static_cast<float>(data.surfaceY);
            prepass.surfaceBlockY[idx] = data.surfaceY;
            prepass.surfaceKind[idx] = data.surfaceKind;
            prepass.biome[idx] = data.biome;
            prepass.continentalness[idx] = data.continentalness;
            prepass.erosion[idx] = data.erosion;
            prepass.peaksValleys[idx] = data.peaksValleys;
            prepass.temperature[idx] = data.temperature;
            prepass.humidity[idx] = data.humidity;
            prepass.weirdness[idx] = data.weirdness;
            prepass.oceanDepth[idx] = data.oceanDepth;
            prepass.biomeBlend[idx] = data.biomeBlend;
            prepass.seaMask[idx] = data.surfaceY < data.seaLevel;
            prepass.beachMask[idx] = data.isBeach;
            prepass.riverCandidateMask[idx] = data.isRiverCandidate;
        }
    }

    return prepass;
}

void NoiseTerrainGenerator::generate(Chunk& chunk)
{
    const auto chunkCoord = chunk.coord();
    const TerrainColumnCoord columnCoord{chunkCoord.x, chunkCoord.z};
    TerrainColumnPrepass prepass;
    LastGenerationMode = TerrainGenerationMode::Direct;

    if (prepassCache_) {
        if (auto cached = prepassCache_->find(prepassKey(columnCoord))) {
            prepass = std::move(*cached);
            LastGenerationMode = TerrainGenerationMode::CachedPrepass;
        } else {
            prepass = buildColumnPrepass(columnCoord);
            prepassCache_->insert(prepass);
        }
    } else {
        prepass = buildColumnPrepass(columnCoord);
    }

    const auto worldBaseX = static_cast<float>(chunkCoord.x * ChunkSize);
    const auto worldBaseY = static_cast<float>(chunkCoord.y * ChunkSize);
    const auto worldBaseZ = static_cast<float>(chunkCoord.z * ChunkSize);
    const int chunkBaseBlockY = static_cast<int>(chunkCoord.y * ChunkSize);
    const int seaTopForChunk = std::clamp(static_cast<int>(std::floor(settings_.seaLevel)) - chunkBaseBlockY, -1, ChunkSize - 1);

    ChunkTerrainBounds bounds{};
    bounds.maxSeaTop = seaTopForChunk;
    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            const auto prepassIndex = columnIndex(x, z);
            const int surfaceBlockY = static_cast<int>(prepass.surfaceBlockY[prepassIndex]);
            const int solidTop = std::clamp(surfaceBlockY - chunkBaseBlockY, -1, ChunkSize - 1);
            const int stoneTop = std::min(solidTop - 4, ChunkSize - 1);
            bounds.maxSurfaceBlockY = std::max(bounds.maxSurfaceBlockY, surfaceBlockY);
            bounds.maxSolidTop = std::max(bounds.maxSolidTop, solidTop);
            bounds.maxStoneTop = std::max(bounds.maxStoneTop, stoneTop);
            if (stoneTop >= 0
                && surfaceBlockY - chunkBaseBlockY > static_cast<int>(settings_.caveCeilingOffset)) {
                bounds.needsCaveSamples = true;
            }
        }
    }

    if (chunkBaseBlockY > bounds.maxSurfaceBlockY && bounds.maxSeaTop < 0) {
        chunk.markGenerated();
        return;
    }

    constexpr int kCoarseStep = 4;
    constexpr int kCoarseSize = (ChunkSize / kCoarseStep) + 1;
    std::array<float, static_cast<std::size_t>(kCoarseSize * kCoarseSize * kCoarseSize)> caveSamples{};
    std::array<float, static_cast<std::size_t>(kCoarseSize * kCoarseSize * kCoarseSize)> coalSamples{};
    std::array<float, static_cast<std::size_t>(kCoarseSize * kCoarseSize * kCoarseSize)> ironSamples{};

    auto caveIndex = [](int gx, int gy, int gz) constexpr {
        return static_cast<std::size_t>(gx + (gy * kCoarseSize) + (gz * kCoarseSize * kCoarseSize));
    };

    const int maxDensityGy = std::clamp((bounds.maxStoneTop / kCoarseStep) + 1, 0, kCoarseSize - 1);
    if (bounds.needsCaveSamples) {
        for (int gz = 0; gz < kCoarseSize; ++gz) {
            const float sampleZ = (worldBaseZ + static_cast<float>(gz * kCoarseStep)) * settings_.caveFrequency;
            for (int gy = 0; gy <= maxDensityGy; ++gy) {
                const float sampleY = (worldBaseY + static_cast<float>(gy * kCoarseStep)) * settings_.caveFrequency;
                for (int gx = 0; gx < kCoarseSize; ++gx) {
                    const float sampleX = (worldBaseX + static_cast<float>(gx * kCoarseStep)) * settings_.caveFrequency;
                    caveSamples[caveIndex(gx, gy, gz)] = core::fbm3D(
                        sampleX,
                        sampleY,
                        sampleZ,
                        settings_.seed ^ 0x9E3779B9U,
                        settings_.caveOctaves, 2.0F, 0.5F);
                }
            }
        }
    }

    if (bounds.maxStoneTop >= 0) {
        for (int gz = 0; gz < kCoarseSize; ++gz) {
            const float worldZ = worldBaseZ + static_cast<float>(gz * kCoarseStep);
            for (int gy = 0; gy <= maxDensityGy; ++gy) {
                const float worldY = worldBaseY + static_cast<float>(gy * kCoarseStep);
                for (int gx = 0; gx < kCoarseSize; ++gx) {
                    const float worldX = worldBaseX + static_cast<float>(gx * kCoarseStep);
                    coalSamples[caveIndex(gx, gy, gz)] = core::valueNoise3D(
                        worldX * settings_.coalFrequency,
                        worldY * settings_.coalFrequency,
                        worldZ * settings_.coalFrequency,
                        settings_.seed ^ 0xCC8C2B43U);
                    ironSamples[caveIndex(gx, gy, gz)] = core::valueNoise3D(
                        worldX * settings_.ironFrequency,
                        worldY * settings_.ironFrequency,
                        worldZ * settings_.ironFrequency,
                        settings_.seed ^ 0x4D7A1F69U);
                }
            }
        }
    }

    const auto densityAt = [&](const auto& samples, int x, int y, int z) noexcept {
        const int gx = std::clamp(x / kCoarseStep, 0, kCoarseSize - 2);
        const int gy = std::clamp(y / kCoarseStep, 0, kCoarseSize - 2);
        const int gz = std::clamp(z / kCoarseStep, 0, kCoarseSize - 2);
        const float tx = static_cast<float>(x - gx * kCoarseStep) / static_cast<float>(kCoarseStep);
        const float ty = static_cast<float>(y - gy * kCoarseStep) / static_cast<float>(kCoarseStep);
        const float tz = static_cast<float>(z - gz * kCoarseStep) / static_cast<float>(kCoarseStep);

        const float c000 = samples[caveIndex(gx, gy, gz)];
        const float c100 = samples[caveIndex(gx + 1, gy, gz)];
        const float c010 = samples[caveIndex(gx, gy + 1, gz)];
        const float c110 = samples[caveIndex(gx + 1, gy + 1, gz)];
        const float c001 = samples[caveIndex(gx, gy, gz + 1)];
        const float c101 = samples[caveIndex(gx + 1, gy, gz + 1)];
        const float c011 = samples[caveIndex(gx, gy + 1, gz + 1)];
        const float c111 = samples[caveIndex(gx + 1, gy + 1, gz + 1)];

        const float x00 = lerp(c000, c100, tx);
        const float x10 = lerp(c010, c110, tx);
        const float x01 = lerp(c001, c101, tx);
        const float x11 = lerp(c011, c111, tx);
        return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
    };

    const auto densityCellMayExceed = [&](const auto& samples, float threshold, int x, int y, int z) noexcept {
        const int gx = std::clamp(x / kCoarseStep, 0, kCoarseSize - 2);
        const int gy = std::clamp(y / kCoarseStep, 0, kCoarseSize - 2);
        const int gz = std::clamp(z / kCoarseStep, 0, kCoarseSize - 2);
        return samples[caveIndex(gx, gy, gz)] > threshold
            || samples[caveIndex(gx + 1, gy, gz)] > threshold
            || samples[caveIndex(gx, gy + 1, gz)] > threshold
            || samples[caveIndex(gx + 1, gy + 1, gz)] > threshold
            || samples[caveIndex(gx, gy, gz + 1)] > threshold
            || samples[caveIndex(gx + 1, gy, gz + 1)] > threshold
            || samples[caveIndex(gx, gy + 1, gz + 1)] > threshold
            || samples[caveIndex(gx + 1, gy + 1, gz + 1)] > threshold;
    };

    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            const auto prepassIndex = columnIndex(x, z);
            const int surfaceBlockY = static_cast<int>(prepass.surfaceBlockY[prepassIndex]);
            const int solidTop = std::clamp(surfaceBlockY - chunkBaseBlockY, -1, ChunkSize - 1);

            if (solidTop >= 0) {
                const auto surfaceKind = prepass.surfaceKind[prepassIndex];
                const auto biome = prepass.biome[prepassIndex];
                BlockStateId topBlock = settings_.grassBlock;
                BlockStateId subSurfaceBlock = settings_.dirtBlock;
                int soilDepth = 3;
                if (surfaceKind == TerrainSurfaceKind::DeepOcean || surfaceKind == TerrainSurfaceKind::Ocean) {
                    topBlock = settings_.stoneBlock;
                    subSurfaceBlock = settings_.stoneBlock;
                    soilDepth = 1;
                } else if (surfaceKind == TerrainSurfaceKind::ShallowOcean || surfaceKind == TerrainSurfaceKind::Beach) {
                    topBlock = settings_.dirtBlock;
                    subSurfaceBlock = settings_.dirtBlock;
                    soilDepth = 4;
                } else if (biome == TerrainBiomeId::Mountains || biome == TerrainBiomeId::SnowyMountains
                    || biome == TerrainBiomeId::VolcanicWastes) {
                    topBlock = settings_.stoneBlock;
                    subSurfaceBlock = settings_.stoneBlock;
                    soilDepth = 1;
                } else if (biome == TerrainBiomeId::Desert || biome == TerrainBiomeId::Badlands) {
                    topBlock = settings_.dirtBlock;
                    subSurfaceBlock = settings_.dirtBlock;
                    soilDepth = 5;
                } else if (biome == TerrainBiomeId::Swamp) {
                    topBlock = settings_.dirtBlock;
                    subSurfaceBlock = settings_.dirtBlock;
                    soilDepth = 4;
                }
                chunk.setBlockSilently(x, solidTop, z, topBlock);
                chunk.fillColumnRangeSilently(x, z, solidTop - soilDepth, solidTop - 1, subSurfaceBlock);
                chunk.fillColumnRangeSilently(x, z, 0, solidTop - soilDepth - 1, settings_.stoneBlock);
            }

            const int stoneTop = std::min(solidTop - 4, ChunkSize - 1);
            for (int y = 0; y <= stoneTop; ++y) {
                const float worldY = worldBaseY + static_cast<float>(y);
                const int depthFromSurface = surfaceBlockY - static_cast<int>(worldY);

                // Carve caves: only in the stone layer, never within the top few blocks
                // so we don't break the dirt/grass cap.
                if (bounds.needsCaveSamples
                    && depthFromSurface > static_cast<int>(settings_.caveCeilingOffset)
                    && densityCellMayExceed(caveSamples, settings_.caveThreshold, x, y, z)) {
                    const float cave = densityAt(caveSamples, x, y, z);
                    if (cave > settings_.caveThreshold) {
                        // Air pocket. Fill with water below sea level so caves under the
                        // ocean don't expose unrealistic dry voids.
                        if (worldY <= settings_.seaLevel) {
                            chunk.setBlockSilently(x, y, z, settings_.waterBlock);
                        } else {
                            chunk.setBlockSilently(x, y, z, AirBlockState);
                        }
                        continue;
                    }
                }

                // F5: ores in the stone band. Coal is shallow, iron deeper.
                BlockStateId oreBlock = settings_.stoneBlock;
                if (depthFromSurface >= settings_.coalMinDepth
                    && densityCellMayExceed(coalSamples, settings_.coalThreshold, x, y, z)) {
                    const float coal = densityAt(coalSamples, x, y, z);
                    if (coal > settings_.coalThreshold) {
                        oreBlock = settings_.coalOreBlock;
                    }
                }
                if (oreBlock.value == settings_.stoneBlock.value
                    && depthFromSurface >= settings_.ironMinDepth
                    && densityCellMayExceed(ironSamples, settings_.ironThreshold, x, y, z)) {
                    const float iron = densityAt(ironSamples, x, y, z);
                    if (iron > settings_.ironThreshold) {
                        oreBlock = settings_.ironOreBlock;
                    }
                }

                if (oreBlock.value != settings_.stoneBlock.value) {
                    chunk.setBlockSilently(x, y, z, oreBlock);
                }
            }

            if (seaTopForChunk > solidTop) {
                chunk.fillColumnRangeSilently(x, z, solidTop + 1, seaTopForChunk, settings_.waterBlock);
            }
        }
    }

    chunk.markGenerated();
}

} // namespace voxel::world
