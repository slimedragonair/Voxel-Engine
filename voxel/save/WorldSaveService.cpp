#include <voxel/save/WorldSaveService.hpp>

namespace voxel::save {

std::size_t WorldSaveService::saveDirtyChunks(world::ChunkManager& chunks, ISaveStore& store) const
{
    return saveDirtyChunks(chunks, store, static_cast<std::size_t>(-1));
}

std::size_t WorldSaveService::saveDirtyChunks(world::ChunkManager& chunks, ISaveStore& store, std::size_t maxChunks) const
{
    std::size_t saved = 0;
    chunks.forEach([&store, &saved, maxChunks](world::Chunk& chunk) {
        if (saved >= maxChunks) {
            return;
        }
        if (!chunk.dirty().save) {
            return;
        }

        store.saveChunk(chunk);
        chunk.clearSaveDirty();
        ++saved;
    });
    return saved;
}

std::size_t WorldSaveService::dirtyChunkCount(const world::ChunkManager& chunks) const
{
    std::size_t dirty = 0;
    chunks.forEach([&dirty](const world::Chunk& chunk) {
        if (chunk.dirty().save) {
            ++dirty;
        }
    });
    return dirty;
}

} // namespace voxel::save
