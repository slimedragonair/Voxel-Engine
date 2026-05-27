#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_set>

#include <voxel/save/ISaveStore.hpp>

namespace voxel::save {

// On-disk chunk file format:
//   magic[4]   = 'V','X','C','2'
//   version    = uint16  (currently kSaveFormatVersion)
//   flags      = uint16  (bit 0 = body compressed with Zstd)
//   coord      = int64 x 3
//   revision   = uint64
//   body       = (raw OR Zstd-compressed) body bytes
//
// Body layout (uncompressed):
//   palette_count   = uint16
//   palette         = uint32 x palette_count
//   bits_per_entry  = uint8
//   word_count      = uint32
//   words           = uint64 x word_count
//
// Old "VCHK" (Phase D) saves are not migrated. Delete `saves/dev_world/`
// the first time you run after upgrading.
class RegionFileStore final : public ISaveStore {
public:
    static constexpr std::uint16_t kSaveFormatVersion = 1;

    explicit RegionFileStore(std::filesystem::path root = "saves/dev_world");

    void saveChunk(const world::Chunk& chunk) override;
    std::optional<world::Chunk> loadChunk(world::ChunkCoord coord) override;
    [[nodiscard]] static std::optional<world::Chunk> loadChunkFromRoot(
        const std::filesystem::path& root,
        world::ChunkCoord coord);

    // True if this build was compiled with Zstd support and the store
    // writes compressed bodies. Otherwise bodies are stored uncompressed
    // (still in the versioned binary format).
    [[nodiscard]] static bool zstdEnabled() noexcept;

private:
    void rebuildChunkIndex();

    std::filesystem::path root_;
    std::unordered_set<world::ChunkCoord, world::ChunkCoordHash> knownChunkFiles_;
};

} // namespace voxel::save
