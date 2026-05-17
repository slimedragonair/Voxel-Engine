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

void ChunkJobMailbox::pushGeneration(GeneratedChunkResult result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    generationResults_.push_back(std::move(result));
}

std::vector<GeneratedChunkResult> ChunkJobMailbox::drainGeneration()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<GeneratedChunkResult> drained;
    drained.swap(generationResults_);
    return drained;
}

std::vector<GeneratedChunkResult> ChunkJobMailbox::drainGeneration(std::size_t maxResults)
{
    std::lock_guard<std::mutex> lock(mutex_);
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
    return drained;
}

std::vector<GeneratedChunkResult> ChunkJobMailbox::drainGenerationClosest(ChunkCoord center, std::size_t maxResults)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (maxResults == 0 || generationResults_.empty()) {
        return {};
    }

    std::sort(generationResults_.begin(), generationResults_.end(), [center](const auto& lhs, const auto& rhs) {
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
    });

    const std::size_t count = std::min(maxResults, generationResults_.size());
    std::vector<GeneratedChunkResult> drained;
    drained.reserve(count);
    auto first = generationResults_.begin();
    auto last = first + static_cast<std::ptrdiff_t>(count);
    std::move(first, last, std::back_inserter(drained));
    generationResults_.erase(first, last);
    return drained;
}

void ChunkJobMailbox::pushMesh(ChunkMeshResult result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    meshResults_.push_back(std::move(result));
}

std::vector<ChunkMeshResult> ChunkJobMailbox::drainMesh()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChunkMeshResult> drained;
    drained.swap(meshResults_);
    return drained;
}

void ChunkJobMailbox::pushLighting(ChunkLightingResult result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lightingResults_.push_back(std::move(result));
}

std::vector<ChunkLightingResult> ChunkJobMailbox::drainLighting()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChunkLightingResult> drained;
    drained.swap(lightingResults_);
    return drained;
}

bool ChunkJobMailbox::tryBeginGeneration(ChunkCoord coord)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlightGeneration_.insert(coord).second;
}

void ChunkJobMailbox::endGeneration(ChunkCoord coord)
{
    std::lock_guard<std::mutex> lock(mutex_);
    inFlightGeneration_.erase(coord);
}

bool ChunkJobMailbox::isGenerationInFlight(ChunkCoord coord) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlightGeneration_.find(coord) != inFlightGeneration_.end();
}

bool ChunkJobMailbox::tryBeginMesh(MeshJobKey key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlightMesh_.insert(key).second;
}

void ChunkJobMailbox::endMesh(MeshJobKey key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    inFlightMesh_.erase(key);
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
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlightLighting_.insert(key).second;
}

void ChunkJobMailbox::endLighting(LightingJobKey key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    inFlightLighting_.erase(key);
}

std::size_t ChunkJobMailbox::inFlightGenerationCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlightGeneration_.size();
}

std::size_t ChunkJobMailbox::inFlightMeshCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlightMesh_.size();
}

std::size_t ChunkJobMailbox::inFlightLightingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlightLighting_.size();
}

std::size_t ChunkJobMailbox::pendingGenerationResults() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return generationResults_.size();
}

std::size_t ChunkJobMailbox::pendingMeshResults() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return meshResults_.size();
}

std::size_t ChunkJobMailbox::pendingLightingResults() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lightingResults_.size();
}

} // namespace voxel::world
