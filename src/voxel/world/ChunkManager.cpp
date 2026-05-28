#include <voxel/world/ChunkManager.hpp>

#include <memory>
#include <utility>

namespace voxel::world {

Chunk& ChunkManager::createOrGet(ChunkCoord coord)
{
    if (auto* chunk = find(coord)) {
        return *chunk;
    }

    auto inserted = chunks_.emplace(coord, std::make_unique<Chunk>(coord));
    return *inserted.first->second;
}

Chunk& ChunkManager::store(Chunk chunk)
{
    const auto coord = chunk.coord();
    auto owned = std::make_unique<Chunk>(std::move(chunk));
    auto [it, inserted] = chunks_.insert_or_assign(coord, std::move(owned));
    (void)inserted;
    return *it->second;
}

Chunk* ChunkManager::find(ChunkCoord coord)
{
    const auto found = chunks_.find(coord);
    if (found == chunks_.end()) {
        return nullptr;
    }
    return found->second.get();
}

const Chunk* ChunkManager::find(ChunkCoord coord) const
{
    const auto found = chunks_.find(coord);
    if (found == chunks_.end()) {
        return nullptr;
    }
    return found->second.get();
}

void ChunkManager::evict(ChunkCoord coord)
{
    chunks_.erase(coord);
}

std::vector<ChunkCoord> ChunkManager::evictOutsideRadius(
    ChunkCoord center,
    int horizontalRadius,
    int verticalRadius,
    std::size_t maxEvictions,
    const std::function<bool(ChunkCoord)>& keep)
{
    std::vector<ChunkCoord> evicted;
    evicted.reserve(std::min<std::size_t>(maxEvictions, chunks_.size()));

    for (auto it = chunks_.begin(); it != chunks_.end() && evicted.size() < maxEvictions;) {
        const auto coord = it->first;
        const auto& chunk = *it->second;
        if (keep && keep(coord)) {
            ++it;
            continue;
        }

        const auto dx = coord.x - center.x;
        const auto dy = coord.y - center.y;
        const auto dz = coord.z - center.z;
        const auto adx = dx >= 0 ? dx : -dx;
        const auto ady = dy >= 0 ? dy : -dy;
        const auto adz = dz >= 0 ? dz : -dz;

        const bool outOfRange = (adx > horizontalRadius)
                             || (adz > horizontalRadius)
                             || (ady > verticalRadius);
        if (!outOfRange) {
            ++it;
            continue;
        }

        // CRITICAL: never evict dirty chunks — that drops unsaved
        // player edits on the floor. They'll get persisted by the save
        // flush and become eligible on a later eviction pass.
        if (chunk.dirty().save) {
            ++it;
            continue;
        }

        evicted.push_back(coord);
        it = chunks_.erase(it);
    }
    return evicted;
}

void ChunkManager::reserve(std::size_t chunkCount)
{
    chunks_.reserve(chunkCount);
}

std::size_t ChunkManager::residentCount() const noexcept
{
    return chunks_.size();
}

void ChunkManager::forEach(const std::function<void(Chunk&)>& visitor)
{
    for (auto& [coord, chunk] : chunks_) {
        (void)coord;
        visitor(*chunk);
    }
}

void ChunkManager::forEach(const std::function<void(const Chunk&)>& visitor) const
{
    for (const auto& [coord, chunk] : chunks_) {
        (void)coord;
        visitor(*chunk);
    }
}

} // namespace voxel::world
