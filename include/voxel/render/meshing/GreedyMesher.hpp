#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <voxel/world/Chunk.hpp>
#include <voxel/world/ChunkLightData.hpp>

namespace voxel::render::meshing {

struct VoxelVertex {
    std::uint32_t packedPos{};
    std::uint32_t packedUv{};
    std::uint32_t packedLight{};
    std::uint32_t packedMaterial{};
};

enum class MeshSurface : std::uint8_t {
    Opaque,
    Cutout,
    Transparent
};

struct MeshDrawRange {
    MeshSurface surface{MeshSurface::Opaque};
    std::uint32_t materialId{};
    std::uint32_t indexOffset{};
    std::uint32_t indexCount{};
};

struct ChunkMesh {
    struct Section {
        std::uint32_t vertexOffset{};
        std::uint32_t indexOffset{};
        std::uint32_t opaqueIndexCount{};
        std::uint32_t opaqueFirstIndex{};
        std::uint32_t cutoutIndexCount{};
        std::uint32_t cutoutFirstIndex{};
        std::uint32_t transparentIndexCount{};
        std::uint32_t transparentFirstIndex{};
        std::int32_t vertexOffsetInVertices{};
        float minY{};
        float maxY{};
    };
    std::vector<VoxelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MeshDrawRange> drawRanges;
    // Section bookkeeping is now per-chunk (1 section per chunk). The earlier
    // 2-section split had no real culling benefit because the renderer used a
    // conservative full-chunk AABB for both halves, so we paid double the
    // scene-entry hash-map churn for zero pay-off. The single-element array
    // preserves the field name for any callers that still indexed `[0]`.
    std::array<Section, 1> sections{};
    std::uint64_t sourceRevision{};
    std::uint64_t sourceMeshRevisionHash{};
};

// Output contract for hybrid meshing. A GPU classifier can emit these records,
// then the CPU can run the existing greedy merge to produce ChunkMesh.
struct VisibleFaceRecord {
    std::uint16_t localIndex{}; // x + y * 32 + z * 32 * 32
    std::uint8_t faceIndex{};   // matches the packed vertex face index
    MeshSurface surface{MeshSurface::Opaque};
    std::uint32_t materialId{};
    std::uint32_t packedLight{};
};

struct BlockRenderInfo;
class BlockRenderCatalog;

// Snapshot of a chunk's 6 face neighbours used by the mesher for cross-chunk
// face culling. Any pointer may be null if the neighbour isn't loaded — the
// mesher then conservatively keeps the face (the chunk will be re-meshed once
// the neighbour arrives).
struct ChunkNeighborhood {
    const world::Chunk* negX{nullptr};
    const world::Chunk* posX{nullptr};
    const world::Chunk* negY{nullptr};
    const world::Chunk* posY{nullptr};
    const world::Chunk* negZ{nullptr};
    const world::Chunk* posZ{nullptr};
};

struct MeshingOptions {
    // Inclusive world-space block Y for the static ocean/lake water surface.
    // When set, missing neighbours do not cause underwater static-fluid caps
    // or chunk-border walls to render while adjacent chunks stream in.
    std::optional<int> staticWaterSurfaceY{};
};

class GreedyMesher {
public:
    [[nodiscard]] ChunkMesh build(const world::Chunk& chunk) const;
    [[nodiscard]] ChunkMesh build(const world::Chunk& chunk, const BlockRenderCatalog& catalog) const;
    // When `light` is non-null, vertex `packedLight` is computed from baked sky+block light
    // sampled at the visible-side cell. When null, falls back to a hardcoded face-direction shade.
    [[nodiscard]] ChunkMesh build(const world::Chunk& chunk, const BlockRenderCatalog& catalog,
                                   const world::ChunkLightData* light) const;
    // Cross-chunk-aware overload (F2). Reads the supplied neighbour chunks
    // when a face touches the chunk boundary so seams between adjacent solid
    // chunks aren't double-drawn.
    [[nodiscard]] ChunkMesh build(const world::Chunk& chunk, const BlockRenderCatalog& catalog,
                                   const world::ChunkLightData* light,
                                   const ChunkNeighborhood& neighborhood,
                                   MeshingOptions options = {}) const;

    // Hybrid-meshing seam: classify visible faces separately from final CPU
    // greedy merge. The future GPU path will replace classifyVisibleFaces();
    // buildFromVisibleFaces() remains the CPU merge/install-compatible path.
    [[nodiscard]] std::vector<VisibleFaceRecord> classifyVisibleFaces(
        const world::Chunk& chunk,
        const BlockRenderCatalog& catalog,
        const world::ChunkLightData* light,
        const ChunkNeighborhood& neighborhood,
        MeshingOptions options = {}) const;
    [[nodiscard]] ChunkMesh buildFromVisibleFaces(
        const world::Chunk& chunk,
        const std::vector<VisibleFaceRecord>& faces) const;
};

} // namespace voxel::render::meshing
