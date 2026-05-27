#include <voxel/render/meshing/GreedyMesher.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <voxel/render/meshing/BlockRenderCatalog.hpp>

namespace voxel::render::meshing {

namespace {

// Optimization (Pass W0): precomputed per-cell block info for the chunk being
// meshed. Built once at the top of build(); the hot mesher loop then does
// array indexing instead of palette + bit-packed lookups + catalog hashes
// for every one of the ~200,000 cell visits per chunk.
//
// Memory: 8 bytes × ChunkVolume (32³) = 256 KB per cache, fits comfortably in
// L2 on any modern CPU. We use thread_local storage so each worker thread has
// its own cache (the mesher can run on N workers concurrently).
struct CachedBlock {
    std::uint32_t blockValue{};   // 0 = air
    std::uint8_t  surface{};      // MeshSurface enum value (0/1/2)
    std::uint8_t  flags{};        // bit 0 = occludes, bit 1 = isFluid
    std::uint8_t  _pad[2]{};

    [[nodiscard]] bool isAir() const noexcept { return blockValue == 0u; }
    [[nodiscard]] bool occludes() const noexcept { return (flags & 1u) != 0u; }
    [[nodiscard]] bool isFluidBlock() const noexcept { return (flags & 2u) != 0u; }
};
static_assert(sizeof(CachedBlock) == 8, "CachedBlock should be 8 bytes");

constexpr std::size_t kCacheCount = static_cast<std::size_t>(world::ChunkVolume);

[[nodiscard]] constexpr std::size_t flatIndex(int x, int y, int z) noexcept
{
    return static_cast<std::size_t>(x)
         + static_cast<std::size_t>(y) * world::ChunkSize
         + static_cast<std::size_t>(z) * world::ChunkSize * world::ChunkSize;
}

[[nodiscard]] constexpr std::uint16_t localIndex(int x, int y, int z) noexcept
{
    return static_cast<std::uint16_t>(
        x + y * world::ChunkSize + z * world::ChunkSize * world::ChunkSize);
}

[[nodiscard]] constexpr std::array<int, 3> decodeLocalIndex(std::uint16_t index) noexcept
{
    const int x = static_cast<int>(index & 31U);
    const int y = static_cast<int>((index >> 5U) & 31U);
    const int z = static_cast<int>((index >> 10U) & 31U);
    return {x, y, z};
}

// Per-thread scratch buffers: avoids 6 vector allocations (mask) + 1 cache
// allocation (256 KB) per chunk. Workers are persistent, so this storage
// lives for the worker's lifetime.
struct MesherScratch {
    std::array<CachedBlock, kCacheCount> cache{};
    // Mask state in SoA form. The inner equality check compares all three
    // arrays; keeping them split into tight byte/uint vectors avoids touching
    // unused fields the way a fat struct would.
    std::vector<std::uint8_t> maskSurfaces;
    std::vector<std::uint32_t> maskLight;
    std::vector<std::uint32_t> maskBlockValue;

    MesherScratch()
    {
        const auto n = static_cast<std::size_t>(world::ChunkSize) * world::ChunkSize;
        maskSurfaces.resize(n);
        maskLight.resize(n);
        maskBlockValue.resize(n);
    }
};

thread_local MesherScratch tlsScratch;

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

struct ChunkAirBounds {
    int minY{world::ChunkSize};  // first Y row with a non-air block
    int maxY{-1};                // last Y row with a non-air block
    int minX{world::ChunkSize};
    int maxX{-1};
    int minZ{world::ChunkSize};
    int maxZ{-1};

    [[nodiscard]] bool empty() const noexcept { return maxY < minY; }
};

// Fill the thread-local cache from chunk data. One pass over all 32³ cells.
// After this, the mesher's hot loops can use direct array indexing on
// `cache[]` instead of going through palette + bit-packed lookups.
// Also returns axis-aligned bounds over non-air cells so face passes can
// skip slices that are guaranteed to produce no quads.
ChunkAirBounds populateBlockCache(const world::Chunk& chunk, const BlockRenderCatalog& catalog,
                                   std::array<CachedBlock, kCacheCount>& cache)
{
    // Per-palette-entry lookup table: chunks usually have ≤16 unique block
    // states, so resolving the catalog 16× is much cheaper than resolving it
    // 32K× even with hash caching. We then look up by palette index in the
    // inner loop. NB: requires that the palette is stable for the duration of
    // this function (we're called inside a single threaded build() with a
    // snapshot of the chunk — no concurrent mutation).
    const auto& palette = chunk.blockData().palette;
    const auto& indices = chunk.blockData().indices;
    const auto paletteSize = palette.size();

    // Resolve renderInfo once per palette entry.
    struct PaletteEntry {
        std::uint32_t blockValue{};
        std::uint8_t surface{};
        std::uint8_t flags{};
    };
    std::array<PaletteEntry, 256> paletteCache{};
    const std::size_t entryCount = std::min<std::size_t>(paletteSize, paletteCache.size());
    for (std::size_t i = 0; i < entryCount; ++i) {
        const auto state = palette.at(static_cast<std::uint16_t>(i));
        const auto info = catalog.get(state);
        const bool isAirEntry = state.value == world::AirBlockState.value;
        paletteCache[i].blockValue = isAirEntry ? 0u : state.value;
        paletteCache[i].surface = static_cast<std::uint8_t>(info.surface);
        std::uint8_t flags = 0u;
        if (info.occludesNeighborFaces) flags |= 1u;
        if (info.isFluid) flags |= 2u;
        paletteCache[i].flags = flags;
    }

    // Iterate XYZ in storage order — matches the chunk's flat index layout for
    // sequential reads. CachedBlock is also stored in the same order so the
    // writes are sequential too. Track per-axis bounds of non-air content so
    // face passes can skip empty slices.
    ChunkAirBounds bounds;
    for (int z = 0; z < world::ChunkSize; ++z) {
        for (int y = 0; y < world::ChunkSize; ++y) {
            for (int x = 0; x < world::ChunkSize; ++x) {
                const auto chunkIdx = world::Chunk::index(x, y, z);
                const auto paletteIdx = indices.at(chunkIdx);
                const auto& entry = paletteCache[paletteIdx];
                auto& slot = cache[flatIndex(x, y, z)];
                slot.blockValue = entry.blockValue;
                slot.surface = entry.surface;
                slot.flags = entry.flags;
                if (entry.blockValue != 0u) {
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
    catalog.set(voxel::BlockTypeId{0}, {MeshSurface::Transparent, false, false});
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

// Optimized face-visibility check using the precomputed CachedBlock array.
// Local-chunk lookups become array indexing; only cross-chunk neighbour reads
// still go through the slow path. Same semantics as the old version.
bool faceVisibleCached(const std::array<CachedBlock, kCacheCount>& cache,
                       const world::Chunk& chunk, const BlockRenderCatalog& catalog,
                       const ChunkNeighborhood& neighborhood,
                       int x, int y, int z, const FaceDesc& face,
                       const CachedBlock& current,
                       const MeshingOptions& options)
{
    if (current.isFluidBlock() && face.axis == 1 && face.sign > 0 && options.staticWaterSurfaceY.has_value()) {
        const std::int64_t worldFaceY = chunk.coord().y * static_cast<std::int64_t>(world::ChunkSize) + y + 1;
        if (worldFaceY <= static_cast<std::int64_t>(*options.staticWaterSurfaceY)) {
            return false;
        }
    }

    const int nx = x + (face.axis == 0 ? face.sign : 0);
    const int ny = y + (face.axis == 1 ? face.sign : 0);
    const int nz = z + (face.axis == 2 ? face.sign : 0);
    const bool outOfBounds = (nx < 0 || ny < 0 || nz < 0
        || nx >= world::ChunkSize || ny >= world::ChunkSize || nz >= world::ChunkSize);

    if (outOfBounds) {
        // Cross-chunk neighbour: only a small fraction of all face checks,
        // and only on the chunk's outer shell — leave on the slow path.
        const auto* neighbourChunk = neighborForFace(neighborhood, face);
        if (neighbourChunk == nullptr) {
            // Static ocean/lake water is continuous across chunks. Emitting a
            // conservative transparent face at a missing neighbor creates
            // chunk-sized water walls/floors until that neighbor streams in,
            // which reads as transparency flicker. Local loaded air pockets
            // still render through the non-out-of-bounds path.
            if (current.isFluidBlock() && options.staticWaterSurfaceY.has_value()) {
                const std::int64_t worldBlockY =
                    chunk.coord().y * static_cast<std::int64_t>(world::ChunkSize) + y;
                if (worldBlockY <= static_cast<std::int64_t>(*options.staticWaterSurfaceY)) {
                    return false;
                }
            }
            return true;
        }
        const auto adjacent = sampleNeighbouringChunk(neighbourChunk, face, x, y, z);
        if (isAir(adjacent)) {
            return true;
        }
        const auto adjacentInfo = catalog.get(adjacent);
        if (current.isFluidBlock() && adjacentInfo.isFluid
            && world::blockTypeOf(voxel::BlockStateId{current.blockValue}) == world::blockTypeOf(adjacent)) {
            return false;
        }
        return !adjacentInfo.occludesNeighborFaces;
    }

    // Local: direct array lookup — the hot path.
    const auto& adj = cache[flatIndex(nx, ny, nz)];
    if (adj.isAir()) {
        return true;
    }
    if (current.isFluidBlock() && adj.isFluidBlock()
        && world::blockTypeOf(voxel::BlockStateId{current.blockValue})
         == world::blockTypeOf(voxel::BlockStateId{adj.blockValue})) {
        return false;
    }
    return !adj.occludes();
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

ChunkMesh finalizeMeshFromQuads(const world::Chunk& chunk, std::vector<Quad>& quads)
{
    std::sort(quads.begin(), quads.end(), [](const Quad& lhs, const Quad& rhs) {
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

    auto& sec = mesh.sections[0];
    sec.minY = 0.0F;
    sec.maxY = static_cast<float>(world::ChunkSize);
    sec.vertexOffsetInVertices = 0;
    sec.vertexOffset = 0;
    sec.indexOffset = 0;

    for (const auto& range : mesh.drawRanges) {
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
    return mesh;
}

// Clamp a slice/u/v range to the bounded interval per axis so we never scan
// rows that the cache guarantees are entirely air.
struct AxisRange {
    int sliceLo, sliceHi;   // inclusive, along face.axis
    int uLo, uHi;           // inclusive, along (axis+1)%3
    int vLo, vHi;           // inclusive, along (axis+2)%3
};

[[nodiscard]] AxisRange axisRangeFor(const FaceDesc& face, const ChunkAirBounds& bounds)
{
    const int axisLo[3] = {bounds.minX, bounds.minY, bounds.minZ};
    const int axisHi[3] = {bounds.maxX, bounds.maxY, bounds.maxZ};
    const int uAxis = (face.axis + 1) % 3;
    const int vAxis = (face.axis + 2) % 3;
    return {
        axisLo[face.axis], axisHi[face.axis],
        axisLo[uAxis],     axisHi[uAxis],
        axisLo[vAxis],     axisHi[vAxis],
    };
}

void greedyFacePass(const std::array<CachedBlock, kCacheCount>& cache,
                    const ChunkAirBounds& airBounds,
                    const world::Chunk& chunk, const BlockRenderCatalog& catalog,
                    const world::ChunkLightData* light, const ChunkNeighborhood& neighborhood,
                    const FaceDesc& face, const MeshingOptions& options, std::vector<Quad>& quads)
{
    const int uAxis = (face.axis + 1) % 3;
    const int vAxis = (face.axis + 2) % 3;
    const auto r = axisRangeFor(face, airBounds);

    // Reuse thread-local mask buffer instead of allocating per call.
    // Three parallel arrays in SoA form so the inner equality check (which is
    // the hottest comparison in the greedy merge) touches the smallest
    // possible amount of cache.
    auto& maskSurfaces = tlsScratch.maskSurfaces;
    auto& maskLight = tlsScratch.maskLight;
    auto& maskBlockValue = tlsScratch.maskBlockValue;
    const auto maskSize = static_cast<std::size_t>(world::ChunkSize) * world::ChunkSize;

    // Slices outside the air-bounds along face.axis can still produce faces
    // *facing into* the bounded region, so we expand by one in the direction
    // the face points to. Positive faces gain sliceHi+1; negative gain sliceLo-1.
    const int sliceMin = std::max(0, r.sliceLo + (face.sign < 0 ? -1 : 0));
    const int sliceMax = std::min(world::ChunkSize - 1, r.sliceHi + (face.sign > 0 ? 1 : 0));

    for (int slice = sliceMin; slice <= sliceMax; ++slice) {
        // Clear only the cells we'll touch — sized to exactly one slice plane.
        // memset on a byte buffer is much faster than std::fill on a struct.
        std::fill(maskBlockValue.begin(), maskBlockValue.begin() + maskSize, 0u);

        // --- Build mask: which cells in this slice need a face? --------------
        // Iterate the bounded UV rectangle; cells outside the bounds in u/v
        // can never have a non-air block.
        for (int v = r.vLo; v <= r.vHi; ++v) {
            for (int u = r.uLo; u <= r.uHi; ++u) {
                int pos[3];
                pos[face.axis] = slice;
                pos[uAxis] = u;
                pos[vAxis] = v;

                const auto& current = cache[flatIndex(pos[0], pos[1], pos[2])];
                if (current.isAir()) {
                    continue;
                }
                if (!faceVisibleCached(cache, chunk, catalog, neighborhood,
                                       pos[0], pos[1], pos[2], face, current, options)) {
                    continue;
                }

                const std::size_t maskIdx = static_cast<std::size_t>(u + v * world::ChunkSize);
                maskBlockValue[maskIdx] = current.blockValue;
                maskSurfaces[maskIdx] = current.surface;
                maskLight[maskIdx] = (light != nullptr)
                    ? packLightFromBaked(*light, face, pos[0], pos[1], pos[2])
                    : shadeForFace(face);
            }
        }

        // --- Greedy merge: scan the mask and emit maximal quads --------------
        for (int v = 0; v < world::ChunkSize; ++v) {
            for (int u = 0; u < world::ChunkSize;) {
                const std::size_t baseIdx = static_cast<std::size_t>(u + v * world::ChunkSize);
                const auto blockValue = maskBlockValue[baseIdx];
                if (blockValue == 0u) {
                    ++u;
                    continue;
                }
                const auto surface = maskSurfaces[baseIdx];
                const auto light0 = maskLight[baseIdx];

                // Extend width along U while cells match.
                int width = 1;
                while (u + width < world::ChunkSize) {
                    const std::size_t i = baseIdx + width;
                    if (maskBlockValue[i] != blockValue
                        || maskSurfaces[i] != surface
                        || maskLight[i] != light0) {
                        break;
                    }
                    ++width;
                }

                // Extend height along V while every cell in [u, u+width) matches.
                int height = 1;
                bool done = false;
                while (v + height < world::ChunkSize && !done) {
                    const std::size_t rowBase = static_cast<std::size_t>(u + (v + height) * world::ChunkSize);
                    for (int testU = 0; testU < width; ++testU) {
                        const std::size_t i = rowBase + testU;
                        if (maskBlockValue[i] != blockValue
                            || maskSurfaces[i] != surface
                            || maskLight[i] != light0) {
                            done = true;
                            break;
                        }
                    }
                    if (!done) {
                        ++height;
                    }
                }

                // Clear consumed mask cells (only blockValue, since the others
                // are written-then-read pairwise — the zero check is enough).
                for (int clearV = 0; clearV < height; ++clearV) {
                    const std::size_t rowBase = static_cast<std::size_t>(u + (v + clearV) * world::ChunkSize);
                    for (int clearU = 0; clearU < width; ++clearU) {
                        maskBlockValue[rowBase + clearU] = 0u;
                    }
                }

                const int plane = face.sign > 0 ? slice + 1 : slice;
                MaskCell cell{blockValue, light0, static_cast<MeshSurface>(surface)};
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
                               const ChunkNeighborhood& neighborhood,
                               MeshingOptions options) const
{
    // Optimization (Pass W0): build a flat block cache up-front. Hot loops now
    // hit `cache[]` in ~1ns/cell instead of palette + bit-packed + catalog
    // lookups in ~10-15ns/cell. Saves ~70% of the per-cell cost in the inner
    // greedy passes.
    auto& cache = tlsScratch.cache;
    const auto airBounds = populateBlockCache(chunk, catalog, cache);

    ChunkMesh emptyMesh;
    emptyMesh.sourceRevision = chunk.revision();
    emptyMesh.sourceMeshRevisionHash = chunk.meshRevision();
    // All-air chunk: nothing to mesh, return immediately.
    if (airBounds.empty()) {
        return emptyMesh;
    }

    std::vector<Quad> quads;
    constexpr std::size_t kMaxQuads = static_cast<std::size_t>(world::ChunkVolume) / 4;
    quads.reserve(kMaxQuads);

    for (const auto& face : Faces) {
        greedyFacePass(cache, airBounds, chunk, catalog, light, neighborhood, face, options, quads);
    }

    std::sort(quads.begin(), quads.end(), [](const Quad& lhs, const Quad& rhs) {
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

    // Single section spans the full 32^3 chunk Y range. The y-locality
    // bookkeeping that the 2-section split used (firstIdx → packedPos → y /
    // 16) is no longer needed; all draw ranges contribute to one bucket.
    auto& sec = mesh.sections[0];
    sec.minY = 0.0F;
    sec.maxY = static_cast<float>(world::ChunkSize);
    sec.vertexOffsetInVertices = 0;
    sec.vertexOffset = 0;
    sec.indexOffset = 0;

    for (const auto& range : mesh.drawRanges) {
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

    return mesh;
}

std::vector<VisibleFaceRecord> GreedyMesher::classifyVisibleFaces(
    const world::Chunk& chunk,
    const BlockRenderCatalog& catalog,
    const world::ChunkLightData* light,
    const ChunkNeighborhood& neighborhood,
    MeshingOptions options) const
{
    auto& cache = tlsScratch.cache;
    const auto airBounds = populateBlockCache(chunk, catalog, cache);
    if (airBounds.empty()) {
        return {};
    }

    std::vector<VisibleFaceRecord> records;
    records.reserve(static_cast<std::size_t>(world::ChunkVolume) / 2U);

    for (const auto& face : Faces) {
        const int uAxis = (face.axis + 1) % 3;
        const int vAxis = (face.axis + 2) % 3;
        const auto r = axisRangeFor(face, airBounds);
        const int sliceMin = std::max(0, r.sliceLo + (face.sign < 0 ? -1 : 0));
        const int sliceMax = std::min(world::ChunkSize - 1, r.sliceHi + (face.sign > 0 ? 1 : 0));

        for (int slice = sliceMin; slice <= sliceMax; ++slice) {
            for (int v = r.vLo; v <= r.vHi; ++v) {
                for (int u = r.uLo; u <= r.uHi; ++u) {
                    int pos[3];
                    pos[face.axis] = slice;
                    pos[uAxis] = u;
                    pos[vAxis] = v;

                    const auto& current = cache[flatIndex(pos[0], pos[1], pos[2])];
                    if (current.isAir()) {
                        continue;
                    }
                    if (!faceVisibleCached(cache, chunk, catalog, neighborhood,
                                           pos[0], pos[1], pos[2], face, current, options)) {
                        continue;
                    }

                    records.push_back({
                        localIndex(pos[0], pos[1], pos[2]),
                        static_cast<std::uint8_t>(face.faceIndex),
                        static_cast<MeshSurface>(current.surface),
                        current.blockValue,
                        (light != nullptr)
                            ? packLightFromBaked(*light, face, pos[0], pos[1], pos[2])
                            : shadeForFace(face)
                    });
                }
            }
        }
    }

    return records;
}

ChunkMesh GreedyMesher::buildFromVisibleFaces(
    const world::Chunk& chunk,
    const std::vector<VisibleFaceRecord>& faces) const
{
    if (faces.empty()) {
        ChunkMesh mesh;
        mesh.sourceRevision = chunk.revision();
        mesh.sourceMeshRevisionHash = chunk.meshRevision();
        return mesh;
    }

    constexpr std::size_t kBucketCount = static_cast<std::size_t>(world::ChunkSize) * Faces.size();
    std::array<std::vector<const VisibleFaceRecord*>, kBucketCount> buckets;
    for (const auto& record : faces) {
        if (record.faceIndex >= Faces.size() || record.localIndex >= world::ChunkVolume || record.materialId == 0U) {
            continue;
        }
        const auto& face = Faces[record.faceIndex];
        const auto pos = decodeLocalIndex(record.localIndex);
        const int slice = pos[face.axis];
        buckets[static_cast<std::size_t>(record.faceIndex) * world::ChunkSize + static_cast<std::size_t>(slice)]
            .push_back(&record);
    }

    auto& maskSurfaces = tlsScratch.maskSurfaces;
    auto& maskLight = tlsScratch.maskLight;
    auto& maskBlockValue = tlsScratch.maskBlockValue;
    const auto maskSize = static_cast<std::size_t>(world::ChunkSize) * world::ChunkSize;

    std::vector<Quad> quads;
    quads.reserve(faces.size() / 2U + 1U);

    for (const auto& face : Faces) {
        const int uAxis = (face.axis + 1) % 3;
        const int vAxis = (face.axis + 2) % 3;

        for (int slice = 0; slice < world::ChunkSize; ++slice) {
            const auto& bucket =
                buckets[static_cast<std::size_t>(face.faceIndex) * world::ChunkSize + static_cast<std::size_t>(slice)];
            if (bucket.empty()) {
                continue;
            }
            std::fill(maskBlockValue.begin(), maskBlockValue.begin() + maskSize, 0u);

            for (const auto* record : bucket) {
                const auto pos = decodeLocalIndex(record->localIndex);
                const int u = pos[uAxis];
                const int v = pos[vAxis];
                const std::size_t maskIdx = static_cast<std::size_t>(u + v * world::ChunkSize);
                maskBlockValue[maskIdx] = record->materialId;
                maskSurfaces[maskIdx] = static_cast<std::uint8_t>(record->surface);
                maskLight[maskIdx] = record->packedLight;
            }

            for (int v = 0; v < world::ChunkSize; ++v) {
                for (int u = 0; u < world::ChunkSize;) {
                    const std::size_t baseIdx = static_cast<std::size_t>(u + v * world::ChunkSize);
                    const auto blockValue = maskBlockValue[baseIdx];
                    if (blockValue == 0u) {
                        ++u;
                        continue;
                    }
                    const auto surface = maskSurfaces[baseIdx];
                    const auto light0 = maskLight[baseIdx];

                    int width = 1;
                    while (u + width < world::ChunkSize) {
                        const std::size_t i = baseIdx + width;
                        if (maskBlockValue[i] != blockValue
                            || maskSurfaces[i] != surface
                            || maskLight[i] != light0) {
                            break;
                        }
                        ++width;
                    }

                    int height = 1;
                    bool done = false;
                    while (v + height < world::ChunkSize && !done) {
                        const std::size_t rowBase = static_cast<std::size_t>(u + (v + height) * world::ChunkSize);
                        for (int testU = 0; testU < width; ++testU) {
                            const std::size_t i = rowBase + testU;
                            if (maskBlockValue[i] != blockValue
                                || maskSurfaces[i] != surface
                                || maskLight[i] != light0) {
                                done = true;
                                break;
                            }
                        }
                        if (!done) {
                            ++height;
                        }
                    }

                    for (int clearV = 0; clearV < height; ++clearV) {
                        const std::size_t rowBase = static_cast<std::size_t>(u + (v + clearV) * world::ChunkSize);
                        for (int clearU = 0; clearU < width; ++clearU) {
                            maskBlockValue[rowBase + clearU] = 0u;
                        }
                    }

                    const int plane = face.sign > 0 ? slice + 1 : slice;
                    MaskCell cell{blockValue, light0, static_cast<MeshSurface>(surface)};
                    quads.push_back(makeQuad(face, plane, u, v, width, height, cell));
                    u += width;
                }
            }
        }
    }

    return finalizeMeshFromQuads(chunk, quads);
}

} // namespace voxel::render::meshing
