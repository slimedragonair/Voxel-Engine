#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

#include <voxel/core/JobSystem.hpp>
#include <voxel/core/RuntimeStats.hpp>
#include <voxel/save/ISaveStore.hpp>
#include <voxel/world/ChunkJobMailbox.hpp>
#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/ChunkStreamer.hpp>
#include <voxel/world/IChunkGenerator.hpp>

namespace voxel::world {

struct ChunkPipelineSettings {
    std::size_t maxLoadsOrGenerationsPerTick{16};
    std::size_t maxGenerationInstallsPerTick{24};
    std::size_t minGenerationInstallsPerTick{4};
    double maxGenerationInstallMsPerTick{3.0};
    std::size_t maxInFlightGeneration{96};
    ChunkCoord installPriorityCenter{};
    std::optional<std::filesystem::path> workerLoadRoot{};
};

struct ChunkPipelineStats {
    std::size_t loaded{};
    std::size_t generated{};
    std::size_t skipped{};
    std::size_t dispatched{};
    std::size_t neighborRemeshes{};
    std::vector<ChunkCoord> installedChunks;
    std::vector<ChunkCoord> neighborDirtyChunks;
    core::RuntimeCounters::Timer generationTime{};
    core::RuntimeCounters::Timer generationFromPrepassTime{};
    core::RuntimeCounters::Timer generationDirectTime{};
    core::RuntimeCounters::Timer queueWaitTime{};
    core::RuntimeCounters::Timer loadTime{};
};

class ChunkPipeline {
public:
    // Synchronous path: loads or generates chunks on the calling thread.
    [[nodiscard]] ChunkPipelineStats processRequests(
        ChunkManager& chunks,
        save::ISaveStore& saveStore,
        IChunkGenerator& generator,
        const std::vector<ChunkRequest>& requests,
        const ChunkPipelineSettings& settings) const;

    // Async path: drains completed generation results into the chunk manager,
    // then dispatches new generate-or-load jobs to the JobSystem for any chunk
    // that does not yet exist and is not currently in flight. Cap by settings.
    // Loads from disk are kept on the main thread for now (the SaveStore is not
    // yet thread-safe for concurrent reads); only generation runs on workers.
    [[nodiscard]] ChunkPipelineStats processRequestsAsync(
        ChunkManager& chunks,
        save::ISaveStore& saveStore,
        IChunkGenerator& generator,
        core::JobSystem& jobs,
        ChunkJobMailbox& mailbox,
        const std::vector<ChunkRequest>& requests,
        const ChunkPipelineSettings& settings) const;
};

} // namespace voxel::world
