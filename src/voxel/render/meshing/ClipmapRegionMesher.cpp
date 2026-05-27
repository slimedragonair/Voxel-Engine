#include <voxel/render/meshing/ClipmapRegionMesher.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>

#include <voxel/core/Logger.hpp>
#include <voxel/render/meshing/GreedyMesher.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/ChunkConstants.hpp>
#include <voxel/world/NoiseTerrainGenerator.hpp>
#include <voxel/world/TerrainColumnPrepass.hpp>

namespace voxel::render::meshing {

namespace {

// 65 vertices × 65 vertices per region = 64 × 64 quad cells, each
// covering 8×8 world blocks. 8192 triangles per region. Compare to
// voxel LOD3's ~80k tris/region — ~10× lighter.
constexpr int kSamples = 65;
constexpr int kCells = kSamples - 1; // 64
constexpr int kWorldBlocksPerCell = world::RegionBlockExtent / kCells; // 512/64 = 8

// In our 0..128 supervoxel-pair vertex unit space (which the LOD3
// scale-4 shader maps to 0..512 world blocks), one cell takes 2 units.
constexpr int kVertexStridePerCell = 2;

// Y range of the clipmap region. Vertex.y stores 0..127 in supervoxel-
// pair units; with LOD3 scale=4 this maps to 0..508 world blocks of
// height range. We clamp surface Y to this range; mountains taller
// than 508 blocks above the region origin get flat-topped (acceptable
// at LOD3 distance — sub-pixel artifact).
constexpr int kMaxVertexY = 127;

// Bump when clipmap mesh packing/windowing changes so the LOD3 disk cache
// rejects stale meshes instead of reloading old artifacts.
constexpr std::uint64_t kClipmapRegionMeshVersion = 0x4f7032a87c6d51b9ULL;

// Map TerrainSurfaceKind to a block state ID drawn from the noise
// generator's settings. Falls back to `Land` (grass) for unknown.
[[nodiscard]] voxel::BlockStateId materialForSurface(
    voxel::world::TerrainSurfaceKind kind,
    const voxel::world::NoiseTerrainSettings& s) noexcept
{
    switch (kind) {
        case voxel::world::TerrainSurfaceKind::Beach:        return s.sandBlock;
        case voxel::world::TerrainSurfaceKind::ShallowOcean: return s.waterBlock;
        case voxel::world::TerrainSurfaceKind::Ocean:        return s.waterBlock;
        case voxel::world::TerrainSurfaceKind::DeepOcean:    return s.waterBlock;
        case voxel::world::TerrainSurfaceKind::Land:
        default:                                              return s.grassBlock;
    }
}

// Apply a biome refinement on top of the surface-kind material. This
// lets snowy mountains show snow, deserts show sand even where the
// raw surface-kind says "Land," etc.
[[nodiscard]] voxel::BlockStateId materialForBiome(
    voxel::world::TerrainBiomeId biome,
    voxel::BlockStateId defaultMat,
    const voxel::world::NoiseTerrainSettings& s) noexcept
{
    using B = voxel::world::TerrainBiomeId;
    switch (biome) {
        case B::Desert:           return s.sandBlock;
        case B::Badlands:         return s.redSandBlock;
        case B::VolcanicWastes:
        case B::ArcaneFractureZone:
                                  return s.basaltBlock;
        case B::Savanna:          return s.grassBlock;
        case B::Jungle:
        case B::LushHighlandsValley:
        case B::MagicalGrove:
        case B::FloatingIslands:
                                  return s.grassBlock;
        case B::DenseForest:
        case B::RedwoodForest:
        case B::Taiga:
                                  return s.podzolBlock;
        case B::Tundra:
        case B::SnowyMountains:   return s.snowBlock;
        case B::IceCaps:          return s.iceBlock;
        case B::Mountains:        return s.stoneBlock;
        case B::Beach:            return s.sandBlock;
        case B::ElementalCrystalCave:
                                  return s.mossyStoneBlock;
        case B::OceanAbyss:
        case B::DeepOcean:
        case B::Ocean:
        case B::WarmOcean:
        case B::ColdOcean:        return s.waterBlock;
        default:                  return defaultMat;
    }
}

// MeshSurface classification: water is transparent, everything else
// is opaque. Mirror what GreedyMesher does for the voxel path.
[[nodiscard]] MeshSurface surfaceClass(
    voxel::BlockStateId mat,
    const voxel::world::NoiseTerrainSettings& s) noexcept
{
    if (mat.value == s.waterBlock.value) return MeshSurface::Transparent;
    return MeshSurface::Opaque;
}

// Encode a single grid sample into a ClusterVertex. The face index 2
// = +Y (top), matching the face table in ClusterMesher / RegionMesher
// so voxel.frag picks the right normal/lighting path.
struct GridSample {
    std::uint8_t posX;
    std::uint8_t posY;
    std::uint8_t posZ;
    std::int8_t verticalSide;
    std::uint32_t materialId;
    MeshSurface surface;
};

} // namespace

std::uint64_t hashClipmapRegion(
    voxel::world::RegionCoord coord,
    const voxel::world::NoiseTerrainGenerator& terrainGen) noexcept
{
    // FNV-1a over terrain version + coord. The terrain version
    // captures the seed + settings; the coord differentiates spatial
    // tiles. No chunk revisions involved (clipmap doesn't read chunks).
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    auto mix = [&hash](std::uint64_t v) {
        hash ^= v;
        hash *= 0x100000001b3ULL;
    };
    mix(kClipmapRegionMeshVersion);
    mix(terrainGen.terrainVersion());
    mix(static_cast<std::uint64_t>(coord.x));
    mix(static_cast<std::uint64_t>(coord.y));
    mix(static_cast<std::uint64_t>(coord.z));
    return hash;
}

ClusterMesh ClipmapRegionMesher::build(
    voxel::world::RegionCoord coord,
    const voxel::world::NoiseTerrainGenerator& terrainGen) const
{
    ClusterMesh mesh;
    // The cluster renderer's upload routine derives the world origin
    // from the alias-cluster coord (region × RegionClusterExtent). We
    // record the same alias here so any downstream code that reads
    // mesh.coord gets the matching value.
    mesh.coord = voxel::world::ClusterCoord{
        coord.x * voxel::world::RegionClusterExtent,
        coord.y * voxel::world::RegionClusterExtent,
        coord.z * voxel::world::RegionClusterExtent,
    };
    mesh.sourceRevisionsHash = hashClipmapRegion(coord, terrainGen);

    // World-space origin of this region (block coords of its (0,0,0)
    // corner). Sample positions add (sampleIdx * 8) blocks to these.
    const float worldOriginX = static_cast<float>(
        coord.x * voxel::world::RegionBlockExtent);
    const float worldOriginY = static_cast<float>(
        coord.y * voxel::world::RegionBlockExtent);
    const float worldOriginZ = static_cast<float>(
        coord.z * voxel::world::RegionBlockExtent);

    const auto& settings = terrainGen.settings();

    // ---- 1. Sample the 65×65 grid -----------------------------------
    // Each sample collects height + material. Stored in a flat array
    // for the quad-emit pass below.
    std::array<GridSample, kSamples * kSamples> grid{};
    // Diagnostic: track min/max sampled surfaceY and emitted vertexY
    // for the first ~8 regions built. Helps verify the Y math (is the
    // mesh at the right altitude relative to the world surface?).
    static std::atomic<int> diagCount{0};
    const bool wantDiag = diagCount.fetch_add(1, std::memory_order_relaxed) < 100;
    int diagSurfaceYMin = std::numeric_limits<int>::max();
    int diagSurfaceYMax = std::numeric_limits<int>::min();
    int diagVertexYMin = std::numeric_limits<int>::max();
    int diagVertexYMax = std::numeric_limits<int>::min();

    for (int sz = 0; sz < kSamples; ++sz) {
        const float wz = worldOriginZ + static_cast<float>(sz) * kWorldBlocksPerCell;
        for (int sx = 0; sx < kSamples; ++sx) {
            const float wx = worldOriginX + static_cast<float>(sx) * kWorldBlocksPerCell;
            const auto column = terrainGen.sampleColumnAt(wx, wz);

            // Choose material: surface kind first, biome refinement on
            // top. Ocean cells render as water regardless of biome.
            voxel::BlockStateId mat = materialForSurface(column.surfaceKind, settings);
            if (!column.isOcean) {
                mat = materialForBiome(column.biome, mat, settings);
            }

            // For ocean cells: render at sea level, not at the bottom
            // of the ocean floor. Players see "water surface" at LOD3.
            const int surfaceY = column.isOcean
                ? column.seaLevel
                : column.surfaceY;

            // Convert world-Y → vertex-Y. Vertex unit = supervoxel-pair
            // = 4 world blocks at LOD3 scale. Region's Y bottom is
            // worldOriginY; vertex.y stores the offset above that,
            // divided by 4.
            const int relY = surfaceY - static_cast<int>(worldOriginY);
            int vertexY = relY / 4;
            const std::int8_t verticalSide = relY < 0
                ? std::int8_t{-1}
                : (relY > 511 ? std::int8_t{1} : std::int8_t{0});
            // Clamp to [0, kMaxVertexY]. Values outside this range mean
            // the surface is above or below the region's vertical
            // window; we flatten to the boundary. Visible artifact
            // only when the player can see distant mountains/canyons
            // taller than the LOD3 region's 508-block vertical range
            // — sub-pixel at LOD3 distance.
            if (vertexY < 0) vertexY = 0;
            if (vertexY > kMaxVertexY) vertexY = kMaxVertexY;

            auto& sample = grid[static_cast<std::size_t>(sz * kSamples + sx)];
            sample.posX = static_cast<std::uint8_t>(sx * kVertexStridePerCell);
            sample.posY = static_cast<std::uint8_t>(vertexY);
            sample.posZ = static_cast<std::uint8_t>(sz * kVertexStridePerCell);
            sample.verticalSide = verticalSide;
            sample.materialId = mat.value;
            sample.surface = surfaceClass(mat, settings);

            if (wantDiag) {
                if (surfaceY < diagSurfaceYMin) diagSurfaceYMin = surfaceY;
                if (surfaceY > diagSurfaceYMax) diagSurfaceYMax = surfaceY;
                if (vertexY < diagVertexYMin) diagVertexYMin = vertexY;
                if (vertexY > diagVertexYMax) diagVertexYMax = vertexY;
            }
        }
    }

    if (wantDiag) {
        // worldY (expected, after shader applies lodScale=4) = posY * 4 + worldOriginY.
        // For region.y=0 the origin is 0, so this is just vertexY * 4.
        const int worldYMin = diagVertexYMin * 4 + static_cast<int>(worldOriginY);
        const int worldYMax = diagVertexYMax * 4 + static_cast<int>(worldOriginY);
        const std::string line =
            "ClipmapLOD3: region(" + std::to_string(coord.x) + ","
            + std::to_string(coord.y) + "," + std::to_string(coord.z) + ")"
            + " worldOriginY=" + std::to_string(static_cast<int>(worldOriginY))
            + " surfaceY=[" + std::to_string(diagSurfaceYMin)
            + ".." + std::to_string(diagSurfaceYMax) + "]"
            + " vertexY=[" + std::to_string(diagVertexYMin)
            + ".." + std::to_string(diagVertexYMax) + "]"
            + " expectedWorldY=[" + std::to_string(worldYMin)
            + ".." + std::to_string(worldYMax) + "]";
        voxel::Logger::info(line);
        // Also append to a file the user can paste back. The Logger
        // only writes to stdout, which is invisible when launched via
        // double-click on Windows. Path is relative to CWD which is
        // the exe directory (e.g. build/Release/).
        static std::mutex fileMutex;
        std::lock_guard<std::mutex> lock(fileMutex);
        std::ofstream out("logs/lod3_diag.log", std::ios::app);
        if (out) {
            out << line << '\n';
        }
    }

    // ---- 2. Emit the 64×64 quad grid as a single big surface ---------
    // We emit per-quad without greedy merging. The mesh is small
    // enough (8k tris) that greedy merge wouldn't save enough to
    // justify the per-quad material comparison loop. If profile shows
    // upload bandwidth pressure, we can add greedy merge along axis-
    // aligned material runs later.
    constexpr std::uint8_t kTopFaceIndex = 2; // +Y face
    constexpr std::uint32_t kTopFaceLight = 255U; // matches shadeForFace(+Y)

    mesh.vertices.reserve(kSamples * kSamples);
    mesh.indices.reserve(static_cast<std::size_t>(kCells * kCells) * 6U);

    // For each grid cell, take the 4 corners and emit two triangles.
    // Cell at (cx, cz) uses samples (cx, cz), (cx+1, cz), (cx+1, cz+1),
    // (cx, cz+1). Material for the quad = sample at (cx, cz) corner
    // — biome boundaries get a 1-cell stair-step which is invisible at
    // LOD3 distance.
    //
    // Run length per material: track current draw range so consecutive
    // same-material quads share a draw range entry (analogous to
    // appendQuadToMesh in ClusterMesher).
    for (int cz = 0; cz < kCells; ++cz) {
        for (int cx = 0; cx < kCells; ++cx) {
            const auto& s00 = grid[static_cast<std::size_t>(cz * kSamples + cx)];
            const auto& s10 = grid[static_cast<std::size_t>(cz * kSamples + (cx + 1))];
            const auto& s11 = grid[static_cast<std::size_t>((cz + 1) * kSamples + (cx + 1))];
            const auto& s01 = grid[static_cast<std::size_t>((cz + 1) * kSamples + cx)];

            const bool allBelow = s00.verticalSide < 0 && s10.verticalSide < 0
                && s11.verticalSide < 0 && s01.verticalSide < 0;
            const bool allAbove = s00.verticalSide > 0 && s10.verticalSide > 0
                && s11.verticalSide > 0 && s01.verticalSide > 0;
            if (allBelow || allAbove) {
                continue;
            }

            // Material + surface come from the (cx, cz) corner. The
            // entire quad uses one material — biome edges get crisp
            // 1-cell-wide transitions, fine at LOD3.
            const std::uint32_t material = s00.materialId;
            const MeshSurface surface = s00.surface;
            const std::uint32_t baseIndex =
                static_cast<std::uint32_t>(mesh.vertices.size());

            auto push = [&](const GridSample& s) {
                ClusterVertex v{};
                v.posX = s.posX;
                v.posY = s.posY;
                v.posZ = s.posZ;
                v.faceIndex = kTopFaceIndex;
                v.materialId = material;
                v.packedLight = kTopFaceLight;
                mesh.vertices.push_back(v);
            };
            // Winding: counter-clockwise when viewed from above so the
            // top-face triangle's normal points +Y. The cluster pipeline
            // uses VK_FRONT_FACE_COUNTER_CLOCKWISE.
            push(s00);
            push(s10);
            push(s11);
            push(s01);

            const auto indexBefore =
                static_cast<std::uint32_t>(mesh.indices.size());
            mesh.indices.push_back(baseIndex + 0U);
            mesh.indices.push_back(baseIndex + 1U);
            mesh.indices.push_back(baseIndex + 2U);
            mesh.indices.push_back(baseIndex + 0U);
            mesh.indices.push_back(baseIndex + 2U);
            mesh.indices.push_back(baseIndex + 3U);

            constexpr std::uint32_t kQuadIndexCount = 6;
            // Coalesce into existing draw range if same surface + material.
            if (!mesh.drawRanges.empty()) {
                auto& prev = mesh.drawRanges.back();
                if (prev.surface == surface
                    && prev.materialId == material
                    && prev.indexOffset + prev.indexCount == indexBefore) {
                    prev.indexCount += kQuadIndexCount;
                    continue;
                }
            }
            mesh.drawRanges.push_back({surface, material, indexBefore, kQuadIndexCount});
        }
    }
    return mesh;
}

} // namespace voxel::render::meshing
