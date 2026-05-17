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
