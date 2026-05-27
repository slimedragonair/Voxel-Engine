#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <voxel/render/meshing/GreedyMesher.hpp>
#include <voxel/world/Chunk.hpp>
#include <voxel/world/Lod.hpp>

// Output types for LOD2 cluster meshing. A cluster covers 4×4×4 chunks
// (= 128×128×128 world blocks) and is baked into a *half-resolution* mesh:
// the source 128³ block grid is reduced to a 64³ supervoxel grid (each
// supervoxel = a 2×2×2 group of source blocks), then greedy-merged.
//
// Vertex format differs from `VoxelVertex` because:
//   - Position needs 0..128 inclusive range (chunk needed only 0..32).
//   - We don't need UVs (light is baked, materials are atlas indices).
//
// `ClusterVertex` is 12 bytes (vs. 16 for VoxelVertex). Phase 1C will
// decide whether to share the chunk vertex shader with a scale-factor
// push constant or compile a separate cluster shader; for now this struct
// is the CPU-side mesh representation only.

namespace voxel::render::meshing {

struct ClusterVertex {
    // Position in cluster-local block units. Range [0, 128] inclusive
    // (vertices live on supervoxel-pair boundaries, which fall on even
    // block coordinates). Fits in 8 bits per axis.
    std::uint8_t posX{};
    std::uint8_t posY{};
    std::uint8_t posZ{};
    std::uint8_t faceIndex{};       // 0..5, matches FaceDesc::faceIndex in GreedyMesher.cpp
    std::uint32_t materialId{};     // BlockStateId.value of the dominant supervoxel
    std::uint32_t packedLight{};    // shadeForFace value; baked light comes in Phase 1B+1
};
static_assert(sizeof(ClusterVertex) == 12, "ClusterVertex must be 12 bytes for tight packing");

// Vulkan vertex-input layout for ClusterVertex. Kept here next to the type
// so any future change to the field order forces an update to the format
// table. Consumed by VulkanRenderer when building the cluster graphics
// pipeline; matches `cluster.vert` shader inputs:
//   location 0: uvec4 inPosAndFace  → R8G8B8A8_UINT at offset 0
//   location 1: uint  inMaterialId  → R32_UINT      at offset 4
//   location 2: uint  inPackedLight → R32_UINT      at offset 8
struct ClusterVertexInputDesc {
    static constexpr std::uint32_t kStride = 12U; // = sizeof(ClusterVertex)
    static constexpr std::uint32_t kBinding = 0U;
    struct Attribute {
        std::uint32_t location;
        std::uint32_t format;  // VkFormat as raw integer to avoid pulling vulkan.h into this header
        std::uint32_t offset;
    };
    // VkFormat numeric values (from vulkan_core.h, locked here so the
    // public header doesn't need to include vulkan.h):
    //   VK_FORMAT_R8G8B8A8_UINT = 41
    //   VK_FORMAT_R32_UINT      = 98
    static constexpr Attribute kAttributes[3] = {
        {0, 41U, 0U},  // inPosAndFace
        {1, 98U, 4U},  // inMaterialId
        {2, 98U, 8U},  // inPackedLight
    };
};

struct ClusterMesh {
    std::vector<ClusterVertex> vertices;
    std::vector<std::uint32_t> indices;
    // Per-(surface, material) draw ranges, same shape as ChunkMesh's
    // drawRanges. The renderer consumes these to emit indirect draws.
    std::vector<MeshDrawRange> drawRanges;

    world::ClusterCoord coord{};
    // XOR-hash of all 64 contained chunks' revisions at the time of
    // build. The install path compares against `currentClusterRevisionHash`
    // to detect "a chunk inside this cluster changed after dispatch" and
    // re-enqueue if stale. Mirrors the meshRevision pattern from ChunkMesh.
    std::uint64_t sourceRevisionsHash{};
};

// Snapshot of one cluster's 64 contained chunks for off-thread meshing.
// Each entry is a shared-blockData clone (see Chunk::cloneBlocksOnly).
// `chunks[(dz * ClusterChunkExtent + dy) * ClusterChunkExtent + dx]` holds
// the chunk at cluster-local offset (dx, dy, dz) where dx,dy,dz ∈
// [0, ClusterChunkExtent).
//
// Nullopt entries mean "that chunk wasn't loaded at snapshot time"; the
// mesher treats them as all-air, producing a conservative but coarse mesh
// for clusters where not all chunks are present. Phase 1D will avoid
// dispatching cluster mesh jobs until enough of the cluster is loaded.
//
// Border culling (Phase 1D-2d): the snapshot can ALSO carry the 16
// chunks on each face of the 6 neighboring clusters. The mesher reads
// these when filling the 1-cell padded border of its supervoxel grid,
// so the GPU classifier produces accurate boundary face emission
// instead of always-emit-conservative behavior. Optionals stay nullopt
// when the neighbor cluster's chunks aren't loaded — those border
// cells default to air, which gives the same conservative emit as
// before (a fine fallback at the world's outer edge).
//
// Slab layout: `neighborSlabs[face][i]` holds the chunk at slab
// position `i` (0..15) on the face indexed by `face` (0..5).
// Face index matches `FaceDesc::faceIndex` from the mesher:
//   0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z.
// Slab indexing: face's u/v axes parameterize the 4×4 chunk grid
// touching that face, using the same (axis+1)%3 / (axis+2)%3 axis
// convention as the mesher's quad-pass code.
constexpr int ClusterChunkSlabSize = world::ClusterChunkExtent * world::ClusterChunkExtent; // 16
struct ClusterChunkSnapshot {
    world::ClusterCoord coord{};
    std::array<std::optional<world::Chunk>,
               static_cast<std::size_t>(world::ClusterChunkVolume)> chunks{};
    std::array<std::array<std::optional<world::Chunk>, ClusterChunkSlabSize>, 6>
        neighborSlabs{};
};

[[nodiscard]] constexpr std::size_t clusterLocalChunkIndex(int dx, int dy, int dz) noexcept
{
    return static_cast<std::size_t>(dx)
        + static_cast<std::size_t>(dy) * world::ClusterChunkExtent
        + static_cast<std::size_t>(dz) * world::ClusterChunkExtent * world::ClusterChunkExtent;
}

// Index a 4×4 chunk slab on one face of a neighbor cluster. `u, v` are
// in [0, ClusterChunkExtent). The face axis is implicit (chosen by the
// caller); only the two tangent axes parameterize the slab. Layout is
// (u + v * extent) so it matches `clusterLocalChunkIndex` for the 2D
// case along whichever axis pair the face uses.
[[nodiscard]] constexpr std::size_t clusterFaceSlabIndex(int u, int v) noexcept
{
    return static_cast<std::size_t>(u)
        + static_cast<std::size_t>(v) * world::ClusterChunkExtent;
}

} // namespace voxel::render::meshing
