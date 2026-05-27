#include <voxel/save/SaveCoordinator.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <voxel/core/JobSystem.hpp>
#include <voxel/core/Logger.hpp>
#include <voxel/save/RegionFileStore.hpp>
#include <voxel/save/WorldSaveService.hpp>
#include <voxel/world/Chunk.hpp>
#include <voxel/world/ChunkManager.hpp>

namespace voxel::save {

namespace {

std::uint64_t elapsedUs(std::chrono::steady_clock::time_point start,
                        std::chrono::steady_clock::time_point end)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

} // namespace

core::RuntimeCounters SaveCoordinator::flushPending(
    bool force,
    world::ChunkManager& chunks,
    const WorldSaveService& worldSaveService,
    core::JobSystem& jobs,
    const SaveCoordinatorSettings& settings,
    std::uint64_t frameIndex)
{
    core::RuntimeCounters stats{};
    stats.savesFlushed += drainCompleted(false);

    const auto dirty = worldSaveService.dirtyChunkCount(chunks);
    stats.saveQueueLength = dirty + asyncJobs_.size();
    const bool intervalDue = frameIndex > 0 && settings.saveFlushIntervalFrames > 0
        && (frameIndex % settings.saveFlushIntervalFrames) == 0;
    if (dirty == 0 || (!force && !intervalDue)) {
        return stats;
    }

    const auto saveStart = std::chrono::steady_clock::now();
    const auto maxSnapshots = settings.maxSavesPerFlush;
    std::vector<world::Chunk> snapshots;
    snapshots.reserve(std::min(dirty, maxSnapshots));
    chunks.forEach([&snapshots, maxSnapshots](world::Chunk& chunk) {
        if (snapshots.size() >= maxSnapshots || !chunk.dirty().save) {
            return;
        }
        snapshots.push_back(chunk);
        chunk.clearSaveDirty();
    });
    core::recordTimer(stats.save, elapsedUs(saveStart, std::chrono::steady_clock::now()));

    const auto scheduled = snapshots.size();
    if (scheduled > 0) {
        const auto saveRoot = settings.saveRoot;
        asyncJobs_.push_back({jobs.submit({"chunk.save", core::JobPriority::Low},
            [saveRoot, snapshots = std::move(snapshots)]() mutable {
                RegionFileStore store(saveRoot);
                std::size_t saved = 0;
                for (const auto& snapshot : snapshots) {
                    store.saveChunk(snapshot);
                    ++saved;
                }
                return saved;
            })});
    }

    stats.saveQueueLength = worldSaveService.dirtyChunkCount(chunks) + asyncJobs_.size();
    if (stats.saveQueueLength > 0) {
        stats.saveBudgetSaturated = 1;
    }
    if (scheduled > 0 || stats.savesFlushed > 0) {
        Logger::info(
            "Save flush: scheduled_chunks=" + std::to_string(scheduled)
            + " completed_chunks=" + std::to_string(stats.savesFlushed)
            + " remaining=" + std::to_string(stats.saveQueueLength)
            + (force ? " forced" : ""));
    }
    return stats;
}

std::size_t SaveCoordinator::drainCompleted(bool wait)
{
    std::size_t completed = 0;
    auto it = asyncJobs_.begin();
    while (it != asyncJobs_.end()) {
        if (wait) {
            it->future.wait();
        } else if (it->future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            ++it;
            continue;
        }
        completed += it->future.get();
        it = asyncJobs_.erase(it);
    }
    return completed;
}

} // namespace voxel::save
