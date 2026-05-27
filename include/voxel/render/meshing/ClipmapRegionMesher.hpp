#pragma once

#include <cstdint>

#include <voxel/render/meshing/ClusterMesh.hpp>
#include <voxel/world/Coordinates.hpp>
#include <voxel/world/Lod.hpp>

namespace voxel::world {
class NoiseTerrainGenerator;
}

namespace voxel::render::meshing {

// Heightmap-derived LOD3 region mesher (clipmap-style).
//
// Replaces the voxel-derived RegionMesher for far terrain. Instead of
// sampling chunks (which must be loaded) and reducing 8:1, this samples
// the SURFACE HEIGHT directly from NoiseTerrainGenerator at a sparse
// 2D grid. The resulting mesh is just the visible top surface — no
// caves, no overhangs, no walls.
//
// Why this is the right LOD3 representation:
//   - Doesn't require chunks to be loaded. Works at any distance.
//   - 10× smaller mesh than voxel LOD3 (~8k tris vs ~80k tris).
//   - 10× faster build (~1 ms vs ~10 ms per region).
//   - Surface-only is fine at LOD3 distance — caves are sub-pixel.
//
// Output format is `ClusterMesh` — the same struct used for LOD2 and
// voxel LOD3. This means it routes through the existing
// ClusterRenderer::uploadRegionMesh API and renders with the existing
// cluster.vert / voxel.frag shader pair (per-tier scale handles the
// 4× world-space scaling for LOD3).
//
// Sample grid: 65×65 vertices per region. 65 = 64 cells + 1 closing
// vertex. Each cell spans 8 blocks of world space (= 512/64). In
// supervoxel-pair vertex units (which the shader scales by 4), each
// cell takes 2 units. Vertex positions span 0..128 which fits in our
// existing uint8 vertex format.
//
// Material picking: each grid vertex queries the column's biome and
// surface kind via NoiseTerrainGenerator::sampleColumnAt, then maps
// the result to a block material ID drawn from
// NoiseTerrainSettings's biome block table. This keeps the visual
// language consistent with the LOD0 voxel terrain.
class ClipmapRegionMesher {
public:
    // Build a clipmap mesh for the given region coord. `terrainGen`
    // must outlive the call; the mesher samples via sampleColumnAt
    // synchronously. Safe to run on a worker thread.
    //
    // Returns an empty mesh if the entire region's vertical span is
    // outside the surface range (e.g., a sky-only region high above
    // terrain). Caller should treat empty meshes as "no upload."
    [[nodiscard]] ClusterMesh build(
        world::RegionCoord coord,
        const world::NoiseTerrainGenerator& terrainGen) const;
};

// Cache invalidation: clipmap meshes ONLY depend on the terrain seed
// + region coord. They don't depend on chunk revisions because they
// don't read chunk data. We hash terrain settings + region coord so
// the on-disk cache keys still differentiate identical regions on
// different worlds.
[[nodiscard]] std::uint64_t hashClipmapRegion(
    world::RegionCoord coord,
    const world::NoiseTerrainGenerator& terrainGen) noexcept;

} // namespace voxel::render::meshing
