#include <voxel/render/meshing/ClusterMesher.hpp>

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

// LOD2 supervoxel grid dimensions. Each supervoxel = 2×2×2 source blocks.
// Cluster covers 128³ source blocks → 64³ supervoxels.
constexpr int kSupervoxelExtent = world::ClusterBlockExtent / 2; // 64
constexpr std::size_t kSupervoxelVolume =
    static_cast<std::size_t>(kSupervoxelExtent) * kSupervoxelExtent * kSupervoxelExtent;

// Cached per-supervoxel info, parallel to GreedyMesher's CachedBlock.
struct ClusterCell {
    std::uint32_t materialId{};   // 0 = air
    std::uint8_t  surface{};      // MeshSurface enum value (0/1/2)
    std::uint8_t  flags{};        // bit 0 = occludes, bit 1 = isFluid, bit 2 = unknown

    [[nodiscard]] bool isAir() const noexcept { return materialId == 0u && (flags & 4u) == 0u; }
    [[nodiscard]] bool occludes() const noexcept { return (flags & 1u) != 0u; }
    [[nodiscard]] bool isFluidBlock() const noexcept { return (flags & 2u) != 0u; }
    // "Unknown" supervoxels straddle a missing chunk in the snapshot. We
    // can't reliably classify them, so we (a) emit no geometry for them
    // and (b) suppress face emission on adjacent loaded supervoxels —
    // otherwise partial cluster builds would show false air/solid seams at
    // the missing-chunk boundary (e.g., horizontal slabs floating in the
    // sky above where the top chunk row hasn't streamed in yet).
    [[nodiscard]] bool isUnknown() const noexcept { return (flags & 4u) != 0u; }
};

// Face descriptors mirror GreedyMesher's `Faces` array exactly so the
// face-index convention stays consistent across the engine.
struct FaceDesc {
    int axis{};
    int sign{};
    std::uint8_t faceIndex{};
};

constexpr std::array<FaceDesc, 6> kFaces{{
    {0,  1, 0},  // +X
    {0, -1, 1},  // -X
    {1,  1, 2},  // +Y
    {1, -1, 3},  // -Y
    {2,  1, 4},  // +Z
    {2, -1, 5},  // -Z
}};

// Match the directional shading used by GreedyMesher::shadeForFace so
// LOD2 clusters lit by the fallback path (no baked-light) match LOD0/1
// shading at the seam.
std::uint32_t shadeForFace(const FaceDesc& face) noexcept
{
    if (face.axis == 1 && face.sign > 0) return 255U;
    if (face.axis == 1 && face.sign < 0) return 120U;
    if (face.axis == 0)                  return 165U;
    return 190U;
}

[[nodiscard]] std::size_t supervoxelIndex(int x, int y, int z) noexcept
{
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(y) * kSupervoxelExtent
        + static_cast<std::size_t>(z) * kSupervoxelExtent * kSupervoxelExtent;
}

// Walk the 8 source blocks belonging to one supervoxel and pick a dominant
// material via majority vote. Policy:
//   - If at least 4 of 8 sub-blocks are non-air, the supervoxel is non-air.
//   - Material chosen by max count among non-air materials; ties broken by
//     LOWER BlockStateId.value so the result is deterministic across runs
//     and across machines.
//
// `chunkAt(dx,dy,dz)` returns a Chunk* for the (dx,dy,dz) chunk within the
// cluster (or nullptr if that chunk wasn't loaded). `(sx,sy,sz)` are
// supervoxel coords in [0, kSupervoxelExtent).
ClusterCell reduceSupervoxel(int sx, int sy, int sz,
                             const ClusterChunkSnapshot& snapshot,
                             const BlockRenderCatalog& catalog) noexcept
{
    // Source block range: [bx0, bx0+2) etc., in cluster-local block units.
    const int bx0 = sx * 2;
    const int by0 = sy * 2;
    const int bz0 = sz * 2;

    // Count materials. Most clusters have few distinct materials per
    // supervoxel (typically 1-3); a small fixed-size array beats a
    // hash map on cache, especially at 262k supervoxels per cluster.
    constexpr std::size_t kMaxCandidates = 8;
    struct Candidate {
        std::uint32_t materialId{};
        std::uint8_t  count{};
    };
    std::array<Candidate, kMaxCandidates> candidates{};
    std::size_t candidateCount = 0;
    std::uint8_t airCount = 0;
    std::uint8_t unknownCount = 0;

    for (int sub = 0; sub < 8; ++sub) {
        const int bx = bx0 + (sub & 1);
        const int by = by0 + ((sub >> 1) & 1);
        const int bz = bz0 + ((sub >> 2) & 1);

        // Which chunk owns this source block?
        const int cx = bx / world::ChunkSize;
        const int cy = by / world::ChunkSize;
        const int cz = bz / world::ChunkSize;
        const int lx = bx - cx * world::ChunkSize;
        const int ly = by - cy * world::ChunkSize;
        const int lz = bz - cz * world::ChunkSize;

        const auto& chunkOpt = snapshot.chunks[clusterLocalChunkIndex(cx, cy, cz)];
        if (!chunkOpt.has_value()) {
            // Partial cluster build: this sub-block's chunk hasn't streamed
            // in yet. Tracked separately from `airCount` so we can flag the
            // whole supervoxel as Unknown (face-suppressing) when most of
            // it is from missing data.
            ++unknownCount;
            continue;
        }
        const auto state = chunkOpt->blockAtUnchecked(lx, ly, lz);
        if (state.value == world::AirBlockState.value) {
            ++airCount;
            continue;
        }

        // Bump candidate count (or insert new candidate).
        bool found = false;
        for (std::size_t i = 0; i < candidateCount; ++i) {
            if (candidates[i].materialId == state.value) {
                ++candidates[i].count;
                found = true;
                break;
            }
        }
        if (!found && candidateCount < kMaxCandidates) {
            candidates[candidateCount].materialId = state.value;
            candidates[candidateCount].count = 1;
            ++candidateCount;
        }
        // If we hit kMaxCandidates (which would require 8 distinct materials
        // in one supervoxel — vanishingly rare in voxel terrain), we ignore
        // further candidates. The dominant material will still be the most
        // frequent one we did record.
    }

    // Unknown supervoxels: any sub-block in a missing chunk poisons the
    // whole supervoxel. We can't tell whether the "true" content is solid
    // or air, so we tell the face-pass to skip these AND suppress face
    // emission on adjacent loaded supervoxels (otherwise we'd see false
    // walls/floors at the missing-chunk seam).
    if (unknownCount > 0) {
        ClusterCell unknown{};
        unknown.flags = 4u; // bit 2 = unknown
        return unknown;
    }

    // Majority threshold: at least half of the 8 sub-blocks must be non-air.
    if (airCount > 4 || candidateCount == 0) {
        return ClusterCell{}; // supervoxel is air
    }

    // Pick the candidate with the highest count; deterministic tiebreak
    // by lowest material ID.
    Candidate best = candidates[0];
    for (std::size_t i = 1; i < candidateCount; ++i) {
        const auto& c = candidates[i];
        if (c.count > best.count
            || (c.count == best.count && c.materialId < best.materialId)) {
            best = c;
        }
    }

    const auto info = catalog.get(voxel::BlockStateId{best.materialId});
    ClusterCell cell;
    cell.materialId = best.materialId;
    cell.surface = static_cast<std::uint8_t>(info.surface);
    std::uint8_t flags = 0;
    if (info.occludesNeighborFaces) flags |= 1u;
    if (info.isFluid)                flags |= 2u;
    cell.flags = flags;
    return cell;
}

// Border-cell variant of reduceSupervoxel for cross-cluster face culling.
// Reads source blocks from `snapshot.neighborSlabs[faceIdx]` instead of
// the cluster's own interior chunks. `sv_u, sv_v` are the supervoxel
// tangent-axis coordinates in [0, kSupervoxelExtent) on the face.
//
// The face's slab-axis supervoxel coord is implicit: 0 for positive
// faces (neighbor's near side), kSupervoxelExtent-1 for negative faces.
// 8 source blocks per supervoxel; each block lookup figures out which
// slab chunk (cu, cv) it lives in and reads from the neighbor's chunk
// data via `clusterFaceSlabIndex`.
//
// Unknown handling matches reduceSupervoxel: a missing slab chunk
// marks the entire supervoxel as Unknown, which the classifier reads
// as "don't emit boundary faces here" — fine fallback at the world's
// outer edge or when neighbor isn't loaded yet.
ClusterCell reduceNeighborSupervoxel(int faceIdx, int sv_u, int sv_v,
                                      const ClusterChunkSnapshot& snapshot,
                                      const BlockRenderCatalog& catalog) noexcept
{
    const auto& desc = kFaces[faceIdx];
    const int axis = desc.axis;
    const int uAxis = (axis + 1) % 3;
    const int vAxis = (axis + 2) % 3;

    // Neighbor's supervoxel coord in its own local supervoxel space.
    int sv_n[3];
    sv_n[axis]  = (desc.sign > 0) ? 0 : (kSupervoxelExtent - 1);
    sv_n[uAxis] = sv_u;
    sv_n[vAxis] = sv_v;

    constexpr std::size_t kMaxCandidates = 8;
    struct Candidate {
        std::uint32_t materialId{};
        std::uint8_t  count{};
    };
    std::array<Candidate, kMaxCandidates> candidates{};
    std::size_t candidateCount = 0;
    std::uint8_t airCount = 0;
    std::uint8_t unknownCount = 0;

    for (int sub = 0; sub < 8; ++sub) {
        // 8 source blocks of this supervoxel in neighbor's local space.
        const int blocks[3] = {
            sv_n[0] * 2 + (sub & 1),
            sv_n[1] * 2 + ((sub >> 1) & 1),
            sv_n[2] * 2 + ((sub >> 2) & 1),
        };
        // Which chunk in the slab? Indexed by chunk coords on the two
        // tangent axes. The slab-axis chunk is implicit (and unused for
        // indexing — there's only one chunk-thick slab per face).
        const int cu = blocks[uAxis] / world::ChunkSize;
        const int cv = blocks[vAxis] / world::ChunkSize;
        if (cu < 0 || cu >= world::ClusterChunkExtent
            || cv < 0 || cv >= world::ClusterChunkExtent) {
            ++unknownCount;
            continue;
        }
        const auto& chunkOpt =
            snapshot.neighborSlabs[faceIdx][clusterFaceSlabIndex(cu, cv)];
        if (!chunkOpt.has_value()) {
            ++unknownCount;
            continue;
        }
        // Local coords within the slab chunk.
        const int lx = blocks[0] % world::ChunkSize;
        const int ly = blocks[1] % world::ChunkSize;
        const int lz = blocks[2] % world::ChunkSize;
        const auto state = chunkOpt->blockAtUnchecked(lx, ly, lz);
        if (state.value == world::AirBlockState.value) {
            ++airCount;
            continue;
        }
        bool found = false;
        for (std::size_t i = 0; i < candidateCount; ++i) {
            if (candidates[i].materialId == state.value) {
                ++candidates[i].count;
                found = true;
                break;
            }
        }
        if (!found && candidateCount < kMaxCandidates) {
            candidates[candidateCount].materialId = state.value;
            candidates[candidateCount].count = 1;
            ++candidateCount;
        }
    }

    if (unknownCount > 0) {
        ClusterCell unknown{};
        unknown.flags = 4u;
        return unknown;
    }
    if (airCount > 4 || candidateCount == 0) {
        return ClusterCell{}; // air
    }
    Candidate best = candidates[0];
    for (std::size_t i = 1; i < candidateCount; ++i) {
        const auto& c = candidates[i];
        if (c.count > best.count
            || (c.count == best.count && c.materialId < best.materialId)) {
            best = c;
        }
    }
    const auto info = catalog.get(voxel::BlockStateId{best.materialId});
    ClusterCell cell;
    cell.materialId = best.materialId;
    cell.surface = static_cast<std::uint8_t>(info.surface);
    std::uint8_t flags = 0;
    if (info.occludesNeighborFaces) flags |= 1u;
    if (info.isFluid)                flags |= 2u;
    cell.flags = flags;
    return cell;
}

// Build the full 64³ supervoxel grid. Returns the populated grid plus the
// bounding box of non-air content so the face passes can skip empty slices.
struct SupervoxelBounds {
    int minX{kSupervoxelExtent}, maxX{-1};
    int minY{kSupervoxelExtent}, maxY{-1};
    int minZ{kSupervoxelExtent}, maxZ{-1};
    [[nodiscard]] bool empty() const noexcept { return maxX < minX; }
};

SupervoxelBounds populateSupervoxelGrid(const ClusterChunkSnapshot& snapshot,
                                        const BlockRenderCatalog& catalog,
                                        std::vector<ClusterCell>& grid)
{
    SupervoxelBounds bounds;
    for (int z = 0; z < kSupervoxelExtent; ++z) {
        for (int y = 0; y < kSupervoxelExtent; ++y) {
            for (int x = 0; x < kSupervoxelExtent; ++x) {
                auto& slot = grid[supervoxelIndex(x, y, z)];
                slot = reduceSupervoxel(x, y, z, snapshot, catalog);
                // Bounds track only solid cells. Unknown (missing-chunk)
                // cells emit no geometry, so they don't contribute either.
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

// Face-visibility check at supervoxel resolution. Cross-cluster lookups
// are NOT implemented yet — out-of-bounds neighbours are treated as air
// (conservative: face is visible). Phase 1B+1 will add neighbour cluster
// snapshots so cluster seams cull cleanly.
[[nodiscard]] bool isFaceVisible(const std::vector<ClusterCell>& grid,
                                  int x, int y, int z, const FaceDesc& face,
                                  const ClusterCell& current) noexcept
{
    const int nx = x + (face.axis == 0 ? face.sign : 0);
    const int ny = y + (face.axis == 1 ? face.sign : 0);
    const int nz = z + (face.axis == 2 ? face.sign : 0);
    if (nx < 0 || ny < 0 || nz < 0
        || nx >= kSupervoxelExtent
        || ny >= kSupervoxelExtent
        || nz >= kSupervoxelExtent) {
        return true; // cluster boundary, conservative emit
    }
    const auto& adj = grid[supervoxelIndex(nx, ny, nz)];
    // Suppress faces adjacent to Unknown cells — those sit on a missing-
    // chunk seam, and we'd rather show no face than a false one. When the
    // missing chunks stream in, builtClusters_'s residency-mask check
    // (Application::tickClusterMaintenance) triggers a rebuild that fills
    // in the now-known geometry.
    if (adj.isUnknown()) return false;
    if (adj.isAir()) return true;
    // Fluid-vs-fluid same-material cull (matches GreedyMesher semantics).
    if (current.isFluidBlock() && adj.isFluidBlock()
        && voxel::world::blockTypeOf(voxel::BlockStateId{current.materialId})
         == voxel::world::blockTypeOf(voxel::BlockStateId{adj.materialId})) {
        return false;
    }
    return !adj.occludes();
}

struct Quad {
    std::array<std::array<int, 3>, 4> corners{}; // in block units
    std::uint8_t faceIndex{};
    std::uint32_t materialId{};
    std::uint32_t packedLight{};
    MeshSurface surface{MeshSurface::Opaque};
};

// Build a single greedy-merged quad. uStart/vStart/width/height are in
// SUPERVOXEL units; we multiply by 2 when emitting to convert to block
// units (the on-disk ClusterVertex coordinate space).
Quad makeQuad(const FaceDesc& face, int slicePlane,
              int uStart, int vStart, int width, int height,
              std::uint32_t materialId, std::uint32_t packedLight, MeshSurface surface) noexcept
{
    const int uAxis = (face.axis + 1) % 3;
    const int vAxis = (face.axis + 2) % 3;

    std::array<int, 3> c0{}, c1{}, c2{}, c3{};
    // Convert supervoxel plane to block coordinate (2 blocks per supervoxel).
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

// Cave-fill pre-pass. For every (x, z) column in the supervoxel grid,
// scan top-down to find the highest non-air supervoxel (the column's
// local "surface"), then fill any AIR cell more than
// kCaveDepthSupervoxels below that surface with the surface's material.
//
// Why: at LOD2 distance (24-48 chunks ≈ 768-1536 blocks), interior
// cave detail is sub-pixel — the GPU classifier emits thousands of
// cave-wall faces nobody can see. Filling them eliminates ~60-85% of
// cluster face count for chunky terrain, which proportionally cuts
// GPU classify output, worker greedy-merge cost, and final draw size.
//
// What's preserved: Unknown cells (missing-chunk seams), the surface
// itself, and any air within kCaveDepthSupervoxels of the surface —
// so open cliffs, valleys, and shallow overhangs the player can
// actually see at LOD distance remain visible. Deep enclosed cave
// systems get flattened to stone.
//
// kCaveDepthSupervoxels = 4 supervoxels = 8 blocks below local surface.
void fillCavesBelowSurface(std::vector<ClusterCell>& grid,
                            const SupervoxelBounds& bounds) noexcept
{
    constexpr int kCaveDepthSupervoxels = 4;
    for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
        for (int x = bounds.minX; x <= bounds.maxX; ++x) {
            // Find topmost solid in this column.
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
            if (topSolidY < 0) continue; // all air or all unknown

            // Fill air below threshold. Leave Unknown cells alone (the
            // mesher already suppresses faces there) and never touch
            // existing solid cells.
            const int caveCutoffY = topSolidY - kCaveDepthSupervoxels;
            for (int y = caveCutoffY; y >= bounds.minY; --y) {
                auto& cell = grid[supervoxelIndex(x, y, z)];
                if (cell.materialId == 0u && (cell.flags & 4u) == 0u) {
                    cell.materialId = topMaterial;
                    cell.surface = topSurface;
                    // Force occludes-bit on so the classifier treats
                    // filled cells exactly like surface stone — no
                    // faces emitted for neighbors against them.
                    cell.flags = topFlags | 1u;
                }
            }
        }
    }
}

// One full face pass over the supervoxel grid. Mirrors GreedyMesher's
// `greedyFacePass`, just operating on `ClusterCell` and emitting quads
// at supervoxel granularity into a flat quad vector.
void runFacePass(const std::vector<ClusterCell>& grid,
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

        // --- Build mask of visible faces in this slice ----------------
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

        // --- Greedy merge sweep ---------------------------------------
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

std::uint64_t hashClusterChunkRevisions(const ClusterChunkSnapshot& snapshot) noexcept
{
    std::uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a offset basis
    for (const auto& chunkOpt : snapshot.chunks) {
        const std::uint64_t rev = chunkOpt.has_value()
            ? static_cast<std::uint64_t>(chunkOpt->revision())
            : 0xFFFFFFFFFFFFFFFFULL; // sentinel for "absent" so a chunk
                                     // appearing/disappearing also flips the hash
        hash ^= rev;
        hash *= 0x100000001b3ULL; // FNV-1a prime
    }
    return hash;
}

ClusterMesh ClusterMesher::build(const ClusterChunkSnapshot& snapshot,
                                  const BlockRenderCatalog& catalog) const
{
    ClusterMesh mesh;
    mesh.coord = snapshot.coord;
    mesh.sourceRevisionsHash = hashClusterChunkRevisions(snapshot);

    // 1. Reduce 128³ source blocks to 64³ supervoxels.
    std::vector<ClusterCell> grid(kSupervoxelVolume);
    const auto bounds = populateSupervoxelGrid(snapshot, catalog, grid);
    if (bounds.empty()) {
        return mesh; // all-air cluster
    }

    // 1b. Cave-fill: same pre-pass as the GPU path — see
    // fillCavesBelowSurface() for rationale.
    fillCavesBelowSurface(grid, bounds);

    // 2. Six greedy face passes.
    std::vector<Quad> quads;
    quads.reserve(kSupervoxelVolume / 4);
    for (const auto& face : kFaces) {
        runFacePass(grid, bounds, face, quads);
    }

    // 3. Sort by (surface, material) to give appendQuadToMesh's coalesce
    //    heuristic the best chance of producing few draw ranges. Same
    //    ordering trick GreedyMesher uses.
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
//  GPU-hybrid path: CPU does reduction, GPU does face classification, CPU
//  does greedy merge. Same final mesh shape as build() — just different
//  staging.
// ============================================================================

std::vector<ClusterGpuMeshing::GpuCellInfo>
ClusterMesher::buildPaddedCellGrid(const ClusterChunkSnapshot& snapshot,
                                    const BlockRenderCatalog& catalog) const
{
    // 1. Run the same supervoxel reduction the CPU path uses.
    std::vector<ClusterCell> grid(kSupervoxelVolume);
    const auto bounds = populateSupervoxelGrid(snapshot, catalog, grid);
    if (bounds.empty()) {
        return {}; // all-air cluster — caller should skip GPU submit
    }

    // ---- 1b. Cave-fill pre-pass ----------------------------------------
    // Shared between the GPU and CPU paths via fillCavesBelowSurface().
    fillCavesBelowSurface(grid, bounds);

    // 2. Pack into the GPU shader's 66³ padded layout. Border cells stay
    //    zero-initialized (= air, since GpuCellInfo{}.materialId == 0).
    //    Interior cells map [0..63] → [1..64] in padded coords.
    //
    //    OPTIMIZATION: we only memset/write the bounding box of non-air
    //    cells (`bounds`). For typical terrain clusters, the non-air
    //    band is ~8-32 supervoxels deep on the Y axis (surface ± nearby
    //    rock), so this skips ~60-95% of the 4.6 MB buffer's memset
    //    that vector(N) would otherwise do.
    constexpr std::uint32_t kPadded = ClusterGpuMeshing::paddedExtent(); // 66
    // Default-construct (uninitialized capacity), then resize to full
    // count with value-init only for the cells we'll actually touch.
    // The std::vector(N) constructor would zero-init the whole 4.6 MB;
    // we avoid that with reserve+resize-uninit-equivalent below.
    std::vector<ClusterGpuMeshing::GpuCellInfo> cells;
    cells.resize(ClusterGpuMeshing::paddedCellCount()); // unavoidable zero-init for safety

    const auto paddedIdx = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
        return x + y * kPadded + z * kPadded * kPadded;
    };

    // Helper to pack a ClusterCell into a GpuCellInfo at a padded index.
    // Used for both the interior loop below and the border fill that
    // follows.
    const auto packCell = [&cells](std::size_t idx, const ClusterCell& cell) {
        auto& dst = cells[idx];
        dst.materialId = cell.materialId;
        dst.packedLight = 0u;
        std::uint32_t flags = 0;
        if (cell.occludes())     flags |= ClusterGpuMeshing::kFlagOccludes;
        if (cell.isFluidBlock()) flags |= ClusterGpuMeshing::kFlagFluid;
        if (cell.isUnknown())    flags |= ClusterGpuMeshing::kFlagUnknown;
        flags |= (static_cast<std::uint32_t>(cell.surface) & 3u)
                  << ClusterGpuMeshing::kSurfaceShift;
        dst.flags = flags;
    };

    // Iterate only the non-air bounding box. Saves ~90% of inner loop
    // iterations for typical clusters (most of the 64³ grid is air for
    // surface-altitude clusters).
    for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
        for (int y = bounds.minY; y <= bounds.maxY; ++y) {
            for (int x = bounds.minX; x <= bounds.maxX; ++x) {
                const auto& cell = grid[supervoxelIndex(x, y, z)];
                if (cell.isAir()) {
                    continue; // leave as zero-init air
                }
                packCell(paddedIdx(
                    static_cast<std::uint32_t>(x + 1),
                    static_cast<std::uint32_t>(y + 1),
                    static_cast<std::uint32_t>(z + 1)),
                    cell);
            }
        }
    }

    // ---- Cross-cluster border fill -------------------------------------
    // Phase 1D-2d: reduce 2-block-thick slabs from each neighbor cluster
    // into our padded grid's 1-cell border. The GPU classifier then
    // produces accurate boundary face emission instead of always-emit
    // (which doubled face geometry at cluster seams).
    //
    // For each of the 6 faces:
    //   - face axis + sign determines which border plane in our padded
    //     grid (x=0/65, y=0/65, or z=0/65) we're filling
    //   - the supervoxel at the neighbor's near-side row maps 1:1 to
    //     our border cells along the two tangent axes
    //   - unloaded neighbor → reduceNeighborSupervoxel returns Unknown,
    //     which the classifier reads as "don't emit boundary face"
    for (int faceIdx = 0; faceIdx < 6; ++faceIdx) {
        // FAST-SKIP: if no slab chunks are loaded for this face, the
        // entire 64×64 border row would just produce Unknown cells —
        // skip the 4096 supervoxel reductions entirely. This is the
        // common case for +/-Y faces (vertical render distance of 2
        // chunks vs cluster height of 4 means the Y neighbors almost
        // never have data) and for the outermost cluster ring (where
        // the neighbor cluster is past the chunk render distance).
        //
        // PERF NOTE: this is the load-bearing optimization. Without
        // it, border culling adds ~4-6 ms per cluster build (24576
        // supervoxel reductions × 8 sub-block lookups each). With it,
        // typical clusters pay <0.5 ms for border culling because
        // most faces no-op.
        const auto& slab = snapshot.neighborSlabs[faceIdx];
        bool anySlabLoaded = false;
        for (const auto& chunk : slab) {
            if (chunk.has_value()) { anySlabLoaded = true; break; }
        }
        if (!anySlabLoaded) {
            continue; // leave this face's border cells zero-init (air)
        }

        const auto& desc = kFaces[faceIdx];
        const int axis = desc.axis;
        const int uAxis = (axis + 1) % 3;
        const int vAxis = (axis + 2) % 3;
        const std::uint32_t borderAxisCoord =
            (desc.sign > 0) ? (kPadded - 1) : 0u;

        for (int sv_v = 0; sv_v < kSupervoxelExtent; ++sv_v) {
            for (int sv_u = 0; sv_u < kSupervoxelExtent; ++sv_u) {
                const auto cell = reduceNeighborSupervoxel(
                    faceIdx, sv_u, sv_v, snapshot, catalog);
                // Air → leave the padded cell zero-initialized.
                if (cell.isAir() && !cell.isUnknown()) {
                    continue;
                }
                std::uint32_t paddedCoords[3];
                paddedCoords[axis]  = borderAxisCoord;
                paddedCoords[uAxis] = static_cast<std::uint32_t>(sv_u + 1);
                paddedCoords[vAxis] = static_cast<std::uint32_t>(sv_v + 1);
                packCell(paddedIdx(paddedCoords[0],
                                   paddedCoords[1],
                                   paddedCoords[2]),
                         cell);
            }
        }
    }

    return cells;
}

namespace {

// Per-slice mask used by the greedy merge. Same shape as runFacePass's
// internal masks (kSupervoxelExtent²) — but populated from the GPU's
// VisibleFace list instead of recomputed via isFaceVisible.
struct SliceMaskScratch {
    std::vector<std::uint32_t> material;
    std::vector<std::uint32_t> light;
    std::vector<std::uint8_t>  surface;
    SliceMaskScratch()
        : material(static_cast<std::size_t>(kSupervoxelExtent) * kSupervoxelExtent, 0u),
          light(material.size(), 0u),
          surface(material.size(), 0u) {}
};

// Decompose a supervoxel-grid cellIndex (x + y*64 + z*64*64) into (x, y, z).
inline void unpackCellIndex(std::uint32_t cellIdx, int& x, int& y, int& z) noexcept
{
    constexpr int kE = kSupervoxelExtent;
    x = static_cast<int>(cellIdx % kE);
    y = static_cast<int>((cellIdx / kE) % kE);
    z = static_cast<int>(cellIdx / (kE * kE));
}

// Greedy-merge a single slice's mask into the quads vector. Mirrors the
// inner loop of runFacePass — extracted so we can call it once per
// (faceDirection, slice) pair from the GPU path.
void greedyMergeSlice(SliceMaskScratch& mask,
                      const FaceDesc& face,
                      int slice,
                      std::vector<Quad>& quads)
{
    auto& maskMaterial = mask.material;
    auto& maskLight = mask.light;
    auto& maskSurface = mask.surface;

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
                const std::size_t rowBase =
                    static_cast<std::size_t>(u + (v + height) * kSupervoxelExtent);
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
                const std::size_t rowBase =
                    static_cast<std::size_t>(u + (v + clearV) * kSupervoxelExtent);
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

} // namespace

ClusterMesh ClusterMesher::buildMeshFromGpuFaces(
    world::ClusterCoord coord,
    std::uint64_t sourceRevisionsHash,
    const std::vector<ClusterGpuMeshing::VisibleFace>& faces) const
{
    ClusterMesh mesh;
    mesh.coord = coord;
    mesh.sourceRevisionsHash = sourceRevisionsHash;
    if (faces.empty()) {
        return mesh;
    }

    // 1. Repack GPU faces into a flat array with a sort key that groups
    //    by (faceIndex, slicePlane). One linear pass instead of 384
    //    separate vector allocations — saves both alloc churn and the
    //    cache misses from scattering writes across many tiny vectors.
    //
    //    SortKey layout (single uint64 so std::sort uses native compares):
    //      bits 60..63: faceIndex (0..5, fits in 3 bits with slack)
    //      bits 48..59: slicePlane (0..63, fits in 6 bits with slack)
    //      bits 0..47:  unused (could pack u, v, material for more
    //                   compact storage; left empty for clarity)
    struct PackedFace {
        std::uint64_t key;          // faceIndex << 60 | slice << 48
        std::uint16_t u;
        std::uint16_t v;
        std::uint32_t materialId;
        std::uint32_t packedLight;
        std::uint8_t  surface;
    };
    std::vector<PackedFace> packed;
    packed.reserve(faces.size());

    for (const auto& face : faces) {
        if (face.faceIndex >= 6) continue;
        const auto& desc = kFaces[face.faceIndex];
        int x = 0, y = 0, z = 0;
        unpackCellIndex(face.cellIndex, x, y, z);
        const int axisCoord[3] = {x, y, z};
        const int sliceCoord  = axisCoord[desc.axis];
        const int uCoord      = axisCoord[(desc.axis + 1) % 3];
        const int vCoord      = axisCoord[(desc.axis + 2) % 3];
        if (sliceCoord < 0 || sliceCoord >= kSupervoxelExtent) continue;
        const std::uint64_t key =
            (static_cast<std::uint64_t>(face.faceIndex) << 60u)
          | (static_cast<std::uint64_t>(sliceCoord) << 48u);
        packed.push_back({
            key,
            static_cast<std::uint16_t>(uCoord),
            static_cast<std::uint16_t>(vCoord),
            face.materialId, face.packedLight,
            static_cast<std::uint8_t>(face.surface)
        });
    }
    std::sort(packed.begin(), packed.end(),
              [](const PackedFace& a, const PackedFace& b) { return a.key < b.key; });

    // 2. Sweep the sorted list. Each run of equal `key` is one
    //    (faceIndex, slice) pair → populate mask, run greedy merge.
    std::vector<Quad> quads;
    quads.reserve(packed.size() / 4U); // rough estimate after greedy merge
    SliceMaskScratch mask;

    std::size_t i = 0;
    while (i < packed.size()) {
        const std::uint64_t key = packed[i].key;
        const std::size_t runStart = i;
        while (i < packed.size() && packed[i].key == key) ++i;
        const std::size_t runEnd = i;

        const std::size_t faceIdx =
            static_cast<std::size_t>(key >> 60u);
        const int slice =
            static_cast<int>((key >> 48u) & 0xFFFFu);
        if (faceIdx >= 6) continue;
        const auto& desc = kFaces[faceIdx];

        std::fill(mask.material.begin(), mask.material.end(), 0u);
        for (std::size_t j = runStart; j < runEnd; ++j) {
            const auto& pf = packed[j];
            if (pf.u >= kSupervoxelExtent) continue;
            if (pf.v >= kSupervoxelExtent) continue;
            const std::size_t mi = static_cast<std::size_t>(
                pf.u + pf.v * kSupervoxelExtent);
            mask.material[mi] = pf.materialId;
            mask.light[mi]    = pf.packedLight;
            mask.surface[mi]  = pf.surface;
        }
        greedyMergeSlice(mask, desc, slice, quads);
    }

    // 3. Sort + append to mesh (mirror of build()'s tail).
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

} // namespace voxel::render::meshing
