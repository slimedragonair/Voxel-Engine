#include <voxel/world/NoiseTerrainGenerator.hpp>

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

struct WorldShapeSignals {
    float continentalness{};
    float tectonicStress{};
    float erosion{};
    float peaksValleys{};
    float temperature{};
    float humidity{};
    float weirdness{};
    float volcanism{};
    float manaField{};
    float polarInfluence{};
    float oceanDepthBias{};
    float detail{};
    // Phase 4: ridged 2D noise for river channels. The carve band is the
    // set of (x,z) where |riverRidge - 0.5| < riverBandHalfWidth, which
    // forms continuous meandering 1D curves through the world.
    float riverRidge{};
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

float smoothRemap(float value, float inMin, float inMax, float outMin, float outMax) noexcept
{
    const float t = smooth01((value - inMin) / (inMax - inMin));
    return outMin + ((outMax - outMin) * t);
}

WorldShapeSignals sampleWorldShapeSignals(float worldX, float worldZ, const NoiseTerrainSettings& settings) noexcept
{
    WorldShapeSignals signals{};
    const float warpX = (core::fbm2D(
        worldX * settings.macroWarpFrequency,
        worldZ * settings.macroWarpFrequency,
        settings.seed ^ 0xC13FA9A1U, 4, 2.0F, 0.52F) * 2.0F - 1.0F) * settings.macroWarpStrength;
    const float warpZ = (core::fbm2D(
        (worldX + 1931.0F) * settings.macroWarpFrequency,
        (worldZ - 7217.0F) * settings.macroWarpFrequency,
        settings.seed ^ 0x9F41D2B7U, 4, 2.0F, 0.52F) * 2.0F - 1.0F) * settings.macroWarpStrength;
    const float wx = worldX + warpX;
    const float wz = worldZ + warpZ;

    signals.continentalness = std::clamp((core::fbm2D(
        wx * settings.continentFrequency,
        wz * settings.continentFrequency,
        settings.seed ^ 0xA511E9B3U, 4, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.85F, -1.0F, 1.0F);
    const float plateA = core::fbm2D(
        wx * settings.tectonicFrequency,
        wz * settings.tectonicFrequency,
        settings.seed ^ 0x5EEDC0DEU, 4, 2.0F, 0.5F) * 2.0F - 1.0F;
    const float plateB = core::fbm2D(
        (wx + 11731.0F) * settings.tectonicFrequency * 1.27F,
        (wz - 4819.0F) * settings.tectonicFrequency * 0.83F,
        settings.seed ^ 0x8A2F14D3U, 3, 2.0F, 0.55F) * 2.0F - 1.0F;
    const float plateBoundary = 1.0F - std::min(1.0F, std::abs(plateA - plateB));
    signals.tectonicStress = smooth01(std::clamp((plateBoundary - 0.18F) / 0.72F, 0.0F, 1.0F));
    signals.erosion = std::clamp((core::fbm2D(
        wx * settings.erosionFrequency,
        wz * settings.erosionFrequency,
        settings.seed ^ 0x6C8E9CF5U, 3, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.4F, -1.0F, 1.0F);
    signals.peaksValleys = std::clamp((core::fbm2D(
        wx * settings.peaksFrequency,
        wz * settings.peaksFrequency,
        settings.seed ^ 0xB5297A4DU, 4, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.5F, -1.0F, 1.0F);
    signals.temperature = std::clamp((core::fbm2D(
        wx * settings.climateFrequency,
        wz * settings.climateFrequency,
        settings.seed ^ 0x1F123BB5U, 3, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.35F, -1.0F, 1.0F);
    signals.humidity = std::clamp((core::fbm2D(
        wx * settings.climateFrequency,
        wz * settings.climateFrequency,
        settings.seed ^ 0x7D2B4A91U, 3, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.35F, -1.0F, 1.0F);
    signals.weirdness = std::clamp((core::fbm2D(
        wx * settings.weirdnessFrequency,
        wz * settings.weirdnessFrequency,
        settings.seed ^ 0xDD4B2F27U, 3, 2.0F, 0.5F) * 2.0F - 1.0F) * 1.45F, -1.0F, 1.0F);
    signals.volcanism = std::clamp(core::fbm2D(
        wx * settings.volcanismFrequency,
        wz * settings.volcanismFrequency,
        settings.seed ^ 0xA7713D5BU, 4, 2.0F, 0.5F) * 1.35F + signals.tectonicStress * 0.28F - 0.24F, 0.0F, 1.0F);
    signals.manaField = std::clamp(core::fbm2D(
        wx * settings.manaFrequency,
        wz * settings.manaFrequency,
        settings.seed ^ 0x4A17E7C1U, 4, 2.0F, 0.5F) * 1.30F + std::max(0.0F, signals.weirdness) * 0.24F - 0.18F, 0.0F, 1.0F);
    signals.polarInfluence = smooth01((std::abs(worldZ) - settings.polarScaleBlocks * 0.58F)
        / std::max(1.0F, settings.polarScaleBlocks * 0.28F));
    signals.oceanDepthBias = std::clamp(core::fbm2D(
        wx * settings.continentFrequency * 2.75F,
        wz * settings.continentFrequency * 2.75F,
        settings.seed ^ 0xE0C0A11FU, 3, 2.0F, 0.55F), 0.0F, 1.0F);
    signals.detail = core::fbm2D(
        worldX * settings.surfaceFrequency,
        worldZ * settings.surfaceFrequency,
        settings.seed, settings.surfaceOctaves, 2.0F, 0.5F) * 2.0F - 1.0F;
    // Phase 4: 2D river ridge noise in [0,1]. The carving band is around 0.5,
    // so we keep this raw (not rescaled to [-1,1] like the others).
    signals.riverRidge = core::fbm2D(
        worldX * settings.riverFrequency,
        worldZ * settings.riverFrequency,
        settings.seed ^ 0x3F8D1B27U, 3, 2.0F, 0.5F);
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
        lerp(a.tectonicStress, b.tectonicStress, t),
        lerp(a.erosion, b.erosion, t),
        lerp(a.peaksValleys, b.peaksValleys, t),
        lerp(a.temperature, b.temperature, t),
        lerp(a.humidity, b.humidity, t),
        lerp(a.weirdness, b.weirdness, t),
        lerp(a.volcanism, b.volcanism, t),
        lerp(a.manaField, b.manaField, t),
        lerp(a.polarInfluence, b.polarInfluence, t),
        lerp(a.oceanDepthBias, b.oceanDepthBias, t),
        lerp(a.detail, b.detail, t),
        lerp(a.riverRidge, b.riverRidge, t),
    };
}

[[nodiscard]] TerrainClass classifyTerrainClass(const WorldShapeSignals& s) noexcept
{
    if (s.continentalness < -0.76F || (s.continentalness < -0.55F && s.oceanDepthBias > 0.72F)) {
        return TerrainClass::Abyss;
    }
    if (s.continentalness < -0.42F) {
        return TerrainClass::DeepOcean;
    }
    if (s.continentalness < -0.12F) {
        return TerrainClass::OceanShelf;
    }
    if (s.manaField > 0.82F && s.weirdness > 0.42F) {
        return TerrainClass::ArcaneFracture;
    }
    if (s.polarInfluence > 0.78F || s.temperature < -0.72F) {
        return TerrainClass::Polar;
    }
    if (s.volcanism > 0.78F && s.tectonicStress > 0.48F) {
        return TerrainClass::Volcanic;
    }
    if (s.manaField > 0.68F) {
        return TerrainClass::Magic;
    }
    if (s.tectonicStress > 0.70F && s.erosion < 0.52F) {
        return TerrainClass::Mountains;
    }
    if (s.tectonicStress > 0.52F && s.erosion > 0.22F) {
        return TerrainClass::Plateau;
    }
    if (s.continentalness > 0.42F || s.peaksValleys > 0.42F) {
        return TerrainClass::Highlands;
    }
    return TerrainClass::Plains;
}

[[nodiscard]] TerrainSurfaceKind surfaceKindForClass(TerrainClass terrainClass, float continentalness) noexcept
{
    switch (terrainClass) {
    case TerrainClass::Abyss:
    case TerrainClass::DeepOcean:
        return TerrainSurfaceKind::DeepOcean;
    case TerrainClass::OceanShelf:
        return continentalness < -0.28F ? TerrainSurfaceKind::Ocean : TerrainSurfaceKind::ShallowOcean;
    default:
        break;
    }
    if (continentalness < 0.02F) {
        return TerrainSurfaceKind::Beach;
    }
    return TerrainSurfaceKind::Land;
}

[[nodiscard]] float terrace(float value, float steps, float strength) noexcept
{
    const float stepped = std::floor(value * steps + 0.5F) / steps;
    return lerp(value, stepped, strength);
}

[[nodiscard]] float terrainClassSurfaceY(
    TerrainClass terrainClass,
    const WorldShapeSignals& s,
    const NoiseTerrainSettings& settings) noexcept
{
    const float landT = smooth01(remap(s.continentalness, -0.06F, 0.82F, 0.0F, 1.0F));
    const float detail = std::clamp(s.detail, -1.0F, 1.0F);
    const float amp = std::max(1.0F, settings.surfaceAmplitude);
    const float peak01 = smooth01(s.peaksValleys * 0.5F + 0.5F);
    const float lowErosion = smooth01((-s.erosion * 0.5F + 0.5F));
    switch (terrainClass) {
    case TerrainClass::Abyss: {
        const float trench = smooth01(s.oceanDepthBias) * 0.55F + smooth01(s.tectonicStress) * 0.45F;
        const float depth = lerp(settings.abyssMinDepth, settings.abyssMaxDepth, trench);
        return settings.seaLevel - depth + detail * (amp * 0.13F);
    }
    case TerrainClass::DeepOcean: {
        const float depthT = smooth01(remap(s.continentalness, -0.76F, -0.42F, 1.0F, 0.0F));
        const float depth = lerp(settings.deepOceanMinDepth, settings.deepOceanMaxDepth, depthT * 0.72F + s.oceanDepthBias * 0.28F);
        return settings.seaLevel - depth + detail * (amp * 0.11F);
    }
    case TerrainClass::OceanShelf: {
        const float shelfT = smooth01(remap(s.continentalness, -0.42F, -0.12F, 1.0F, 0.0F));
        const float depth = lerp(settings.shelfMinDepth, settings.shelfMaxDepth, shelfT);
        return settings.seaLevel - depth + detail * (amp * 0.055F);
    }
    case TerrainClass::Mountains: {
        const float stress = smooth01(s.tectonicStress);
        const float jagged = lowErosion * peak01;
        const float ridge = std::pow(std::max(0.0F, s.peaksValleys * 0.5F + 0.5F), 1.6F);
        return settings.seaLevel
            + 170.0F
            + landT * 68.0F
            + stress * settings.mountainBoost * 0.66F
            + jagged * settings.mountainBoost * 0.34F
            + ridge * 48.0F
            + detail * (amp * 0.24F);
    }
    case TerrainClass::Plateau: {
        const float raw = settings.plateauHeight + landT * (amp * 0.50F) + detail * (amp * 0.26F);
        return terrace(raw / 340.0F, 7.0F, 0.58F) * 340.0F;
    }
    case TerrainClass::Highlands:
        return settings.seaLevel + 72.0F + landT * (amp * 1.28F) + peak01 * 78.0F + detail * (amp * 0.30F);
    case TerrainClass::Volcanic:
        return settings.seaLevel + 58.0F + s.volcanism * 190.0F + s.tectonicStress * 84.0F + detail * (amp * 0.33F);
    case TerrainClass::Polar:
        return settings.seaLevel + 22.0F + landT * (amp * 0.89F) + peak01 * 48.0F + detail * (amp * 0.13F);
    case TerrainClass::Magic:
        return settings.seaLevel + 44.0F + landT * amp + s.manaField * 94.0F + s.weirdness * 32.0F + detail * (amp * 0.24F);
    case TerrainClass::ArcaneFracture:
        return settings.seaLevel + 40.0F + landT * (amp * 0.89F) + s.manaField * 130.0F + std::abs(s.weirdness) * 72.0F + detail * (amp * 0.41F);
    case TerrainClass::Plains:
    default:
        return settings.seaLevel + 18.0F + landT * (amp * 0.91F) + detail * (amp * 0.17F) + (1.0F - lowErosion) * 18.0F;
    }
}

[[nodiscard]] float terrainProfileT(
    TerrainClass terrainClass,
    const WorldShapeSignals& s,
    float currentSurfaceY,
    const NoiseTerrainSettings& settings) noexcept
{
    switch (terrainClass) {
    case TerrainClass::Abyss:
    case TerrainClass::DeepOcean:
    case TerrainClass::OceanShelf:
        return smooth01(remap(settings.seaLevel - currentSurfaceY,
            settings.shelfMinDepth, settings.abyssMaxDepth, 0.0F, 1.0F));
    case TerrainClass::Mountains:
        return smooth01(std::max(s.tectonicStress, s.peaksValleys * 0.5F + 0.5F));
    case TerrainClass::Plateau:
        return smooth01(remap(s.continentalness, 0.0F, 0.85F, 0.25F, 0.9F));
    case TerrainClass::Highlands:
        return smooth01(remap(s.continentalness + s.peaksValleys * 0.35F, 0.0F, 1.15F, 0.2F, 0.9F));
    case TerrainClass::Volcanic:
        return smooth01(s.volcanism * 0.65F + s.tectonicStress * 0.35F);
    case TerrainClass::Polar:
        return smooth01(s.polarInfluence * 0.55F + (s.peaksValleys * 0.5F + 0.5F) * 0.45F);
    case TerrainClass::Magic:
    case TerrainClass::ArcaneFracture:
        return smooth01(s.manaField * 0.55F + std::abs(s.weirdness) * 0.45F);
    case TerrainClass::Plains:
    default:
        return smooth01(remap(s.continentalness, 0.0F, 0.8F, 0.15F, 0.8F));
    }
}

[[nodiscard]] float applyHeightProfile(
    float currentSurfaceY,
    TerrainClass terrainClass,
    const TerrainHeightProfileDefinition& profile,
    const WorldShapeSignals& signals,
    const NoiseTerrainSettings& settings) noexcept
{
    const float minY = settings.seaLevel + profile.minHeight;
    const float maxY = settings.seaLevel + profile.maxHeight;
    const float range = std::max(1.0F, maxY - minY);
    const float baseT = terrainProfileT(terrainClass, signals, currentSurfaceY, settings);
    const float detail = std::clamp(signals.detail, -1.0F, 1.0F) * range * profile.detailScale;
    const float ridge = std::pow(std::max(0.0F, signals.peaksValleys * 0.5F + 0.5F), 1.65F)
        * range * profile.ridgeScale * 0.22F;

    float profiledY = minY + baseT * range + detail + ridge;
    if (profile.terraceSteps > 1.0F && profile.terraceStrength > 0.0F) {
        profiledY = minY + terrace((profiledY - minY) / range,
            profile.terraceSteps,
            std::clamp(profile.terraceStrength, 0.0F, 1.0F)) * range;
    }

    // The profile is authoritative enough to shape the terrain, but keep a
    // portion of the class generator so existing seeds do not snap to a new
    // surface when a pack only tightens ranges slightly.
    constexpr float kProfileBlend = 0.68F;
    return lerp(currentSurfaceY, profiledY, kProfileBlend);
}

[[nodiscard]] TerrainBiomeId biomeIdFromDefinitionId(const std::string& id, float temperature) noexcept
{
    if (id == "forest") return TerrainBiomeId::Forest;
    if (id == "plains") return TerrainBiomeId::Plains;
    if (id == "badlands_plateau") return TerrainBiomeId::Badlands;
    if (id == "lush_highlands_valley") return TerrainBiomeId::LushHighlandsValley;
    if (id == "coast") return TerrainBiomeId::Beach;
    if (id == "ocean") {
        if (temperature > 0.35F) return TerrainBiomeId::WarmOcean;
        if (temperature < -0.35F) return TerrainBiomeId::ColdOcean;
        return TerrainBiomeId::Ocean;
    }
    if (id == "deep_ocean") return temperature < -0.25F ? TerrainBiomeId::ColdOcean : TerrainBiomeId::DeepOcean;
    if (id == "ocean_abyss") return TerrainBiomeId::OceanAbyss;
    if (id == "mountains") return temperature < -0.25F ? TerrainBiomeId::SnowyMountains : TerrainBiomeId::Mountains;
    if (id == "redwood_forest") return TerrainBiomeId::RedwoodForest;
    if (id == "magic_forest") return TerrainBiomeId::MagicalGrove;
    if (id == "elemental_crystal_cave") return TerrainBiomeId::ElementalCrystalCave;
    if (id == "floating_islands") return TerrainBiomeId::FloatingIslands;
    if (id == "swamp") return TerrainBiomeId::Swamp;
    if (id == "tundra") return TerrainBiomeId::Tundra;
    if (id == "jungle") return TerrainBiomeId::Jungle;
    if (id == "volcanic") return TerrainBiomeId::VolcanicWastes;
    if (id == "savanna") return TerrainBiomeId::Savanna;
    if (id == "desert") return TerrainBiomeId::Desert;
    if (id == "ice_caps") return TerrainBiomeId::IceCaps;
    if (id == "arcane_fracture_zone") return TerrainBiomeId::ArcaneFractureZone;
    return TerrainBiomeId::Plains;
}

[[nodiscard]] TerrainBiomeSignalSample makeBiomeSignalSample(
    const WorldShapeSignals& signals,
    float continentalness,
    float elevation,
    float erosion,
    float temperature,
    float humidity,
    float weirdness,
    float oceanDepth) noexcept
{
    TerrainBiomeSignalSample sample{};
    sample.continentalness = continentalness;
    sample.tectonicStress = std::clamp(signals.tectonicStress, 0.0F, 1.0F);
    sample.erosion = erosion;
    sample.weirdness = weirdness;
    sample.temperature = temperature;
    sample.moisture = humidity;
    sample.volcanism = std::clamp(signals.volcanism, 0.0F, 1.0F);
    sample.manaField = std::clamp(signals.manaField, 0.0F, 1.0F);
    sample.polarInfluence = std::clamp(signals.polarInfluence, 0.0F, 1.0F);
    sample.oceanDepthBias = std::clamp(signals.oceanDepthBias, 0.0F, 1.0F);
    sample.height = elevation;
    sample.depth = 0.0F;
    sample.oceanDepth = oceanDepth;
    return sample;
}

[[nodiscard]] const TerrainBiomeDefinition* resolveBiomeDefinition(
    const WorldShapeSignals& signals,
    float continentalness,
    float elevation,
    float erosion,
    float temperature,
    float humidity,
    float weirdness,
    float oceanDepth,
    const TerrainDefinitionRegistry* terrainDefinitions) noexcept
{
    if (terrainDefinitions == nullptr || terrainDefinitions->biomeCount() == 0) {
        return nullptr;
    }
    const auto sample = makeBiomeSignalSample(signals, continentalness, elevation, erosion,
        temperature, humidity, weirdness, oceanDepth);
    return terrainDefinitions->findBestBiome(sample);
}

TerrainBiomeId resolveBiomeFallback(const WorldShapeSignals& signals,
                                    TerrainClass terrainClass,
                                    float continentalness,
                                    float elevation,
                                    float erosion,
                                    float temperature,
                                    float humidity,
                                    float weirdness,
                                    TerrainSurfaceKind surfaceKind) noexcept
{
    if (surfaceKind == TerrainSurfaceKind::DeepOcean) {
        if (terrainClass == TerrainClass::Abyss) return TerrainBiomeId::OceanAbyss;
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
    if (terrainClass == TerrainClass::ArcaneFracture) {
        return TerrainBiomeId::ArcaneFractureZone;
    }
    if (terrainClass == TerrainClass::Magic && elevation > 190.0F) {
        return TerrainBiomeId::FloatingIslands;
    }
    if (terrainClass == TerrainClass::Magic && humidity > 0.15F) {
        return TerrainBiomeId::MagicalGrove;
    }
    if (signals.volcanism > 0.72F || (weirdness > 0.78F && humidity < -0.35F && temperature > 0.25F)) {
        return TerrainBiomeId::VolcanicWastes;
    }
    if (terrainClass == TerrainClass::Plateau && humidity < -0.18F) {
        return TerrainBiomeId::Badlands;
    }
    if (terrainClass == TerrainClass::Highlands && humidity > 0.05F) {
        return TerrainBiomeId::LushHighlandsValley;
    }
    if (terrainClass == TerrainClass::Mountains || elevation > 240.0F || continentalness > 0.65F) {
        return temperature < -0.25F ? TerrainBiomeId::SnowyMountains : TerrainBiomeId::Mountains;
    }
    if (signals.polarInfluence > 0.82F) {
        return TerrainBiomeId::IceCaps;
    }
    if (temperature < -0.52F) {
        return TerrainBiomeId::Tundra;
    }
    if (temperature < -0.35F) {
        return TerrainBiomeId::Taiga;
    }
    if (temperature > 0.50F && humidity > 0.42F) {
        return TerrainBiomeId::Jungle;
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
        return temperature < 0.16F ? TerrainBiomeId::RedwoodForest : TerrainBiomeId::DenseForest;
    }
    if (humidity > 0.12F) {
        return TerrainBiomeId::Forest;
    }
    return TerrainBiomeId::Plains;
}

TerrainBiomeId resolveBiome(const WorldShapeSignals& signals,
                            TerrainClass terrainClass,
                            float continentalness,
                            float elevation,
                            float erosion,
                            float temperature,
                            float humidity,
                            float weirdness,
                            float oceanDepth,
                            TerrainSurfaceKind surfaceKind,
                            const TerrainDefinitionRegistry* terrainDefinitions) noexcept
{
    if (terrainDefinitions != nullptr && terrainDefinitions->biomeCount() > 0) {
        if (const auto* biome = resolveBiomeDefinition(signals, continentalness, elevation, erosion,
            temperature, humidity, weirdness, oceanDepth, terrainDefinitions)) {
            return biomeIdFromDefinitionId(biome->id, temperature);
        }
    }

    return resolveBiomeFallback(signals, terrainClass, continentalness, elevation, erosion, temperature,
        humidity, weirdness, surfaceKind);
}

// --- Phase 2: per-regime surface-Y helpers ----------------------------------
//
// Each regime's surface height is a pure function of the smoothed world-shape
// signals. Computing all five and blending adjacent pairs across transition
// zones (see surfaceYBlended) gives a C0-continuous height field — no more
// cliffs at the beach→land boundary.
//
// Each helper is cheap (a few multiplies + one remap/clamp). The blender
// evaluates the two relevant regimes per column, so worst-case cost is 2x
// the original single-branch math — still trivial relative to noise sampling.

[[nodiscard, maybe_unused]] float deepOceanSurfaceY(float continentalness, float detail, const NoiseTerrainSettings& s) noexcept
{
    const float depth = smoothRemap(continentalness, -1.0F, -0.65F, s.deepOceanMaxDepth, s.deepOceanMinDepth);
    return s.seaLevel - depth + detail * 5.0F;
}

[[nodiscard, maybe_unused]] float oceanSurfaceY(float continentalness, float detail, const NoiseTerrainSettings& s) noexcept
{
    const float depth = smoothRemap(continentalness, -0.65F, -0.35F, s.deepOceanMinDepth, s.shelfMaxDepth);
    return s.seaLevel - depth + detail * 4.0F;
}

[[nodiscard, maybe_unused]] float shallowOceanSurfaceY(float continentalness, float detail, const NoiseTerrainSettings& s) noexcept
{
    const float depth = smoothRemap(continentalness, -0.35F, -0.15F, s.shelfMaxDepth, s.shelfMinDepth);
    return s.seaLevel - depth + detail * 3.0F;
}

[[nodiscard, maybe_unused]] float beachSurfaceY(float continentalness, float detail, const NoiseTerrainSettings& s) noexcept
{
    const float coastT = remap(continentalness, -0.15F, 0.05F, 0.0F, 1.0F);
    return s.seaLevel - 2.0F + coastT * 5.0F + detail * 1.5F;
}

[[nodiscard, maybe_unused]] float landSurfaceY(float continentalness, float erosion, float peaksValleys, float detail,
                                  const NoiseTerrainSettings& s) noexcept
{
    const float landT = remap(continentalness, 0.05F, 1.0F, 0.0F, 1.0F);
    const float coastalT = smooth01(landT);
    const float roughness = (1.0F - clamp01((erosion + 1.0F) * 0.5F)) * s.hillBoost * coastalT;
    const float peak = smooth01((peaksValleys + 1.0F) * 0.5F) * s.mountainBoost * landT * coastalT;
    const float detailAmp = s.surfaceAmplitude * coastalT;
    return s.seaLevel + landT * s.surfaceAmplitude + roughness + peak + detail * detailAmp;
}

// Half-width of the blend zone around each regime boundary. With kBlendHalf =
// 0.05, transitions occupy a 0.10-wide band of continentalness on each side
// of -0.65, -0.35, -0.15, 0.05. The bands don't overlap (the closest pair is
// -0.15 and 0.05, 0.20 apart, so 0.10-wide zones leave a 0.10 gap of pure-
// regime height between them).
//
// Larger kBlendHalf = smoother transitions but wider "mixed" beach/land zones.
// Smaller = sharper transitions; at 0 we recover the old piecewise behavior.
inline constexpr float kBlendHalf = 0.05F;

// Linear remap of `value` from [edge - kBlendHalf, edge + kBlendHalf] to [0,1]
// with smoothstep curvature. Outside the band: 0 below, 1 above.
[[nodiscard, maybe_unused]] float blendWeight(float value, float edge) noexcept
{
    return smooth01((value - (edge - kBlendHalf)) / (2.0F * kBlendHalf));
}

[[nodiscard, maybe_unused]] float surfaceYBlended(float continentalness, float erosion, float peaksValleys, float detail,
                                     const NoiseTerrainSettings& s) noexcept
{
    // Each regime's surface-Y as a free function. Total: 5 cheap evaluations.
    const float yDeep    = deepOceanSurfaceY(continentalness, detail, s);
    const float yOcean   = oceanSurfaceY(continentalness, detail, s);
    const float yShallow = shallowOceanSurfaceY(continentalness, detail, s);
    const float yBeach   = beachSurfaceY(continentalness, detail, s);
    const float yLand    = landSurfaceY(continentalness, erosion, peaksValleys, detail, s);

    // Walk the boundaries in order. In each transition zone, lerp the two
    // adjacent regimes; outside zones, return the dominant regime.
    // The chain produces a single continuous height function.
    float y = yDeep;
    y = lerp(y, yOcean,   blendWeight(continentalness, -0.65F));
    y = lerp(y, yShallow, blendWeight(continentalness, -0.35F));
    y = lerp(y, yBeach,   blendWeight(continentalness, -0.15F));
    y = lerp(y, yLand,    blendWeight(continentalness,  0.05F));
    return y;
}

// Discrete surface kind. Kept piecewise on continentalness — it drives the
// biome painter (sand vs gravel vs grass), where a smooth blend wouldn't make
// sense for a discrete material choice. The thresholds align with the height
// blend boundaries so the painter never disagrees catastrophically with the
// height (e.g., a "Beach" column won't end up tens of blocks underwater).
[[nodiscard, maybe_unused]] TerrainSurfaceKind classifySurfaceKind(float continentalness) noexcept
{
    if (continentalness < -0.65F) return TerrainSurfaceKind::DeepOcean;
    if (continentalness < -0.35F) return TerrainSurfaceKind::Ocean;
    if (continentalness < -0.15F) return TerrainSurfaceKind::ShallowOcean;
    if (continentalness < 0.05F)  return TerrainSurfaceKind::Beach;
    return TerrainSurfaceKind::Land;
}

// Ocean-depth used by downstream stages (e.g., for water column thickness
// hints and biome resolution). Mirrors the original piecewise mapping but
// stays continuous at the boundaries because the remap endpoints already
// agree where they meet.
[[nodiscard, maybe_unused]] float oceanDepthFor(float continentalness, TerrainSurfaceKind kind,
                                   float surfaceY, const NoiseTerrainSettings& s) noexcept
{
    switch (kind) {
        case TerrainSurfaceKind::DeepOcean:
            return remap(continentalness, -1.0F, -0.65F, s.deepOceanMaxDepth, s.deepOceanMinDepth);
        case TerrainSurfaceKind::Ocean:
            return remap(continentalness, -0.65F, -0.35F, s.deepOceanMinDepth, s.shelfMaxDepth);
        case TerrainSurfaceKind::ShallowOcean:
            return remap(continentalness, -0.35F, -0.15F, s.shelfMaxDepth, s.shelfMinDepth);
        case TerrainSurfaceKind::Beach:
            return std::max(0.0F, s.seaLevel - surfaceY);
        case TerrainSurfaceKind::Land:
            return 0.0F;
    }
    return 0.0F;
}

ColumnWorldgenData buildColumnDataFromSignals(const WorldShapeSignals& signals,
                                              const NoiseTerrainSettings& settings,
                                              const TerrainDefinitionRegistry* terrainDefinitions) noexcept
{
    const float continentalness = std::clamp(signals.continentalness, -1.0F, 1.0F);
    const float tectonicStress = std::clamp(signals.tectonicStress, 0.0F, 1.0F);
    const float erosion = std::clamp(signals.erosion, -1.0F, 1.0F);
    const float peaksValleys = std::clamp(signals.peaksValleys, -1.0F, 1.0F);
    const float temperature = std::clamp(signals.temperature, -1.0F, 1.0F);
    const float humidity = std::clamp(signals.humidity, -1.0F, 1.0F);
    const float weirdness = std::clamp(signals.weirdness, -1.0F, 1.0F);
    const float volcanism = std::clamp(signals.volcanism, 0.0F, 1.0F);
    const float manaField = std::clamp(signals.manaField, 0.0F, 1.0F);
    const float polarInfluence = std::clamp(signals.polarInfluence, 0.0F, 1.0F);
    const float oceanDepthBias = std::clamp(signals.oceanDepthBias, 0.0F, 1.0F);
    const TerrainClass terrainClass = classifyTerrainClass(signals);
    float surfaceY = terrainClassSurfaceY(terrainClass, signals, settings);
    TerrainSurfaceKind surfaceKind = surfaceKindForClass(terrainClass, continentalness);
    if (surfaceKind == TerrainSurfaceKind::Beach && surfaceY > settings.seaLevel + 10.0F) {
        surfaceKind = TerrainSurfaceKind::Land;
    }
    float oceanDepth = std::max(0.0F, settings.seaLevel - surfaceY);
    const TerrainBiomeDefinition* biomeDefinition = resolveBiomeDefinition(
        signals,
        continentalness,
        surfaceY - settings.seaLevel,
        erosion,
        temperature,
        humidity,
        weirdness,
        oceanDepth,
        terrainDefinitions);
    if (biomeDefinition != nullptr) {
        if (const auto* profile = terrainDefinitions->findHeightProfile(biomeDefinition->heightProfile)) {
            surfaceY = applyHeightProfile(surfaceY, terrainClass, *profile, signals, settings);
            if (surfaceKind == TerrainSurfaceKind::Beach && surfaceY > settings.seaLevel + 10.0F) {
                surfaceKind = TerrainSurfaceKind::Land;
            }
            oceanDepth = std::max(0.0F, settings.seaLevel - surfaceY);
        }
    }

    // Phase 4: River carving. Two gates must both pass:
    //   (1) Climate: humid, low erosion, not magical-weird, not ocean.
    //   (2) Geometry: this column lies on a river ridge band.
    // The river depth tapers off from the ridge center via smoothstep so the
    // banks slope down gently rather than dropping as a cliff.
    const bool oceanColumn = surfaceKind != TerrainSurfaceKind::Land
        && surfaceKind != TerrainSurfaceKind::Beach;
    const bool climateAllowsRiver = !oceanColumn
        && humidity > settings.riverHumidityMin
        && erosion < settings.riverErosionMax
        && std::abs(weirdness) < settings.riverWeirdnessAbsMax;
    float riverCarveDepth = 0.0F;
    if (climateAllowsRiver) {
        const float ridgeDist = std::abs(signals.riverRidge - 0.5F);
        if (ridgeDist < settings.riverBandHalfWidth) {
            const float t = 1.0F - (ridgeDist / settings.riverBandHalfWidth);
            riverCarveDepth = static_cast<float>(settings.riverMaxDepth) * smooth01(t);
        }
    }
    if (riverCarveDepth > 0.0F) {
        surfaceY -= riverCarveDepth;
    }
    surfaceY = std::clamp(surfaceY, settings.minWorldY, settings.maxWorldY);
    oceanDepth = std::max(0.0F, settings.seaLevel - surfaceY);
    // True when this column is actually on a river channel (not just eligible).
    const bool isRiverChannel = riverCarveDepth >= 1.0F;

    ColumnWorldgenData data{};
    data.continentalness = continentalness;
    data.tectonicStress = tectonicStress;
    data.erosion = erosion;
    data.peaksValleys = peaksValleys;
    data.temperature = temperature;
    data.humidity = humidity;
    data.weirdness = weirdness;
    data.volcanism = volcanism;
    data.manaField = manaField;
    data.polarInfluence = polarInfluence;
    data.oceanDepthBias = oceanDepthBias;
    data.oceanDepth = std::max(0.0F, oceanDepth);
    // Phase 2: biomeBlend is now a sharpness indicator that the painter (or
    // future stochastic-mixing stage) can use to detect "near a boundary".
    // Peaks at the boundaries we just smoothed; 0 deep inside a regime.
    {
        const float dDeep    = std::abs(continentalness - (-0.65F));
        const float dOcean   = std::abs(continentalness - (-0.35F));
        const float dShallow = std::abs(continentalness - (-0.15F));
        const float dBeach   = std::abs(continentalness -  0.05F);
        const float closestEdge = std::min({dDeep, dOcean, dShallow, dBeach});
        data.biomeBlend = smooth01(1.0F - closestEdge / kBlendHalf);
    }
    data.surfaceY = static_cast<std::int32_t>(std::floor(surfaceY));
    data.seaLevel = static_cast<std::int32_t>(std::floor(settings.seaLevel));
    data.terrainClass = terrainClass;
    data.surfaceKind = surfaceKind;
    data.isOcean = surfaceKind == TerrainSurfaceKind::DeepOcean
        || surfaceKind == TerrainSurfaceKind::Ocean
        || surfaceKind == TerrainSurfaceKind::ShallowOcean;
    data.isBeach = surfaceKind == TerrainSurfaceKind::Beach;
    // Phase 4: semantic change — now means "this column is on an actual river
    // channel," not just "climate would allow one." Painter uses this to set
    // a riverbed cap, and a future foliage stage will skip placing trees here.
    data.isRiverCandidate = isRiverChannel;
    data.biome = biomeDefinition != nullptr
        ? biomeIdFromDefinitionId(biomeDefinition->id, temperature)
        : resolveBiome(signals, terrainClass, continentalness, surfaceY - settings.seaLevel,
            erosion, temperature, humidity, weirdness, data.oceanDepth, surfaceKind, terrainDefinitions);
    return data;
}

ColumnWorldgenData buildColumnData(float worldX,
                                   float worldZ,
                                   const NoiseTerrainSettings& settings,
                                   const TerrainDefinitionRegistry* terrainDefinitions) noexcept
{
    return buildColumnDataFromSignals(sampleWorldShapeSignals(worldX, worldZ, settings), settings, terrainDefinitions);
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
    std::uint64_t version = 0xa6d52b90f4831c7eULL;
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
    return buildColumnData(worldX, worldZ, settings_, terrainDefinitions_);
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
            const auto data = buildColumnDataFromSignals(lerpSignals(x0, x1, tz), settings_, terrainDefinitions_);
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
