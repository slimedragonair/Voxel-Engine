#include <voxel/world/NoiseTerrainGenerator.hpp>

#include "NoiseTerrainWorldShape.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

#include <voxel/core/Math.hpp>
#include <voxel/world/TerrainDefinitionRegistry.hpp>
#include <voxel/world/TerrainPipeline.hpp>

namespace voxel::world {

namespace {

thread_local TerrainGenerationMode LastGenerationMode = TerrainGenerationMode::Direct;

std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    return seed;
}

std::uint64_t mixSpaceBits(std::uint64_t value) noexcept
{
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

float spaceUnitFloat(std::uint64_t& state) noexcept
{
    state = mixSpaceBits(state + 0x9e3779b97f4a7c15ULL);
    return static_cast<float>((state >> 40U) & 0xFFFFFFU) / static_cast<float>(0xFFFFFFU);
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

bool solidSpaceFeature(SpaceFeatureType type) noexcept
{
    switch (type) {
    case SpaceFeatureType::AsteroidCluster:
    case SpaceFeatureType::IceField:
    case SpaceFeatureType::MetalRichAsteroids:
    case SpaceFeatureType::CrystalAsteroids:
    case SpaceFeatureType::Comet:
    case SpaceFeatureType::RingDebris:
        return true;
    default:
        return false;
    }
}

bool voxelMoonFeature(SpaceFeatureType type) noexcept
{
    return type == SpaceFeatureType::Moon;
}

bool voxelPlanetFeature(const SpaceFeature& feature) noexcept
{
    return feature.type == SpaceFeatureType::Planet && feature.landable;
}

BlockStateId materialForSpaceBlock(SpaceFeatureType type,
                                   std::uint32_t blockHash,
                                   float shell,
                                   const NoiseTerrainSettings& settings) noexcept
{
    const std::uint32_t roll = blockHash % 100U;
    switch (type) {
    case SpaceFeatureType::IceField:
    case SpaceFeatureType::Comet:
        return roll < 72U ? settings.spaceIceBlock : settings.spaceStoneBlock;
    case SpaceFeatureType::MetalRichAsteroids:
        return roll < 24U ? settings.spaceMetalOreBlock : settings.spaceStoneBlock;
    case SpaceFeatureType::CrystalAsteroids:
        if (roll < 18U || shell > 0.72F) {
            return settings.spaceCrystalBlock;
        }
        return roll < 26U ? settings.spaceMetalOreBlock : settings.spaceStoneBlock;
    case SpaceFeatureType::RingDebris:
        if (roll < 20U) {
            return settings.spaceIceBlock;
        }
        return roll < 28U ? settings.spaceMetalOreBlock : settings.spaceStoneBlock;
    case SpaceFeatureType::AsteroidCluster:
        return roll < 6U ? settings.spaceMetalOreBlock : settings.spaceStoneBlock;
    default:
        return settings.spaceStoneBlock;
    }
}

BlockStateId materialForPlanetBlock(std::uint32_t blockHash,
                                    float shell,
                                    float normalY,
                                    float biomeNoise,
                                    const SpaceFeature& feature,
                                    const NoiseTerrainSettings& settings) noexcept
{
    const std::uint32_t roll = blockHash % 100U;
    const bool surfaceShell = shell > 0.965F;
    const float polar = std::abs(normalY);
    const bool polarCap = polar > 0.72F - feature.oceanCoverage * 0.10F;
    const bool basin = biomeNoise < (-0.10F + feature.oceanCoverage * 0.42F);
    switch (feature.bodyClass) {
    case SpaceBodyClass::Frozen:
        if (surfaceShell || polarCap || roll < 34U) {
            return settings.iceBlock;
        }
        return roll < 42U ? settings.spaceIceBlock : settings.stoneBlock;
    case SpaceBodyClass::MetalRich:
        return roll < static_cast<std::uint32_t>(18U + feature.resourceRichness * 20.0F)
            ? settings.spaceMetalOreBlock
            : settings.stoneBlock;
    case SpaceBodyClass::Volcanic:
        if (surfaceShell || biomeNoise > 0.28F || roll < 70U) {
            return settings.basaltBlock;
        }
        return roll < 82U ? settings.spaceMetalOreBlock : settings.stoneBlock;
    case SpaceBodyClass::Toxic:
        if (surfaceShell) {
            return biomeNoise > 0.1F || roll < 50U ? settings.terracottaBlock : settings.redSandBlock;
        }
        return roll < 10U ? settings.spaceMetalOreBlock : settings.stoneBlock;
    case SpaceBodyClass::Ocean:
        if (surfaceShell) {
            if (polarCap) {
                return settings.iceBlock;
            }
            return basin ? settings.gravelBlock : settings.sandBlock;
        }
        return roll < 8U ? settings.spaceMetalOreBlock : settings.stoneBlock;
    case SpaceBodyClass::LifeBearing:
        if (surfaceShell) {
            if (polarCap && biomeNoise < 0.25F) {
                return settings.snowBlock;
            }
            if (basin) {
                return roll < 45U ? settings.sandBlock : settings.gravelBlock;
            }
            return biomeNoise > 0.42F ? settings.podzolBlock : settings.grassBlock;
        }
        return roll < 5U ? settings.spaceCrystalBlock : settings.stoneBlock;
    case SpaceBodyClass::Crystal:
        return (roll < static_cast<std::uint32_t>(10U + feature.resourceRichness * 24.0F)
                || (surfaceShell && (roll < 45U || biomeNoise > 0.34F)))
            ? settings.spaceCrystalBlock
            : settings.stoneBlock;
    case SpaceBodyClass::DeadRocky:
        if (surfaceShell) {
            return biomeNoise > 0.35F || roll < 55U ? settings.gravelBlock : settings.stoneBlock;
        }
        return roll < 6U ? settings.spaceMetalOreBlock : settings.stoneBlock;
    default:
        return settings.stoneBlock;
    }
}

int rockCountForFeature(SpaceFeatureType type) noexcept
{
    switch (type) {
    case SpaceFeatureType::AsteroidCluster: return 11;
    case SpaceFeatureType::IceField: return 9;
    case SpaceFeatureType::MetalRichAsteroids: return 8;
    case SpaceFeatureType::CrystalAsteroids: return 7;
    case SpaceFeatureType::RingDebris: return 14;
    case SpaceFeatureType::Comet: return 5;
    default: return 0;
    }
}

BlockStateId materialForMoonBlock(std::uint32_t blockHash,
                                  float shell,
                                  float iceBias,
                                  const SpaceFeature& feature,
                                  const NoiseTerrainSettings& settings) noexcept
{
    const std::uint32_t roll = blockHash % 100U;
    const auto bodyClass = feature.bodyClass;
    const auto resourceBias = static_cast<std::uint32_t>(feature.resourceRichness * 16.0F);
    if (bodyClass == SpaceBodyClass::Frozen && roll < 58U) {
        return settings.spaceIceBlock;
    }
    if (bodyClass == SpaceBodyClass::MetalRich && roll < 28U + resourceBias) {
        return settings.spaceMetalOreBlock;
    }
    if (bodyClass == SpaceBodyClass::Crystal && (roll < 20U + resourceBias || shell > 0.76F)) {
        return settings.spaceCrystalBlock;
    }
    if (bodyClass == SpaceBodyClass::Volcanic && roll < 18U + (resourceBias / 2U)) {
        return settings.spaceMetalOreBlock;
    }
    if (shell > 0.82F && roll < 8U) {
        return settings.spaceMetalOreBlock;
    }
    if (iceBias > 0.58F && roll < 32U) {
        return settings.spaceIceBlock;
    }
    if (roll < 3U) {
        return settings.spaceCrystalBlock;
    }
    if (roll < 10U) {
        return settings.spaceMetalOreBlock;
    }
    return settings.spaceStoneBlock;
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

void NoiseTerrainGenerator::setTerrainDefinitions(const TerrainDefinitionRegistry* terrainDefinitions) noexcept
{
    terrainDefinitions_ = terrainDefinitions;
}

std::uint64_t NoiseTerrainGenerator::terrainVersion() const noexcept
{
    // Bump the base magic when the generation algorithm itself changes (not
    // just when settings vary). This invalidates the prepass cache for old
    // chunks so they regenerate with the new heights/painter.
    //   0x1d6f8b2d4c3a0197 — Phase 0/1 (single-pass + biome blocks)
    //   0x2a8e91f6b502d8b3 — Phase 2 (blended surface-Y across regimes)
    //   0x35c11e4a8f976ba1 — Phase 3 (multi-layer caves: cheese+spaghetti+ravine)
    //   0x4ed7f23b91c8a705 — Phase 4 (river carving)
    //   0x5a8c103e7d29f4b6 — Phase 5 (foliage / trees)
    //   0x68f3a5d10b9c277e — Phase 6 (region-grid structures)
    //   0x71b29a4c5e83fd92 — Phase 7 (density-based overhangs)
    //   0x7d3a8f6c10e92541 — Phase 7.1 (smooth amplitude from signals, not biome)
    //   0x84d2f519ab70c631 — Space B/C (landable planet voxel bodies)
    //   0x8fa6d43027b91ce5 — Space B/C profile-driven moon/planet surfaces
    // ATGS v1.1: data-driven height profiles shape live surface heights.
    // ATGS v1.2: mountain profile easing / foothill blending.
    std::uint64_t version = 0xb2e9c4a7d5086f13ULL;
    version = hashCombine(version, floatBits(settings_.minWorldY));
    version = hashCombine(version, floatBits(settings_.maxWorldY));
    version = hashCombine(version, floatBits(settings_.surfaceFrequency));
    version = hashCombine(version, floatBits(settings_.surfaceAmplitude));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.surfaceOctaves));
    version = hashCombine(version, floatBits(settings_.seaLevel));
    version = hashCombine(version, floatBits(settings_.continentFrequency));
    version = hashCombine(version, floatBits(settings_.tectonicFrequency));
    version = hashCombine(version, floatBits(settings_.erosionFrequency));
    version = hashCombine(version, floatBits(settings_.peaksFrequency));
    version = hashCombine(version, floatBits(settings_.climateFrequency));
    version = hashCombine(version, floatBits(settings_.weirdnessFrequency));
    version = hashCombine(version, floatBits(settings_.volcanismFrequency));
    version = hashCombine(version, floatBits(settings_.manaFrequency));
    version = hashCombine(version, floatBits(settings_.macroWarpFrequency));
    version = hashCombine(version, floatBits(settings_.macroWarpStrength));
    version = hashCombine(version, floatBits(settings_.polarScaleBlocks));
    version = hashCombine(version, floatBits(settings_.deepOceanMinDepth));
    version = hashCombine(version, floatBits(settings_.deepOceanMaxDepth));
    version = hashCombine(version, floatBits(settings_.abyssMinDepth));
    version = hashCombine(version, floatBits(settings_.abyssMaxDepth));
    version = hashCombine(version, floatBits(settings_.shelfMinDepth));
    version = hashCombine(version, floatBits(settings_.shelfMaxDepth));
    version = hashCombine(version, floatBits(settings_.mountainBoost));
    version = hashCombine(version, floatBits(settings_.hillBoost));
    version = hashCombine(version, floatBits(settings_.plateauHeight));
    version = hashCombine(version, floatBits(settings_.caveFrequency));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.caveOctaves));
    version = hashCombine(version, floatBits(settings_.caveThreshold));
    version = hashCombine(version, floatBits(settings_.caveCeilingOffset));
    // Phase 3 multi-layer caves.
    version = hashCombine(version, floatBits(settings_.spaghettiFrequency));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.spaghettiOctaves));
    version = hashCombine(version, floatBits(settings_.spaghettiBandHalfWidth));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.spaghettiMinDepth));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.spaghettiMaxDepth));
    version = hashCombine(version, floatBits(settings_.ravineFrequency));
    version = hashCombine(version, floatBits(settings_.ravineBandHalfWidth));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.ravineMinDepth));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.ravineMaxDepth));
    // Phase 4 river settings.
    version = hashCombine(version, floatBits(settings_.riverFrequency));
    version = hashCombine(version, floatBits(settings_.riverBandHalfWidth));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.riverMaxDepth));
    version = hashCombine(version, floatBits(settings_.riverHumidityMin));
    version = hashCombine(version, floatBits(settings_.riverErosionMax));
    version = hashCombine(version, floatBits(settings_.riverWeirdnessAbsMax));
    // Phase 7 overhang settings.
    version = hashCombine(version, floatBits(settings_.overhangFrequency));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.overhangOctaves));
    version = hashCombine(version, floatBits(settings_.overhangAmplitude));
    version = hashCombine(version, floatBits(settings_.overhangBaseAmp));
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
    // Phase 1 biome surface block IDs. Hashing them ensures the prepass cache
    // invalidates if these get reassigned (e.g., dynamic registry remapping).
    version = hashCombine(version, settings_.sandBlock.value);
    version = hashCombine(version, settings_.sandstoneBlock.value);
    version = hashCombine(version, settings_.snowBlock.value);
    version = hashCombine(version, settings_.redSandBlock.value);
    version = hashCombine(version, settings_.terracottaBlock.value);
    version = hashCombine(version, settings_.gravelBlock.value);
    version = hashCombine(version, settings_.basaltBlock.value);
    version = hashCombine(version, settings_.podzolBlock.value);
    version = hashCombine(version, settings_.mossyStoneBlock.value);
    version = hashCombine(version, settings_.iceBlock.value);
    // Phase 5 foliage blocks.
    version = hashCombine(version, settings_.oakLogBlock.value);
    version = hashCombine(version, settings_.leavesBlock.value);
    // Revised Space Phase A settings.
    version = hashCombine(version, settings_.enableSpaceAsteroids ? 1ULL : 0ULL);
    version = hashCombine(version, floatBits(settings_.space.atmosphereTopY));
    version = hashCombine(version, floatBits(settings_.space.nearSpaceStartY));
    version = hashCombine(version, floatBits(settings_.space.gravityFalloffStartY));
    version = hashCombine(version, floatBits(settings_.space.zeroGravityY));
    version = hashCombine(version, static_cast<std::uint64_t>(settings_.space.sectorSizeBlocks));
    version = hashCombine(version, settings_.space.seed);
    version = hashCombine(version, settings_.spaceStoneBlock.value);
    version = hashCombine(version, settings_.spaceMetalOreBlock.value);
    version = hashCombine(version, settings_.spaceIceBlock.value);
    version = hashCombine(version, settings_.spaceCrystalBlock.value);
    version = hashCombine(version, terrainDefinitions_ != nullptr ? terrainDefinitions_->contentHash() : 0ULL);
    return version;
}

ColumnWorldgenData NoiseTerrainGenerator::sampleColumnAt(float worldX, float worldZ) const
{
    return terrain_shape::buildColumnData(worldX, worldZ, settings_, terrainDefinitions_);
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
    std::array<terrain_shape::WorldShapeSignals, kWorldShapeGrid * kWorldShapeGrid> signalGrid{};
    const auto signalIndex = [](int x, int z) constexpr {
        return static_cast<std::size_t>(x + z * kWorldShapeGrid);
    };

    for (int gz = 0; gz < kWorldShapeGrid; ++gz) {
        const float worldZ = worldBaseZ + static_cast<float>(gz * kWorldShapeStep);
        for (int gx = 0; gx < kWorldShapeGrid; ++gx) {
            const float worldX = worldBaseX + static_cast<float>(gx * kWorldShapeStep);
            signalGrid[signalIndex(gx, gz)] = terrain_shape::sampleWorldShapeSignals(worldX, worldZ, settings_);
        }
    }

    for (int z = 0; z < ChunkSize; ++z) {
        const int gz = std::clamp(z / kWorldShapeStep, 0, kWorldShapeGrid - 2);
        const float tz = static_cast<float>(z - (gz * kWorldShapeStep)) / static_cast<float>(kWorldShapeStep);
        for (int x = 0; x < ChunkSize; ++x) {
            const int gx = std::clamp(x / kWorldShapeStep, 0, kWorldShapeGrid - 2);
            const float tx = static_cast<float>(x - (gx * kWorldShapeStep)) / static_cast<float>(kWorldShapeStep);
            const auto x0 = terrain_shape::lerpSignals(signalGrid[signalIndex(gx, gz)], signalGrid[signalIndex(gx + 1, gz)], tx);
            const auto x1 = terrain_shape::lerpSignals(signalGrid[signalIndex(gx, gz + 1)], signalGrid[signalIndex(gx + 1, gz + 1)], tx);
            const auto data = terrain_shape::buildColumnDataFromSignals(
                terrain_shape::lerpSignals(x0, x1, tz), settings_, terrainDefinitions_);
            const auto idx = columnIndex(x, z);
            prepass.surfaceY[idx] = static_cast<float>(data.surfaceY);
            prepass.surfaceBlockY[idx] = data.surfaceY;
            prepass.surfaceKind[idx] = data.surfaceKind;
            prepass.biome[idx] = data.biome;
            prepass.terrainClass[idx] = data.terrainClass;
            prepass.continentalness[idx] = data.continentalness;
            prepass.tectonicStress[idx] = data.tectonicStress;
            prepass.erosion[idx] = data.erosion;
            prepass.peaksValleys[idx] = data.peaksValleys;
            prepass.temperature[idx] = data.temperature;
            prepass.humidity[idx] = data.humidity;
            prepass.weirdness[idx] = data.weirdness;
            prepass.volcanism[idx] = data.volcanism;
            prepass.manaField[idx] = data.manaField;
            prepass.polarInfluence[idx] = data.polarInfluence;
            prepass.oceanDepthBias[idx] = data.oceanDepthBias;
            prepass.oceanDepth[idx] = data.oceanDepth;
            prepass.biomeBlend[idx] = data.biomeBlend;
            prepass.seaMask[idx] = data.surfaceY < data.seaLevel;
            prepass.beachMask[idx] = data.isBeach;
            prepass.riverCandidateMask[idx] = data.isRiverCandidate;
        }
    }

    return prepass;
}

TerrainColumnPrepass NoiseTerrainGenerator::resolvePrepass(TerrainColumnCoord columnCoord, TerrainGenerationMode& mode)
{
    mode = TerrainGenerationMode::Direct;

    if (prepassCache_) {
        if (auto cached = prepassCache_->find(prepassKey(columnCoord))) {
            mode = TerrainGenerationMode::CachedPrepass;
            return std::move(*cached);
        }

        auto prepass = buildColumnPrepass(columnCoord);
        prepassCache_->insert(prepass);
        return prepass;
    }

    return buildColumnPrepass(columnCoord);
}

bool NoiseTerrainGenerator::generateSpaceChunk(Chunk& chunk)
{
    LastGenerationMode = TerrainGenerationMode::Direct;
    const auto coord = chunk.coord();
    const int chunkBaseX = static_cast<int>(coord.x * ChunkSize);
    const int chunkBaseY = static_cast<int>(coord.y * ChunkSize);
    const int chunkBaseZ = static_cast<int>(coord.z * ChunkSize);
    const int chunkTopY = chunkBaseY + ChunkSize - 1;
    if (!settings_.enableSpaceAsteroids || static_cast<float>(chunkTopY) < settings_.space.atmosphereTopY) {
        return false;
    }

    SpaceEnvironment space(settings_.space);
    const int sectorSize = std::max(settings_.space.sectorSizeBlocks, 128);
    constexpr float kMaxRockRadius = 96.0F;
    constexpr float kMaxMoonRadius = 820.0F;
    constexpr float kMaxPlanetRadius = 3800.0F;
    const float searchPadding = std::max({520.0F + kMaxRockRadius, kMaxMoonRadius, kMaxPlanetRadius});
    const auto minSector = space.sectorFor({
        static_cast<float>(chunkBaseX) - searchPadding,
        static_cast<float>(chunkBaseY) - searchPadding,
        static_cast<float>(chunkBaseZ) - searchPadding
    });
    const auto maxSector = space.sectorFor({
        static_cast<float>(chunkBaseX + ChunkSize - 1) + searchPadding,
        static_cast<float>(chunkBaseY + ChunkSize - 1) + searchPadding,
        static_cast<float>(chunkBaseZ + ChunkSize - 1) + searchPadding
    });

    for (std::int64_t sy = minSector.y; sy <= maxSector.y; ++sy) {
        for (std::int64_t sz = minSector.z; sz <= maxSector.z; ++sz) {
            for (std::int64_t sx = minSector.x; sx <= maxSector.x; ++sx) {
                const SpaceSectorCoord sector{sx, sy, sz};
                for (const auto& feature : space.featuresForSector(sector)) {
                    if (!solidSpaceFeature(feature.type) && !voxelMoonFeature(feature.type) && !voxelPlanetFeature(feature)) {
                        continue;
                    }

                    const auto featureCenter = space.featureWorldCenter(feature);
                    if (voxelPlanetFeature(feature)) {
                        const float radius = feature.radius;
                        if (featureCenter.x + radius < static_cast<float>(chunkBaseX)
                            || featureCenter.x - radius > static_cast<float>(chunkBaseX + ChunkSize - 1)
                            || featureCenter.y + radius < static_cast<float>(chunkBaseY)
                            || featureCenter.y - radius > static_cast<float>(chunkBaseY + ChunkSize - 1)
                            || featureCenter.z + radius < static_cast<float>(chunkBaseZ)
                            || featureCenter.z - radius > static_cast<float>(chunkBaseZ + ChunkSize - 1)) {
                            continue;
                        }

                        const int minX = std::clamp(static_cast<int>(std::floor(featureCenter.x - radius)) - chunkBaseX, 0, ChunkSize - 1);
                        const int maxX = std::clamp(static_cast<int>(std::ceil(featureCenter.x + radius)) - chunkBaseX, 0, ChunkSize - 1);
                        const int minY = std::clamp(static_cast<int>(std::floor(featureCenter.y - radius)) - chunkBaseY, 0, ChunkSize - 1);
                        const int maxY = std::clamp(static_cast<int>(std::ceil(featureCenter.y + radius)) - chunkBaseY, 0, ChunkSize - 1);
                        const int minZ = std::clamp(static_cast<int>(std::floor(featureCenter.z - radius)) - chunkBaseZ, 0, ChunkSize - 1);
                        const int maxZ = std::clamp(static_cast<int>(std::ceil(featureCenter.z + radius)) - chunkBaseZ, 0, ChunkSize - 1);
                        const auto planetSeed = feature.resourceSeed ^ 0x9A7E51D3U;
                        const float roughAmplitude = 0.012F + feature.surfaceRoughness * 0.028F;
                        for (int z = minZ; z <= maxZ; ++z) {
                            const float wz = static_cast<float>(chunkBaseZ + z);
                            const float dz = wz - featureCenter.z;
                            for (int y = minY; y <= maxY; ++y) {
                                const float wy = static_cast<float>(chunkBaseY + y);
                                const float dy = wy - featureCenter.y;
                                for (int x = minX; x <= maxX; ++x) {
                                    const float wx = static_cast<float>(chunkBaseX + x);
                                    const float dx = wx - featureCenter.x;
                                    const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                                    const float shell = distance / std::max(radius, 1.0F);
                                    if (shell > 1.08F) {
                                        continue;
                                    }

                                    const float surfaceNoise = core::fbm3D(
                                        wx * 0.0045F,
                                        wy * 0.0045F,
                                        wz * 0.0045F,
                                        planetSeed, 5, 2.0F, 0.5F) * 2.0F - 1.0F;
                                    const float localRadius = radius * (1.0F + surfaceNoise * roughAmplitude);
                                    if (distance > localRadius) {
                                        continue;
                                    }

                                    const float caveNoise = core::fbm3D(
                                        wx * 0.018F,
                                        wy * 0.018F,
                                        wz * 0.018F,
                                        planetSeed ^ 0xC4A7B391U, 3, 2.0F, 0.5F);
                                    if (shell > 0.82F && shell < 0.985F && caveNoise > 0.91F - feature.surfaceRoughness * 0.06F) {
                                        continue;
                                    }

                                    const float biomeNoise = core::fbm3D(
                                        wx * 0.0018F,
                                        wy * 0.0018F,
                                        wz * 0.0018F,
                                        planetSeed ^ 0x5EEDB10DU, 4, 2.0F, 0.5F) * 2.0F - 1.0F;
                                    const float normalY = dy / std::max(distance, 1.0F);
                                    const auto blockHash = core::hash3D(
                                        chunkBaseX + x,
                                        chunkBaseY + y,
                                        chunkBaseZ + z,
                                        planetSeed);
                                    chunk.setBlockSilently(
                                        x, y, z,
                                        materialForPlanetBlock(blockHash, shell, normalY, biomeNoise, feature, settings_));
                                }
                            }
                        }
                        continue;
                    }

                    if (voxelMoonFeature(feature.type)) {
                        const float radius = feature.radius;
                        if (featureCenter.x + radius < static_cast<float>(chunkBaseX)
                            || featureCenter.x - radius > static_cast<float>(chunkBaseX + ChunkSize - 1)
                            || featureCenter.y + radius < static_cast<float>(chunkBaseY)
                            || featureCenter.y - radius > static_cast<float>(chunkBaseY + ChunkSize - 1)
                            || featureCenter.z + radius < static_cast<float>(chunkBaseZ)
                            || featureCenter.z - radius > static_cast<float>(chunkBaseZ + ChunkSize - 1)) {
                            continue;
                        }

                        const int minX = std::clamp(static_cast<int>(std::floor(featureCenter.x - radius)) - chunkBaseX, 0, ChunkSize - 1);
                        const int maxX = std::clamp(static_cast<int>(std::ceil(featureCenter.x + radius)) - chunkBaseX, 0, ChunkSize - 1);
                        const int minY = std::clamp(static_cast<int>(std::floor(featureCenter.y - radius)) - chunkBaseY, 0, ChunkSize - 1);
                        const int maxY = std::clamp(static_cast<int>(std::ceil(featureCenter.y + radius)) - chunkBaseY, 0, ChunkSize - 1);
                        const int minZ = std::clamp(static_cast<int>(std::floor(featureCenter.z - radius)) - chunkBaseZ, 0, ChunkSize - 1);
                        const int maxZ = std::clamp(static_cast<int>(std::ceil(featureCenter.z + radius)) - chunkBaseZ, 0, ChunkSize - 1);
                        const auto moonSeed = feature.resourceSeed ^ 0x4D6F6F6EU;
                        const float moonRoughAmplitude = 0.08F + feature.surfaceRoughness * 0.12F;
                        const float craterThreshold = 0.84F - feature.surfaceRoughness * 0.12F;
                        const float caveThreshold = 0.91F - feature.surfaceRoughness * 0.08F;
                        for (int z = minZ; z <= maxZ; ++z) {
                            const float wz = static_cast<float>(chunkBaseZ + z);
                            const float dz = wz - featureCenter.z;
                            for (int y = minY; y <= maxY; ++y) {
                                const float wy = static_cast<float>(chunkBaseY + y);
                                const float dy = wy - featureCenter.y;
                                for (int x = minX; x <= maxX; ++x) {
                                    const float wx = static_cast<float>(chunkBaseX + x);
                                    const float dx = wx - featureCenter.x;
                                    const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                                    const float shell = distance / std::max(radius, 1.0F);
                                    const float surfaceNoise = core::fbm3D(
                                        wx * 0.018F,
                                        wy * 0.018F,
                                        wz * 0.018F,
                                        moonSeed, 4, 2.0F, 0.5F);
                                    const float localRadius = radius * (0.90F + surfaceNoise * moonRoughAmplitude);
                                    if (distance > localRadius) {
                                        continue;
                                    }

                                    const float craterNoise = core::fbm3D(
                                        wx * 0.011F,
                                        wy * 0.011F,
                                        wz * 0.011F,
                                        moonSeed ^ 0xC2A53A7BU, 3, 2.0F, 0.55F);
                                    if (shell > 0.72F && craterNoise > craterThreshold) {
                                        continue;
                                    }

                                    const float caveNoise = core::fbm3D(
                                        wx * 0.026F,
                                        wy * 0.026F,
                                        wz * 0.026F,
                                        moonSeed ^ 0xB17B0A11U, 3, 2.0F, 0.5F);
                                    if (shell < 0.74F && caveNoise > caveThreshold) {
                                        continue;
                                    }

                                    const float iceBias = core::fbm3D(
                                        wx * 0.006F,
                                        wy * 0.006F,
                                        wz * 0.006F,
                                        moonSeed ^ 0x1CEB10C5U, 2, 2.0F, 0.5F);
                                    const auto blockHash = core::hash3D(
                                        chunkBaseX + x,
                                        chunkBaseY + y,
                                        chunkBaseZ + z,
                                        moonSeed);
                                    chunk.setBlockSilently(
                                        x, y, z,
                                        materialForMoonBlock(blockHash, shell, iceBias, feature, settings_));
                                }
                            }
                        }
                        continue;
                    }

                    std::uint64_t featureRng = mixSpaceBits(
                        (static_cast<std::uint64_t>(feature.resourceSeed) << 32U)
                        ^ static_cast<std::uint64_t>(sectorSize)
                        ^ settings_.space.seed);
                    const int rockCount = rockCountForFeature(feature.type);
                    for (int rock = 0; rock < rockCount; ++rock) {
                        std::uint64_t rockRng = mixSpaceBits(featureRng + static_cast<std::uint64_t>(rock) * 0x9e3779b97f4a7c15ULL);
                        const float spread = feature.radius * (feature.type == SpaceFeatureType::RingDebris ? 1.15F : 0.72F);
                        const bool anchorRock = rock == 0;
                        const core::Vec3 center{
                            featureCenter.x + (anchorRock ? 0.0F : (spaceUnitFloat(rockRng) - 0.5F) * spread),
                            featureCenter.y + (anchorRock ? 0.0F : (spaceUnitFloat(rockRng) - 0.5F) * spread * 0.55F),
                            featureCenter.z + (anchorRock ? 0.0F : (spaceUnitFloat(rockRng) - 0.5F) * spread)
                        };
                        const float baseRadius = (anchorRock ? 42.0F : (feature.type == SpaceFeatureType::Comet ? 28.0F : 16.0F))
                            + spaceUnitFloat(rockRng) * (feature.type == SpaceFeatureType::RingDebris ? 22.0F : 42.0F);
                        const float rx = baseRadius * (0.75F + spaceUnitFloat(rockRng) * 0.85F);
                        const float ry = baseRadius * (0.55F + spaceUnitFloat(rockRng) * 0.65F);
                        const float rz = baseRadius * (0.75F + spaceUnitFloat(rockRng) * 0.85F);
                        const float maxRadius = std::max({rx, ry, rz});

                        if (center.x + maxRadius < static_cast<float>(chunkBaseX)
                            || center.x - maxRadius > static_cast<float>(chunkBaseX + ChunkSize - 1)
                            || center.y + maxRadius < static_cast<float>(chunkBaseY)
                            || center.y - maxRadius > static_cast<float>(chunkBaseY + ChunkSize - 1)
                            || center.z + maxRadius < static_cast<float>(chunkBaseZ)
                            || center.z - maxRadius > static_cast<float>(chunkBaseZ + ChunkSize - 1)) {
                            continue;
                        }

                        const int minX = std::clamp(static_cast<int>(std::floor(center.x - maxRadius)) - chunkBaseX, 0, ChunkSize - 1);
                        const int maxX = std::clamp(static_cast<int>(std::ceil(center.x + maxRadius)) - chunkBaseX, 0, ChunkSize - 1);
                        const int minY = std::clamp(static_cast<int>(std::floor(center.y - maxRadius)) - chunkBaseY, 0, ChunkSize - 1);
                        const int maxY = std::clamp(static_cast<int>(std::ceil(center.y + maxRadius)) - chunkBaseY, 0, ChunkSize - 1);
                        const int minZ = std::clamp(static_cast<int>(std::floor(center.z - maxRadius)) - chunkBaseZ, 0, ChunkSize - 1);
                        const int maxZ = std::clamp(static_cast<int>(std::ceil(center.z + maxRadius)) - chunkBaseZ, 0, ChunkSize - 1);
                        const auto rockSeed = static_cast<std::uint32_t>(mixSpaceBits(rockRng) & 0xFFFFFFFFU);
                        for (int z = minZ; z <= maxZ; ++z) {
                            const float wz = static_cast<float>(chunkBaseZ + z);
                            const float nz = (wz - center.z) / rz;
                            for (int y = minY; y <= maxY; ++y) {
                                const float wy = static_cast<float>(chunkBaseY + y);
                                const float ny = (wy - center.y) / ry;
                                for (int x = minX; x <= maxX; ++x) {
                                    const float wx = static_cast<float>(chunkBaseX + x);
                                    const float nx = (wx - center.x) / rx;
                                    const float shell = (nx * nx) + (ny * ny) + (nz * nz);
                                    if (shell > 1.22F) {
                                        continue;
                                    }

                                    const float roughness = core::fbm3D(
                                        wx * 0.055F,
                                        wy * 0.055F,
                                        wz * 0.055F,
                                        rockSeed, 2, 2.0F, 0.5F);
                                    if (shell > 0.72F + roughness * 0.38F) {
                                        continue;
                                    }
                                    const float cavernNoise = core::fbm3D(
                                        wx * 0.032F,
                                        wy * 0.032F,
                                        wz * 0.032F,
                                        rockSeed ^ 0x5B6D2F17U, 3, 2.0F, 0.52F);
                                    const bool cavernPocket = anchorRock
                                        && shell < 0.58F
                                        && cavernNoise > (feature.type == SpaceFeatureType::AsteroidCluster ? 0.70F : 0.76F);
                                    if ((shell < 0.26F && roughness > 0.83F) || cavernPocket) {
                                        continue;
                                    }

                                    const auto blockHash = core::hash3D(
                                        chunkBaseX + x,
                                        chunkBaseY + y,
                                        chunkBaseZ + z,
                                        rockSeed ^ feature.resourceSeed);
                                    chunk.setBlockSilently(
                                        x, y, z,
                                        materialForSpaceBlock(feature.type, blockHash, shell, settings_));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    chunk.markGenerated();
    chunk.setTerrainVersion(terrainVersion());
    return true;
}

void NoiseTerrainGenerator::generateWithPrepass(Chunk& chunk, const TerrainColumnPrepass& prepass, TerrainGenerationMode mode)
{
    if (generateSpaceChunk(chunk)) {
        return;
    }

    LastGenerationMode = mode;
    const auto chunkCoord = chunk.coord();

    // The pipeline owns bounds + density-grid sampling + per-stage work.
    // Each stage is a free function in TerrainPipeline; new stages (foliage,
    // structures, overhang density) slot in by adding one call here.
    const auto bounds = computeTerrainBounds(chunk, prepass, settings_);
    const int chunkBaseBlockY = static_cast<int>(chunkCoord.y * ChunkSize);

    // Early-exit: nothing to fill above OR below this chunk's vertical slab.
    if (chunkBaseBlockY > bounds.maxSurfaceBlockY && bounds.seaTopForChunk < 0) {
        chunk.markGenerated();
        chunk.setTerrainVersion(terrainVersion());
        return;
    }

    const float worldBaseX = static_cast<float>(chunkCoord.x * ChunkSize);
    const float worldBaseY = static_cast<float>(chunkCoord.y * ChunkSize);
    const float worldBaseZ = static_cast<float>(chunkCoord.z * ChunkSize);

    TerrainDensityField density{};
    sampleCaveDensityField(density, bounds, settings_, worldBaseX, worldBaseY, worldBaseZ);
    sampleOreDensityFields(density, bounds, settings_, worldBaseX, worldBaseY, worldBaseZ);
    // Phase 7: overhang noise. We only need to sample if at least one column
    // in this chunk has a non-zero amplitude biome. Cheap conservative test:
    // sample if any column is land (oceans get zero amplitude).
    bool needOverhang = false;
    if (settings_.overhangAmplitude > 0.0F && bounds.maxSolidTop >= 0) {
        // Without a faster check, just sample whenever there's stone in the
        // chunk. The runBaseTerrainStage path falls back to a fast batch-fill
        // for columns whose biome amplitude is 0, so an unused sample is cheap.
        needOverhang = true;
    }
    if (needOverhang) {
        sampleOverhangDensityField(density, settings_, worldBaseX, worldBaseY, worldBaseZ);
    }

    ActualTopMap actualTop{};
    TerrainPipelineContext ctx{
        chunk, prepass, settings_, bounds, density,
        chunkBaseBlockY,
        worldBaseX, worldBaseY, worldBaseZ,
        this,             // Phase 5: generator pointer for cross-chunk lookups.
        &actualTop,       // Phase 7: per-column actual top after overhang carving.
    };

    runBaseTerrainStage(ctx);
    runSurfacePaintStage(ctx);
    runCaveCarveStage(ctx);
    runOreStage(ctx);
    runFluidStage(ctx);
    runStructureStage(ctx);
    runFoliageStage(ctx);

    chunk.markGenerated();
    chunk.setTerrainVersion(terrainVersion());
}

void NoiseTerrainGenerator::generate(Chunk& chunk)
{
    if (generateSpaceChunk(chunk)) {
        return;
    }

    const auto chunkCoord = chunk.coord();
    const TerrainColumnCoord columnCoord{chunkCoord.x, chunkCoord.z};
    TerrainGenerationMode mode = TerrainGenerationMode::Direct;
    const auto prepass = resolvePrepass(columnCoord, mode);
    generateWithPrepass(chunk, prepass, mode);
}

void NoiseTerrainGenerator::generateColumn(std::vector<Chunk>& chunks, std::vector<TerrainGenerationMode>& modes)
{
    modes.clear();
    modes.reserve(chunks.size());
    if (chunks.empty()) {
        return;
    }

    std::sort(chunks.begin(), chunks.end(), [](const Chunk& lhs, const Chunk& rhs) {
        return lhs.coord().y < rhs.coord().y;
    });

    TerrainGenerationMode mode = TerrainGenerationMode::Direct;
    TerrainColumnCoord columnCoord{};
    TerrainColumnPrepass prepass{};
    bool havePrepass = false;

    for (auto& chunk : chunks) {
        if (generateSpaceChunk(chunk)) {
            modes.push_back(TerrainGenerationMode::Direct);
            continue;
        }

        if (!havePrepass) {
            const auto baseCoord = chunk.coord();
            columnCoord = {baseCoord.x, baseCoord.z};
            prepass = resolvePrepass(columnCoord, mode);
            havePrepass = true;
        }
        generateWithPrepass(chunk, prepass, mode);
        modes.push_back(mode);
    }
}

} // namespace voxel::world
