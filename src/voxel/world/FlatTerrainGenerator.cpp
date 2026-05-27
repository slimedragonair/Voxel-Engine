#include <voxel/world/FlatTerrainGenerator.hpp>

namespace voxel::world {

FlatTerrainGenerator::FlatTerrainGenerator(FlatTerrainSettings settings)
    : settings_(settings)
{
}

void FlatTerrainGenerator::generate(Chunk& chunk)
{
    const auto coord = chunk.coord();
    for (int z = 0; z < ChunkSize; ++z) {
        for (int y = 0; y < ChunkSize; ++y) {
            const auto worldY = static_cast<int>(coord.y * ChunkSize + y);
            if (worldY <= settings_.surfaceBlockY) {
                for (int x = 0; x < ChunkSize; ++x) {
                    chunk.setBlockSilently(x, y, z, settings_.fillState);
                }
            }
        }
    }

    chunk.markGenerated();
}

} // namespace voxel::world

