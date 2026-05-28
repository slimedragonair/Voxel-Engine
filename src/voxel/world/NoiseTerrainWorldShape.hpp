#pragma once

#include <voxel/world/NoiseTerrainGenerator.hpp>

namespace voxel::world {

class TerrainDefinitionRegistry;

namespace terrain_shape {

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

[[nodiscard]] WorldShapeSignals sampleWorldShapeSignals(
    float worldX, float worldZ, const NoiseTerrainSettings& settings) noexcept;

[[nodiscard]] WorldShapeSignals lerpSignals(
    const WorldShapeSignals& a, const WorldShapeSignals& b, float t) noexcept;

[[nodiscard]] ColumnWorldgenData buildColumnDataFromSignals(
    const WorldShapeSignals& signals,
    const NoiseTerrainSettings& settings,
    const TerrainDefinitionRegistry* terrainDefinitions) noexcept;

[[nodiscard]] ColumnWorldgenData buildColumnData(
    float worldX,
    float worldZ,
    const NoiseTerrainSettings& settings,
    const TerrainDefinitionRegistry* terrainDefinitions) noexcept;

} // namespace terrain_shape

} // namespace voxel::world
