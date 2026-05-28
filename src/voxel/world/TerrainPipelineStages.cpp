#include <voxel/world/TerrainPipeline.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <voxel/core/Math.hpp>
#include <voxel/world/BlockState.hpp>

namespace voxel::world {

namespace {

[[nodiscard]] constexpr std::size_t columnIndex(int x, int z) noexcept
{
    return static_cast<std::size_t>(x + (z * ChunkSize));
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

} // namespace

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

} // namespace voxel::world
