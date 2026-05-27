#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include <voxel/render/meshing/ClusterMesh.hpp>

namespace voxel::core {
class JobSystem;
}

namespace voxel::render::meshing {

// Persistent on-disk cache for built LOD2 cluster meshes.
//
// Inspired by Distant Horizons' SQLite cache: once a cluster mesh is
// classified by the GPU and greedy-merged, store it to disk so the next
// session (or revisit) loads it in ~1 ms instead of re-running the full
// pipeline. Eliminates the "world fills in over 10 seconds" pattern
// after world load.
//
// Storage layout: one file per cluster at
//   {baseDir}/{cx}_{cy}_{cz}.lodc
//
// File format (v1, plain binary, little-endian, no compression):
//   [4 bytes ] magic 'LODC'
//   [4 bytes ] version (1)
//   [8 bytes ] sourceRevisionsHash      ← cache key
//   [24 bytes] cluster coord (3 × int64)
//   [4 bytes ] vertex_count
//   [4 bytes ] index_count
//   [4 bytes ] draw_range_count
//   [vertex_count × 12 bytes ] ClusterVertex blob
//   [index_count × 4 bytes   ] index blob
//   [draw_range_count × 16 b ] MeshDrawRange blob
//
// Invalidation strategy: lazy. We don't actively delete stale cache
// entries when a chunk is edited — the `sourceRevisionsHash` check on
// load handles that: if the cluster's input chunks have changed since
// the cache was written, the hash won't match and we treat it as a
// miss. The stale file gets overwritten on the next successful build.
class ClusterMeshDiskCache {
public:
    ClusterMeshDiskCache() = default;

    // Initialize with the cache directory. Creates the directory if it
    // doesn't exist. Returns false on I/O error (callers should disable
    // caching in that case — engine continues fine without it).
    [[nodiscard]] bool initialize(std::filesystem::path baseDir);

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    // Try to load a cluster mesh from disk. Returns nullopt if:
    //   - cache not initialized
    //   - file doesn't exist
    //   - file exists but sourceRevisionsHash doesn't match expectedHash
    //   - file is corrupt (magic/version mismatch)
    // On any miss, the caller falls back to full build.
    [[nodiscard]] std::optional<ClusterMesh> tryLoad(
        world::ClusterCoord coord, std::uint64_t expectedHash) const;

    // Asynchronously write a cluster mesh to disk via the JobSystem.
    // Returns immediately — actual write happens on a Low-priority
    // worker. Mesh data is copied (cheap — most of the cost is the
    // serialization + write which happens off-thread).
    void storeAsync(core::JobSystem& jobs,
                    world::ClusterCoord coord,
                    const ClusterMesh& mesh) const;

    [[nodiscard]] const std::filesystem::path& baseDir() const noexcept { return baseDir_; }

private:
    [[nodiscard]] std::filesystem::path pathFor(world::ClusterCoord coord) const;

    std::filesystem::path baseDir_;
    bool initialized_{false};
};

} // namespace voxel::render::meshing
