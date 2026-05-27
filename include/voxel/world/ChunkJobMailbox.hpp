#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

#include <voxel/render/meshing/GreedyMesher.hpp>
#include <voxel/world/Chunk.hpp>
#include <voxel/world/ChunkLightData.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::world {

struct GeneratedChunkResult {
    ChunkCoord coord{};
    Chunk chunk{};
    std::uint64_t generationTimeUs{};
    std::uint64_t loadTimeUs{};
    std::uint64_t queueWaitUs{};
    bool generatedFromPrepass{};
    bool loadedFromSave{};
};

struct MeshJobKey {
    ChunkCoord coord{};
    Revision sourceRevision{};
    std::uint64_t neighborRevisionHash{};

    [[nodiscard]] bool operator==(const MeshJobKey& other) const noexcept
    {
        return coord == other.coord
            && sourceRevision == other.sourceRevision
            && neighborRevisionHash == other.neighborRevisionHash;
    }
};

struct MeshJobKeyHash {
    [[nodiscard]] std::size_t operator()(const MeshJobKey& key) const noexcept
    {
        std::size_t seed = ChunkCoordHash{}(key.coord);
        seed ^= std::hash<std::uint64_t>{}(key.sourceRevision + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
        seed ^= std::hash<std::uint64_t>{}(key.neighborRevisionHash + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
        return seed;
    }
};

struct ChunkMeshResult {
    ChunkCoord coord{};
    Revision sourceRevision{};
    // Mesh-revision the chunk had at the moment the dispatch took its
    // snapshot. The install path compares this to the chunk's CURRENT
    // meshRevision; if they differ, a subsequent dirty-mark happened
    // after dispatch (e.g. fluid sim carved more cells while the job
    // was on a worker), so the install discards this result and
    // re-enqueues the chunk for a fresh build.
    Revision sourceMeshRevision{};
    std::uint64_t neighborRevisionHash{};
    std::uint64_t buildTimeUs{};
    std::uint64_t queueWaitUs{};
    render::meshing::ChunkMesh mesh{};
};

struct LightingJobKey {
    ChunkCoord coord{};
    Revision sourceRevision{};
    std::uint64_t neighborLightHash{};

    [[nodiscard]] bool operator==(const LightingJobKey& other) const noexcept
    {
        return coord == other.coord
            && sourceRevision == other.sourceRevision
            && neighborLightHash == other.neighborLightHash;
    }
};

struct LightingJobKeyHash {
    [[nodiscard]] std::size_t operator()(const LightingJobKey& key) const noexcept
    {
        std::size_t seed = ChunkCoordHash{}(key.coord);
        seed ^= std::hash<std::uint64_t>{}(key.sourceRevision + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
        seed ^= std::hash<std::uint64_t>{}(key.neighborLightHash + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
        return seed;
    }
};

struct ChunkLightingResult {
    ChunkCoord coord{};
    Revision sourceRevision{};
    std::uint64_t neighborLightHash{};
    std::uint64_t propagationTimeUs{};
    std::uint64_t queueWaitUs{};
    ChunkLightData light{};
};

// Thread-safe results sink + in-flight tracker for async chunk jobs.
// Workers push results, main thread drains and installs/uploads them.
// `tryBegin*` is the main thread's deduplication guard so the same chunk
// isn't dispatched twice while a job is still running for it.
class ChunkJobMailbox {
public:
    void pushGeneration(GeneratedChunkResult result);
    void requeueGeneration(std::vector<GeneratedChunkResult> results);
    std::vector<GeneratedChunkResult> drainGeneration();
    std::vector<GeneratedChunkResult> drainGeneration(std::size_t maxResults);
    std::vector<GeneratedChunkResult> drainGenerationClosest(ChunkCoord center, std::size_t maxResults);

    void pushMesh(ChunkMeshResult result);
    std::vector<ChunkMeshResult> drainMesh();

    void pushLighting(ChunkLightingResult result);
    std::vector<ChunkLightingResult> drainLighting();

    // Returns true if no generation job is in flight for `coord`. Marks it in-flight.
    bool tryBeginGeneration(ChunkCoord coord);
    void endGeneration(ChunkCoord coord);
    [[nodiscard]] bool isGenerationInFlight(ChunkCoord coord) const;

    // OPTIMIZATION (fast-movement hot path): take a single-lock snapshot of
    // the in-flight generation set. Hot per-frame code (streaming dispatch
    // filter, mesh neighbour-busy check) was calling `isGenerationInFlight`
    // hundreds of times per frame, each acquiring the mutex while workers
    // were also pushing results. Replace with one snapshot + lock-free lookups.
    [[nodiscard]] std::unordered_set<ChunkCoord, ChunkCoordHash> snapshotInFlightGeneration() const;

    bool tryBeginMesh(MeshJobKey key);
    void endMesh(MeshJobKey key);
    bool tryBeginMesh(ChunkCoord coord);
    void endMesh(ChunkCoord coord);

    bool tryBeginLighting(LightingJobKey key);
    void endLighting(LightingJobKey key);

    [[nodiscard]] std::size_t inFlightGenerationCount() const;
    [[nodiscard]] std::size_t inFlightMeshCount() const;
    [[nodiscard]] std::size_t inFlightLightingCount() const;
    [[nodiscard]] std::size_t pendingGenerationResults() const;
    [[nodiscard]] std::size_t pendingMeshResults() const;
    [[nodiscard]] std::size_t pendingLightingResults() const;

private:
    // C2ME-style scheduler tightening (Phase 1D-3): three independent
    // mutexes — one per job stream — instead of one shared mutex.
    // Generation / meshing / lighting are conceptually disjoint flows;
    // serializing their bookkeeping caused the chunk pipeline to ping-
    // pong worker threads onto the same lock. With per-stream mutexes,
    // a worker pushing a mesh result no longer blocks a worker pushing
    // a generation result, and the main thread's frequent
    // `inFlightCount()` polls don't back-pressure unrelated pushes.
    mutable std::mutex generationMutex_;
    mutable std::mutex meshMutex_;
    mutable std::mutex lightingMutex_;

    std::vector<GeneratedChunkResult> generationResults_;
    std::vector<ChunkMeshResult> meshResults_;
    std::vector<ChunkLightingResult> lightingResults_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> inFlightGeneration_;
    std::unordered_set<MeshJobKey, MeshJobKeyHash> inFlightMesh_;
    std::unordered_set<LightingJobKey, LightingJobKeyHash> inFlightLighting_;

    // Atomic mirrors of set/vector sizes. Hot per-frame code
    // (streaming dispatch, mesh job throttle) reads these dozens of
    // times per tick — locking a mutex just to return a size_t is a
    // textbook serialization gotcha. The atomic is updated *inside*
    // the corresponding mutex on tryBegin/end/push/drain so it stays
    // consistent; reads use memory_order_relaxed because we only need
    // "approximately current" for budget decisions.
    std::atomic<std::size_t> inFlightGenerationCount_{0};
    std::atomic<std::size_t> inFlightMeshCount_{0};
    std::atomic<std::size_t> inFlightLightingCount_{0};
    std::atomic<std::size_t> pendingGenerationCount_{0};
    std::atomic<std::size_t> pendingMeshCount_{0};
    std::atomic<std::size_t> pendingLightingCount_{0};
};

} // namespace voxel::world
