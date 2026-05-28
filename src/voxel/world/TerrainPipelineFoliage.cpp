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

} // namespace

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
