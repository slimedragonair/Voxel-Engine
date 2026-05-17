#include <voxel/world/BlockEditQueue.hpp>

#include <algorithm>

namespace voxel::world {

namespace {

void pushUnique(std::vector<ChunkCoord>& coords, ChunkCoord coord)
{
    if (std::find(coords.begin(), coords.end(), coord) == coords.end()) {
        coords.push_back(coord);
    }
}

} // namespace

void BlockEditQueue::enqueueBreak(PlanetCoord position)
{
    edits_.push_back(QueuedBlockEdit{QueuedBlockEdit::Kind::Break, position, AirBlockState});
}

void BlockEditQueue::enqueuePlace(PlanetCoord position, BlockStateId block)
{
    edits_.push_back(QueuedBlockEdit{QueuedBlockEdit::Kind::Place, position, block});
}

std::size_t BlockEditQueue::size() const noexcept
{
    return edits_.size();
}

void BlockEditQueue::clear() noexcept
{
    edits_.clear();
}

BlockEditQueueStats BlockEditQueue::flush(ChunkManager& chunks, const BlockEditor& editor)
{
    BlockEditQueueStats stats{};
    stats.requested = edits_.size();
    std::vector<ChunkCoord> uniqueDirtied;

    for (const auto& edit : edits_) {
        const auto result = edit.kind == QueuedBlockEdit::Kind::Break
            ? editor.breakBlock(chunks, edit.position)
            : editor.placeBlock(chunks, edit.position, edit.block);
        if (!result.changed) {
            ++stats.rejected;
            continue;
        }

        ++stats.accepted;
        stats.deltas.insert(stats.deltas.end(), result.deltas.begin(), result.deltas.end());
        for (const auto coord : result.dirtiedChunks) {
            const auto before = uniqueDirtied.size();
            pushUnique(uniqueDirtied, coord);
            if (uniqueDirtied.size() == before) {
                ++stats.dirtyMeshCoalesced;
                ++stats.dirtyLightingCoalesced;
            } else {
                stats.dirtyChunks.push_back(coord);
                ++stats.dirtyMeshQueued;
                ++stats.dirtyLightingQueued;
            }
        }
    }

    edits_.clear();
    return stats;
}

} // namespace voxel::world
