#pragma once

#include <cstddef>
#include <cstdint>

#include <voxel/world/ChunkConstants.hpp>
#include <voxel/world/Coordinates.hpp>

// Level-of-detail foundation. Six tiers map to representation strategies
// rather than progressive downsamples of the same data structure:
//
//   LOD0 Active     — real 32³ chunks; full simulation + interaction.
//   LOD1 Simplified — real chunks, simulation budgets throttled.
//   LOD2 Cluster    — 4×4×4-chunk groups baked into a half-resolution mesh.
//                     No simulation. Visible at medium distance.
//   LOD3 Region     — heightfield + volumetric impostor at ~512-block grid.
//   LOD4 Orbital    — cube-sphere planet patches (no voxels).
//   LOD5 Solar      — Kepler orbital proxies / billboards (no terrain).
//
// Phase 1 implements LOD2 only. Higher tiers exist in this header so the
// `LodLevel` enum and `LodSettings` distance bands are stable references
// the rest of the engine can match against.
//
// See `docs/lod_design.md` (forthcoming) for the long-form rationale.

namespace voxel::world {

// One cluster spans `ClusterChunkExtent` chunks along each axis. With the
// default chunk extent of 32 blocks, a cluster covers 128³ world blocks.
//
// 4 is the smallest power-of-two that:
//   - Produces meaningful draw-call batching (64 chunks → 1 mesh).
//   - Keeps per-cluster RAM bounded (~1 MB half-res mesh at terrain density).
//   - Has acceptable invalidation granularity (one block edit dirties one
//     cluster's mesh, not a giant region).
constexpr int ClusterChunkExtent = 4;
constexpr int ClusterBlockExtent = ClusterChunkExtent * ChunkSize;

// Number of chunks in a single cluster (4 × 4 × 4 = 64).
constexpr int ClusterChunkVolume =
    ClusterChunkExtent * ClusterChunkExtent * ClusterChunkExtent;

// Cluster-space coordinate. `cluster = (chunk.x >> 2, chunk.y >> 2, chunk.z >> 2)`
// for the default ClusterChunkExtent=4. We use signed division (not bit shift)
// so negative chunk coords round toward -infinity, keeping clusters contiguous
// across the origin.
struct ClusterCoord {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};

    [[nodiscard]] friend bool operator==(ClusterCoord lhs, ClusterCoord rhs) noexcept
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }
};

// Floor-division by `ClusterChunkExtent` for negative-coord correctness.
// `static_cast<int64_t>(chunk.x / extent)` would round toward zero, splitting
// cluster (-1) into chunks {-4..-1} but also {0..3} miscounted at the origin.
[[nodiscard]] constexpr ClusterCoord chunkToCluster(ChunkCoord chunk) noexcept
{
    constexpr std::int64_t extent = ClusterChunkExtent;
    const auto floorDiv = [](std::int64_t a, std::int64_t b) noexcept -> std::int64_t {
        const std::int64_t q = a / b;
        const std::int64_t r = a % b;
        // If non-zero remainder and signs differ, the division rounded toward
        // zero — subtract 1 to floor.
        return (r != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    };
    return ClusterCoord{
        floorDiv(chunk.x, extent),
        floorDiv(chunk.y, extent),
        floorDiv(chunk.z, extent),
    };
}

// The chunk at the (0,0,0) corner of the cluster — the cluster's *origin*
// in chunk space. Iterate `[0..ClusterChunkExtent)` along each axis to walk
// the cluster's contained chunks.
[[nodiscard]] constexpr ChunkCoord clusterChunkOrigin(ClusterCoord cluster) noexcept
{
    constexpr std::int64_t extent = ClusterChunkExtent;
    return ChunkCoord{
        cluster.x * extent,
        cluster.y * extent,
        cluster.z * extent,
    };
}

// Phase 1D-3 (LOD3 Region): regions are 16x16x16 chunks = 512^3 world
// blocks. Same 64^3 supervoxel output grid as clusters — each region
// supervoxel is 8 blocks (vs cluster's 2). The mesher and renderer use
// the same code paths; only the "supervoxel-to-block scale" changes
// (LOD2=2, LOD3=8). Scale is communicated to the shader via the .w
// component of the per-instance origins SSBO.
//
// A region is exactly 4x4x4 clusters, just like a cluster is 4x4x4
// chunks. The factor-of-4 nesting keeps the LOD hierarchy regular.
constexpr int RegionChunkExtent = 16;
constexpr int RegionBlockExtent = RegionChunkExtent * ChunkSize; // 512
constexpr int RegionChunkVolume =
    RegionChunkExtent * RegionChunkExtent * RegionChunkExtent;     // 4096
constexpr int RegionClusterExtent = RegionChunkExtent / ClusterChunkExtent; // 4

[[nodiscard]] constexpr RegionCoord chunkToRegion(ChunkCoord chunk) noexcept
{
    constexpr std::int64_t extent = RegionChunkExtent;
    const auto floorDiv = [](std::int64_t a, std::int64_t b) noexcept -> std::int64_t {
        const std::int64_t q = a / b;
        const std::int64_t r = a % b;
        return (r != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    };
    return RegionCoord{
        floorDiv(chunk.x, extent),
        floorDiv(chunk.y, extent),
        floorDiv(chunk.z, extent),
    };
}

[[nodiscard]] constexpr ChunkCoord regionChunkOrigin(RegionCoord region) noexcept
{
    constexpr std::int64_t extent = RegionChunkExtent;
    return ChunkCoord{
        region.x * extent,
        region.y * extent,
        region.z * extent,
    };
}

struct LodEditInvalidationTargets {
    ClusterCoord cluster{};
    RegionCoord region{};
};

// Player edits currently rebuild the owning LOD2 cluster. The LOD3 clipmap
// remains terrain-noise-only, but returning the region here keeps the future
// edit-aware clipmap target explicit and testable.
[[nodiscard]] constexpr LodEditInvalidationTargets lodInvalidationTargetsForEditedChunk(
    ChunkCoord chunk) noexcept
{
    return LodEditInvalidationTargets{
        chunkToCluster(chunk),
        chunkToRegion(chunk),
    };
}

struct RegionCoordHash {
    [[nodiscard]] std::size_t operator()(RegionCoord coord) const noexcept
    {
        // Different prime triple from ChunkCoordHash and ClusterCoordHash
        // so the three hash spaces don't collide when used in shared
        // containers (rare but possible in debug overlays).
        const auto x = static_cast<std::uint64_t>(coord.x);
        const auto y = static_cast<std::uint64_t>(coord.y);
        const auto z = static_cast<std::uint64_t>(coord.z);
        return static_cast<std::size_t>(
            (x * 73856093ULL) ^ (y * 19349663ULL) ^ (z * 83492791ULL));
    }
};

struct ClusterCoordHash {
    [[nodiscard]] std::size_t operator()(ClusterCoord coord) const noexcept
    {
        // Different prime triple from ChunkCoordHash to keep maps that mix
        // cluster + chunk keys (rare, but possible in debug overlays) from
        // pathological bucket overlap.
        const auto x = static_cast<std::uint64_t>(coord.x);
        const auto y = static_cast<std::uint64_t>(coord.y);
        const auto z = static_cast<std::uint64_t>(coord.z);
        return static_cast<std::size_t>(
            (x * 49979693ULL) ^ (y * 31280487ULL) ^ (z * 67867967ULL));
    }
};

// LOD tier identifier. The numeric value is the rung from the design doc;
// numerically increasing means *less* detail, so simple comparisons
// (`lod < LodLevel::Region`) read intuitively.
enum class LodLevel : std::uint8_t {
    Active     = 0,  // LOD0 — full 32³ chunks, full sim, full interaction
    Simplified = 1,  // LOD1 — real chunks, throttled simulation budgets
    Cluster    = 2,  // LOD2 — 4×4×4-chunk half-resolution baked mesh
    Region     = 3,  // LOD3 — heightfield + impostor (deferred)
    Orbital    = 4,  // LOD4 — planet patch quadtree (deferred)
    Solar      = 5,  // LOD5 — orbital body proxies (deferred)
};

// Distance bands in chunk units (one chunk = ChunkSize blocks). The bands
// are EXCLUSIVE upper bounds: a chunk at distance `d` is in tier `T` when
// `T.lowerBound <= d < T.upperBound`. The first tier whose upper bound
// exceeds the distance wins.
//
// Default bands match `docs/lod_proposal.md`:
//   LOD0 Active     : 0–8  chunks  (0–256m  with 1 block = 1 meter)
//   LOD1 Simplified : 8–24 chunks  (256–768m)
//   LOD2 Cluster    : 24–96 chunks (768m–3km)
//   LOD3 Region     : 96–1600 chunks (3km–50km)
//
// LOD4/5 are space-mode tiers that live in a different coordinate system
// and aren't represented as a chunk-distance band. The streamer routes
// them via a separate path once the player leaves the atmosphere.
struct LodSettings {
    std::int64_t activeMaxChunks{8};
    std::int64_t simplifiedMaxChunks{24};
    std::int64_t clusterMaxChunks{96};
    std::int64_t regionMaxChunks{1600};
};

// Phase 2: R0-based deterministic LOD band formula.
//
// The user setting `R0 = fullChunkRenderDistance` (1..64) is the
// SINGLE knob that drives every LOD's reach. Everything else is
// derived. This replaces the per-call ad-hoc formulas
// (e.g. `chunkRadius/4 + 2`) that were scattered through the cluster
// and region maintenance code.
//
//   LOD0 (full chunks)   : 0 .. R0
//   LOD1 (reduced sim)   : R0 .. R0 + max(8, R0/2)
//   LOD2 (cluster meshes): R0 + ... .. R0 + max(32, R0*2)
//   LOD3 (far terrain)   : R0 + ... .. R0 + max(128, R0 * farQuality)
//
// `farQualityMultiplier` is a separate runtime knob (default 8) so
// the user can dial LOD3 reach independently — high-end PCs crank
// it to 16+, low-end PCs drop it to 2-4. Capped at 64 to bound
// memory budget.
//
// Example numbers:
//
//   R0 = 8,  farQ = 8  →  LOD0:0-8,   LOD1:9-16,   LOD2:17-40,   LOD3:41-72
//   R0 = 16, farQ = 8  →  LOD0:0-16,  LOD1:17-24,  LOD2:25-48,   LOD3:49-144
//   R0 = 32, farQ = 8  →  LOD0:0-32,  LOD1:33-48,  LOD2:49-96,   LOD3:97-288
//   R0 = 64, farQ = 16 →  LOD0:0-64,  LOD1:65-96,  LOD2:97-192,  LOD3:193-1088
//
// All distances are in CHUNK units. Convert to cluster/region rings
// at the use site (cluster ring = chunks / ClusterChunkExtent,
// region ring = chunks / RegionChunkExtent).
struct LodBands {
    std::int64_t lod0End{0};
    std::int64_t lod1End{0};
    std::int64_t lod2End{0};
    std::int64_t lod3End{0};
};

[[nodiscard]] constexpr LodBands computeLodBands(
    std::int64_t fullChunkRenderDistance,
    std::int64_t farQualityMultiplier) noexcept
{
    // Clamp inputs to sane ranges. R0 of 0 is meaningless (would
    // collapse all tiers); R0 above 64 is the practical ceiling.
    // farQ floor 1 (LOD3 disabled = farQ at 1, very tight); ceiling
    // 64 caps memory budget around 16k+ region pages.
    const std::int64_t r0 = fullChunkRenderDistance < 1 ? 1
        : (fullChunkRenderDistance > 64 ? 64 : fullChunkRenderDistance);
    const std::int64_t farQ = farQualityMultiplier < 1 ? 1
        : (farQualityMultiplier > 64 ? 64 : farQualityMultiplier);

    const std::int64_t lod1Reach = (r0 / 2) > 8 ? (r0 / 2) : 8;
    const std::int64_t lod2Reach = (r0 * 2) > 32 ? (r0 * 2) : 32;
    const std::int64_t lod3Reach = (r0 * farQ) > 128 ? (r0 * farQ) : 128;

    LodBands b;
    b.lod0End = r0;
    b.lod1End = r0 + lod1Reach;
    b.lod2End = r0 + lod2Reach;
    b.lod3End = r0 + lod3Reach;
    return b;
}

// Squared Chebyshev distance between two chunk coords, in chunk units. We
// use Chebyshev (max of axis distances) instead of Euclidean so the LOD
// boundaries are axis-aligned cubes — matches how the streamer already
// reasons about chunk neighborhoods.
[[nodiscard]] constexpr std::int64_t chunkChebyshevDistance(
    ChunkCoord a, ChunkCoord b) noexcept
{
    const auto dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const auto dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    const auto dz = a.z > b.z ? a.z - b.z : b.z - a.z;
    auto m = dx > dy ? dx : dy;
    return m > dz ? m : dz;
}

// Pure helper. Given the LOD center (usually `streamingCenter`) and a
// chunk coord, return which LOD tier that chunk falls into. Has zero side
// effects and zero state — callers can use it from anywhere.
//
// Returns `LodLevel::Solar` for chunks beyond the configured `regionMaxChunks`
// band; the streamer interprets that as "out of voxel range" and will
// route through the orbital pathway once that's implemented.
[[nodiscard]] constexpr LodLevel selectLodForChunk(
    ChunkCoord chunk, ChunkCoord center, const LodSettings& settings) noexcept
{
    const auto d = chunkChebyshevDistance(chunk, center);
    if (d < settings.activeMaxChunks)     return LodLevel::Active;
    if (d < settings.simplifiedMaxChunks) return LodLevel::Simplified;
    if (d < settings.clusterMaxChunks)    return LodLevel::Cluster;
    if (d < settings.regionMaxChunks)     return LodLevel::Region;
    return LodLevel::Solar;
}

} // namespace voxel::world
