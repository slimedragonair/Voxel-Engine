#include <voxel/world/TerrainPipeline.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <voxel/core/Math.hpp>
#include <voxel/world/BlockState.hpp>

namespace voxel::world {

namespace {

constexpr int kCoarseStep = TerrainDensityField::kCoarseStep;
constexpr int kCoarseSize = TerrainDensityField::kCoarseSize;

[[nodiscard]] constexpr std::size_t columnIndex(int x, int z) noexcept
{
    return static_cast<std::size_t>(x + (z * ChunkSize));
}

[[nodiscard]] constexpr float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

} // namespace

// ---------------------------------------------------------------------------
// TerrainDensityField

float TerrainDensityField::densityAt(
    const std::array<float, kSampleCount>& samples,
    int x, int y, int z) noexcept
{
    const int gx = std::clamp(x / kCoarseStep, 0, kCoarseSize - 2);
    const int gy = std::clamp(y / kCoarseStep, 0, kCoarseSize - 2);
    const int gz = std::clamp(z / kCoarseStep, 0, kCoarseSize - 2);
    const float tx = static_cast<float>(x - gx * kCoarseStep) / static_cast<float>(kCoarseStep);
    const float ty = static_cast<float>(y - gy * kCoarseStep) / static_cast<float>(kCoarseStep);
    const float tz = static_cast<float>(z - gz * kCoarseStep) / static_cast<float>(kCoarseStep);

    const float c000 = samples[index(gx,     gy,     gz)];
    const float c100 = samples[index(gx + 1, gy,     gz)];
    const float c010 = samples[index(gx,     gy + 1, gz)];
    const float c110 = samples[index(gx + 1, gy + 1, gz)];
    const float c001 = samples[index(gx,     gy,     gz + 1)];
    const float c101 = samples[index(gx + 1, gy,     gz + 1)];
    const float c011 = samples[index(gx,     gy + 1, gz + 1)];
    const float c111 = samples[index(gx + 1, gy + 1, gz + 1)];

    const float x00 = lerp(c000, c100, tx);
    const float x10 = lerp(c010, c110, tx);
    const float x01 = lerp(c001, c101, tx);
    const float x11 = lerp(c011, c111, tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

bool TerrainDensityField::cellMayExceed(
    const std::array<float, kSampleCount>& samples,
    float threshold,
    int x, int y, int z) noexcept
{
    const int gx = std::clamp(x / kCoarseStep, 0, kCoarseSize - 2);
    const int gy = std::clamp(y / kCoarseStep, 0, kCoarseSize - 2);
    const int gz = std::clamp(z / kCoarseStep, 0, kCoarseSize - 2);
    return samples[index(gx,     gy,     gz)]     > threshold
        || samples[index(gx + 1, gy,     gz)]     > threshold
        || samples[index(gx,     gy + 1, gz)]     > threshold
        || samples[index(gx + 1, gy + 1, gz)]     > threshold
        || samples[index(gx,     gy,     gz + 1)] > threshold
        || samples[index(gx + 1, gy,     gz + 1)] > threshold
        || samples[index(gx,     gy + 1, gz + 1)] > threshold
        || samples[index(gx + 1, gy + 1, gz + 1)] > threshold;
}

float TerrainDensityField::density2DAt(
    const std::array<float, kSampleCount2D>& samples,
    int x, int z) noexcept
{
    const int gx = std::clamp(x / kCoarseStep, 0, kCoarseSize - 2);
    const int gz = std::clamp(z / kCoarseStep, 0, kCoarseSize - 2);
    const float tx = static_cast<float>(x - gx * kCoarseStep) / static_cast<float>(kCoarseStep);
    const float tz = static_cast<float>(z - gz * kCoarseStep) / static_cast<float>(kCoarseStep);
    const float c00 = samples[index2D(gx,     gz)];
    const float c10 = samples[index2D(gx + 1, gz)];
    const float c01 = samples[index2D(gx,     gz + 1)];
    const float c11 = samples[index2D(gx + 1, gz + 1)];
    return lerp(lerp(c00, c10, tx), lerp(c01, c11, tx), tz);
}

// ---------------------------------------------------------------------------
// Bounds

TerrainStageBounds computeTerrainBounds(
    const Chunk& chunk,
    const TerrainColumnPrepass& prepass,
    const NoiseTerrainSettings& settings) noexcept
{
    const int chunkBaseBlockY = static_cast<int>(chunk.coord().y * ChunkSize);
    TerrainStageBounds bounds{};
    bounds.seaTopForChunk = std::clamp(
        static_cast<int>(std::floor(settings.seaLevel)) - chunkBaseBlockY,
        -1, ChunkSize - 1);

    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            const auto prepassIdx = columnIndex(x, z);
            const int surfaceBlockY = prepass.surfaceBlockY[prepassIdx];
            const int solidTop = std::clamp(surfaceBlockY - chunkBaseBlockY, -1, ChunkSize - 1);
            const int stoneTop = std::min(solidTop - 4, ChunkSize - 1);
            bounds.maxSurfaceBlockY = std::max(bounds.maxSurfaceBlockY, surfaceBlockY);
            bounds.maxSolidTop = std::max(bounds.maxSolidTop, solidTop);
            bounds.maxStoneTop = std::max(bounds.maxStoneTop, stoneTop);
            if (stoneTop >= 0
                && surfaceBlockY - chunkBaseBlockY > static_cast<int>(settings.caveCeilingOffset)) {
                bounds.needsCaveSamples = true;
            }
        }
    }
    return bounds;
}

// ---------------------------------------------------------------------------
// Density samplers

void sampleCaveDensityField(
    TerrainDensityField& density,
    const TerrainStageBounds& bounds,
    const NoiseTerrainSettings& settings,
    float worldBaseX, float worldBaseY, float worldBaseZ)
{
    if (!bounds.needsCaveSamples) {
        return;
    }
    const int maxGy = std::clamp((bounds.maxStoneTop / kCoarseStep) + 1, 0, kCoarseSize - 1);

    // Cheese caves — large blob caverns. Single fbm3D, thresholded later.
    for (int gz = 0; gz < kCoarseSize; ++gz) {
        const float sampleZ = (worldBaseZ + static_cast<float>(gz * kCoarseStep)) * settings.caveFrequency;
        for (int gy = 0; gy <= maxGy; ++gy) {
            const float sampleY = (worldBaseY + static_cast<float>(gy * kCoarseStep)) * settings.caveFrequency;
            for (int gx = 0; gx < kCoarseSize; ++gx) {
                const float sampleX = (worldBaseX + static_cast<float>(gx * kCoarseStep)) * settings.caveFrequency;
                density.cheese[TerrainDensityField::index(gx, gy, gz)] = core::fbm3D(
                    sampleX, sampleY, sampleZ,
                    settings.seed ^ 0x9E3779B9U,
                    settings.caveOctaves, 2.0F, 0.5F);
            }
        }
    }
    density.cheeseSampled = true;

    // Spaghetti caves — two independent 3D noise fields. A block is "in a
    // tunnel" when both fields are near their iso-value (0.5). The
    // intersection of two iso-surfaces is a 1D curve, hence the tunnel shape.
    for (int gz = 0; gz < kCoarseSize; ++gz) {
        const float sampleZ = (worldBaseZ + static_cast<float>(gz * kCoarseStep)) * settings.spaghettiFrequency;
        for (int gy = 0; gy <= maxGy; ++gy) {
            const float sampleY = (worldBaseY + static_cast<float>(gy * kCoarseStep)) * settings.spaghettiFrequency;
            for (int gx = 0; gx < kCoarseSize; ++gx) {
                const float sampleX = (worldBaseX + static_cast<float>(gx * kCoarseStep)) * settings.spaghettiFrequency;
                density.spaghettiA[TerrainDensityField::index(gx, gy, gz)] = core::fbm3D(
                    sampleX, sampleY, sampleZ,
                    settings.seed ^ 0x4F1B5C72U,
                    settings.spaghettiOctaves, 2.0F, 0.5F);
                density.spaghettiB[TerrainDensityField::index(gx, gy, gz)] = core::fbm3D(
                    sampleX, sampleY, sampleZ,
                    settings.seed ^ 0xB2C9AD81U,
                    settings.spaghettiOctaves, 2.0F, 0.5F);
            }
        }
    }
    density.spaghettiSampled = true;

    // Ravines — ridged 2D noise in XZ. Only kCoarseSize² samples (no Y), so
    // it's free compared to the 3D fields. We store value-in-[0,1] and check
    // `|value - 0.5| < bandHalfWidth` at carve time for the slab shape.
    for (int gz = 0; gz < kCoarseSize; ++gz) {
        const float sampleZ = (worldBaseZ + static_cast<float>(gz * kCoarseStep)) * settings.ravineFrequency;
        for (int gx = 0; gx < kCoarseSize; ++gx) {
            const float sampleX = (worldBaseX + static_cast<float>(gx * kCoarseStep)) * settings.ravineFrequency;
            density.ravine2D[TerrainDensityField::index2D(gx, gz)] = core::fbm2D(
                sampleX, sampleZ,
                settings.seed ^ 0x7AE3D915U,
                3, 2.0F, 0.5F);
        }
    }
    density.ravineSampled = true;
}

void sampleOverhangDensityField(
    TerrainDensityField& density,
    const NoiseTerrainSettings& settings,
    float worldBaseX, float worldBaseY, float worldBaseZ)
{
    // The overhang field shifts the iso-surface vertically by up to
    // overhangAmplitude blocks. We need it everywhere in the chunk because
    // any column's overhang band can fall anywhere in the chunk's Y extent.
    // Output is rescaled to [-1, 1].
    for (int gz = 0; gz < kCoarseSize; ++gz) {
        const float sampleZ = (worldBaseZ + static_cast<float>(gz * kCoarseStep)) * settings.overhangFrequency;
        for (int gy = 0; gy < kCoarseSize; ++gy) {
            const float sampleY = (worldBaseY + static_cast<float>(gy * kCoarseStep)) * settings.overhangFrequency;
            for (int gx = 0; gx < kCoarseSize; ++gx) {
                const float sampleX = (worldBaseX + static_cast<float>(gx * kCoarseStep)) * settings.overhangFrequency;
                const float n = core::fbm3D(
                    sampleX, sampleY, sampleZ,
                    settings.seed ^ 0xE1A7B3D9U,
                    settings.overhangOctaves, 2.0F, 0.5F);
                density.overhang[TerrainDensityField::index(gx, gy, gz)] = n * 2.0F - 1.0F;
            }
        }
    }
    density.overhangSampled = true;
}

void sampleOreDensityFields(
    TerrainDensityField& density,
    const TerrainStageBounds& bounds,
    const NoiseTerrainSettings& settings,
    float worldBaseX, float worldBaseY, float worldBaseZ)
{
    if (bounds.maxStoneTop < 0) {
        return;
    }
    const int maxGy = std::clamp((bounds.maxStoneTop / kCoarseStep) + 1, 0, kCoarseSize - 1);
    for (int gz = 0; gz < kCoarseSize; ++gz) {
        const float worldZ = worldBaseZ + static_cast<float>(gz * kCoarseStep);
        for (int gy = 0; gy <= maxGy; ++gy) {
            const float worldY = worldBaseY + static_cast<float>(gy * kCoarseStep);
            for (int gx = 0; gx < kCoarseSize; ++gx) {
                const float worldX = worldBaseX + static_cast<float>(gx * kCoarseStep);
                density.coal[TerrainDensityField::index(gx, gy, gz)] = core::valueNoise3D(
                    worldX * settings.coalFrequency,
                    worldY * settings.coalFrequency,
                    worldZ * settings.coalFrequency,
                    settings.seed ^ 0xCC8C2B43U);
                density.iron[TerrainDensityField::index(gx, gy, gz)] = core::valueNoise3D(
                    worldX * settings.ironFrequency,
                    worldY * settings.ironFrequency,
                    worldZ * settings.ironFrequency,
                    settings.seed ^ 0x4D7A1F69U);
            }
        }
    }
    density.oreSampled = true;
}

} // namespace voxel::world
