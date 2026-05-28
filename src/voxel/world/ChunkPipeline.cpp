#include <voxel/world/ChunkPipeline.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include <voxel/save/RegionFileStore.hpp>

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

bool savedChunkMatchesGenerator(const Chunk& chunk, const IChunkGenerator& generator) noexcept
{
    const std::uint64_t expectedTerrainVersion = generator.terrainVersion();
    if (expectedTerrainVersion == 0 || chunk.terrainVersion() == expectedTerrainVersion) {
        return true;
    }

    // Known-version procedural chunks are safe to regenerate when stale.
    // Edited chunks are written with terrainVersion=0 by Chunk::setBlock.
    if (chunk.terrainVersion() != 0) {
        return false;
    }

    // Legacy saves had no terrain-version field, so revision is the only
    // safe edit signal there. Freshly generated chunks start at revision 1;
    // block edits bump beyond that.
    return chunk.revision() > 1;
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
            if (savedChunkMatchesGenerator(*loaded, generator)) {
                chunks.store(std::move(*loaded));
                ++stats.loaded;
                stats.installedChunks.push_back(request.coord);
                continue;
            }
        } else {
            core::recordTimer(stats.loadTime, elapsedUs(loadStart, std::chrono::steady_clock::now()));
        }

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
    // Each installed chunk marks up to 6 neighbours dirty, each of which may
    // push a coord into `neighborDirtyChunks`. Pre-reserve to avoid 7+
    // allocator reallocations during the install pass (each reallocation
    // costs ~10µs in Debug due to scribble + move-construction).
    stats.neighborDirtyChunks.reserve(completed.size() * 6U);
    const auto installStart = std::chrono::steady_clock::now();
    std::size_t installedResultsThisTick = 0;
    std::vector<GeneratedChunkResult> deferredGenerationResults;
    for (std::size_t resultIndex = 0; resultIndex < completed.size(); ++resultIndex) {
        auto& result = completed[resultIndex];
        if (installedResultsThisTick >= settings.minGenerationInstallsPerTick
            && settings.maxGenerationInstallMsPerTick > 0.0
            && std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - installStart).count() >= settings.maxGenerationInstallMsPerTick) {
            deferredGenerationResults.reserve(completed.size() - resultIndex);
            for (std::size_t deferredIndex = resultIndex; deferredIndex < completed.size(); ++deferredIndex) {
                deferredGenerationResults.push_back(std::move(completed[deferredIndex]));
            }
            break;
        }

        const auto installedCoord = result.coord;
        const bool wasNew = (chunks.find(installedCoord) == nullptr);
        if (wasNew) {
            chunks.store(std::move(result.chunk));
            ++stats.loaded; // accounted as "ready chunk produced" for caller visibility
            stats.installedChunks.push_back(installedCoord);
            if (result.loadedFromSave) {
                core::recordTimer(stats.loadTime, result.loadTimeUs);
            } else {
                core::recordTimer(stats.generationTime, result.generationTimeUs);
                if (result.generatedFromPrepass) {
                    core::recordTimer(stats.generationFromPrepassTime, result.generationTimeUs);
                } else {
                    core::recordTimer(stats.generationDirectTime, result.generationTimeUs);
                }
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
                    const bool wasMeshDirty = neighbour->dirty().mesh;
                    const bool wasLightingDirty = neighbour->dirty().lighting;
                    neighbour->markMeshDirtyNoRevision();
                    neighbour->markLightingDirtyNoRevision();
                    if (!wasMeshDirty || !wasLightingDirty) {
                        stats.neighborDirtyChunks.push_back(nCoord);
                    }
                    if (!wasMeshDirty) {
                        ++stats.neighborRemeshes;
                    }
                }
            }
        }
        ++installedResultsThisTick;
    }
    mailbox.requeueGeneration(std::move(deferredGenerationResults));

    std::vector<ChunkCoord> queuedThisTick;
    queuedThisTick.reserve(settings.maxLoadsOrGenerationsPerTick);
    constexpr std::size_t kMaxVerticalColumnBatch = 8;

    auto prepareGenerationCoord = [&](const ChunkRequest& request) -> bool {
        if (stats.dispatched >= settings.maxLoadsOrGenerationsPerTick) {
            return false;
        }
        if (settings.maxInFlightGeneration > 0 && mailbox.inFlightGenerationCount() >= settings.maxInFlightGeneration) {
            return false;
        }
        if (std::find(queuedThisTick.begin(), queuedThisTick.end(), request.coord) != queuedThisTick.end()) {
            return false;
        }

        if (const auto* existing = chunks.find(request.coord)) {
            const auto state = existing->state();
            if (state == ChunkState::Resident || state == ChunkState::Meshing || state == ChunkState::MeshReady || state == ChunkState::Generating) {
                ++stats.skipped;
                return false;
            }
        }

        if (!settings.workerLoadRoot.has_value()) {
            // Legacy synchronous path for tests/custom stores that have not opted into
            // worker-local RegionFileStore loads.
            const auto loadStart = std::chrono::steady_clock::now();
            if (auto loaded = saveStore.loadChunk(request.coord)) {
                core::recordTimer(stats.loadTime, elapsedUs(loadStart, std::chrono::steady_clock::now()));
                if (savedChunkMatchesGenerator(*loaded, generator)) {
                    chunks.store(std::move(*loaded));
                    ++stats.loaded;
                    stats.installedChunks.push_back(request.coord);
                    return false;
                }
            } else {
                core::recordTimer(stats.loadTime, elapsedUs(loadStart, std::chrono::steady_clock::now()));
            }
        }

        if (!mailbox.tryBeginGeneration(request.coord)) {
            ++stats.skipped;
            return false;
        }

        queuedThisTick.push_back(request.coord);
        ++stats.dispatched;
        ++stats.generated;
        return true;
    };

    for (std::size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex) {
        if (stats.dispatched >= settings.maxLoadsOrGenerationsPerTick) {
            break;
        }
        if (settings.maxInFlightGeneration > 0 && mailbox.inFlightGenerationCount() >= settings.maxInFlightGeneration) {
            break;
        }

        const auto& request = requests[requestIndex];
        if (!prepareGenerationCoord(request)) {
            continue;
        }

        std::vector<ChunkCoord> batchCoords;
        batchCoords.reserve(kMaxVerticalColumnBatch);
        batchCoords.push_back(request.coord);

        // Column-batch generation: keep runtime/render/save chunks as 32^3,
        // but generate requested vertical slices of the same X/Z column in one
        // worker job. NoiseTerrainGenerator can then reuse one prepass and
        // workers amortize job overhead while the main-thread ChunkManager
        // still installs ordinary chunks one at a time.
        for (std::size_t lookahead = requestIndex + 1;
             lookahead < requests.size()
             && batchCoords.size() < kMaxVerticalColumnBatch
             && stats.dispatched < settings.maxLoadsOrGenerationsPerTick;
             ++lookahead) {
            const auto& candidate = requests[lookahead];
            if (candidate.coord.x != request.coord.x || candidate.coord.z != request.coord.z) {
                continue;
            }
            if (settings.maxInFlightGeneration > 0
                && mailbox.inFlightGenerationCount() >= settings.maxInFlightGeneration) {
                break;
            }
            if (prepareGenerationCoord(candidate)) {
                batchCoords.push_back(candidate.coord);
            }
        }

        std::sort(batchCoords.begin(), batchCoords.end(), [](ChunkCoord lhs, ChunkCoord rhs) {
            return lhs.y < rhs.y;
        });

        const auto priority = priorityForRequest(request.priority);
        const auto enqueueTime = std::chrono::steady_clock::now();

        // Run generation entirely off the main thread; main thread will install the chunk
        // into ChunkManager on the next drain.
        IChunkGenerator* generatorPtr = &generator;
        ChunkJobMailbox* mailboxPtr = &mailbox;
        const auto workerLoadRoot = settings.workerLoadRoot;
        jobs.submit({"chunk.generate", priority},
            [batchCoords = std::move(batchCoords), generatorPtr, mailboxPtr, enqueueTime, workerLoadRoot]() {
                const auto start = std::chrono::steady_clock::now();
                std::vector<Chunk> chunksToGenerate;
                chunksToGenerate.reserve(batchCoords.size());

                for (const auto coord : batchCoords) {
                    GeneratedChunkResult result{};
                    result.coord = coord;
                    result.queueWaitUs = elapsedUs(enqueueTime, start);

                    if (!workerLoadRoot.has_value()) {
                        chunksToGenerate.emplace_back(coord);
                        continue;
                    }

                    const auto loadStart = std::chrono::steady_clock::now();
                    if (auto loaded = save::RegionFileStore::loadChunkFromRoot(*workerLoadRoot, coord)) {
                        const auto loadEnd = std::chrono::steady_clock::now();
                        if (savedChunkMatchesGenerator(*loaded, *generatorPtr)) {
                            result.chunk = std::move(*loaded);
                            result.loadTimeUs = elapsedUs(loadStart, loadEnd);
                            result.loadedFromSave = true;
                            mailboxPtr->pushGeneration(std::move(result));
                            continue;
                        }
                    }

                    chunksToGenerate.emplace_back(coord);
                }

                if (chunksToGenerate.empty()) {
                    return;
                }

                const auto generationStart = std::chrono::steady_clock::now();
                std::vector<TerrainGenerationMode> modes;
                generatorPtr->generateColumn(chunksToGenerate, modes);
                const auto end = std::chrono::steady_clock::now();
                const auto totalGenerationUs = elapsedUs(generationStart, end);
                const auto perChunkGenerationUs =
                    chunksToGenerate.empty() ? 0U : totalGenerationUs / static_cast<std::uint64_t>(chunksToGenerate.size());

                for (std::size_t i = 0; i < chunksToGenerate.size(); ++i) {
                    GeneratedChunkResult result{};
                    result.coord = chunksToGenerate[i].coord();
                    result.chunk = std::move(chunksToGenerate[i]);
                    result.queueWaitUs = elapsedUs(enqueueTime, start);
                    result.generationTimeUs = perChunkGenerationUs;
                    const auto mode = i < modes.size() ? modes[i] : generatorPtr->lastGenerationMode();
                    result.generatedFromPrepass = mode == TerrainGenerationMode::CachedPrepass;
                    mailboxPtr->pushGeneration(std::move(result));
                }
            });
    }

    return stats;
}

} // namespace voxel::world
