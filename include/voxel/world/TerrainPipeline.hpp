#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <voxel/world/Chunk.hpp>
#include <voxel/world/ChunkConstants.hpp>
#include <voxel/world/NoiseTerrainGenerator.hpp>
#include <voxel/world/TerrainColumnPrepass.hpp>

// Layered terrain generation pipeline.
//
// The NoiseTerrainGenerator was historically a single ~230-line function that
// interleaved base-terrain fill, biome surface painting, cave carving, ore
// placement, and water filling. Splitting it into discrete stages makes each
// piece independently testable and allows future stages (foliage, structures,
// density-based overhangs) to slot in without touching unrelated code.
//
// Behavior contract: this refactor is byte-identical to the prior
// implementation when the same stages run in the same order with the same
// settings. The smoke tests' determinism checks (gen1 vs gen2, seed4242 vs
// seed9999) are the regression guard.
namespace voxel::world {

// Per-(chunk, column) cached bounds. Computed once at the top of generate(),
// then read by every stage so each stage can skip work that falls outside
// the chunk's vertical extent.
struct TerrainStageBounds {
    int maxSurfaceBlockY{std::numeric_limits<int>::min()};
    int maxSolidTop{-1};   // highest local y where stone-or-soil exists
    int maxStoneTop{-1};   // highest local y where stone exists (solidTop - 4)
    int seaTopForChunk{-1};
    bool needsCaveSamples{false};

    // No stone, no soil, and no water to fill: the chunk is empty above the
    // surface or below it entirely. Caller can mark generated and skip stages.
    [[nodiscard]] bool empty() const noexcept
    {
        return maxSolidTop < 0 && seaTopForChunk < 0;
    }
};

// Coarse 3D density grids (4-block step) for caves and ores. We sample on
// the coarse grid once, then trilinearly interpolate per-block. The
// `cellMayExceed` short-circuit lets ore/cave stages skip entire 4x4x4
// cells where no corner exceeds the threshold.
//
// Phase 3: three independent cave layers, plus a 2D ridged field for ravines.
//   - cheese (was `cave`)   : large 3D blobs above threshold
//   - spaghetti A/B         : thin tunnels = intersection of two iso-surfaces
//   - ravine                : vertical slabs from a 2D XZ ridged-noise field
//                             (stored in the y=0 plane of the coarse grid)
struct TerrainDensityField {
    static constexpr int kCoarseStep = 4;
    static constexpr int kCoarseSize = (ChunkSize / kCoarseStep) + 1;
    static constexpr std::size_t kSampleCount =
        static_cast<std::size_t>(kCoarseSize) * kCoarseSize * kCoarseSize;
    // 2D ridged field for ravines — only kCoarseSize² samples needed (no Y).
    static constexpr std::size_t kSampleCount2D =
        static_cast<std::size_t>(kCoarseSize) * kCoarseSize;

    std::array<float, kSampleCount>  cheese{};
    std::array<float, kSampleCount>  spaghettiA{};
    std::array<float, kSampleCount>  spaghettiB{};
    std::array<float, kSampleCount2D> ravine2D{};
    std::array<float, kSampleCount>  coal{};
    std::array<float, kSampleCount>  iron{};
    // Phase 7: overhang field. Same coarse grid as caves; trilerp gives
    // smooth surface distortion. Values normalized to [-1, 1] so the pipeline
    // can multiply by a per-column amplitude in blocks.
    std::array<float, kSampleCount>  overhang{};

    bool cheeseSampled{false};
    bool spaghettiSampled{false};
    bool ravineSampled{false};
    bool oreSampled{false};
    bool overhangSampled{false};

    [[nodiscard]] static std::size_t index(int gx, int gy, int gz) noexcept
    {
        return static_cast<std::size_t>(gx)
            + static_cast<std::size_t>(gy) * kCoarseSize
            + static_cast<std::size_t>(gz) * kCoarseSize * kCoarseSize;
    }

    // Trilinear interpolation of one of the coarse grids. Caller passes the
    // chosen sample array (cave/coal/iron).
    [[nodiscard]] static float densityAt(
        const std::array<float, kSampleCount>& samples,
        int x, int y, int z) noexcept;

    // Returns true if any of the 8 corners of the enclosing coarse cell
    // exceed the threshold. Used to skip the (expensive) trilerp when no
    // corner could possibly exceed.
    [[nodiscard]] static bool cellMayExceed(
        const std::array<float, kSampleCount>& samples,
        float threshold,
        int x, int y, int z) noexcept;

    // 2D bilinear interpolation of the XZ ravine field (no Y axis).
    [[nodiscard]] static std::size_t index2D(int gx, int gz) noexcept
    {
        return static_cast<std::size_t>(gx) + static_cast<std::size_t>(gz) * kCoarseSize;
    }
    [[nodiscard]] static float density2DAt(
        const std::array<float, kSampleCount2D>& samples,
        int x, int z) noexcept;
};

// Per-column "actual top" world-Y after base terrain runs. The painter and
// fluid stage read these to find the real surface, which differs from the
// prepass `surfaceY` when overhang noise (Phase 7) shifts the iso-surface.
// Indexed by [z * ChunkSize + x]. Initialized to INT_MIN; populated by
// runBaseTerrainStage when overhangs are active.
struct ActualTopMap {
    std::array<std::int32_t, ChunkSize * ChunkSize> worldY{};
    bool populated{false};
};

// Bundle of everything the stages read. The chunk and density field are
// mutable; everything else is read-only.
struct TerrainPipelineContext {
    Chunk& chunk;
    const TerrainColumnPrepass& prepass;
    const NoiseTerrainSettings& settings;
    const TerrainStageBounds& bounds;
    TerrainDensityField& density;
    int chunkBaseBlockY{};
    float worldBaseX{};
    float worldBaseY{};
    float worldBaseZ{};
    // Phase 5: optional pointer for cross-chunk surface lookups. Stages that
    // need surface heights at arbitrary world XZ (e.g., FoliageStage placing
    // trees whose canopy extends into this chunk from a neighbor) call
    // generator->sampleColumnAt(). Null means "skip such queries".
    const NoiseTerrainGenerator* generator{nullptr};
    // Phase 7: per-column actual top after base terrain. Painter / fluid /
    // foliage read this when populated; otherwise fall back to prepass.surfaceY.
    ActualTopMap* actualTop{nullptr};
};

// Compute per-chunk bounds from the column prepass. Cheap (1024-column scan).
[[nodiscard]] TerrainStageBounds computeTerrainBounds(
    const Chunk& chunk,
    const TerrainColumnPrepass& prepass,
    const NoiseTerrainSettings& settings) noexcept;

// Lazy samplers. Each populates one or more grids in `density`. Safe to call
// when not needed (early-exits via the bounds check), but the orchestrator
// usually checks bounds first to avoid the call overhead.
//
// `sampleCaveDensityField` populates all three cave layers (cheese, spaghetti,
// ravine). Each layer is independently flagged so the carve stage can skip a
// layer if its sampling was disabled (e.g., chunks too high for ravines).
void sampleCaveDensityField(
    TerrainDensityField& density,
    const TerrainStageBounds& bounds,
    const NoiseTerrainSettings& settings,
    float worldBaseX, float worldBaseY, float worldBaseZ);

// Phase 7: sample the 3D overhang noise. Sampled across the chunk's full Y
// extent (the overhang band can be anywhere relative to this slab). Cheaper
// than caves because we only ever use it where it matters — caller is
// expected to skip stages outside the surface ± amplitude band.
void sampleOverhangDensityField(
    TerrainDensityField& density,
    const NoiseTerrainSettings& settings,
    float worldBaseX, float worldBaseY, float worldBaseZ);

void sampleOreDensityFields(
    TerrainDensityField& density,
    const TerrainStageBounds& bounds,
    const NoiseTerrainSettings& settings,
    float worldBaseX, float worldBaseY, float worldBaseZ);

// ---------------------------------------------------------------------------
// Stages. Each is a free function with no internal state. Call order matters:
//   1. BaseTerrain  — fills stone up to surface, dirt in soil band
//   2. SurfacePaint — overwrites top block per biome (grass/sand/snow/...)
//   3. CaveCarve    — replaces stone with air (or water below sea level)
//   4. Ore          — replaces stone with coal/iron where density exceeds
//   5. Fluid        — fills water above the solid top up to sea level
//
// Important ordering note: CaveCarve runs BEFORE Ore, but Ore skips any
// cell that is no longer stone (so ores never overwrite carved air). This
// matches the prior behavior where the carve `continue`'d past the ore check.
// ---------------------------------------------------------------------------

void runBaseTerrainStage(TerrainPipelineContext& ctx);
void runSurfacePaintStage(TerrainPipelineContext& ctx);
void runCaveCarveStage(TerrainPipelineContext& ctx);
void runOreStage(TerrainPipelineContext& ctx);
void runFluidStage(TerrainPipelineContext& ctx);
// Phase 6: place deterministic structures (boulders, ruined pillars,
// shipwrecks). Uses a region-grid approach: the world is divided into NxN
// block regions, each region rolls for one structure type via deterministic
// hash, and every chunk that the structure's footprint touches places its
// own portion of the blocks. No state crosses between chunks.
void runStructureStage(TerrainPipelineContext& ctx);
// Phase 5: place biome-appropriate trees. Uses a cell-based hashing scheme so
// trees whose center is in a neighbor chunk but whose canopy extends into
// this chunk are placed identically by every chunk they touch — no seams.
// Skips river-channel columns (uses prepass.riverCandidateMask).
void runFoliageStage(TerrainPipelineContext& ctx);

} // namespace voxel::world
