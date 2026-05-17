#include <voxel/world/ChunkPipeline.hpp>

#include <array>
#include <chrono>
#include <utility>

namespace voxel::world {

namespace {

std::uint64_t elapsedUs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

core::JobPriority priorityForRequest(float distancePriority)
{
    // ChunkStreamer::planRequests returns lower values for closer requests.
    if (distancePriority <= 4.0F) {
        return core::JobPriority::Critical;
    }
    if (distancePriority <= 64.0F) {
        return core::JobPriority::High;
    }
    return core::JobPriority::Medium;
}

} // namespace

ChunkPipelineStats ChunkPipeline::processRequests(
    ChunkManager& chunks,
    save::ISaveStore& saveStore,
    IChunkGenerator& generator,
    const std::vector<ChunkRequest>& requests,
    const ChunkPipelineSettings& settings) const
{
    ChunkPipelineStats stats;

    for (const auto& request : requests) {
        if ((stats.loaded + stats.generated) >= settings.maxLoadsOrGenerationsPerTick) {
            break;
        }

        if (const auto* existing = chunks.find(request.coord)) {
            const auto state = existing->state();
            if (state == ChunkState::Resident || state == ChunkState::Meshing || state == ChunkState::MeshReady || state == ChunkState::Generating) {
                ++stats.skipped;
                continue;
            }
        }

        const auto loadStart = std::chrono::steady_clock::now();
        if (auto loaded = saveStore.loadChunk(request.coord)) {
            core::recordTimer(stats.loadTime, elapsedUs(loadStart, std::chrono::steady_clock::now()));
            chunks.store(std::move(*loaded));
            ++stats.loaded;
            stats.installedChunks.push_back(request.coord);
            continue;
        }
        core::recordTimer(stats.loadTime, elapsedUs(loadStart, std::chrono::steady_clock::now()));

        auto& chunk = chunks.createOrGet(request.coord);
        chunk.setState(ChunkState::Generating);
        const auto generationStart = std::chrono::steady_clock::now();
        generator.generate(chunk);
        const auto generationUs = elapsedUs(generationStart, std::chrono::steady_clock::now());
        core::recordTimer(stats.generationTime, generationUs);
        if (generator.lastGenerationMode() == TerrainGenerationMode::CachedPrepass) {
            core::recordTimer(stats.generationFromPrepassTime, generationUs);
        } else {
            core::recordTimer(stats.generationDirectTime, generationUs);
        }
        ++stats.generated;
        stats.installedChunks.push_back(request.coord);
    }

    return stats;
}

ChunkPipelineStats ChunkPipeline::processRequestsAsync(
    ChunkManager& chunks,
    save::ISaveStore& saveStore,
    IChunkGenerator& generator,
    core::JobSystem& jobs,
    ChunkJobMailbox& mailbox,
    const std::vector<ChunkRequest>& requests,
    const ChunkPipelineSettings& settings) const
{
    ChunkPipelineStats stats;

    // Install completed generation results from previous ticks.
    auto completed = mailbox.drainGenerationClosest(settings.installPriorityCenter, settings.maxGenerationInstallsPerTick);
    for (auto& result : completed) {
        const auto installedCoord = result.coord;
        const bool wasNew = (chunks.find(installedCoord) == nullptr);
        if (wasNew) {
            chunks.store(std::move(result.chunk));
            ++stats.loaded; // accounted as "ready chunk produced" for caller visibility
            stats.installedChunks.push_back(installedCoord);
            core::recordTimer(stats.generationTime, result.generationTimeUs);
            if (result.generatedFromPrepass) {
                core::recordTimer(stats.generationFromPrepassTime, result.generationTimeUs);
            } else {
                core::recordTimer(stats.generationDirectTime, result.generationTimeUs);
            }
            core::recordTimer(stats.queueWaitTime, result.queueWaitUs);
        }
        mailbox.endGeneration(installedCoord);

        if (wasNew) {
            // F2: existing neighbours need to re-cull their boundary faces now
            // that a real neighbour exists. revision is NOT bumped — blocks
            // haven't changed in those chunks.
            constexpr std::array<ChunkCoord, 6> kFaceDeltas{{
                {-1, 0, 0}, {1, 0, 0},
                {0, -1, 0}, {0, 1, 0},
                {0, 0, -1}, {0, 0, 1},
            }};
            for (const auto& delta : kFaceDeltas) {
                const ChunkCoord nCoord{
                    installedCoord.x + delta.x,
                    installedCoord.y + delta.y,
                    installedCoord.z + delta.z};
                if (auto* neighbour = chunks.find(nCoord)) {
                    neighbour->markMeshDirtyNoRevision();
                    neighbour->markLightingDirtyNoRevision();
                    stats.neighborDirtyChunks.push_back(nCoord);
                    ++stats.neighborRemeshes;
                }
            }
        }
    }

    for (const auto& request : requests) {
        if (stats.dispatched >= settings.maxLoadsOrGenerationsPerTick) {
            break;
        }
        if (settings.maxInFlightGeneration > 0 && mailbox.inFlightGenerationCount() >= settings.maxInFlightGeneration) {
            break;
        }

        if (const auto* existing = chunks.find(request.coord)) {
            const auto state = existing->state();
            if (state == ChunkState::Resident || state == ChunkState::Meshing || state == ChunkState::MeshReady || state == ChunkState::Generating) {
                ++stats.skipped;
                continue;
            }
        }

        // Disk load remains on the main thread; SaveStore is not yet documented as thread-safe.
        const auto loadStart = std::chrono::steady_clock::now();
        if (auto loaded = saveStore.loadChunk(request.coord)) {
            core::recordTimer(stats.loadTime, elapsedUs(loadStart, std::chrono::steady_clock::now()));
            chunks.store(std::move(*loaded));
            ++stats.loaded;
            stats.installedChunks.push_back(request.coord);
            continue;
        }
        core::recordTimer(stats.loadTime, elapsedUs(loadStart, std::chrono::steady_clock::now()));

        if (!mailbox.tryBeginGeneration(request.coord)) {
            ++stats.skipped;
            continue;
        }

        const auto coord = request.coord;
        const auto priority = priorityForRequest(request.priority);
        const auto enqueueTime = std::chrono::steady_clock::now();

        // Run generation entirely off the main thread; main thread will install the chunk
        // into ChunkManager on the next drain.
        IChunkGenerator* generatorPtr = &generator;
        ChunkJobMailbox* mailboxPtr = &mailbox;
        jobs.submit({"chunk.generate", priority},
            [coord, generatorPtr, mailboxPtr, enqueueTime]() {
                const auto start = std::chrono::steady_clock::now();
                Chunk chunk(coord);
                generatorPtr->generate(chunk);
                const bool generatedFromPrepass = generatorPtr->lastGenerationMode() == TerrainGenerationMode::CachedPrepass;
                const auto end = std::chrono::steady_clock::now();
                GeneratedChunkResult result{};
                result.coord = coord;
                result.chunk = std::move(chunk);
                result.generationTimeUs = elapsedUs(start, end);
                result.queueWaitUs = elapsedUs(enqueueTime, start);
                result.generatedFromPrepass = generatedFromPrepass;
                mailboxPtr->pushGeneration(std::move(result));
            });

        ++stats.dispatched;
        ++stats.generated;
    }

    return stats;
}

} // namespace voxel::world
