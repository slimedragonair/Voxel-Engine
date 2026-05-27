#pragma once

#include <cstddef>
#include <vector>

#include <voxel/world/BlockEditor.hpp>

namespace voxel::world {

struct QueuedBlockEdit {
    enum class Kind {
        Break,
        Place
    };

    Kind kind{Kind::Break};
    PlanetCoord position{};
    BlockStateId block{AirBlockState};
};

struct BlockEditQueueStats {
    std::size_t requested{};
    std::size_t accepted{};
    std::size_t rejected{};
    std::size_t dirtyMeshQueued{};
    std::size_t dirtyMeshCoalesced{};
    std::size_t dirtyLightingQueued{};
    std::size_t dirtyLightingCoalesced{};
    std::vector<ChunkCoord> dirtyChunks;
    WorldDeltaBatch deltas;
};

class BlockEditQueue {
public:
    void enqueueBreak(PlanetCoord position);
    void enqueuePlace(PlanetCoord position, BlockStateId block);
    [[nodiscard]] std::size_t size() const noexcept;
    void clear() noexcept;

    [[nodiscard]] BlockEditQueueStats flush(ChunkManager& chunks, const BlockEditor& editor);

private:
    std::vector<QueuedBlockEdit> edits_;
};

} // namespace voxel::world
