#pragma once

#include <optional>
#include <vector>

#include <voxel/core/Math.hpp>
#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::world {

struct StreamingSettings {
    // Streamer radius. `renderDistanceChunks` covers the X/Z axes; cubic
    // chunks make a 32-radius cube blow up to ~35k chunks, so a separate
    // vertical radius keeps the working set manageable.
    int renderDistanceChunks{8};
    int verticalRenderDistanceChunks{2};
    int simulationDistanceChunks{8};
    int physicsDistanceChunks{4};
    // Optional globally important vertical layer. Static oceans use this to
    // keep the sea-level chunk resident across the visible X/Z range even
    // when the camera/player is above or below sea level.
    std::optional<int> pinnedVerticalChunkY{};
    // Extra vertical layers around pinnedVerticalChunkY to request as a
    // high-priority visual band. This keeps ocean surfaces and the first
    // underwater slab from lagging badly when the camera is far above sea
    // level, without changing the 32^3 storage/render chunk size.
    int pinnedVerticalChunkRadius{1};
};

struct ChunkRequest {
    ChunkCoord coord{};
    float priority{};
};

class ChunkStreamer {
public:
    explicit ChunkStreamer(ChunkManager& chunks);

    [[nodiscard]] std::vector<ChunkRequest> planRequests(ChunkCoord center, const StreamingSettings& settings) const;
    [[nodiscard]] std::vector<ChunkRequest> planRequests(
        ChunkCoord center,
        const StreamingSettings& settings,
        core::Vec3 forward) const;
    void pump(ChunkCoord center, const StreamingSettings& settings);

    // TODO(streaming): Add velocity-aware prediction, LOD rings, and async load/generate/mesh queues.

private:
    ChunkManager& chunks_;
};

} // namespace voxel::world
