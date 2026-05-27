#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <voxel/render/meshing/GreedyMesher.hpp>

namespace voxel::render {
class VulkanRenderer;
}

namespace voxel::render::meshing {

// GPU visible-face classifier for hybrid meshing.
//
// This system owns the Vulkan resources for `mesh_face_classify.comp`. It does
// replace the visible-face classification step when
// ApplicationConfig::useGpuHybridMeshing is enabled. The first live path is a
// single-slot pipelined path: submit one chunk, poll the fence on later frames,
// then let the CPU greedy-merger consume the returned visible-face records.
class HybridMeshingGpuSystem {
public:
    explicit HybridMeshingGpuSystem(render::VulkanRenderer& renderer);
    ~HybridMeshingGpuSystem();

    HybridMeshingGpuSystem(const HybridMeshingGpuSystem&) = delete;
    HybridMeshingGpuSystem& operator=(const HybridMeshingGpuSystem&) = delete;
    HybridMeshingGpuSystem(HybridMeshingGpuSystem&&) = delete;
    HybridMeshingGpuSystem& operator=(HybridMeshingGpuSystem&&) = delete;

    [[nodiscard]] bool initialize();
    void shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] std::size_t maxFacesPerChunk() const noexcept;
    [[nodiscard]] std::size_t inputBufferBytes() const noexcept;
    [[nodiscard]] std::size_t faceBufferBytes() const noexcept;

    // Blocking bringup path for one chunk. The current experimental live path
    // uses this for correctness validation. The production path will pipeline
    // readback and batch several chunks.
    [[nodiscard]] std::vector<VisibleFaceRecord> classifyBlocking(
        const world::Chunk& chunk,
        const BlockRenderCatalog& catalog,
        const world::ChunkLightData* light,
        const ChunkNeighborhood& neighborhood,
        MeshingOptions options = {});

    struct ClassificationResult {
        std::vector<VisibleFaceRecord> faces;
        std::uint32_t rawFaceCount{};
        bool overflow{};
    };
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool submitClassification(
        const world::Chunk& chunk,
        const BlockRenderCatalog& catalog,
        const world::ChunkLightData* light,
        const ChunkNeighborhood& neighborhood,
        MeshingOptions options = {});
    [[nodiscard]] std::optional<ClassificationResult> pollClassification();

private:
    struct GpuResources;

    render::VulkanRenderer& renderer_;
    std::unique_ptr<GpuResources> gpu_;
    bool initialized_{false};
};

} // namespace voxel::render::meshing
