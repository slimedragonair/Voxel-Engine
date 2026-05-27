#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <voxel/core/Math.hpp>
#include <voxel/render/meshing/ClusterMesh.hpp>
#include <voxel/world/Lod.hpp>

// Forward-declare VkCommandBuffer so this public header stays
// Vulkan-include-free (matches FluidGpuSystem.hpp's pattern). The .cpp
// pulls in <vulkan/vulkan.h> directly.
struct VkCommandBuffer_T;
using VkCommandBuffer = VkCommandBuffer_T*;

namespace voxel::render {

class VulkanRenderer;

// LOD2 cluster rendering subsystem. Owns:
//   - Vertex + index BufferArenas dedicated to cluster meshes (separate
//     from chunk arenas so per-LOD churn doesn't fragment the other).
//   - A scene-entry SSBO consumed by the existing GPU cull shader
//     (voxel_cull.comp) — same struct layout as ChunkSceneEntry so the
//     same compute pass culls clusters too.
//   - Future: cluster graphics pipeline (Phase 1C-4) + indirect draw
//     command buffer.
//
// PERSISTENCE INVARIANT (Phase 1D enforces, Phase 1C-2 supports):
//
//   When a chunk transitions from LOD0/1 into LOD2 range, its real-block
//   mesh must remain on the GPU until the containing cluster's mesh has
//   been uploaded. Otherwise the player sees a "fade-out" gap between
//   chunk-mesh eviction and cluster-mesh installation.
//
//   This renderer enables that by RENDERING BOTH chunk and cluster
//   meshes concurrently for any chunks in the transition window. Depth
//   buffer naturally resolves overdraw: LOD0 chunks are at the same
//   depth as the cluster region behind them, so the depth test picks
//   the foreground (chunk) when both exist, and the cluster only shows
//   through where the chunk mesh has been evicted.
//
//   The streamer's eviction policy (in Application.cpp / ChunkStreamer)
//   must therefore not evict a chunk's mesh until its cluster mesh is
//   live. That's a Phase 1D change, but this renderer supports
//   simultaneous rendering of both representations from day one.
class ClusterRenderer {
public:
    explicit ClusterRenderer(VulkanRenderer& renderer);
    ~ClusterRenderer();

    ClusterRenderer(const ClusterRenderer&) = delete;
    ClusterRenderer& operator=(const ClusterRenderer&) = delete;

    // Allocate Vulkan resources. Must be called after the VulkanRenderer
    // is initialized. Returns false on any allocation failure — caller
    // should disable LOD2 rendering and proceed with LOD0/1 only.
    [[nodiscard]] bool initialize();

    // Release all resources. Idempotent.
    void shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    // Rebuild only graphics pipelines that were baked against
    // VulkanRenderer::renderPass(). Mesh arenas, descriptor sets, and uploaded
    // LOD meshes survive swapchain recreation.
    [[nodiscard]] bool rebuildSwapchainResources();

    // LOD tier classification for uploaded meshes. Determines the per-
    // instance scale factor written to the origins SSBO .w component:
    //   Cluster (LOD2): scale = 1.0 — vertex positions are already in
    //                   block units (0..128).
    //   Region  (LOD3): scale = 4.0 — vertex positions are 0..128 in
    //                   "supervoxel × 2" units that the shader scales
    //                   up to 0..512 actual world blocks.
    // Higher tiers extend the pattern (LOD4 macro-region: scale = 16).
    enum class LodTier : std::uint8_t {
        Cluster = 2,
        Region  = 3,
    };

    // Upload a freshly-built cluster mesh. Allocates GPU vertex/index
    // slices and stages an upload via the renderer's staging arena.
    // If a mesh for `coord` already exists, the old slices are retired
    // and the new mesh replaces it.
    //
    // `tier` controls the LOD scale factor the vertex shader applies;
    // see LodTier above. Defaulting to Cluster keeps the original LOD2
    // call sites working unchanged.
    //
    // Returns false if (a) renderer not initialized, (b) arenas full,
    // (c) staging arena full this frame. Caller should defer in that
    // case — the mesh will be re-attempted next frame.
    [[nodiscard]] bool uploadClusterMesh(world::ClusterCoord coord,
                                          const meshing::ClusterMesh& mesh,
                                          LodTier tier = LodTier::Cluster);

    // Evict a cluster mesh from the GPU. No-op if not currently uploaded.
    // Slices go into the retire queue and are released after the GPU has
    // finished with them (mirrors the chunk-mesh retire path).
    void removeClusterMesh(world::ClusterCoord coord);

    // LOD3 region equivalents. Region meshes live in their own map so
    // their coord space (RegionCoord, 16-chunk extent) doesn't collide
    // with the cluster coord space (ClusterCoord, 4-chunk extent).
    [[nodiscard]] bool uploadRegionMesh(world::RegionCoord coord,
                                         const meshing::ClusterMesh& mesh);
    void removeRegionMesh(world::RegionCoord coord);
    [[nodiscard]] std::size_t residentRegionCount() const noexcept;
    void setSkipDrawRegions(
        std::unordered_set<world::RegionCoord, world::RegionCoordHash> skip);

    // Drop everything. Used on swapchain rebuild / world reload.
    void clearAllClusters();

    // Number of cluster meshes currently resident on the GPU.
    [[nodiscard]] std::size_t residentClusterCount() const noexcept;

    // PERSISTENCE POLICY ENFORCEMENT (the missing half of the design):
    //
    //   The depth-buffer-arbitrates story in the class comment assumed
    //   cluster Y matched chunk Y bit-for-bit. In practice, supervoxel
    //   reduction rounds UP — a 2-block-tall stone ledge becomes a full
    //   supervoxel-tall ledge, putting the cluster top 1-2 blocks
    //   *higher* than the chunk top. The cluster then wins every depth
    //   test and completely occludes chunk meshes underneath.
    //
    //   The fix is to keep the cluster mesh on the GPU (so it's ready
    //   when chunks evict) but SKIP DRAWING IT in frames where the
    //   chunks at that cluster's footprint already have meshes. Caller
    //   (Application) owns the policy decision since it has the
    //   ChunkMeshCache; this renderer just respects the skip list.
    void setSkipDrawClusters(
        std::unordered_set<world::ClusterCoord, world::ClusterCoordHash> skip);

    // Runtime master switch. When `false`, recordDraws becomes a no-op
    // — every resident cluster mesh stays on the GPU but nothing is
    // drawn. Lets the user flip LOD2 on/off via the Runtime Settings
    // overlay without restarting (or tearing down ClusterRenderer).
    // Default: true. The Application sets this each tick from
    // config_.useClusterLod.
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Cached frustum from the most recent recordDraws. `valid` is false
    // until the first frame has rendered (e.g., during initialization
    // or the first tick). Application reads this for build-time cull —
    // a one-frame-old frustum is fine for "should I bother classifying
    // this cluster" decisions.
    struct CachedFrustum {
        voxel::core::FrustumPlanes planes{};
        bool valid{false};
    };
    [[nodiscard]] const CachedFrustum& lastFrustum() const noexcept { return lastFrustum_; }

    // Record the cluster draw pass into the given command buffer. Must
    // be called inside an active render pass (typically as the
    // VulkanRenderer external-draw-hook). Binds the cluster pipeline,
    // pushes the supplied per-frame constants, writes resident cluster
    // origins to the GPU origins SSBO, then issues one draw per resident
    // cluster with opaque + cutout + transparent surfaces handled in
    // sequence.
    //
    // Receives the same push-constants payload the chunk pass used so
    // lighting / underwater fog stays consistent across LOD boundaries.
    void recordDraws(VkCommandBuffer commandBuffer,
                     const float viewProjection[16],
                     const float lightParams[4],
                     const float cameraParams[4],
                     const float cameraWorldParams[4],
                     const float atmosphereParams[4]);

private:
    struct GpuResources;
    void destroyPipelines() noexcept;
    [[nodiscard]] bool createPipelines();

    VulkanRenderer& renderer_;
    std::unique_ptr<GpuResources> gpu_;

    // Per-cluster bookkeeping: which arena slices each resident cluster
    // owns, plus scene-entry index for fast updates / removal.
    struct UploadedCluster {
        std::uint64_t vertexOffset{};  // BufferArena::Slice.offset
        std::uint64_t vertexBytes{};
        std::uint64_t indexOffset{};
        std::uint64_t indexBytes{};
        std::uint32_t sceneEntryIndex{0xFFFFFFFFu};
        std::uint64_t sourceRevisionsHash{};
        LodTier tier{LodTier::Cluster};
    };
    std::unordered_map<world::ClusterCoord, UploadedCluster,
                       world::ClusterCoordHash> uploadedClusters_;

    // LOD3 region bookkeeping. Same UploadedCluster shape as LOD2,
    // tagged tier=Region. Lives in a separate map because regions
    // are keyed by world::RegionCoord (different coord space from
    // ClusterCoord — a region at (1, 0, 0) covers cluster (4..7,
    // 0..3, 0..3) which would collide with LOD2 cluster (4, 0, 0)
    // if they shared a single map).
    std::unordered_map<world::RegionCoord, UploadedCluster,
                       world::RegionCoordHash> uploadedRegions_;

    // Clusters to skip in recordDraws. Updated each frame by Application
    // before the external-draw hook fires.
    std::unordered_set<world::ClusterCoord, world::ClusterCoordHash>
        skipDrawClusters_;
    // Region equivalent. Populated by Application when LOD3 regions
    // get fully covered by LOD2 clusters (one tier below) — same
    // persistence-policy logic as cluster skip-draw.
    std::unordered_set<world::RegionCoord, world::RegionCoordHash>
        skipDrawRegions_;

    bool initialized_{false};
    bool enabled_{true};
    CachedFrustum lastFrustum_{};
    // One-shot diagnostic flag — set the first time recordDraws actually
    // dispatches at least one cluster draw to the GPU. Logged so the
    // user can distinguish "no LOD because nothing drew" from "no LOD
    // because nothing built" from "no LOD because nothing's visible".
    bool firstDrawLogged_{false};
};

} // namespace voxel::render
