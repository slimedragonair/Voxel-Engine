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

// Resolves the per-column "soil cap" for the surface paint stage.
//
// Phase 1: biome-aware blocks (sand for desert, snow for snowy mountains,
// basalt for volcanic, podzol for dense forest/taiga, etc.).
//
// This is the data table that drives the visual identity of every biome.
// In a future Phase, this will move to JSON via a BiomeSurfaceProfile
// loaded by BlockRegistry. For now, hardcoded here keeps it close to the
// stage that uses it.
struct ColumnSoilCap {
    BlockStateId topBlock;
    BlockStateId subSurfaceBlock;
    int soilDepth;
};

[[nodiscard]] ColumnSoilCap resolveSoilCap(
    TerrainSurfaceKind surfaceKind,
    TerrainBiomeId biome,
    bool isRiverChannel,
    const NoiseTerrainSettings& settings) noexcept
{
    // Phase 4: river beds take priority over biome painting. A river through
    // a forest still has a gravel/sand bed (not grass under the water).
    if (isRiverChannel) {
        // Warm-climate rivers get sandy beds; cold/snowy rivers get gravel.
        // Use biome as a proxy for climate (the cold/snow biomes are easy
        // to spot here without needing temperature on the prepass).
        const bool coldRiver = biome == TerrainBiomeId::Taiga
            || biome == TerrainBiomeId::SnowyMountains
            || biome == TerrainBiomeId::Mountains
            || biome == TerrainBiomeId::Tundra
            || biome == TerrainBiomeId::IceCaps;
        if (coldRiver) {
            return {settings.gravelBlock, settings.gravelBlock, 3};
        }
        return {settings.sandBlock, settings.gravelBlock, 3};
    }

    // Ocean/beach surface kinds short-circuit biome resolution: a desert
    // biome at the coast still gets a beach cap, not sand-over-sand.
    switch (surfaceKind) {
        case TerrainSurfaceKind::DeepOcean:
            return {settings.stoneBlock, settings.stoneBlock, 1};
        case TerrainSurfaceKind::Ocean:
            // Warm oceans get sandy floors, cold oceans get gravel.
            if (biome == TerrainBiomeId::WarmOcean) {
                return {settings.sandBlock, settings.sandBlock, 2};
            }
            if (biome == TerrainBiomeId::ColdOcean) {
                return {settings.gravelBlock, settings.stoneBlock, 2};
            }
            return {settings.gravelBlock, settings.stoneBlock, 2};
        case TerrainSurfaceKind::ShallowOcean:
            return {settings.gravelBlock, settings.gravelBlock, 3};
        case TerrainSurfaceKind::Beach:
            // Warm beaches get sand; cold beaches get gravel.
            if (biome == TerrainBiomeId::ColdOcean
                || biome == TerrainBiomeId::SnowyMountains
                || biome == TerrainBiomeId::Taiga
                || biome == TerrainBiomeId::Tundra
                || biome == TerrainBiomeId::IceCaps) {
                return {settings.gravelBlock, settings.gravelBlock, 4};
            }
            return {settings.sandBlock, settings.sandBlock, 4};
        case TerrainSurfaceKind::Land:
            break;
    }

    // Land biomes — visual identity table.
    switch (biome) {
        case TerrainBiomeId::SnowyMountains:
            // Snow cap over stone — mountains stay rocky underneath.
            return {settings.snowBlock, settings.stoneBlock, 2};
        case TerrainBiomeId::Tundra:
            return {settings.snowBlock, settings.dirtBlock, 2};
        case TerrainBiomeId::IceCaps:
            return {settings.iceBlock, settings.snowBlock, 3};
        case TerrainBiomeId::Mountains:
            return {settings.stoneBlock, settings.stoneBlock, 1};
        case TerrainBiomeId::ArcaneFractureZone:
        case TerrainBiomeId::VolcanicWastes:
            return {settings.basaltBlock, settings.basaltBlock, 2};
        case TerrainBiomeId::Desert:
            return {settings.sandBlock, settings.sandstoneBlock, 5};
        case TerrainBiomeId::Badlands:
            return {settings.redSandBlock, settings.terracottaBlock, 5};
        case TerrainBiomeId::Jungle:
        case TerrainBiomeId::Swamp:
            // Grass on top, but with mossy_stone below dirt for that
            // wet/overgrown swamp look when you dig.
            return {settings.grassBlock, settings.mossyStoneBlock, 4};
        case TerrainBiomeId::RedwoodForest:
        case TerrainBiomeId::Taiga:
        case TerrainBiomeId::DenseForest:
            // Forest floor is podzol under grass.
            return {settings.grassBlock, settings.podzolBlock, 3};
        case TerrainBiomeId::LushHighlandsValley:
            return {settings.grassBlock, settings.mossyStoneBlock, 3};
        case TerrainBiomeId::FloatingIslands:
        case TerrainBiomeId::Savanna:
        case TerrainBiomeId::Forest:
        case TerrainBiomeId::Plains:
        case TerrainBiomeId::MagicalGrove:
            return {settings.grassBlock, settings.dirtBlock, 3};
        case TerrainBiomeId::ElementalCrystalCave:
            return {settings.mossyStoneBlock, settings.stoneBlock, 2};
        // Ocean biome values can appear here for inland water columns whose
        // surfaceKind happens to be Land; fall through to the default.
        case TerrainBiomeId::OceanAbyss:
        case TerrainBiomeId::DeepOcean:
        case TerrainBiomeId::Ocean:
        case TerrainBiomeId::WarmOcean:
        case TerrainBiomeId::ColdOcean:
        case TerrainBiomeId::Beach:
            return {settings.sandBlock, settings.sandBlock, 4};
    }
    // Safety fallback — should be unreachable.
    return {settings.grassBlock, settings.dirtBlock, 3};
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

// Phase 7: overhang amplitude (in blocks) for a column.
//
// IMPORTANT — driven by smooth signals, not discrete biome.
// The previous version branched on TerrainBiomeId (Mountains=8, Plains=0,
// etc.), but biome IDs are computed via hard if/else cuts on the underlying
// noise signals. Adjacent columns could disagree on biome by one ID and
// produce an 8-block discontinuity in surface height — exactly the cliff
// seam we fixed in Phase 2 for the continentalness regimes.
//
// The fix: `amplitude` is a continuous function of `peaksValleys` and
// `erosion`, both of which are smoothly interpolated by the prepass
// supersampler. Mountain-like regions (high peaks, low erosion) get full
// amplitude; flat regions (low peaks, high erosion) get zero. Adjacent
// columns transition smoothly.
//
// The discrete biome enum stays for OTHER decisions (surface materials,
// foliage rules, etc.) where step changes are acceptable.
[[nodiscard]] float terrainAmplitude(
    float peaksValleys, float erosion, TerrainSurfaceKind surfaceKind,
    const NoiseTerrainSettings& settings) noexcept
{
    // Ocean and beach columns: keep the floor smooth.
    if (surfaceKind != TerrainSurfaceKind::Land) {
        return settings.overhangBaseAmp;
    }

    // Map peaksValleys [-1,1] → [0,1] but bias so only the upper half
    // produces meaningful amplitude. Flat plains (pv near -1) → 0; high
    // peaks (pv near 1) → 1.
    const float pv01 = std::clamp(peaksValleys * 0.5F + 0.5F, 0.0F, 1.0F);
    const float pvIntensity = std::clamp((pv01 - 0.35F) / 0.45F, 0.0F, 1.0F);

    // Low erosion = jagged, eroded = smooth. Erosion ∈ [-1, 1]: highly
    // eroded (positive) = pancake, jagged (negative) = mountainous.
    const float er01 = std::clamp(-erosion * 0.5F + 0.5F, 0.0F, 1.0F);
    const float erIntensity = std::clamp((er01 - 0.45F) / 0.40F, 0.0F, 1.0F);

    // Combine: both factors must be high to get big overhangs. This
    // confines them to genuine mountain/badlands columns without needing
    // to branch on the biome enum.
    const float intensity = pvIntensity * erIntensity;
    const float amp = settings.overhangAmplitude * intensity;
    return std::max(settings.overhangBaseAmp, amp);
}

// ---------------------------------------------------------------------------
// Stage 1: Base terrain — fills stone up to the column's surface height.
// Subsurface soil and the surface block are written by SurfacePaintStage,
// which overwrites the top portion of this stone column. We use the batch
// `fillColumnRangeSilently` here because writing one contiguous stone band
// per column is the cheapest path (single palette lookup, single bit-pack
// run). The double-write cost (stone here, then dirt/grass/sand by paint)
// is a few hundred nanoseconds per column and is dwarfed by the noise
// sampling work.

void runBaseTerrainStage(TerrainPipelineContext& ctx)
{
    const auto& prepass = ctx.prepass;
    const auto& settings = ctx.settings;
    const auto& bounds = ctx.bounds;
    Chunk& chunk = ctx.chunk;

    // Initialize actual-top map (Phase 7). If unpopulated, downstream stages
    // fall back to prepass.surfaceY (Phase <=6 behavior). The baseline must
    // be the true column surface even for water-only chunks above a deep
    // seabed; otherwise the fluid stage treats the top of each vertical chunk
    // as solid and creates 32-block stepped ocean surfaces.
    if (ctx.actualTop != nullptr) {
        for (int z = 0; z < ChunkSize; ++z) {
            for (int x = 0; x < ChunkSize; ++x) {
                const auto prepassIdx = columnIndex(x, z);
                ctx.actualTop->worldY[prepassIdx] = prepass.surfaceBlockY[prepassIdx];
            }
        }
        ctx.actualTop->populated = true;
    }
    if (bounds.maxSolidTop < 0) {
        return;
    }
    const bool overhangsActive = (ctx.actualTop != nullptr) && ctx.density.overhangSampled;

    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            const auto prepassIdx = columnIndex(x, z);
            const int surfaceBlockY = prepass.surfaceBlockY[prepassIdx];
            const int solidTop = std::clamp(surfaceBlockY - ctx.chunkBaseBlockY, -1, ChunkSize - 1);
            if (solidTop < 0) {
                if (ctx.actualTop != nullptr) {
                    ctx.actualTop->worldY[prepassIdx] = surfaceBlockY; // baseline
                }
                continue;
            }

            // ---- Heightmap path (no overhangs for this column) ----
            // Amplitude is driven by SMOOTH signals (peaksValleys, erosion)
            // rather than the discrete biome enum, so adjacent columns never
            // disagree by more than the supersampled interpolation gradient.
            // This is the fix for the cliff-seam at biome boundaries.
            const float amp = overhangsActive
                ? terrainAmplitude(
                    prepass.peaksValleys[prepassIdx],
                    prepass.erosion[prepassIdx],
                    prepass.surfaceKind[prepassIdx],
                    settings)
                : 0.0F;
            if (amp <= 0.0F) {
                chunk.fillColumnRangeSilently(x, z, 0, solidTop, settings.stoneBlock);
                if (ctx.actualTop != nullptr) {
                    ctx.actualTop->worldY[prepassIdx] = surfaceBlockY;
                }
                continue;
            }

            // ---- Density path: overhang noise shifts the iso-surface ----
            // density(y) = (surfaceBlockY + overhang(x,y,z) * amp) - y
            // We fill stone where density > 0 in the band [surfaceY-amp, surfaceY+amp];
            // outside the band the answer is trivially solid/air.
            const int bandLocalLo = std::clamp(
                static_cast<int>(std::floor(surfaceBlockY - amp)) - ctx.chunkBaseBlockY,
                0, ChunkSize - 1);
            const int bandLocalHi = std::clamp(
                static_cast<int>(std::ceil(surfaceBlockY + amp)) - ctx.chunkBaseBlockY,
                0, ChunkSize - 1);
            // Below the band: pure stone.
            if (bandLocalLo > 0) {
                chunk.fillColumnRangeSilently(x, z, 0, bandLocalLo - 1, settings.stoneBlock);
            }
            // Inside the band: per-block density check.
            int newTop = surfaceBlockY; // will refine
            int highestStoneLocalY = -1;
            for (int y = bandLocalLo; y <= bandLocalHi; ++y) {
                const float worldY = static_cast<float>(ctx.chunkBaseBlockY + y);
                const float noise = TerrainDensityField::densityAt(ctx.density.overhang, x, y, z);
                const float density = (static_cast<float>(surfaceBlockY) + noise * amp) - worldY;
                if (density > 0.0F) {
                    chunk.setBlockSilently(x, y, z, settings.stoneBlock);
                    if (y > highestStoneLocalY) highestStoneLocalY = y;
                }
            }
            if (highestStoneLocalY >= 0) {
                newTop = ctx.chunkBaseBlockY + highestStoneLocalY;
            } else if (bandLocalLo > 0) {
                // Whole band was air; the highest stone is bandLocalLo-1.
                newTop = ctx.chunkBaseBlockY + bandLocalLo - 1;
            } else {
                // Whole column was carved away in the band starting at y=0.
                newTop = std::numeric_limits<std::int32_t>::min();
            }
            if (ctx.actualTop != nullptr) {
                ctx.actualTop->worldY[prepassIdx] = newTop;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 2: Surface paint — overwrites the top of each column with the
// biome-appropriate soil cap. After this stage:
//   - solidTop                          = topBlock (grass, sand, snow, basalt, ...)
//   - [solidTop - soilDepth, solidTop-1] = subSurfaceBlock (dirt, sandstone, ...)
//   - everything below                  = stone (written by base terrain)
//
// Cave carving runs AFTER this stage but only targets the stone band
// (depthFromSurface > caveCeilingOffset), so it never touches the cap.

// Resolves the actual surface world-Y for a column. Uses Phase 7's
// per-column top if the overhang stage populated it; otherwise falls back
// to the prepass heightmap value.
[[nodiscard]] inline std::int32_t resolveActualSurfaceY(
    const TerrainPipelineContext& ctx, std::size_t prepassIdx) noexcept
{
    if (ctx.actualTop != nullptr) {
        if (!ctx.actualTop->populated) {
            return ctx.prepass.surfaceBlockY[prepassIdx];
        }
        const auto y = ctx.actualTop->worldY[prepassIdx];
        if (y != std::numeric_limits<std::int32_t>::min()) {
            return y;
        }
    }
    return ctx.prepass.surfaceBlockY[prepassIdx];
}

void runSurfacePaintStage(TerrainPipelineContext& ctx)
{
    const auto& prepass = ctx.prepass;
    const auto& settings = ctx.settings;
    const auto& bounds = ctx.bounds;
    Chunk& chunk = ctx.chunk;
    if (bounds.maxSolidTop < 0) {
        return;
    }

    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            const auto prepassIdx = columnIndex(x, z);
            const int surfaceBlockY = resolveActualSurfaceY(ctx, prepassIdx);
            const int solidTop = std::clamp(surfaceBlockY - ctx.chunkBaseBlockY, -1, ChunkSize - 1);
            if (solidTop < 0) {
                continue;
            }
            // Phase 7: the actual top might not actually have stone underneath
            // (overhang noise could have created a thin ledge). Skip painting
            // if the block at solidTop is not stone — it's already painted or
            // already air.
            if (chunk.blockAt(x, solidTop, z).value != settings.stoneBlock.value) {
                continue;
            }
            const auto cap = resolveSoilCap(
                prepass.surfaceKind[prepassIdx],
                prepass.biome[prepassIdx],
                prepass.riverCandidateMask[prepassIdx],
                settings);

            // Subsurface band first (uses batch fill), then the top block.
            // We only overwrite blocks that are still stone — overhang voids
            // (air pockets carved into the band) must stay as air.
            const int subBottom = std::max(0, solidTop - cap.soilDepth);
            const int subTop = solidTop - 1;
            for (int y = subTop; y >= subBottom; --y) {
                if (chunk.blockAt(x, y, z).value == settings.stoneBlock.value) {
                    chunk.setBlockSilently(x, y, z, cap.subSurfaceBlock);
                }
            }
            chunk.setBlockSilently(x, solidTop, z, cap.topBlock);
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 3: Cave carve — three-layer cave system.
//
//   1. Cheese caves : large 3D fbm blobs above caveThreshold
//   2. Spaghetti    : intersection of two iso-surfaces => thin tunnels
//   3. Ravine       : 2D ridged noise in XZ + Y-window mask => vertical slabs
//
// A block is carved if *any* layer says "carve". Each layer has its own
// depth gating (caveCeilingOffset for cheese, spaghettiMinDepth for tunnels,
// ravineMinDepth..ravineMaxDepth for ravines). Below sea level, carved
// blocks become water; above, they become air.
//
// Performance: 3 fields × 8-corner cellMayExceed = 24 cheap reads per
// candidate block. When all three short-circuit (most blocks), we never
// do the expensive trilerp. In dense cave regions, 2-3 trilerps per block
// is still much cheaper than meshing.

void runCaveCarveStage(TerrainPipelineContext& ctx)
{
    if (!ctx.bounds.needsCaveSamples) {
        return;
    }
    const auto& prepass = ctx.prepass;
    const auto& settings = ctx.settings;
    auto& density = ctx.density;
    Chunk& chunk = ctx.chunk;
    const float ravineBandSq = settings.ravineBandHalfWidth * settings.ravineBandHalfWidth;

    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            const auto prepassIdx = columnIndex(x, z);
            const int surfaceBlockY = prepass.surfaceBlockY[prepassIdx];
            const int solidTop = std::clamp(surfaceBlockY - ctx.chunkBaseBlockY, -1, ChunkSize - 1);
            const int stoneTop = std::min(solidTop - 4, ChunkSize - 1);

            // 2D ravine ridge value is the same for the whole column — sample once.
            // ridgedDistance = |value - 0.5|, smaller = closer to the ridge line.
            float ravineRidgeDistSq = 1.0F;
            if (density.ravineSampled) {
                const float ravineValue = TerrainDensityField::density2DAt(density.ravine2D, x, z);
                const float d = ravineValue - 0.5F;
                ravineRidgeDistSq = d * d;
            }

            for (int y = 0; y <= stoneTop; ++y) {
                const float worldY = ctx.worldBaseY + static_cast<float>(y);
                const int depthFromSurface = surfaceBlockY - static_cast<int>(worldY);
                if (depthFromSurface <= static_cast<int>(settings.caveCeilingOffset)) {
                    continue;
                }

                bool carve = false;

                // Layer 1: cheese caves.
                if (density.cheeseSampled
                    && TerrainDensityField::cellMayExceed(density.cheese, settings.caveThreshold, x, y, z)) {
                    if (TerrainDensityField::densityAt(density.cheese, x, y, z) > settings.caveThreshold) {
                        carve = true;
                    }
                }

                // Layer 2: spaghetti tunnels. Carve where both noise fields are
                // within bandHalfWidth of 0.5 (their iso-surfaces both pass
                // through this point). Squared distance avoids two `abs` calls.
                if (!carve
                    && density.spaghettiSampled
                    && depthFromSurface >= settings.spaghettiMinDepth
                    && depthFromSurface <= settings.spaghettiMaxDepth) {
                    const float a = TerrainDensityField::densityAt(density.spaghettiA, x, y, z) - 0.5F;
                    const float b = TerrainDensityField::densityAt(density.spaghettiB, x, y, z) - 0.5F;
                    const float band = settings.spaghettiBandHalfWidth;
                    if (a * a < band * band && b * b < band * band) {
                        carve = true;
                    }
                }

                // Layer 3: ravines. Vertical slab where the 2D ridge is close
                // to its midline AND we're inside the Y depth window.
                if (!carve
                    && density.ravineSampled
                    && depthFromSurface >= settings.ravineMinDepth
                    && depthFromSurface <= settings.ravineMaxDepth
                    && ravineRidgeDistSq < ravineBandSq) {
                    carve = true;
                }

                if (!carve) {
                    continue;
                }

                if (worldY <= settings.seaLevel) {
                    chunk.setBlockSilently(x, y, z, settings.waterBlock);
                } else {
                    chunk.setBlockSilently(x, y, z, AirBlockState);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 4: Ore — replaces stone with coal/iron where ore density exceeds.
// Skips cells that aren't currently stone (caves carved them, or future
// stages already wrote something). Preserves the prior behavior where caves
// won over ores via `continue`.

void runOreStage(TerrainPipelineContext& ctx)
{
    if (!ctx.density.oreSampled || ctx.bounds.maxStoneTop < 0) {
        return;
    }
    const auto& prepass = ctx.prepass;
    const auto& settings = ctx.settings;
    Chunk& chunk = ctx.chunk;
    const auto stoneId = settings.stoneBlock.value;

    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            const auto prepassIdx = columnIndex(x, z);
            const int surfaceBlockY = prepass.surfaceBlockY[prepassIdx];
            const int solidTop = std::clamp(surfaceBlockY - ctx.chunkBaseBlockY, -1, ChunkSize - 1);
            const int stoneTop = std::min(solidTop - 4, ChunkSize - 1);
            for (int y = 0; y <= stoneTop; ++y) {
                // Skip cells where caves (or anything else) already replaced stone.
                if (chunk.blockAt(x, y, z).value != stoneId) {
                    continue;
                }
                const float worldY = ctx.worldBaseY + static_cast<float>(y);
                const int depthFromSurface = surfaceBlockY - static_cast<int>(worldY);

                BlockStateId oreBlock = settings.stoneBlock;
                if (depthFromSurface >= settings.coalMinDepth
                    && TerrainDensityField::cellMayExceed(ctx.density.coal, settings.coalThreshold, x, y, z)) {
                    if (TerrainDensityField::densityAt(ctx.density.coal, x, y, z) > settings.coalThreshold) {
                        oreBlock = settings.coalOreBlock;
                    }
                }
                if (oreBlock.value == stoneId
                    && depthFromSurface >= settings.ironMinDepth
                    && TerrainDensityField::cellMayExceed(ctx.density.iron, settings.ironThreshold, x, y, z)) {
                    if (TerrainDensityField::densityAt(ctx.density.iron, x, y, z) > settings.ironThreshold) {
                        oreBlock = settings.ironOreBlock;
                    }
                }
                if (oreBlock.value != stoneId) {
                    chunk.setBlockSilently(x, y, z, oreBlock);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 5: Fluid — fills water above each column's solidTop up to sea level.
// Caves below sea level were already filled with water by the carve stage.

void runFluidStage(TerrainPipelineContext& ctx)
{
    if (ctx.bounds.seaTopForChunk < 0) {
        return;
    }
    const auto& settings = ctx.settings;
    Chunk& chunk = ctx.chunk;
    const int seaTop = ctx.bounds.seaTopForChunk;

    for (int z = 0; z < ChunkSize; ++z) {
        for (int x = 0; x < ChunkSize; ++x) {
            const auto prepassIdx = columnIndex(x, z);
            // Phase 7: use the actual top (after overhang carving) rather
            // than the prepass surfaceY. Overhang-carved cliff faces sitting
            // below sea level get flooded correctly this way.
            const int surfaceBlockY = resolveActualSurfaceY(ctx, prepassIdx);
            const int solidTop = std::clamp(surfaceBlockY - ctx.chunkBaseBlockY, -1, ChunkSize - 1);
            if (seaTop > solidTop) {
                // Only fill air cells — don't trample stone left by an overhang
                // above sea level on the same column.
                for (int y = solidTop + 1; y <= seaTop; ++y) {
                    if (chunk.blockAt(x, y, z).value == AirBlockState.value) {
                        chunk.setBlockSilently(x, y, z, settings.waterBlock);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 6: Structures — deterministic per-region placement of small features
// (boulders, ruined pillars, shipwrecks).
//
// Each StructureRegion has one chance to spawn a structure. The structure
// kind, position offset within the region, and per-instance shape variation
// are all derived from a single hash of (regionX, regionZ, seed). Every chunk
// that the structure's footprint touches independently runs the same hash,
// derives the same structure, and writes the blocks that fall inside it.
// No cross-chunk state, no neighbor handshake.

namespace structures {

enum class StructureKind : std::uint8_t {
    None = 0,
    Boulder,
    Pillar,
    Shipwreck,
};

struct StructurePlacement {
    StructureKind kind{StructureKind::None};
    std::int32_t worldX{};        // origin (lower-XZ corner of footprint)
    std::int32_t worldY{};        // ground anchor (bottom of structure)
    std::int32_t worldZ{};
    std::uint32_t shapeHash{};    // per-instance shape variation seed
};

constexpr int kStructureRegionSize = 96;      // 3 chunks wide
constexpr int kStructureMaxFootprint = 8;     // max XZ extent of any structure
constexpr std::uint32_t kStructureSeedSalt = 0xC2A1F4D7U;

// Block-write helper: replaces only certain existing-block types.
enum ReplaceMask : std::uint8_t {
    ReplaceAir   = 1U << 0U,
    ReplaceWater = 1U << 1U,
};

inline bool placeStructureBlock(Chunk& chunk, int localX, int localY, int localZ,
                                BlockStateId block, std::uint8_t replaceMask,
                                std::uint32_t waterId) noexcept
{
    if (localX < 0 || localX >= ChunkSize) return false;
    if (localY < 0 || localY >= ChunkSize) return false;
    if (localZ < 0 || localZ >= ChunkSize) return false;
    const auto existing = chunk.blockAt(localX, localY, localZ).value;
    const bool isAir   = existing == AirBlockState.value;
    const bool isWater = existing == waterId;
    if ((replaceMask & ReplaceAir)   && isAir) {
        chunk.setBlockSilently(localX, localY, localZ, block);
        return true;
    }
    if ((replaceMask & ReplaceWater) && isWater) {
        chunk.setBlockSilently(localX, localY, localZ, block);
        return true;
    }
    return false;
}

// Decide which structure (if any) lives in a region, and where.
[[nodiscard]] StructurePlacement sampleStructureForRegion(
    std::int32_t regionX, std::int32_t regionZ,
    const NoiseTerrainSettings& settings,
    const NoiseTerrainGenerator& generator) noexcept
{
    const std::uint32_t h = core::hash3D(regionX, 0, regionZ,
        settings.seed ^ kStructureSeedSalt);

    // Roll: about 60% empty, then split among the kinds.
    const float roll = static_cast<float>(h & 0xFFFFU) / 65536.0F;
    StructureKind kind = StructureKind::None;
    if (roll < 0.40F) {
        kind = StructureKind::None;
    } else if (roll < 0.70F) {
        kind = StructureKind::Boulder;
    } else if (roll < 0.88F) {
        kind = StructureKind::Pillar;
    } else {
        kind = StructureKind::Shipwreck;
    }
    if (kind == StructureKind::None) {
        return {};
    }

    // Position within the region (leave room for the footprint).
    const int slack = kStructureRegionSize - kStructureMaxFootprint;
    const int dx = static_cast<int>((h >> 16) & 0x7FU) % slack;
    const int dz = static_cast<int>((h >> 24) & 0x7FU) % slack;
    const std::int32_t worldX = regionX * kStructureRegionSize + dx;
    const std::int32_t worldZ = regionZ * kStructureRegionSize + dz;

    // Look up surface conditions at the structure's center.
    const float centerX = static_cast<float>(worldX + kStructureMaxFootprint / 2);
    const float centerZ = static_cast<float>(worldZ + kStructureMaxFootprint / 2);
    const auto col = generator.sampleColumnAt(centerX, centerZ);

    // W0 fix: keep land structures off the shoreline. A column whose
    // surface is within `kShorelineSkip` blocks of sea level will spawn
    // boulders/pillars that stick partway out of the water — looks like
    // "floating stairs" in the sea. Require a healthy margin above water
    // for any land-based structure.
    constexpr int kShorelineSkip = 4;
    const int aboveSeaLevel = col.surfaceY - col.seaLevel;

    // Biome / surface gating per kind.
    switch (kind) {
        case StructureKind::Boulder: {
            // Land only, not rivers, not deserts (boulders look weird in pure sand).
            if (col.isOcean || col.isRiverCandidate) return {};
            if (col.biome == TerrainBiomeId::Desert) return {};
            if (aboveSeaLevel < kShorelineSkip) return {};
            return {kind, worldX, col.surfaceY + 1, worldZ, h};
        }
        case StructureKind::Pillar: {
            if (col.isOcean || col.isRiverCandidate) return {};
            // Pillars perch on the surface — accept any land biome, but
            // keep them well above the waterline so the base doesn't flood.
            if (aboveSeaLevel < kShorelineSkip) return {};
            return {kind, worldX, col.surfaceY + 1, worldZ, h};
        }
        case StructureKind::Shipwreck: {
            // Shallow oceans only — surface needs to be below sea level but
            // not in the deep abyss (so divers can find it later).
            if (!col.isOcean) return {};
            const int depthBelowSea = col.seaLevel - col.surfaceY;
            if (depthBelowSea < 3 || depthBelowSea > 28) return {};
            return {kind, worldX, col.surfaceY + 1, worldZ, h};
        }
        case StructureKind::None:
            break;
    }
    return {};
}

// ---- Per-kind placement functions ------------------------------------------

void placeBoulder(TerrainPipelineContext& ctx, const StructurePlacement& p) noexcept
{
    // 3-wide noisy cluster of mossy_stone, 2 blocks tall, centered on origin.
    const auto& s = ctx.settings;
    const int chunkX0 = static_cast<int>(ctx.worldBaseX);
    const int chunkY0 = ctx.chunkBaseBlockY;
    const int chunkZ0 = static_cast<int>(ctx.worldBaseZ);
    for (int dy = 0; dy < 2; ++dy) {
        for (int dz = 0; dz < 3; ++dz) {
            for (int dx = 0; dx < 3; ++dx) {
                // Per-block presence via hash — gives an irregular silhouette.
                const std::uint32_t hh = core::hash3D(dx, dy, dz, p.shapeHash);
                const bool present = (hh & 0xFFU) < 200U; // ~78% chance per slot
                if (!present) continue;
                placeStructureBlock(ctx.chunk,
                    (p.worldX + dx) - chunkX0,
                    (p.worldY + dy) - chunkY0,
                    (p.worldZ + dz) - chunkZ0,
                    s.mossyStoneBlock, ReplaceAir, s.waterBlock.value);
            }
        }
    }
}

void placePillar(TerrainPipelineContext& ctx, const StructurePlacement& p) noexcept
{
    // Single-column ruin: stone pedestal of height 4-6 with a sandstone cap.
    const auto& s = ctx.settings;
    const int chunkX0 = static_cast<int>(ctx.worldBaseX);
    const int chunkY0 = ctx.chunkBaseBlockY;
    const int chunkZ0 = static_cast<int>(ctx.worldBaseZ);
    const int height = 4 + static_cast<int>((p.shapeHash >> 8) % 3U); // 4..6
    const int cx = p.worldX + 1;
    const int cz = p.worldZ + 1;
    for (int dy = 0; dy < height; ++dy) {
        const bool top = (dy == height - 1);
        const BlockStateId block = top ? s.sandstoneBlock : s.stoneBlock;
        placeStructureBlock(ctx.chunk,
            cx - chunkX0,
            (p.worldY + dy) - chunkY0,
            cz - chunkZ0,
            block, ReplaceAir, s.waterBlock.value);
    }
    // Crumbled top: one or two pieces fallen at the base in random direction.
    if ((p.shapeHash & 0xFU) >= 4U) {
        const int rubbleDx = ((p.shapeHash >> 4) & 1U) ? 1 : -1;
        const int rubbleDz = ((p.shapeHash >> 5) & 1U) ? 1 : -1;
        placeStructureBlock(ctx.chunk,
            (cx + rubbleDx) - chunkX0,
            p.worldY - chunkY0,
            (cz + rubbleDz) - chunkZ0,
            s.stoneBlock, ReplaceAir, s.waterBlock.value);
    }
}

void placeShipwreck(TerrainPipelineContext& ctx, const StructurePlacement& p) noexcept
{
    // 5-long oak_log hull, 3 wide, with a 2-block-tall mast amidships.
    // Replaces water (it's submerged) and air (parts may stick up).
    const auto& s = ctx.settings;
    const int chunkX0 = static_cast<int>(ctx.worldBaseX);
    const int chunkY0 = ctx.chunkBaseBlockY;
    const int chunkZ0 = static_cast<int>(ctx.worldBaseZ);
    constexpr std::uint8_t kReplaceAirOrWater = ReplaceAir | ReplaceWater;

    // Hull rows: 5 long along X, 3 wide along Z, 2 tall.
    for (int dy = 0; dy < 2; ++dy) {
        for (int dz = 0; dz < 3; ++dz) {
            for (int dx = 0; dx < 5; ++dx) {
                // Hull is taller in the middle, lower at bow/stern.
                const bool dropEdgeTop = (dy == 1) && (dx == 0 || dx == 4);
                if (dropEdgeTop) continue;
                // Per-instance deterioration — some hull boards missing.
                const std::uint32_t hh = core::hash3D(dx, dy, dz, p.shapeHash ^ 0x53A1U);
                if ((hh & 0xFFU) < 40U) continue; // ~16% missing
                placeStructureBlock(ctx.chunk,
                    (p.worldX + dx) - chunkX0,
                    (p.worldY + dy) - chunkY0,
                    (p.worldZ + dz) - chunkZ0,
                    s.oakLogBlock, kReplaceAirOrWater, s.waterBlock.value);
            }
        }
    }
    // Mast: amidships, 2 blocks tall above the deck.
    for (int dy = 2; dy < 4; ++dy) {
        placeStructureBlock(ctx.chunk,
            (p.worldX + 2) - chunkX0,
            (p.worldY + dy) - chunkY0,
            (p.worldZ + 1) - chunkZ0,
            s.oakLogBlock, kReplaceAirOrWater, s.waterBlock.value);
    }
}

[[nodiscard]] constexpr int floorDivStruct(int a, int b) noexcept
{
    return (a >= 0) ? (a / b) : (-((-a + b - 1) / b));
}

} // namespace structures

void runStructureStage(TerrainPipelineContext& ctx)
{
    using namespace structures;
    if (ctx.generator == nullptr) {
        return;
    }

    const int chunkX0 = static_cast<int>(ctx.worldBaseX);
    const int chunkY0 = ctx.chunkBaseBlockY;
    const int chunkZ0 = static_cast<int>(ctx.worldBaseZ);
    const int chunkXmax = chunkX0 + ChunkSize - 1;
    const int chunkZmax = chunkZ0 + ChunkSize - 1;

    // Determine which regions could intersect this chunk's XZ extent.
    // A structure's footprint is at most kStructureMaxFootprint, so a region
    // at boundary R..R+regionSize potentially writes blocks up to R+regionSize+footprint.
    const int rXmin = floorDivStruct(chunkX0 - kStructureMaxFootprint, kStructureRegionSize);
    const int rXmax = floorDivStruct(chunkXmax, kStructureRegionSize);
    const int rZmin = floorDivStruct(chunkZ0 - kStructureMaxFootprint, kStructureRegionSize);
    const int rZmax = floorDivStruct(chunkZmax, kStructureRegionSize);

    for (int rZ = rZmin; rZ <= rZmax; ++rZ) {
        for (int rX = rXmin; rX <= rXmax; ++rX) {
            const auto placement = sampleStructureForRegion(rX, rZ, ctx.settings, *ctx.generator);
            if (placement.kind == StructureKind::None) continue;

            // Quick Y-bounds check: structures are at most 6 blocks tall
            // (pillar). If the structure's Y range is fully outside this
            // chunk's Y slab, skip.
            const int structYTop = placement.worldY + 5;
            const int chunkYTop = chunkY0 + ChunkSize - 1;
            if (placement.worldY > chunkYTop || structYTop < chunkY0) continue;

            // Quick XZ-overlap check: skip if footprint can't intersect chunk.
            const int structXEnd = placement.worldX + kStructureMaxFootprint;
            const int structZEnd = placement.worldZ + kStructureMaxFootprint;
            if (structXEnd < chunkX0 || placement.worldX > chunkXmax) continue;
            if (structZEnd < chunkZ0 || placement.worldZ > chunkZmax) continue;

            switch (placement.kind) {
                case StructureKind::Boulder:   placeBoulder(ctx, placement);   break;
                case StructureKind::Pillar:    placePillar(ctx, placement);    break;
                case StructureKind::Shipwreck: placeShipwreck(ctx, placement); break;
                case StructureKind::None:      break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 7 (was 6): Foliage — places biome-appropriate trees.
//
// Placement strategy: the world is divided into kTreeCellSize-sized cells in
// XZ. Each cell hashes deterministically to (treeExists, offsetInCell,
// trunkHeight). For each cell whose canopy could intersect this chunk, we
// look up the surface height + biome at the tree center (from the prepass
// if the center is in this chunk, or via generator->sampleColumnAt for cells
// just outside), then write the trunk and canopy blocks that fall within
// this chunk.
//
// Cross-chunk seamlessness: every chunk that the canopy touches independently
// computes the same hash → same tree → identical block writes. No state
// passed across chunks; no neighbor-aware coordination required.
//
// Skips: river-channel columns, ocean columns, non-grass surfaces (sand/
// snow/basalt biomes already opt out via density=0 in the rule table).

namespace foliage {

struct TreeRule {
    float density;       // Probability per cell that a tree spawns (0..1).
    int minTrunkHeight;
    int maxTrunkHeight;
    int canopyRadius;    // Half-width of the cube canopy (1-3 typical).
    int canopyHeight;    // Vertical extent of the canopy above trunk top.
    bool valid;
};

[[nodiscard]] TreeRule resolveTreeRule(TerrainBiomeId biome) noexcept
{
    switch (biome) {
        case TerrainBiomeId::Plains:        return {0.05F, 4, 5, 2, 2, true};
        case TerrainBiomeId::Forest:        return {0.30F, 4, 6, 2, 2, true};
        case TerrainBiomeId::DenseForest:   return {0.55F, 5, 7, 2, 3, true};
        case TerrainBiomeId::RedwoodForest: return {0.32F, 8, 13, 2, 5, true};
        case TerrainBiomeId::LushHighlandsValley:
                                            return {0.28F, 5, 8, 2, 3, true};
        case TerrainBiomeId::Jungle:        return {0.62F, 6, 10, 3, 4, true};
        case TerrainBiomeId::Swamp:         return {0.18F, 3, 5, 2, 2, true};
        case TerrainBiomeId::Tundra:        return {0.015F, 3, 4, 1, 1, true};
        case TerrainBiomeId::Taiga:         return {0.40F, 5, 8, 1, 3, true}; // narrow tall pines
        case TerrainBiomeId::Savanna:       return {0.06F, 4, 6, 3, 1, true}; // wide flat acacia-like
        case TerrainBiomeId::MagicalGrove:  return {0.35F, 6, 10, 3, 4, true};
        case TerrainBiomeId::FloatingIslands:
                                            return {0.18F, 5, 8, 2, 3, true};
        case TerrainBiomeId::Beach:         return {0.02F, 4, 5, 2, 2, true}; // rare palms
        default:                            return {0.0F, 0, 0, 0, 0, false};
    }
}

constexpr int kTreeCellSize = 5;            // <= sqrt of typical canopy area
constexpr int kCanopyMaxReach = 3;          // worst case canopyRadius across all rules
constexpr std::uint32_t kFoliageSeedSalt = 0x84A38275U;

// Floor-division that works for negative numerators (std::div has the wrong sign).
[[nodiscard]] constexpr int floorDivInt(int a, int b) noexcept
{
    return (a >= 0) ? (a / b) : (-((-a + b - 1) / b));
}

// Writes one block into the chunk if it's currently air and the local coord
// is in range. Returns true if the write happened.
inline bool placeFoliageBlock(Chunk& chunk, int localX, int localY, int localZ, BlockStateId block) noexcept
{
    if (localX < 0 || localX >= ChunkSize) return false;
    if (localY < 0 || localY >= ChunkSize) return false;
    if (localZ < 0 || localZ >= ChunkSize) return false;
    if (chunk.blockAt(localX, localY, localZ).value != AirBlockState.value) return false;
    chunk.setBlockSilently(localX, localY, localZ, block);
    return true;
}

void emitTree(TerrainPipelineContext& ctx,
              int treeWorldX, int treeSurfaceY, int treeWorldZ,
              int trunkHeight, const TreeRule& rule) noexcept
{
    const int chunkX0 = static_cast<int>(ctx.worldBaseX);
    const int chunkY0 = ctx.chunkBaseBlockY;
    const int chunkZ0 = static_cast<int>(ctx.worldBaseZ);
    const int localCx = treeWorldX - chunkX0;
    const int localCz = treeWorldZ - chunkZ0;

    // Trunk: one column from (treeSurfaceY+1) up to (treeSurfaceY+trunkHeight).
    for (int dy = 1; dy <= trunkHeight; ++dy) {
        const int localY = (treeSurfaceY + dy) - chunkY0;
        placeFoliageBlock(ctx.chunk, localCx, localY, localCz, ctx.settings.oakLogBlock);
    }

    // Canopy: a small dome centered at trunk top, extending canopyHeight above.
    // Slightly narrower on top and bottom slices for a rounded look.
    const int canopyBaseY = treeSurfaceY + trunkHeight;
    for (int dy = 0; dy <= rule.canopyHeight; ++dy) {
        const int worldY = canopyBaseY + dy;
        const int localY = worldY - chunkY0;
        if (localY < 0 || localY >= ChunkSize) continue;

        // Taper: top and bottom slices use one less radius for a dome shape.
        int radius = rule.canopyRadius;
        if (dy == 0 || dy == rule.canopyHeight) {
            radius = std::max(0, rule.canopyRadius - 1);
        }

        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                // Round the corners off when at full radius.
                if (std::abs(dx) == radius && std::abs(dz) == radius && radius >= 2) {
                    continue;
                }
                placeFoliageBlock(ctx.chunk, localCx + dx, localY, localCz + dz, ctx.settings.leavesBlock);
            }
        }
    }
}

} // namespace foliage

void runFoliageStage(TerrainPipelineContext& ctx)
{
    using namespace foliage;
    if (ctx.generator == nullptr) {
        return;
    }
    // No foliage in chunks that are entirely below the highest surface — only
    // chunks straddling the surface band can host trees.
    if (ctx.bounds.maxSolidTop < 0 && ctx.bounds.seaTopForChunk < 0) {
        return;
    }

    const int chunkX0 = static_cast<int>(ctx.worldBaseX);
    const int chunkZ0 = static_cast<int>(ctx.worldBaseZ);

    // Cells whose tree+canopy footprint could intersect our XZ extent.
    const int chunkXmin = chunkX0 - kCanopyMaxReach;
    const int chunkXmax = chunkX0 + ChunkSize + kCanopyMaxReach;
    const int chunkZmin = chunkZ0 - kCanopyMaxReach;
    const int chunkZmax = chunkZ0 + ChunkSize + kCanopyMaxReach;
    const int cellXmin = floorDivInt(chunkXmin, kTreeCellSize);
    const int cellXmax = floorDivInt(chunkXmax, kTreeCellSize);
    const int cellZmin = floorDivInt(chunkZmin, kTreeCellSize);
    const int cellZmax = floorDivInt(chunkZmax, kTreeCellSize);

    for (int cellZ = cellZmin; cellZ <= cellZmax; ++cellZ) {
        for (int cellX = cellXmin; cellX <= cellXmax; ++cellX) {
            // Deterministic per-cell hash. y=0 (we're working in XZ).
            const std::uint32_t h = core::hash3D(cellX, 0, cellZ,
                ctx.settings.seed ^ kFoliageSeedSalt);

            // Tree position within cell.
            const int dx = static_cast<int>((h >>  0) & 0xFFU) % kTreeCellSize;
            const int dz = static_cast<int>((h >>  8) & 0xFFU) % kTreeCellSize;
            const int worldX = cellX * kTreeCellSize + dx;
            const int worldZ = cellZ * kTreeCellSize + dz;

            // Look up surface + biome + river status at the tree center.
            // Prepass for cells inside this chunk (cheap), generator sampler
            // for cells outside (one fbm round, ~1µs).
            int treeSurfaceY;
            TerrainBiomeId biome;
            bool isRiverChannel;
            const bool inChunk = (worldX >= chunkX0 && worldX < chunkX0 + ChunkSize
                               && worldZ >= chunkZ0 && worldZ < chunkZ0 + ChunkSize);
            if (inChunk) {
                const int localX = worldX - chunkX0;
                const int localZ = worldZ - chunkZ0;
                const auto idx = columnIndex(localX, localZ);
                // Phase 7: prefer the post-overhang actual top so trees plant
                // on real surface blocks rather than floating where the
                // prepass heightmap would have been.
                treeSurfaceY = resolveActualSurfaceY(ctx, idx);
                biome = ctx.prepass.biome[idx];
                isRiverChannel = ctx.prepass.riverCandidateMask[idx];
            } else {
                const auto column = ctx.generator->sampleColumnAt(
                    static_cast<float>(worldX), static_cast<float>(worldZ));
                treeSurfaceY = column.surfaceY;
                biome = column.biome;
                isRiverChannel = column.isRiverCandidate;
            }

            // Skip river beds — no trees in the water.
            if (isRiverChannel) continue;

            const auto rule = resolveTreeRule(biome);
            if (!rule.valid || rule.density <= 0.0F) continue;

            // Density roll using upper hash bits.
            const float densityRoll = static_cast<float>((h >> 16) & 0xFFFFU) / 65536.0F;
            if (densityRoll >= rule.density) continue;

            // Bail if the tree's footprint can't possibly intersect this chunk
            // vertically — tree top will be treeSurfaceY + trunkHeight + canopyHeight.
            const int trunkRange = rule.maxTrunkHeight - rule.minTrunkHeight + 1;
            const int trunkHeight = rule.minTrunkHeight
                + static_cast<int>((h >> 24) & 0xFFU) % std::max(1, trunkRange);
            const int treeTopY = treeSurfaceY + trunkHeight + rule.canopyHeight;
            const int treeBottomY = treeSurfaceY + 1;
            const int chunkTopY = ctx.chunkBaseBlockY + ChunkSize - 1;
            const int chunkBotY = ctx.chunkBaseBlockY;
            if (treeBottomY > chunkTopY || treeTopY < chunkBotY) continue;

            emitTree(ctx, worldX, treeSurfaceY, worldZ, trunkHeight, rule);
        }
    }
}

} // namespace voxel::world
