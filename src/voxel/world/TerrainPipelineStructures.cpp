#include <voxel/world/TerrainPipeline.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <voxel/core/Math.hpp>
#include <voxel/world/BlockState.hpp>

namespace voxel::world {

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

} // namespace voxel::world
