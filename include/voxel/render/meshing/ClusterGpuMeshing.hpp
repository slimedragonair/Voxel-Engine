#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <voxel/render/meshing/ClusterMesh.hpp>
#include <voxel/render/meshing/GreedyMesher.hpp>

namespace voxel::render {
class VulkanRenderer;
}

namespace voxel::render::meshing {

// GPU LOD2 cluster face classifier.
//
// Mirrors HybridMeshingGpuSystem for clusters: owns the Vulkan resources
// for `cluster_mesh_classify.comp`. CPU does supervoxel reduction (which
// is fast and stays a serial step), GPU classifies which of the 6
// supervoxel faces are visible, CPU greedy-merges the result.
//
// Single-slot pipelined: submit one cluster, poll on a later frame.
// Wired into Application::tickClusterMaintenance — when initialized and
// not busy, replaces the synchronous CPU ClusterMesher::build path. The
// CPU path stays available as fallback.
//
// One classifier instance handles the whole engine — there's no point
// in N parallel GPU pipelines for a 0.1ms compute job, the bottleneck is
// the upload+readback round-trip.
class ClusterGpuMeshing {
public:
    explicit ClusterGpuMeshing(render::VulkanRenderer& renderer);
    ~ClusterGpuMeshing();

    ClusterGpuMeshing(const ClusterGpuMeshing&) = delete;
    ClusterGpuMeshing& operator=(const ClusterGpuMeshing&) = delete;
    ClusterGpuMeshing(ClusterGpuMeshing&&) = delete;
    ClusterGpuMeshing& operator=(ClusterGpuMeshing&&) = delete;

    [[nodiscard]] bool initialize();
    void shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool busy() const noexcept;

    // Cell flags packed into the input buffer's per-cell `flags` field.
    // Must match cluster_mesh_classify.comp exactly.
    static constexpr std::uint32_t kFlagOccludes = 1u << 0;
    static constexpr std::uint32_t kFlagFluid    = 1u << 1;
    static constexpr std::uint32_t kFlagUnknown  = 1u << 2;
    static constexpr std::uint32_t kSurfaceShift = 8u;

    // CPU-side cell layout — matches the shader's CellInfo struct. The
    // caller (ClusterMesher) populates a flat 66³ array of these
    // (interior at [1..64], 1-cell border at 0 and 65 filled with
    // zero/air for now since cross-cluster culling is Phase 1B+1).
    struct GpuCellInfo {
        std::uint32_t materialId{};
        std::uint32_t flags{};
        std::uint32_t packedLight{};
        std::uint32_t pad{};
    };

    // One classified visible face, decoded from the GPU's output buffer.
    // `cellIndex` is a flat index into the 64³ supervoxel grid:
    // x + y*64 + z*64*64.
    struct VisibleFace {
        std::uint32_t cellIndex{};      // 0..262143
        std::uint8_t  faceIndex{};      // 0..5
        MeshSurface   surface{MeshSurface::Opaque};
        std::uint32_t materialId{};
        std::uint32_t packedLight{};
    };

    struct ClassificationResult {
        std::vector<VisibleFace> faces;
        std::uint32_t rawFaceCount{};   // GPU's atomic counter before clamp
        bool overflow{};                // count > maxFaces
    };

    // Padded cell-grid extent (66 = 64 interior + 2 border). Exposed
    // so the caller knows the expected input buffer size.
    [[nodiscard]] static constexpr std::uint32_t paddedExtent() noexcept { return 66u; }
    [[nodiscard]] static constexpr std::size_t paddedCellCount() noexcept
    {
        return std::size_t{paddedExtent()} * paddedExtent() * paddedExtent();
    }
    [[nodiscard]] std::size_t maxFacesPerCluster() const noexcept;

    // Submit a classification job. Returns false if the system is busy
    // (already has an in-flight job), uninitialized, or the input was
    // invalid. The cells span is taken by const& because the upload
    // copies it directly into a host-visible buffer — caller owns the
    // memory and may free it after the call returns.
    [[nodiscard]] bool submit(const std::vector<GpuCellInfo>& paddedCells);

    // Poll the GPU fence. Returns the parsed face list when ready;
    // nullopt if still in flight; an empty (but valid) result on
    // GPU error. Safe to call every tick.
    [[nodiscard]] std::optional<ClassificationResult> poll();

private:
    struct GpuResources;

    render::VulkanRenderer& renderer_;
    std::unique_ptr<GpuResources> gpu_;
    bool initialized_{false};
};

} // namespace voxel::render::meshing
