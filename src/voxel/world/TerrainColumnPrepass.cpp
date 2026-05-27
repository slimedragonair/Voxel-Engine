#include <voxel/world/TerrainColumnPrepass.hpp>

#include <utility>

namespace voxel::world {

std::optional<TerrainColumnPrepass> TerrainColumnPrepassCache::find(const TerrainColumnKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        ++stats_.misses;
        return std::nullopt;
    }
    ++stats_.hits;
    return found->second;
}

bool TerrainColumnPrepassCache::contains(const TerrainColumnKey& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(key) != entries_.end();
}

void TerrainColumnPrepassCache::insert(TerrainColumnPrepass prepass)
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[prepass.key] = std::move(prepass);
}

bool TerrainColumnPrepassCache::tryBeginJob(const TerrainColumnKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.find(key) != entries_.end()) {
        return false;
    }
    const bool inserted = inFlight_.insert(key).second;
    if (inserted) {
        inFlightCount_.fetch_add(1, std::memory_order_acq_rel);
    }
    return inserted;
}

void TerrainColumnPrepassCache::completeJob(TerrainColumnPrepass prepass, std::uint64_t buildTimeUs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inFlight_.erase(prepass.key) != 0) {
        inFlightCount_.fetch_sub(1, std::memory_order_acq_rel);
    }
    entries_[prepass.key] = std::move(prepass);
    ++stats_.jobsCompleted;
    core::recordTimer(stats_.prepassBuildTime, buildTimeUs);
}

void TerrainColumnPrepassCache::endJobWithoutStore(const TerrainColumnKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inFlight_.erase(key) != 0) {
        inFlightCount_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

std::size_t TerrainColumnPrepassCache::entryCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::size_t TerrainColumnPrepassCache::inFlightCount() const noexcept
{
    return inFlightCount_.load(std::memory_order_acquire);
}

TerrainColumnPrepassCacheStats TerrainColumnPrepassCache::drainStats()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto stats = stats_;
    stats_ = {};
    return stats;
}

} // namespace voxel::world
