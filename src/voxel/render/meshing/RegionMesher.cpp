#include <voxel/render/meshing/RegionMesher.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <voxel/render/meshing/BlockRenderCatalog.hpp>
#include <voxel/render/meshing/GreedyMesher.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/Chunk.hpp>

namespace voxel::render::meshing {

namespace {

// LOD3 supervoxel grid: 64^3 cells, each representing an 8^3 block cube
// of source data. Output mesh has vertex positions in 0..128 "supervoxel
// × 2" units; the shader scales by 4.0 to map to the 0..512 region.
constexpr int kSupervoxelExtent = world::RegionBlockExtent / 8; // 64
constexpr std::size_t kSupervoxelVolume =
    static_cast<std::size_t>(kSupervoxelExtent) * kSupervoxelExtent * kSupervoxelExtent;

constexpr int kSupervoxelBlockSize = 8;     // region supervoxel = 8 blocks
constexpr int kSampleOffset = kSupervoxelBlockSize / 2; // 4 — center of cube

// Mirror of ClusterCell from the cluster mesher — same shape, just
// scoped to this TU. The unknown flag is used the same way: missing-
// chunk supervoxels suppress face emission against them.
struct RegionCell {
    std::uint32_t materialId{};
    std::uint8_t  surface{};
    std::uint8_t  flags{};   // bit 0 = occludes, bit 1 = isFluid, bit 2 = unknown

    [[nodiscard]] bool isAir() const noexcept { return materialId == 0u && (flags & 4u) == 0u; }
    [[nodiscard]] bool occludes() const noexcept { return (flags & 1u) != 0u; }
    [[nodiscard]] bool isFluidBlock() const noexcept { return (flags & 2u) != 0u; }
    [[nodiscard]] bool isUnknown() const noexcept { return (flags & 4u) != 0u; }
};

struct FaceDesc {
    int axis{};
    int sign{};
    std::uint8_t faceIndex{};
};

constexpr std::array<FaceDesc, 6> kFaces{{
    {0,  1, 0}, {0, -1, 1},
    {1,  1, 2}, {1, -1, 3},
    {2,  1, 4}, {2, -1, 5},
}};

[[nodiscard]] std::uint32_t shadeForFace(const FaceDesc& face) noexcept
{
    if (face.axis == 1 && face.sign > 0) return 255U;
    if (face.axis == 1 && face.sign < 0) return 120U;
    if (face.axis == 0)                  return 165U;
    return 190U;
}

[[nodiscard]] constexpr std::size_t supervoxelIndex(int x, int y, int z) noexcept
{
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(y) * kSupervoxelExtent
        + static_cast<std::size_t>(z) * kSupervoxelExtent * kSupervoxelExtent;
}

// Reduce one region supervoxel by sampling its center block. Saves
// 511× the lookups vs visiting every block in the 8^3 cube; quality
// is "sub-pixel" at LOD3 viewing distances.
RegionCell reduceRegionSupervoxel(int sx, int sy, int sz,
                                   const RegionChunkSnapshot& snapshot,
                                   const BlockRenderCatalog& catalog) noexcept
{
    // Region-local block coords of the supervoxel's center.
    const int bx = sx * kSupervoxelBlockSize + kSampleOffset;
    const int by = sy * kSupervoxelBlockSize + kSampleOffset;
    const int bz = sz * kSupervoxelBlockSize + kSampleOffset;

    // Which chunk owns this center block?
    const int cx = bx / world::ChunkSize;
    const int cy = by / world::ChunkSize;
    const int cz = bz / world::ChunkSize;
    const int lx = bx - cx * world::ChunkSize;
    const int ly = by - cy * world::ChunkSize;
    const int lz = bz - cz * world::ChunkSize;

    const auto& chunkOpt = snapshot.chunks[regionLocalChunkIndex(cx, cy, cz)];
    if (!chunkOpt.has_value()) {
        // Missing chunk → Unknown (face-suppressing) per the LOD2
        // cluster mesher's convention.
        RegionCell cell;
        cell.flags = 4u;
        return cell;
    }
    const auto state = chunkOpt->blockAtUnchecked(lx, ly, lz);
    if (state.value == world::AirBlockState.value) {
        return RegionCell{}; // air
    }
    const auto info = catalog.get(voxel::BlockStateId{state.value});
    RegionCell cell;
    cell.materialId = state.value;
    cell.surface = static_cast<std::uint8_t>(info.surface);
    std::uint8_t flags = 0;
    if (info.occludesNeighborFaces) flags |= 1u;
    if (info.isFluid)                flags |= 2u;
    cell.flags = flags;
    return cell;
}

struct SupervoxelBounds {
    int minX{kSupervoxelExtent}, maxX{-1};
    int minY{kSupervoxelExtent}, maxY{-1};
    int minZ{kSupervoxelExtent}, maxZ{-1};
    [[nodiscard]] bool empty() const noexcept { return maxX < minX; }
};

SupervoxelBounds populateRegionGrid(const RegionChunkSnapshot& snapshot,
                                     const BlockRenderCatalog& catalog,
                                     std::vector<RegionCell>& grid)
{
    SupervoxelBounds bounds;
    for (int z = 0; z < kSupervoxelExtent; ++z) {
        for (int y = 0; y < kSupervoxelExtent; ++y) {
            for (int x = 0; x < kSupervoxelExtent; ++x) {
                auto& slot = grid[supervoxelIndex(x, y, z)];
                slot = reduceRegionSupervoxel(x, y, z, snapshot, catalog);
                if (!slot.isAir() && !slot.isUnknown()) {
                    if (x < bounds.minX) bounds.minX = x;
                    if (x > bounds.maxX) bounds.maxX = x;
                    if (y < bounds.minY) bounds.minY = y;
                    if (y > bounds.maxY) bounds.maxY = y;
                    if (z < bounds.minZ) bounds.minZ = z;
                    if (z > bounds.maxZ) bounds.maxZ = z;
                }
            }
        }
    }
    return bounds;
}

// Cave-fill: column-scan top-down, fill air below kCaveDepthSupervoxels
// from the surface. Same algorithm as ClusterMesher's helper. At LOD3
// distance the threshold can be more aggressive — caves we'd preserve
// at LOD2 are sub-pixel at LOD3 distance anyway.
void fillCavesBelowSurface(std::vector<RegionCell>& grid,
                            const SupervoxelBounds& bounds) noexcept
{
    constexpr int kCaveDepthSupervoxels = 2; // ~16 blocks below local surface
    for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
        for (int x = bounds.minX; x <= bounds.maxX; ++x) {
            int topSolidY = -1;
            std::uint32_t topMaterial = 0;
            std::uint8_t topSurface = 0;
            std::uint8_t topFlags = 0;
            for (int y = bounds.maxY; y >= bounds.minY; --y) {
                const auto& cell = grid[supervoxelIndex(x, y, z)];
                if (!cell.isAir() && !cell.isUnknown()) {
                    topSolidY = y;
                    topMaterial = cell.materialId;
                    topSurface = cell.surface;
                    topFlags = cell.flags;
                    break;
                }
            }
            if (topSolidY < 0) continue;

            const int caveCutoffY = topSolidY - kCaveDepthSupervoxels;
            for (int y = caveCutoffY; y >= bounds.minY; --y) {
                auto& cell = grid[supervoxelIndex(x, y, z)];
                if (cell.materialId == 0u && (cell.flags & 4u) == 0u) {
                    cell.materialId = topMaterial;
                    cell.surface = topSurface;
                    cell.flags = topFlags | 1u;
                }
            }
        }
    }
}

// Face-visibility check. Cross-region boundary lookups are conservative
// (treated as air → face emitted) — same compromise as LOD2 cluster
// borders. Phase 1D+ could add neighbor-region slabs analogous to
// cluster border culling if seam doubling becomes visible.
[[nodiscard]] bool isFaceVisible(const std::vector<RegionCell>& grid,
                                  int x, int y, int z, const FaceDesc& face,
                                  const RegionCell& current) noexcept
{
    const int nx = x + (face.axis == 0 ? face.sign : 0);
    const int ny = y + (face.axis == 1 ? face.sign : 0);
    const int nz = z + (face.axis == 2 ? face.sign : 0);
    if (nx < 0 || ny < 0 || nz < 0
        || nx >= kSupervoxelExtent
        || ny >= kSupervoxelExtent
        || nz >= kSupervoxelExtent) {
        return true; // region boundary, conservative emit
    }
    const auto& adj = grid[supervoxelIndex(nx, ny, nz)];
    if (adj.isUnknown()) return false;
    if (adj.isAir()) return true;
    if (current.isFluidBlock() && adj.isFluidBlock()
        && voxel::world::blockTypeOf(voxel::BlockStateId{current.materialId})
         == voxel::world::blockTypeOf(voxel::BlockStateId{adj.materialId})) {
        return false;
    }
    return !adj.occludes();
}

struct Quad {
    std::array<std::array<int, 3>, 4> corners{};
    std::uint8_t faceIndex{};
    std::uint32_t materialId{};
    std::uint32_t packedLight{};
    MeshSurface surface{MeshSurface::Opaque};
};

// Same vertex-position convention as ClusterMesher: emit in "supervoxel
// × 2" units (0..128). The shader's lodScale=4.0 maps those to actual
// world block units (0..512) for LOD3.
Quad makeQuad(const FaceDesc& face, int slicePlane,
              int uStart, int vStart, int width, int height,
              std::uint32_t materialId, std::uint32_t packedLight, MeshSurface surface) noexcept
{
    const int uAxis = (face.axis + 1) % 3;
    const int vAxis = (face.axis + 2) % 3;

    std::array<int, 3> c0{}, c1{}, c2{}, c3{};
    const int planeBlock = slicePlane * 2;
    c0[face.axis] = planeBlock;
    c1[face.axis] = planeBlock;
    c2[face.axis] = planeBlock;
    c3[face.axis] = planeBlock;

    const int uStartB = uStart * 2;
    const int vStartB = vStart * 2;
    const int uEndB   = (uStart + width) * 2;
    const int vEndB   = (vStart + height) * 2;
    c0[uAxis] = uStartB; c0[vAxis] = vStartB;
    c1[uAxis] = uEndB;   c1[vAxis] = vStartB;
    c2[uAxis] = uEndB;   c2[vAxis] = vEndB;
    c3[uAxis] = uStartB; c3[vAxis] = vEndB;

    Quad q;
    q.faceIndex = face.faceIndex;
    q.materialId = materialId;
    q.packedLight = packedLight;
    q.surface = surface;
    if (face.sign > 0) {
        q.corners = {c0, c1, c2, c3};
    } else {
        q.corners = {c3, c2, c1, c0};
    }
    return q;
}

void appendQuadToMesh(ClusterMesh& mesh, const Quad& quad)
{
    const auto baseIndex = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto indexOffset = static_cast<std::uint32_t>(mesh.indices.size());

    for (std::uint32_t i = 0; i < quad.corners.size(); ++i) {
        const auto& c = quad.corners[i];
        ClusterVertex v;
        v.posX = static_cast<std::uint8_t>(c[0]);
        v.posY = static_cast<std::uint8_t>(c[1]);
        v.posZ = static_cast<std::uint8_t>(c[2]);
        v.faceIndex = quad.faceIndex;
        v.materialId = quad.materialId;
        v.packedLight = quad.packedLight;
        mesh.vertices.push_back(v);
    }
    mesh.indices.push_back(baseIndex + 0U);
    mesh.indices.push_back(baseIndex + 1U);
    mesh.indices.push_back(baseIndex + 2U);
    mesh.indices.push_back(baseIndex + 0U);
    mesh.indices.push_back(baseIndex + 2U);
    mesh.indices.push_back(baseIndex + 3U);

    constexpr std::uint32_t kQuadIndexCount = 6;
    if (!mesh.drawRanges.empty()) {
        auto& prev = mesh.drawRanges.back();
        if (prev.surface == quad.surface
            && prev.materialId == quad.materialId
            && prev.indexOffset + prev.indexCount == indexOffset) {
            prev.indexCount += kQuadIndexCount;
            return;
        }
    }
    mesh.drawRanges.push_back({quad.surface, quad.materialId, indexOffset, kQuadIndexCount});
}

void runFacePass(const std::vector<RegionCell>& grid,
                  const SupervoxelBounds& bounds,
                  const FaceDesc& face,
                  std::vector<Quad>& quads)
{
    const int uAxis = (face.axis + 1) % 3;
    const int vAxis = (face.axis + 2) % 3;
    const int axisLo[3] = {bounds.minX, bounds.minY, bounds.minZ};
    const int axisHi[3] = {bounds.maxX, bounds.maxY, bounds.maxZ};
    const int sliceLo = axisLo[face.axis];
    const int sliceHi = axisHi[face.axis];
    const int uLo = axisLo[uAxis];
    const int uHi = axisHi[uAxis];
    const int vLo = axisLo[vAxis];
    const int vHi = axisHi[vAxis];

    constexpr std::size_t kMaskCells =
        static_cast<std::size_t>(kSupervoxelExtent) * kSupervoxelExtent;
    std::vector<std::uint32_t> maskMaterial(kMaskCells, 0u);
    std::vector<std::uint32_t> maskLight(kMaskCells, 0u);
    std::vector<std::uint8_t>  maskSurface(kMaskCells, 0u);

    const std::uint32_t lightForThisFace = shadeForFace(face);

    for (int slice = sliceLo; slice <= sliceHi; ++slice) {
        std::fill(maskMaterial.begin(), maskMaterial.end(), 0u);

        for (int v = vLo; v <= vHi; ++v) {
            for (int u = uLo; u <= uHi; ++u) {
                int pos[3];
                pos[face.axis] = slice;
                pos[uAxis] = u;
                pos[vAxis] = v;
                const auto& cell = grid[supervoxelIndex(pos[0], pos[1], pos[2])];
                if (cell.isAir() || cell.isUnknown()) continue;
                if (!isFaceVisible(grid, pos[0], pos[1], pos[2], face, cell)) continue;

                const std::size_t maskIdx = static_cast<std::size_t>(u + v * kSupervoxelExtent);
                maskMaterial[maskIdx] = cell.materialId;
                maskLight[maskIdx] = lightForThisFace;
                maskSurface[maskIdx] = cell.surface;
            }
        }

        for (int v = 0; v < kSupervoxelExtent; ++v) {
            for (int u = 0; u < kSupervoxelExtent;) {
                const std::size_t baseIdx = static_cast<std::size_t>(u + v * kSupervoxelExtent);
                const auto material = maskMaterial[baseIdx];
                if (material == 0u) {
                    ++u;
                    continue;
                }
                const auto surface = maskSurface[baseIdx];
                const auto light = maskLight[baseIdx];

                int width = 1;
                while (u + width < kSupervoxelExtent) {
                    const std::size_t i = baseIdx + static_cast<std::size_t>(width);
                    if (maskMaterial[i] != material
                        || maskSurface[i] != surface
                        || maskLight[i] != light) {
                        break;
                    }
                    ++width;
                }

                int height = 1;
                bool done = false;
                while (v + height < kSupervoxelExtent && !done) {
                    const std::size_t rowBase = static_cast<std::size_t>(u + (v + height) * kSupervoxelExtent);
                    for (int testU = 0; testU < width; ++testU) {
                        const std::size_t i = rowBase + static_cast<std::size_t>(testU);
                        if (maskMaterial[i] != material
                            || maskSurface[i] != surface
                            || maskLight[i] != light) {
                            done = true;
                            break;
                        }
                    }
                    if (!done) ++height;
                }

                for (int clearV = 0; clearV < height; ++clearV) {
                    const std::size_t rowBase = static_cast<std::size_t>(u + (v + clearV) * kSupervoxelExtent);
                    for (int clearU = 0; clearU < width; ++clearU) {
                        maskMaterial[rowBase + static_cast<std::size_t>(clearU)] = 0u;
                    }
                }

                const int planeForQuad = (face.sign > 0) ? slice + 1 : slice;
                quads.push_back(makeQuad(face, planeForQuad, u, v, width, height,
                                         material, light, static_cast<MeshSurface>(surface)));
                u += width;
            }
        }
    }
}

} // namespace

std::uint64_t hashRegionChunkRevisions(const RegionChunkSnapshot& snapshot) noexcept
{
    std::uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a offset basis
    for (const auto& chunkOpt : snapshot.chunks) {
        const std::uint64_t rev = chunkOpt.has_value()
            ? static_cast<std::uint64_t>(chunkOpt->revision())
            : 0xFFFFFFFFFFFFFFFFULL;
        hash ^= rev;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

ClusterMesh RegionMesher::build(const RegionChunkSnapshot& snapshot,
                                 const BlockRenderCatalog& catalog) const
{
    ClusterMesh mesh;
    // Reuse the ClusterMesh.coord field by casting the RegionCoord into
    // its alias-cluster position. This keeps disk-cache file naming and
    // the renderer's upload path simple — they don't need to know the
    // tier from the mesh alone.
    mesh.coord = world::ClusterCoord{
        snapshot.coord.x * world::RegionClusterExtent,
        snapshot.coord.y * world::RegionClusterExtent,
        snapshot.coord.z * world::RegionClusterExtent,
    };
    mesh.sourceRevisionsHash = hashRegionChunkRevisions(snapshot);

    // 1. Reduce 4096 chunks (sampled) to 64^3 supervoxels.
    std::vector<RegionCell> grid(kSupervoxelVolume);
    const auto bounds = populateRegionGrid(snapshot, catalog, grid);
    if (bounds.empty()) {
        return mesh; // entire region is air/unknown
    }

    // 2. Cave-fill (more aggressive than LOD2 because we're farther out).
    fillCavesBelowSurface(grid, bounds);

    // 3. Six greedy face passes.
    std::vector<Quad> quads;
    quads.reserve(kSupervoxelVolume / 8);
    for (const auto& face : kFaces) {
        runFacePass(grid, bounds, face, quads);
    }

    // 4. Sort + append (same as cluster path).
    std::sort(quads.begin(), quads.end(), [](const Quad& lhs, const Quad& rhs) {
        if (lhs.surface != rhs.surface) {
            return static_cast<int>(lhs.surface) < static_cast<int>(rhs.surface);
        }
        return lhs.materialId < rhs.materialId;
    });
    mesh.vertices.reserve(quads.size() * 4U);
    mesh.indices.reserve(quads.size() * 6U);
    for (const auto& quad : quads) {
        appendQuadToMesh(mesh, quad);
    }
    return mesh;
}

// ============================================================================
// GPU-hybrid path. Same reduce + cave-fill as build() above, but instead
// of running the 6 face passes on CPU we pack the supervoxel grid into
// the 66³ padded GpuCellInfo layout that the cluster_mesh_classify.comp
// compute shader expects. The shader is LOD-tier-agnostic — it operates
// on a 64³ supervoxel grid regardless of how much world space each
// supervoxel represents.
// ============================================================================

std::vector<ClusterGpuMeshing::GpuCellInfo>
RegionMesher::buildPaddedCellGrid(const RegionChunkSnapshot& snapshot,
                                   const BlockRenderCatalog& catalog) const
{
    // 1. Reduce 4096 source chunks (sampled center-of-cube) to 64³
    //    region supervoxels. Same routine the all-CPU build() uses.
    std::vector<RegionCell> grid(kSupervoxelVolume);
    const auto bounds = populateRegionGrid(snapshot, catalog, grid);
    if (bounds.empty()) {
        return {}; // entire region is air/unknown — caller skips submit
    }

    // 2. Cave-fill: same column-scan as the all-CPU path. Removes
    //    interior cave detail that's sub-pixel at LOD3 distance.
    fillCavesBelowSurface(grid, bounds);

    // 3. Pack into the GPU shader's 66³ padded layout. Interior cells
    //    map [0..63] → [1..64] in padded coords; the 1-cell border
    //    stays zero-initialized (= air, which the shader treats as
    //    "open" → emits outer face). v1 doesn't fill the border from
    //    neighbor regions; the visible artifact is doubled face
    //    geometry at region seams, sub-pixel at LOD3 distance.
    constexpr std::uint32_t kPadded = ClusterGpuMeshing::paddedExtent(); // 66
    std::vector<ClusterGpuMeshing::GpuCellInfo> cells;
    cells.resize(ClusterGpuMeshing::paddedCellCount());

    const auto paddedIdx = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
        return x + y * kPadded + z * kPadded * kPadded;
    };

    // Iterate only the non-air bounding box, mirroring the cluster
    // path's optimization.
    for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
        for (int y = bounds.minY; y <= bounds.maxY; ++y) {
            for (int x = bounds.minX; x <= bounds.maxX; ++x) {
                const auto& cell = grid[supervoxelIndex(x, y, z)];
                if (cell.isAir()) {
                    continue; // leave as zero-init air
                }
                auto& dst = cells[paddedIdx(
                    static_cast<std::uint32_t>(x + 1),
                    static_cast<std::uint32_t>(y + 1),
                    static_cast<std::uint32_t>(z + 1))];
                dst.materialId = cell.materialId;
                dst.packedLight = 0u;
                std::uint32_t flags = 0;
                if (cell.occludes())     flags |= ClusterGpuMeshing::kFlagOccludes;
                if (cell.isFluidBlock()) flags |= ClusterGpuMeshing::kFlagFluid;
                if (cell.isUnknown())    flags |= ClusterGpuMeshing::kFlagUnknown;
                flags |= (static_cast<std::uint32_t>(cell.surface) & 3u)
                          << ClusterGpuMeshing::kSurfaceShift;
                dst.flags = flags;
            }
        }
    }
    return cells;
}

} // namespace voxel::render::meshing
