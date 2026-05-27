#pragma once

#include <vector>

#include <voxel/world/Chunk.hpp>

namespace voxel::world {

enum class TerrainGenerationMode {
    Direct,
    CachedPrepass
};

class IChunkGenerator {
public:
    virtual ~IChunkGenerator() = default;
    virtual void generate(Chunk& chunk) = 0;
    virtual void generateColumn(std::vector<Chunk>& chunks, std::vector<TerrainGenerationMode>& modes)
    {
        modes.clear();
        modes.reserve(chunks.size());
        for (auto& chunk : chunks) {
            generate(chunk);
            modes.push_back(lastGenerationMode());
        }
    }
    [[nodiscard]] virtual TerrainGenerationMode lastGenerationMode() const noexcept { return TerrainGenerationMode::Direct; }
};

} // namespace voxel::world
