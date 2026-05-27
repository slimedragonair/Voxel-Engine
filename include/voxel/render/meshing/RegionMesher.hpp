#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <voxel/render/meshing/ClusterGpuMeshing.hpp>
#include <voxel/render/meshing/ClusterMesh.hpp>
#include <voxel/world/Chunk.hpp>
#include <voxel/world/Coordinates.hpp>
#include <voxel/world/Lod.hpp>

namespace voxel::render::meshing {

class BlockRenderCatalog;

// Snapshot of one LOD3 region's source chunks for off-thread meshing.
//
// A region spans 16×16×16 chunks = 512^3 world blocks. We don't need
// to clone all 4096 chunks for the 1-sample-per-supervoxel reduction
// the RegionMesher does, but we DO need direct addressing by region-
// local chunk coordinates, so the array is sized for the worst case.
// Missing entries (nullopt) are treated as "all air" — same convention
// as ClusterChunkSnapshot.
//
// Memory: 4096 × sizeof(optional<Chunk>) ≈ 4096 × 40 B = 160 KB of
// pointer storage. Chunk clones are shared_ptr-based, so actual data
// isn't copied — just refcount bumps.
struct RegionChunkSnapshot {
    world::RegionCoord coord{};
    std::array<std::optional<world::Chunk>,
               static_cast<std::size_t>(world::RegionChunkVolume)> chunks{};
};

// Index helper: (cx, cy, cz) ∈ [0, RegionChunkExtent) → flat index into
// RegionChunkSnapshot::chunks. Same layout as `clusterLocalChunkIndex`,
// just sized for the bigger extent.
[[nodiscard]] constexpr std::size_t regionLocalChunkIndex(int cx, int cy, int cz) noexcept
{
    return static_cast<std::size_t>(cx)
        + static_cast<std::size_t>(cy) * world::RegionChunkExtent
        + static_cast<std::size_t>(cz) * world::RegionChunkExtent * world::RegionChunkExtent;
}

// Hash a region's source-chunk revisions. Mirrors
// hashClusterChunkRevisions for the cluster path. Cache invalidation
// + dedup at the renderer use this to detect "rebuild needed".
[[nodiscard]] std::uint64_t hashRegionChunkRevisions(
    const RegionChunkSnapshot& snapshot) noexcept;

// LOD3 region mesher. Produces a `ClusterMesh` in the same vertex
// format as LOD2 clusters — `ClusterVertex` positions in 0..128 units
// that the shader scales by 4.0 to get 0..512 world blocks. The
// ClusterRenderer treats the resulting mesh identically to a LOD2
// cluster mesh except for the per-instance scale factor written to the
// origins SSBO .w component.
//
// Reduction strategy: 1-sample-per-supervoxel. Each region supervoxel
// covers 8^3 = 512 source blocks; we sample the CENTER block (offset
// (4,4,4) within the cube) and use its material. ~262K block lookups
// per region build (~10 ms in Release on a worker thread). Quality is
// "good enough" at the 24-48 chunk distance LOD3 is shown at — finer
// detail would be sub-pixel.
//
// After supervoxel population: same cave-fill pass as ClusterMesher
// (underground caves below the column's surface get flattened),
// followed by the same six greedy face passes that LOD2 uses.
class RegionMesher {
public:
    // All-CPU path: snapshot → reduce → cave-fill → 6 face passes →
    // ClusterMesh. Used when the GPU classifier isn't available
    // (initialization failed or GPU is busy on a sibling LOD3 build).
    [[nodiscard]] ClusterMesh build(const RegionChunkSnapshot& snapshot,
                                    const BlockRenderCatalog& catalog) const;

    // GPU-hybrid path Stage A: reduce 4096 source chunks to a 66³
    // padded GpuCellInfo grid that the cluster_mesh_classify.comp
    // compute shader consumes. Worker-runnable (no Vulkan calls inside).
    //
    // Reduction is the same as the all-CPU path: 1 sample per
    // supervoxel (center block of each 8^3 cube) + cave-fill. The
    // result is packed into the 66³ layout the GPU classifier
    // expects, with the 1-cell padded border left as air (cross-
    // region border culling is not implemented in v1 — visible
    // artifact is doubled face geometry at region seams, sub-pixel
    // at LOD3 distance).
    //
    // Returns an empty vector if the region reduced to all-air; the
    // caller should skip the GPU submit in that case.
    [[nodiscard]] std::vector<ClusterGpuMeshing::GpuCellInfo>
    buildPaddedCellGrid(const RegionChunkSnapshot& snapshot,
                        const BlockRenderCatalog& catalog) const;
};

} // namespace voxel::render::meshing
