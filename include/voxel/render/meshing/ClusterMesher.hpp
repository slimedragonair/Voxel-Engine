#pragma once

#include <cstdint>
#include <vector>

#include <voxel/render/meshing/ClusterGpuMeshing.hpp>
#include <voxel/render/meshing/ClusterMesh.hpp>

namespace voxel::render::meshing {

class BlockRenderCatalog;

// Half-resolution greedy mesher for LOD2 cluster meshes.
//
// Two execution paths:
//
//  - All-CPU (`build`): the original Phase 1B-1 path. Reduces 128³ source
//    blocks to a 64³ supervoxel grid, does six greedy face passes,
//    appends quads. Used when ClusterGpuMeshing is unavailable.
//
//  - Hybrid CPU + GPU (`buildPaddedCellGrid` + `buildMeshFromGpuFaces`):
//    CPU does supervoxel reduction once and emits a 66³ flat array
//    matching the GPU shader's input layout. GPU runs the face-
//    visibility classifier (`cluster_mesh_classify.comp`). CPU
//    consumes the resulting face list and does greedy merge. Used
//    when ClusterGpuMeshing::initialized() is true. See
//    Application::tickClusterMaintenance.
//
// Cross-cluster face culling is NOT implemented yet — outer-face quads
// at the cluster boundary are emitted conservatively (visible). Phase
// 1B+1 will plumb neighbor-cluster cells into the padded grid's border.
class ClusterMesher {
public:
    [[nodiscard]] ClusterMesh build(const ClusterChunkSnapshot& snapshot,
                                    const BlockRenderCatalog& catalog) const;

    // Reduces a ClusterChunkSnapshot to a 66³ padded GpuCellInfo grid in
    // shader-compatible layout. The interior occupies [1, 64] on each
    // axis; the 1-cell border is zero-initialized (treated as air by
    // the shader). This is the input to ClusterGpuMeshing::submit.
    //
    // Returns an empty vector if every interior cell reduced to air or
    // Unknown — caller should skip the GPU submit in that case.
    [[nodiscard]] std::vector<ClusterGpuMeshing::GpuCellInfo>
    buildPaddedCellGrid(const ClusterChunkSnapshot& snapshot,
                        const BlockRenderCatalog& catalog) const;

    // Greedy-merges a list of GPU-classified visible faces into a
    // ClusterMesh. The face records come from ClusterGpuMeshing::poll.
    // sourceRevisionsHash is taken from the snapshot the GPU pass ran
    // against — the caller is responsible for passing the matching one
    // (typically captured at submit time).
    [[nodiscard]] ClusterMesh buildMeshFromGpuFaces(
        world::ClusterCoord coord,
        std::uint64_t sourceRevisionsHash,
        const std::vector<ClusterGpuMeshing::VisibleFace>& faces) const;
};

// Compute the source-revisions XOR hash over a snapshot. Exposed so callers
// can recompute it after a dispatch to detect staleness (see Chunk::revision
// invariants in installMeshResults).
[[nodiscard]] std::uint64_t hashClusterChunkRevisions(
    const ClusterChunkSnapshot& snapshot) noexcept;

} // namespace voxel::render::meshing
