#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <voxel/world/Chunk.hpp>

namespace voxel::world {

class ChunkManager {
public:
    Chunk& createOrGet(ChunkCoord coord);
    Chunk& store(Chunk chunk);
    [[nodiscard]] Chunk* find(ChunkCoord coord);
    [[nodiscard]] const Chunk* find(ChunkCoord coord) const;
    void evict(ChunkCoord coord);

    // Evict every chunk whose Chebyshev distance from `center` exceeds
    // `(horizontalRadius, verticalRadius)`, with two guards:
    //   - Skip chunks marked dirty().save (data not yet persisted —
    //     evicting would lose the player's edits).
    //   - Optionally skip chunks the caller flags as "in flight" so we
    //     don't evict block data underneath an active mesh / lighting
    //     job (the install path checks find()==nullptr, so it's safe
    //     correctness-wise, but the wasted work is annoying).
    //
    // Returns the list of evicted coords so the caller can drop the
    // corresponding mesh cache entry, GPU upload, etc. Caps eviction
    // count at `maxEvictions` to keep per-tick work bounded.
    [[nodiscard]] std::vector<ChunkCoord> evictOutsideRadius(
        ChunkCoord center,
        int horizontalRadius,
        int verticalRadius,
        std::size_t maxEvictions);

    void reserve(std::size_t chunkCount);
    [[nodiscard]] std::size_t residentCount() const noexcept;
    void forEach(const std::function<void(Chunk&)>& visitor);
    void forEach(const std::function<void(const Chunk&)>& visitor) const;

private:
    std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash> chunks_;
};

} // namespace voxel::world
