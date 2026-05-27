#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/world/BitPackedArray.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/ChunkConstants.hpp>
#include <voxel/world/ChunkLightData.hpp>
#include <voxel/world/Coordinates.hpp>
#include <voxel/world/Palette.hpp>

namespace voxel::world {

struct BlockEntityData {
    std::vector<std::byte> payload;
};

struct ChunkBlockData {
    Palette<BlockStateId, BlockStateIdHash> palette;
    BitPackedArray indices;
};

enum class ChunkState : std::uint8_t {
    Empty,
    Requested,
    Loading,
    Generating,
    Resident,
    Meshing,
    MeshReady,
    Evicting
};

struct ChunkDirtyFlags {
    bool blocks{false};
    bool mesh{false};
    bool lighting{false};
    bool save{false};
    bool collision{false};
};

class Chunk {
public:
    explicit Chunk(ChunkCoord coord = {});

    Chunk(const Chunk& other);
    Chunk& operator=(const Chunk& other);
    Chunk(Chunk&&) noexcept = default;
    Chunk& operator=(Chunk&&) noexcept = default;
    ~Chunk() = default;

    // Returns a copy of this chunk that shares the block data (via shared_ptr
    // ref inc) but does NOT copy the light data. Used by the mesh/light
    // worker dispatch path to avoid 32 KB-per-neighbour allocator churn.
    // When shader-lighting is on (light data unused on the worker), this
    // form skips ~7 × 32 KB allocations per dispatched mesh job.
    [[nodiscard]] Chunk cloneBlocksOnly() const;

    [[nodiscard]] ChunkCoord coord() const noexcept;
    [[nodiscard]] ChunkState state() const noexcept;
    void setState(ChunkState state) noexcept;

    [[nodiscard]] BlockStateId blockAt(int x, int y, int z) const;
    void setBlock(int x, int y, int z, BlockStateId state);
    void setBlockSilently(int x, int y, int z, BlockStateId state);
    void fillSilently(BlockStateId state);
    void fillColumnRangeSilently(int x, int z, int yBeginInclusive, int yEndInclusive, BlockStateId state);

    [[nodiscard]] Revision revision() const noexcept;
    [[nodiscard]] Revision meshRevision() const noexcept;
    [[nodiscard]] const ChunkDirtyFlags& dirty() const noexcept;
    void clearDirty() noexcept;
    void clearSaveDirty() noexcept;
    void clearMeshDirtyOnly() noexcept;
    void clearLightingDirtyOnly() noexcept;
    void markGenerated() noexcept;
    void markLoaded(Revision revision = 0) noexcept;
    void markGeometryDirty() noexcept;
    void markMeshDirtyNoRevision() noexcept;
    void markLightingDirtyNoRevision() noexcept;

    void resetFromStorage(Palette<BlockStateId, BlockStateIdHash> palette, BitPackedArray indices);

    [[nodiscard]] const ChunkLightData* lightData() const noexcept;
    void setLightData(ChunkLightData data);
    void clearLightData() noexcept;

    [[nodiscard]] const ChunkBlockData& blockData() const noexcept { return *blockData_; }

    [[nodiscard]] bool hasBlockEntity(std::size_t localIndex) const noexcept;
    [[nodiscard]] const BlockEntityData* blockEntity(std::size_t localIndex) const noexcept;
    BlockEntityData& createBlockEntity(std::size_t localIndex);
    void removeBlockEntity(std::size_t localIndex) noexcept;
    [[nodiscard]] std::size_t blockEntityCount() const noexcept;

    [[nodiscard]] static std::size_t index(int x, int y, int z);

    [[nodiscard]] static inline std::size_t uncheckedIndex(int x, int y, int z) noexcept
    {
        return static_cast<std::size_t>(x + (y * ChunkSize) + (z * ChunkSize * ChunkSize));
    }

    [[nodiscard]] inline BlockStateId blockAtUnchecked(int x, int y, int z) const noexcept
    {
        const auto idx = blockData_->indices.at_unchecked(uncheckedIndex(x, y, z));
        return blockData_->palette.entries()[idx];
    }

    inline void setBlockSilentlyUnchecked(int x, int y, int z, std::uint16_t paletteIdx) noexcept
    {
        blockData_->indices.set_unchecked(uncheckedIndex(x, y, z), paletteIdx);
    }

    void ensureUniqueBlockData();
    std::uint16_t ensurePaletteEntry(BlockStateId state);

private:

    ChunkCoord coord_{};
    ChunkState state_{ChunkState::Empty};
    std::shared_ptr<ChunkBlockData> blockData_;
    std::unique_ptr<ChunkLightData> light_;
    std::unordered_map<std::size_t, BlockEntityData> blockEntities_;
    ChunkDirtyFlags dirty_{};
    Revision revision_{0};
    Revision meshRevision_{0};
};

} // namespace voxel::world
