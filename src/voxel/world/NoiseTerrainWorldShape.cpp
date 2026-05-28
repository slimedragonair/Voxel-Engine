#include "NoiseTerrainWorldShape.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <voxel/core/Math.hpp>
#include <voxel/world/TerrainDefinitionRegistry.hpp>

namespace voxel::world::terrain_shape {

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

[[nodiscard]] float mountainInfluence(const WorldShapeSignals& s) noexcept
{
    const float stress = smooth01(remap(s.tectonicStress, 0.58F, 0.92F, 0.0F, 1.0F));
    const float erosionGate = smooth01(remap(0.58F - s.erosion, 0.0F, 0.74F, 0.0F, 1.0F));
    const float peakGate = smooth01(remap(s.peaksValleys, -0.20F, 0.72F, 0.0F, 1.0F));
    return smooth01(std::clamp(stress * erosionGate * (0.55F + peakGate * 0.45F), 0.0F, 1.0F));
}

[[nodiscard]] float highlandsSurfaceY(
    const WorldShapeSignals& s,
    const NoiseTerrainSettings& settings) noexcept
{
    const float landT = smooth01(remap(s.continentalness, -0.06F, 0.82F, 0.0F, 1.0F));
    const float detail = std::clamp(s.detail, -1.0F, 1.0F);
    const float amp = std::max(1.0F, settings.surfaceAmplitude);
    const float peak01 = smooth01(s.peaksValleys * 0.5F + 0.5F);
    return settings.seaLevel + 72.0F + landT * (amp * 1.28F) + peak01 * 78.0F + detail * (amp * 0.30F);
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
        const float mountainY = settings.seaLevel
            + 170.0F
            + landT * 68.0F
            + stress * settings.mountainBoost * 0.66F
            + jagged * settings.mountainBoost * 0.34F
            + ridge * 48.0F
            + detail * (amp * 0.24F);
        return lerp(highlandsSurfaceY(s, settings), mountainY, mountainInfluence(s));
    }
    case TerrainClass::Plateau: {
        const float raw = settings.plateauHeight + landT * (amp * 0.50F) + detail * (amp * 0.26F);
        return terrace(raw / 340.0F, 7.0F, 0.58F) * 340.0F;
    }
    case TerrainClass::Highlands:
        return highlandsSurfaceY(s, settings);
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
    const float classBlend = terrainClass == TerrainClass::Mountains
        ? mountainInfluence(signals)
        : 1.0F;
    return lerp(currentSurfaceY, profiledY, kProfileBlend * classBlend);
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

} // namespace voxel::world::terrain_shape
