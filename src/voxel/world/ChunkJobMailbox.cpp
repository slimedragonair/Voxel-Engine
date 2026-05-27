#include <voxel/world/ChunkJobMailbox.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

namespace voxel::world {

namespace {

std::int64_t distanceScore(ChunkCoord coord, ChunkCoord center) noexcept
{
    const auto dx = coord.x - center.x;
    const auto dy = coord.y - center.y;
    const auto dz = coord.z - center.z;
    return (dx * dx) + (dy * dy * 4) + (dz * dz);
}

} // namespace

// ============================================================================
// Generation stream — per-stream `generationMutex_` only.
// ============================================================================

void ChunkJobMailbox::pushGeneration(GeneratedChunkResult result)
{
    std::lock_guard<std::mutex> lock(generationMutex_);
    generationResults_.push_back(std::move(result));
    pendingGenerationCount_.store(generationResults_.size(), std::memory_order_relaxed);
}

void ChunkJobMailbox::requeueGeneration(std::vector<GeneratedChunkResult> results)
{
    if (results.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(generationMutex_);
    generationResults_.reserve(generationResults_.size() + results.size());
    std::move(results.begin(), results.end(), std::back_inserter(generationResults_));
    pendingGenerationCount_.store(generationResults_.size(), std::memory_order_relaxed);
}

std::vector<GeneratedChunkResult> ChunkJobMailbox::drainGeneration()
{
    std::lock_guard<std::mutex> lock(generationMutex_);
    std::vector<GeneratedChunkResult> drained;
    drained.swap(generationResults_);
    pendingGenerationCount_.store(0, std::memory_order_relaxed);
    return drained;
}

std::vector<GeneratedChunkResult> ChunkJobMailbox::drainGeneration(std::size_t maxResults)
{
    std::lock_guard<std::mutex> lock(generationMutex_);
    if (maxResults == 0 || generationResults_.empty()) {
        return {};
    }

    const std::size_t count = std::min(maxResults, generationResults_.size());
    std::vector<GeneratedChunkResult> drained;
    drained.reserve(count);
    auto first = generationResults_.begin();
    auto last = first + static_cast<std::ptrdiff_t>(count);
    std::move(first, last, std::back_inserter(drained));
    generationResults_.erase(first, last);
    pendingGenerationCount_.store(generationResults_.size(), std::memory_order_relaxed);
    return drained;
}

std::vector<GeneratedChunkResult> ChunkJobMailbox::drainGenerationClosest(ChunkCoord center, std::size_t maxResults)
{
    if (maxResults == 0) {
        return {};
    }

    // Step 1: swap the entire pending vector out under a short lock.
    // After this point, workers can push new results freely — they
    // hit an empty (or near-empty) generationResults_ while we do the
    // heavy partition work without holding the lock.
    //
    // This replaces what used to be a single ~100-200 µs locked
    // operation (nth_element + sort + erase, all under the mutex)
    // with: ~100 ns lock → unlocked CPU work → ~30 µs merge-back lock.
    // Lock-held time drops 3-6×, AND worker pushes during the unlocked
    // sort phase don't block — the cold-start parallelism win that
    // C2ME-style scheduler tightening is really about.
    std::vector<GeneratedChunkResult> snapshot;
    {
        std::lock_guard<std::mutex> lock(generationMutex_);
        if (generationResults_.empty()) {
            return {};
        }
        snapshot.swap(generationResults_);
        pendingGenerationCount_.store(0, std::memory_order_relaxed);
    }

    // Step 2: outside the lock, partition the snapshot to find the K
    // closest. Workers may concurrently push to generationResults_;
    // we'll merge our leftovers back at the end.
    auto cmp = [center](const auto& lhs, const auto& rhs) {
        const auto l = distanceScore(lhs.coord, center);
        const auto r = distanceScore(rhs.coord, center);
        if (l != r) {
            return l < r;
        }
        if (lhs.coord.y != rhs.coord.y) {
            return lhs.coord.y < rhs.coord.y;
        }
        if (lhs.coord.x != rhs.coord.x) {
            return lhs.coord.x < rhs.coord.x;
        }
        return lhs.coord.z < rhs.coord.z;
    };

    const std::size_t count = std::min(maxResults, snapshot.size());
    if (count < snapshot.size()) {
        std::nth_element(snapshot.begin(),
                         snapshot.begin() + static_cast<std::ptrdiff_t>(count),
                         snapshot.end(),
                         cmp);
        std::sort(snapshot.begin(),
                  snapshot.begin() + static_cast<std::ptrdiff_t>(count),
                  cmp);
    } else {
        std::sort(snapshot.begin(), snapshot.end(), cmp);
    }

    // Step 3: extract the K closest into the return value.
    std::vector<GeneratedChunkResult> drained;
    drained.reserve(count);
    std::move(snapshot.begin(),
              snapshot.begin() + static_cast<std::ptrdiff_t>(count),
              std::back_inserter(drained));

    // Step 4: merge leftovers back into generationResults_. Workers may
    // have pushed new results while we were sorting; those sit at the
    // tail of generationResults_. Our leftovers go after them. Order
    // doesn't matter — the next drainClosest call sorts by distance
    // from scratch anyway.
    const std::size_t leftoverCount = snapshot.size() - count;
    if (leftoverCount > 0) {
        std::lock_guard<std::mutex> lock(generationMutex_);
        generationResults_.reserve(generationResults_.size() + leftoverCount);
        std::move(snapshot.begin() + static_cast<std::ptrdiff_t>(count),
                  snapshot.end(),
                  std::back_inserter(generationResults_));
        pendingGenerationCount_.store(generationResults_.size(),
                                       std::memory_order_relaxed);
    }

    return drained;
}

// ============================================================================
// Mesh stream — per-stream `meshMutex_` only.
// ============================================================================

void ChunkJobMailbox::pushMesh(ChunkMeshResult result)
{
    std::lock_guard<std::mutex> lock(meshMutex_);
    meshResults_.push_back(std::move(result));
    pendingMeshCount_.store(meshResults_.size(), std::memory_order_relaxed);
}

std::vector<ChunkMeshResult> ChunkJobMailbox::drainMesh()
{
    std::lock_guard<std::mutex> lock(meshMutex_);
    std::vector<ChunkMeshResult> drained;
    drained.swap(meshResults_);
    pendingMeshCount_.store(0, std::memory_order_relaxed);
    return drained;
}

// ============================================================================
// Lighting stream — per-stream `lightingMutex_` only.
// ============================================================================

void ChunkJobMailbox::pushLighting(ChunkLightingResult result)
{
    std::lock_guard<std::mutex> lock(lightingMutex_);
    lightingResults_.push_back(std::move(result));
    pendingLightingCount_.store(lightingResults_.size(), std::memory_order_relaxed);
}

std::vector<ChunkLightingResult> ChunkJobMailbox::drainLighting()
{
    std::lock_guard<std::mutex> lock(lightingMutex_);
    std::vector<ChunkLightingResult> drained;
    drained.swap(lightingResults_);
    pendingLightingCount_.store(0, std::memory_order_relaxed);
    return drained;
}

// ============================================================================
// In-flight tracking — each set under its own stream's mutex. Atomic
// counters are bumped at insert/erase so reads (the hot-path budget
// checks) are lock-free.
// ============================================================================

bool ChunkJobMailbox::tryBeginGeneration(ChunkCoord coord)
{
    std::lock_guard<std::mutex> lock(generationMutex_);
    const auto inserted = inFlightGeneration_.insert(coord).second;
    if (inserted) {
        inFlightGenerationCount_.store(inFlightGeneration_.size(), std::memory_order_relaxed);
    }
    return inserted;
}

void ChunkJobMailbox::endGeneration(ChunkCoord coord)
{
    std::lock_guard<std::mutex> lock(generationMutex_);
    inFlightGeneration_.erase(coord);
    inFlightGenerationCount_.store(inFlightGeneration_.size(), std::memory_order_relaxed);
}

bool ChunkJobMailbox::isGenerationInFlight(ChunkCoord coord) const
{
    std::lock_guard<std::mutex> lock(generationMutex_);
    return inFlightGeneration_.find(coord) != inFlightGeneration_.end();
}

std::unordered_set<ChunkCoord, ChunkCoordHash> ChunkJobMailbox::snapshotInFlightGeneration() const
{
    std::lock_guard<std::mutex> lock(generationMutex_);
    // Single allocation + bulk copy under one lock. Caller can query the
    // returned set without further synchronization for the rest of the frame.
    return inFlightGeneration_;
}

bool ChunkJobMailbox::tryBeginMesh(MeshJobKey key)
{
    std::lock_guard<std::mutex> lock(meshMutex_);
    const auto inserted = inFlightMesh_.insert(key).second;
    if (inserted) {
        inFlightMeshCount_.store(inFlightMesh_.size(), std::memory_order_relaxed);
    }
    return inserted;
}

void ChunkJobMailbox::endMesh(MeshJobKey key)
{
    std::lock_guard<std::mutex> lock(meshMutex_);
    inFlightMesh_.erase(key);
    inFlightMeshCount_.store(inFlightMesh_.size(), std::memory_order_relaxed);
}

bool ChunkJobMailbox::tryBeginMesh(ChunkCoord coord)
{
    return tryBeginMesh(MeshJobKey{coord, 0, 0});
}

void ChunkJobMailbox::endMesh(ChunkCoord coord)
{
    endMesh(MeshJobKey{coord, 0, 0});
}

bool ChunkJobMailbox::tryBeginLighting(LightingJobKey key)
{
    std::lock_guard<std::mutex> lock(lightingMutex_);
    const auto inserted = inFlightLighting_.insert(key).second;
    if (inserted) {
        inFlightLightingCount_.store(inFlightLighting_.size(), std::memory_order_relaxed);
    }
    return inserted;
}

void ChunkJobMailbox::endLighting(LightingJobKey key)
{
    std::lock_guard<std::mutex> lock(lightingMutex_);
    inFlightLighting_.erase(key);
    inFlightLightingCount_.store(inFlightLighting_.size(), std::memory_order_relaxed);
}

// ============================================================================
// Lock-free count readers. These were 30-60 mutex acquisitions per frame
// each in the worst case (streaming dispatch + mesh dispatch + budget
// checks). Now they're one atomic load.
// ============================================================================

std::size_t ChunkJobMailbox::inFlightGenerationCount() const
{
    return inFlightGenerationCount_.load(std::memory_order_relaxed);
}

std::size_t ChunkJobMailbox::inFlightMeshCount() const
{
    return inFlightMeshCount_.load(std::memory_order_relaxed);
}

std::size_t ChunkJobMailbox::inFlightLightingCount() const
{
    return inFlightLightingCount_.load(std::memory_order_relaxed);
}

std::size_t ChunkJobMailbox::pendingGenerationResults() const
{
    return pendingGenerationCount_.load(std::memory_order_relaxed);
}

std::size_t ChunkJobMailbox::pendingMeshResults() const
{
    return pendingMeshCount_.load(std::memory_order_relaxed);
}

std::size_t ChunkJobMailbox::pendingLightingResults() const
{
    return pendingLightingCount_.load(std::memory_order_relaxed);
}

} // namespace voxel::world
