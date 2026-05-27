#pragma once

#include <chrono>
#include <cstdint>

namespace voxel::core {

struct RuntimeCounters {
    struct Timer {
        std::uint64_t count{};
        std::uint64_t totalUs{};
        std::uint64_t maxUs{};
    };

    std::uint64_t frames{};
    std::uint64_t chunkRequestsPlanned{};
    std::uint64_t generationJobsSubmitted{};
    std::uint64_t generationJobsCompleted{};
    std::uint64_t meshJobsSubmitted{};
    std::uint64_t meshJobsCompleted{};
    std::uint64_t meshJobsDiscardedStale{};
    std::uint64_t lightingRecomputes{};
    std::uint64_t lightingJobsSubmitted{};
    std::uint64_t lightingJobsCompleted{};
    std::uint64_t lightingJobsDiscardedStale{};
    std::uint64_t remeshesCausedByNeighborInstall{};
    std::uint64_t gpuUploads{};
    std::uint64_t duplicateGpuUploadSkips{};
    std::uint64_t stagingUploadBytes{};
    // J2a: per-frame CPU frustum-cull counts (chunks that passed / were rejected
    // before being submitted as draws). Accumulated across the interval.
    std::uint64_t chunksDrawn{};
    std::uint64_t chunksCulled{};
    std::uint64_t terrainPrepassJobsSubmitted{};
    std::uint64_t terrainPrepassJobsCompleted{};
    std::uint64_t terrainPrepassCacheHits{};
    std::uint64_t terrainPrepassCacheMisses{};
    std::uint64_t terrainPrepassCacheEntries{};
    std::uint64_t blockEditsAccepted{};
    std::uint64_t blockEditsRejected{};
    std::uint64_t dirtyMeshChunksQueued{};
    std::uint64_t dirtyMeshChunksCoalesced{};
    std::uint64_t dirtyLightingChunksQueued{};
    std::uint64_t dirtyLightingChunksCoalesced{};
    std::uint64_t dirtyLightingQueueLength{};
    std::uint64_t dirtyMeshQueueLength{};
    std::uint64_t dirtyQueueScanTimeUs{};
    std::uint64_t saveQueueLength{};
    std::uint64_t savesFlushed{};
    std::uint64_t uploadBudgetDeferrals{};
    std::uint64_t lightingBudgetSaturated{};
    std::uint64_t meshInstallBudgetSaturated{};
    std::uint64_t saveBudgetSaturated{};
    std::uint64_t uploadBatchCount{};
    std::uint64_t uploadBatchBytes{};
    std::uint64_t uploadQueueLength{};
    std::uint64_t chunksMadeDrawable{};
    std::uint64_t gpuCullDispatches{};
    std::uint64_t gpuCullSections{};
    std::uint64_t gpuCullVisible{};
    std::uint64_t gpuCullCpuVisible{};
    std::uint64_t gpuCullMismatches{};
    std::uint64_t gpuCullDrawCommands{};
    std::uint64_t sceneEntriesSynced{};
    std::uint64_t sceneFullSyncs{};
    std::uint64_t jobSystemPending{};
    std::uint64_t workerCount{};
    std::uint64_t pendingGenerationResults{};
    std::uint64_t pendingMeshResults{};
    std::uint64_t pendingLightingResults{};
    std::uint64_t residentChunks{};
    std::uint64_t meshCacheEntries{};
    std::uint64_t inFlightGeneration{};
    std::uint64_t inFlightMesh{};
    std::uint64_t inFlightLighting{};
    std::uint64_t slowFrames{};
    Timer rendererFenceWait{};
    Timer terrainGeneration{};
    Timer terrainPrepass{};
    Timer terrainGenerationFromPrepass{};
    Timer terrainGenerationDirect{};
    Timer lightingPropagation{};
    Timer meshBuild{};
    Timer meshSnapshot{};
    Timer gpuUpload{};
    Timer save{};
    Timer load{};
    Timer queueWait{};
    Timer stageStreaming{};
    Timer stageStreamPlan{};
    Timer stageStreamDispatch{};
    Timer stageStreamPrepass{};
    Timer stageStreamPipeline{};
    Timer stageStreamEnqueue{};
    Timer stagePlayer{};
    Timer stageMeshInstall{};
    Timer stageMeshDispatch{};
    Timer stageLighting{};
    Timer stageSave{};
    Timer stageSimulation{};
    Timer stageRender{};
};

inline void recordTimer(RuntimeCounters::Timer& timer, std::uint64_t elapsedUs) noexcept
{
    ++timer.count;
    timer.totalUs += elapsedUs;
    if (elapsedUs > timer.maxUs) {
        timer.maxUs = elapsedUs;
    }
}

inline void mergeTimer(RuntimeCounters::Timer& target, const RuntimeCounters::Timer& delta) noexcept
{
    target.count += delta.count;
    target.totalUs += delta.totalUs;
    if (delta.maxUs > target.maxUs) {
        target.maxUs = delta.maxUs;
    }
}

class RuntimeStats {
public:
    void reset() noexcept
    {
        totals_ = {};
        interval_ = {};
        totalFrameMs_ = 0.0;
        intervalFrameMs_ = 0.0;
        lastFrameMs_ = 0.0;
        lastReport_ = std::chrono::steady_clock::now();
    }

    void recordFrame(double frameMs, const RuntimeCounters& counters) noexcept
    {
        lastFrameMs_ = frameMs;
        totalFrameMs_ += frameMs;
        intervalFrameMs_ += frameMs;
        add(totals_, counters);
        add(interval_, counters);
    }

    [[nodiscard]] bool shouldReport(std::chrono::steady_clock::time_point now) const noexcept
    {
        return now - lastReport_ >= std::chrono::seconds(3);
    }

    void resetInterval(std::chrono::steady_clock::time_point now) noexcept
    {
        interval_ = {};
        intervalFrameMs_ = 0.0;
        lastReport_ = now;
    }

    [[nodiscard]] const RuntimeCounters& totals() const noexcept { return totals_; }
    [[nodiscard]] const RuntimeCounters& interval() const noexcept { return interval_; }
    [[nodiscard]] double lastFrameMs() const noexcept { return lastFrameMs_; }

    [[nodiscard]] double averageFrameMs() const noexcept
    {
        return totals_.frames == 0 ? 0.0 : totalFrameMs_ / static_cast<double>(totals_.frames);
    }

    [[nodiscard]] double intervalAverageFrameMs() const noexcept
    {
        return interval_.frames == 0 ? 0.0 : intervalFrameMs_ / static_cast<double>(interval_.frames);
    }

private:
    static void add(RuntimeCounters& target, const RuntimeCounters& delta) noexcept
    {
        target.frames += delta.frames;
        target.chunkRequestsPlanned += delta.chunkRequestsPlanned;
        target.generationJobsSubmitted += delta.generationJobsSubmitted;
        target.generationJobsCompleted += delta.generationJobsCompleted;
        target.meshJobsSubmitted += delta.meshJobsSubmitted;
        target.meshJobsCompleted += delta.meshJobsCompleted;
        target.meshJobsDiscardedStale += delta.meshJobsDiscardedStale;
        target.lightingRecomputes += delta.lightingRecomputes;
        target.lightingJobsSubmitted += delta.lightingJobsSubmitted;
        target.lightingJobsCompleted += delta.lightingJobsCompleted;
        target.lightingJobsDiscardedStale += delta.lightingJobsDiscardedStale;
        target.remeshesCausedByNeighborInstall += delta.remeshesCausedByNeighborInstall;
        target.gpuUploads += delta.gpuUploads;
        target.duplicateGpuUploadSkips += delta.duplicateGpuUploadSkips;
        target.stagingUploadBytes += delta.stagingUploadBytes;
        target.chunksDrawn += delta.chunksDrawn;
        target.chunksCulled += delta.chunksCulled;
        target.terrainPrepassJobsSubmitted += delta.terrainPrepassJobsSubmitted;
        target.terrainPrepassJobsCompleted += delta.terrainPrepassJobsCompleted;
        target.terrainPrepassCacheHits += delta.terrainPrepassCacheHits;
        target.terrainPrepassCacheMisses += delta.terrainPrepassCacheMisses;
        if (delta.terrainPrepassCacheEntries != 0) {
            target.terrainPrepassCacheEntries = delta.terrainPrepassCacheEntries;
        }
        target.blockEditsAccepted += delta.blockEditsAccepted;
        target.blockEditsRejected += delta.blockEditsRejected;
        target.dirtyMeshChunksQueued += delta.dirtyMeshChunksQueued;
        target.dirtyMeshChunksCoalesced += delta.dirtyMeshChunksCoalesced;
        target.dirtyLightingChunksQueued += delta.dirtyLightingChunksQueued;
        target.dirtyLightingChunksCoalesced += delta.dirtyLightingChunksCoalesced;
        target.dirtyLightingQueueLength = delta.dirtyLightingQueueLength;
        target.dirtyMeshQueueLength = delta.dirtyMeshQueueLength;
        target.dirtyQueueScanTimeUs += delta.dirtyQueueScanTimeUs;
        target.saveQueueLength = delta.saveQueueLength;
        target.savesFlushed += delta.savesFlushed;
        target.uploadBudgetDeferrals += delta.uploadBudgetDeferrals;
        target.lightingBudgetSaturated += delta.lightingBudgetSaturated;
        target.meshInstallBudgetSaturated += delta.meshInstallBudgetSaturated;
        target.saveBudgetSaturated += delta.saveBudgetSaturated;
        target.uploadBatchCount += delta.uploadBatchCount;
        target.uploadBatchBytes += delta.uploadBatchBytes;
        target.uploadQueueLength = delta.uploadQueueLength;
        target.chunksMadeDrawable += delta.chunksMadeDrawable;
        target.gpuCullDispatches += delta.gpuCullDispatches;
        target.gpuCullSections += delta.gpuCullSections;
        target.gpuCullVisible += delta.gpuCullVisible;
        target.gpuCullCpuVisible += delta.gpuCullCpuVisible;
        target.gpuCullMismatches += delta.gpuCullMismatches;
        target.gpuCullDrawCommands += delta.gpuCullDrawCommands;
        target.sceneEntriesSynced += delta.sceneEntriesSynced;
        target.sceneFullSyncs += delta.sceneFullSyncs;
        if (delta.frames != 0) {
            target.jobSystemPending = delta.jobSystemPending;
            target.workerCount = delta.workerCount;
            target.pendingGenerationResults = delta.pendingGenerationResults;
            target.pendingMeshResults = delta.pendingMeshResults;
            target.pendingLightingResults = delta.pendingLightingResults;
            target.residentChunks = delta.residentChunks;
            target.meshCacheEntries = delta.meshCacheEntries;
            target.inFlightGeneration = delta.inFlightGeneration;
            target.inFlightMesh = delta.inFlightMesh;
            target.inFlightLighting = delta.inFlightLighting;
        }
        target.slowFrames += delta.slowFrames;
        mergeTimer(target.terrainGeneration, delta.terrainGeneration);
        mergeTimer(target.terrainPrepass, delta.terrainPrepass);
        mergeTimer(target.terrainGenerationFromPrepass, delta.terrainGenerationFromPrepass);
        mergeTimer(target.terrainGenerationDirect, delta.terrainGenerationDirect);
        mergeTimer(target.lightingPropagation, delta.lightingPropagation);
        mergeTimer(target.meshBuild, delta.meshBuild);
        mergeTimer(target.meshSnapshot, delta.meshSnapshot);
        mergeTimer(target.gpuUpload, delta.gpuUpload);
        mergeTimer(target.save, delta.save);
        mergeTimer(target.load, delta.load);
        mergeTimer(target.queueWait, delta.queueWait);
        mergeTimer(target.rendererFenceWait, delta.rendererFenceWait);
        mergeTimer(target.stageStreaming, delta.stageStreaming);
        mergeTimer(target.stageStreamPlan, delta.stageStreamPlan);
        mergeTimer(target.stageStreamDispatch, delta.stageStreamDispatch);
        mergeTimer(target.stageStreamPrepass, delta.stageStreamPrepass);
        mergeTimer(target.stageStreamPipeline, delta.stageStreamPipeline);
        mergeTimer(target.stageStreamEnqueue, delta.stageStreamEnqueue);
        mergeTimer(target.stagePlayer, delta.stagePlayer);
        mergeTimer(target.stageMeshInstall, delta.stageMeshInstall);
        mergeTimer(target.stageMeshDispatch, delta.stageMeshDispatch);
        mergeTimer(target.stageLighting, delta.stageLighting);
        mergeTimer(target.stageSave, delta.stageSave);
        mergeTimer(target.stageSimulation, delta.stageSimulation);
        mergeTimer(target.stageRender, delta.stageRender);
    }

    RuntimeCounters totals_{};
    RuntimeCounters interval_{};
    double totalFrameMs_{0.0};
    double intervalFrameMs_{0.0};
    double lastFrameMs_{0.0};
    std::chrono::steady_clock::time_point lastReport_{std::chrono::steady_clock::now()};
};

} // namespace voxel::core
