#include <voxel/render/meshing/GreedyMesher.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <voxel/render/meshing/BlockRenderCatalog.hpp>

namespace voxel::render::meshing {

namespace {

struct Quad {
    std::array<std::array<int, 3>, 4> corners{};
    std::uint32_t faceIndex{};
    std::uint32_t materialId{};
    std::uint32_t packedLight{};
    MeshSurface surface{MeshSurface::Opaque};
};

struct MaskCell {
    std::uint32_t materialId{};
    std::uint32_t packedLight{};
    MeshSurface surface{MeshSurface::Opaque};

    [[nodiscard]] explicit operator bool() const noexcept { return materialId != 0; }
    [[nodiscard]] friend bool operator==(MaskCell lhs, MaskCell rhs) noexcept
    {
        return lhs.materialId == rhs.materialId && lhs.surface == rhs.surface && lhs.packedLight == rhs.packedLight;
    }
};

struct FaceDesc {
    int axis{};
    int sign{};
    std::uint32_t faceIndex{};
};

constexpr std::array<FaceDesc, 6> Faces{{
    {0, 1, 0},
    {0, -1, 1},
    {1, 1, 2},
    {1, -1, 3},
    {2, 1, 4},
    {2, -1, 5},
}};

bool isAir(voxel::BlockStateId state)
{
    return state.value == world::AirBlockState.value;
}

BlockRenderCatalog defaultCatalog()
{
    BlockRenderCatalog catalog;
    catalog.set(voxel::BlockTypeId{0}, {MeshSurface::Transparent, false});
    return catalog;
}

std::uint32_t shadeForFace(const FaceDesc& face)
{
    // Default-shade fallback when no light data is available (E6 lighting catalog absent).
    if (face.axis == 1 && face.sign > 0) {
        return 255U;
    }
    if (face.axis == 1 && face.sign < 0) {
        return 120U;
    }
    if (face.axis == 0) {
        return 165U;
    }
    return 190U;
}

// Computes packed light for a face, sampled from baked ChunkLightData at the
// cell on the AIR side of the face. Falls back to face-direction shade when
// the neighbour is out of bounds (no cross-chunk read this phase).
std::uint32_t packLightFromBaked(const world::ChunkLightData& data, const FaceDesc& face,
                                 int x, int y, int z)
{
    std::array<int, 3> neighbour{x, y, z};
    neighbour[face.axis] += face.sign;
    if (neighbour[0] < 0 || neighbour[1] < 0 || neighbour[2] < 0
        || neighbour[0] >= world::ChunkSize || neighbour[1] >= world::ChunkSize || neighbour[2] >= world::ChunkSize) {
        return shadeForFace(face);
    }
    const auto sky = data.skyLight(neighbour[0], neighbour[1], neighbour[2]);
    const auto block = data.blockLight(neighbour[0], neighbour[1], neighbour[2]);
    const auto best = static_cast<std::uint32_t>(sky > block ? sky : block);
    // Map 0..15 to a 8-bit shade in [40, 255] so caves stay dark but not pitch black.
    const auto shade = 40U + (best * (255U - 40U)) / 15U;
    return shade | (static_cast<std::uint32_t>(sky) << 12U) | (static_cast<std::uint32_t>(block) << 16U);
}

const world::Chunk* neighborForFace(const ChunkNeighborhood& neighborhood, const FaceDesc& face)
{
    if (face.axis == 0) {
        return face.sign > 0 ? neighborhood.posX : neighborhood.negX;
    }
    if (face.axis == 1) {
        return face.sign > 0 ? neighborhood.posY : neighborhood.negY;
    }
    return face.sign > 0 ? neighborhood.posZ : neighborhood.negZ;
}

voxel::BlockStateId sampleNeighbouringChunk(const world::Chunk* neighbour, const FaceDesc& face,
                                            int x, int y, int z)
{
    // The "neighbour" coordinate (one cell past the boundary in the face's
    // direction) wraps to (face.sign > 0 ? 0 : ChunkSize-1) on the perpendicular axis.
    int wrapped[3]{x, y, z};
    wrapped[face.axis] = face.sign > 0 ? 0 : (world::ChunkSize - 1);
    return neighbour->blockAt(wrapped[0], wrapped[1], wrapped[2]);
}

bool faceVisible(const world::Chunk& chunk, const BlockRenderCatalog& catalog,
                 const ChunkNeighborhood& neighborhood,
                 int x, int y, int z, const FaceDesc& face,
                 voxel::BlockStateId currentState)
{
    std::array<int, 3> neighbor{x, y, z};
    neighbor[face.axis] += face.sign;

    voxel::BlockStateId adjacent{};
    const bool outOfBounds = (neighbor[0] < 0 || neighbor[1] < 0 || neighbor[2] < 0
        || neighbor[0] >= world::ChunkSize || neighbor[1] >= world::ChunkSize || neighbor[2] >= world::ChunkSize);

    if (outOfBounds) {
        const auto* neighbourChunk = neighborForFace(neighborhood, face);
        if (neighbourChunk == nullptr) {
            return true;
        }
        adjacent = sampleNeighbouringChunk(neighbourChunk, face, x, y, z);
    } else {
        adjacent = chunk.blockAt(neighbor[0], neighbor[1], neighbor[2]);
    }

    if (isAir(adjacent)) {
        return true;
    }

    const auto adjacentInfo = catalog.get(adjacent);
    if (!adjacentInfo.occludesNeighborFaces) {
        return true;
    }

    const auto currentInfo = catalog.get(currentState);
    return currentInfo.surface == MeshSurface::Transparent && currentState.value != adjacent.value;
}

std::uint32_t packPosition(int x, int y, int z, std::uint32_t face, std::uint32_t corner)
{
    return static_cast<std::uint32_t>(x)
        | (static_cast<std::uint32_t>(y) << 6U)
        | (static_cast<std::uint32_t>(z) << 12U)
        | (face << 18U)
        | (corner << 21U);
}

Quad makeQuad(const FaceDesc& face, int plane, int uStart, int vStart, int width, int height, MaskCell cell)
{
    const int uAxis = (face.axis + 1) % 3;
    const int vAxis = (face.axis + 2) % 3;

    std::array<int, 3> c0{};
    std::array<int, 3> c1{};
    std::array<int, 3> c2{};
    std::array<int, 3> c3{};

    c0[face.axis] = plane;
    c1[face.axis] = plane;
    c2[face.axis] = plane;
    c3[face.axis] = plane;

    c0[uAxis] = uStart;
    c0[vAxis] = vStart;
    c1[uAxis] = uStart + width;
    c1[vAxis] = vStart;
    c2[uAxis] = uStart + width;
    c2[vAxis] = vStart + height;
    c3[uAxis] = uStart;
    c3[vAxis] = vStart + height;

    Quad quad;
    quad.faceIndex = face.faceIndex;
    quad.materialId = cell.materialId;
    quad.packedLight = cell.packedLight;
    quad.surface = cell.surface;
    if (face.sign > 0) {
        quad.corners = {c0, c1, c2, c3};
    } else {
        quad.corners = {c3, c2, c1, c0};
    }
    return quad;
}

void appendQuad(ChunkMesh& mesh, const Quad& quad)
{
    const auto baseIndex = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto indexOffset = static_cast<std::uint32_t>(mesh.indices.size());

    for (std::uint32_t cornerIndex = 0; cornerIndex < quad.corners.size(); ++cornerIndex) {
        const auto& corner = quad.corners[cornerIndex];
        mesh.vertices.push_back({
            packPosition(corner[0], corner[1], corner[2], quad.faceIndex, cornerIndex),
            cornerIndex,
            quad.packedLight,
            quad.materialId
        });
    }

    mesh.indices.push_back(baseIndex + 0U);
    mesh.indices.push_back(baseIndex + 1U);
    mesh.indices.push_back(baseIndex + 2U);
    mesh.indices.push_back(baseIndex + 0U);
    mesh.indices.push_back(baseIndex + 2U);
    mesh.indices.push_back(baseIndex + 3U);

    constexpr std::uint32_t quadIndexCount = 6;
    if (!mesh.drawRanges.empty()) {
        auto& previous = mesh.drawRanges.back();
        if (previous.surface == quad.surface
            && previous.materialId == quad.materialId
            && previous.indexOffset + previous.indexCount == indexOffset) {
            previous.indexCount += quadIndexCount;
            return;
        }
    }

    mesh.drawRanges.push_back({quad.surface, quad.materialId, indexOffset, quadIndexCount});
}

void greedyFacePass(const world::Chunk& chunk, const BlockRenderCatalog& catalog,
                    const world::ChunkLightData* light, const ChunkNeighborhood& neighborhood,
                    const FaceDesc& face, std::vector<Quad>& quads)
{
    const int uAxis = (face.axis + 1) % 3;
    const int vAxis = (face.axis + 2) % 3;
    std::vector<MaskCell> mask(static_cast<std::size_t>(world::ChunkSize * world::ChunkSize));

    for (int slice = 0; slice < world::ChunkSize; ++slice) {
        std::fill(mask.begin(), mask.end(), MaskCell{});

        for (int v = 0; v < world::ChunkSize; ++v) {
            for (int u = 0; u < world::ChunkSize; ++u) {
                std::array<int, 3> pos{};
                pos[face.axis] = slice;
                pos[uAxis] = u;
                pos[vAxis] = v;

                const auto state = chunk.blockAt(pos[0], pos[1], pos[2]);
                if (isAir(state)) {
                    continue;
                }
                if (!faceVisible(chunk, catalog, neighborhood, pos[0], pos[1], pos[2], face, state)) {
                    continue;
                }

                const auto renderInfo = catalog.get(state);
                const std::uint32_t packedLight = (light != nullptr)
                    ? packLightFromBaked(*light, face, pos[0], pos[1], pos[2])
                    : shadeForFace(face);
                mask[static_cast<std::size_t>(u + v * world::ChunkSize)] = {state.value, packedLight, renderInfo.surface};
            }
        }

        for (int v = 0; v < world::ChunkSize; ++v) {
            for (int u = 0; u < world::ChunkSize;) {
                const auto cell = mask[static_cast<std::size_t>(u + v * world::ChunkSize)];
                if (!cell) {
                    ++u;
                    continue;
                }

                int width = 1;
                while (u + width < world::ChunkSize
                    && mask[static_cast<std::size_t>(u + width + v * world::ChunkSize)] == cell) {
                    ++width;
                }

                int height = 1;
                bool done = false;
                while (v + height < world::ChunkSize && !done) {
                    for (int testU = 0; testU < width; ++testU) {
                        if (!(mask[static_cast<std::size_t>(u + testU + (v + height) * world::ChunkSize)] == cell)) {
                            done = true;
                            break;
                        }
                    }
                    if (!done) {
                        ++height;
                    }
                }

                for (int clearV = 0; clearV < height; ++clearV) {
                    for (int clearU = 0; clearU < width; ++clearU) {
                        mask[static_cast<std::size_t>(u + clearU + (v + clearV) * world::ChunkSize)] = {};
                    }
                }

                const int plane = face.sign > 0 ? slice + 1 : slice;
                quads.push_back(makeQuad(face, plane, u, v, width, height, cell));
                u += width;
            }
        }
    }
}

} // namespace

ChunkMesh GreedyMesher::build(const world::Chunk& chunk) const
{
    const auto catalog = defaultCatalog();
    return build(chunk, catalog, nullptr, ChunkNeighborhood{});
}

ChunkMesh GreedyMesher::build(const world::Chunk& chunk, const BlockRenderCatalog& catalog) const
{
    return build(chunk, catalog, nullptr, ChunkNeighborhood{});
}

ChunkMesh GreedyMesher::build(const world::Chunk& chunk, const BlockRenderCatalog& catalog,
                               const world::ChunkLightData* light) const
{
    return build(chunk, catalog, light, ChunkNeighborhood{});
}

ChunkMesh GreedyMesher::build(const world::Chunk& chunk, const BlockRenderCatalog& catalog,
                               const world::ChunkLightData* light,
                               const ChunkNeighborhood& neighborhood) const
{
    std::vector<Quad> quads;
    constexpr std::size_t kMaxQuads = static_cast<std::size_t>(world::ChunkVolume) / 4;
    quads.reserve(kMaxQuads);

    for (const auto& face : Faces) {
        greedyFacePass(chunk, catalog, light, neighborhood, face, quads);
    }

    std::sort(quads.begin(), quads.end(), [](const Quad& lhs, const Quad& rhs) {
        const auto lhsSection = lhs.corners[0][1] / (world::ChunkSize / 2);
        const auto rhsSection = rhs.corners[0][1] / (world::ChunkSize / 2);
        if (lhsSection != rhsSection) {
            return lhsSection < rhsSection;
        }
        if (lhs.surface != rhs.surface) {
            return static_cast<int>(lhs.surface) < static_cast<int>(rhs.surface);
        }
        return lhs.materialId < rhs.materialId;
    });

    ChunkMesh mesh;
    mesh.sourceRevision = chunk.revision();
    mesh.sourceMeshRevisionHash = chunk.meshRevision();
    mesh.vertices.reserve(quads.size() * 4U);
    mesh.indices.reserve(quads.size() * 6U);

    for (const auto& quad : quads) {
        appendQuad(mesh, quad);
    }

    for (std::size_t s = 0; s < mesh.sections.size(); ++s) {
        const auto yBase = static_cast<float>(s * (world::ChunkSize / 2));
        mesh.sections[s].minY = yBase;
        mesh.sections[s].maxY = yBase + static_cast<float>(world::ChunkSize / 2);
    }

    for (const auto& range : mesh.drawRanges) {
        std::uint32_t sectionIdx = 0;
        if (!mesh.indices.empty() && range.indexCount > 0) {
            const auto firstIdx = mesh.indices[range.indexOffset];
            const auto packedPos = mesh.vertices[firstIdx].packedPos;
            const auto y = (packedPos >> 6U) & 63U;
            sectionIdx = y / (world::ChunkSize / 2);
            if (sectionIdx >= mesh.sections.size()) {
                sectionIdx = static_cast<std::uint32_t>(mesh.sections.size() - 1);
            }
        }
        auto& sec = mesh.sections[sectionIdx];
        if (range.surface == MeshSurface::Opaque) {
            if (sec.opaqueIndexCount == 0) {
                sec.opaqueFirstIndex = range.indexOffset;
            }
            sec.opaqueIndexCount += range.indexCount;
        } else if (range.surface == MeshSurface::Cutout) {
            if (sec.cutoutIndexCount == 0) {
                sec.cutoutFirstIndex = range.indexOffset;
            }
            sec.cutoutIndexCount += range.indexCount;
        } else {
            if (sec.transparentIndexCount == 0) {
                sec.transparentFirstIndex = range.indexOffset;
            }
            sec.transparentIndexCount += range.indexCount;
        }
    }

    for (auto& sec : mesh.sections) {
        sec.vertexOffsetInVertices = 0;
        sec.vertexOffset = 0;
        sec.indexOffset = 0;
    }

    return mesh;
}

} // namespace voxel::render::meshing
