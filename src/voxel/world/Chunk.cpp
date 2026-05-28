#include <voxel/world/Chunk.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace voxel::world {

namespace {

constexpr std::size_t kChunkBlockCount = static_cast<std::size_t>(ChunkVolume);

} // namespace

Chunk::Chunk(ChunkCoord coord)
    : coord_(coord),
      blockData_(std::make_shared<ChunkBlockData>())
{
    blockData_->indices = BitPackedArray(kChunkBlockCount, 1U);
    blockData_->palette.getOrInsert(AirBlockState);
}

Chunk::Chunk(const Chunk& other)
    : coord_(other.coord_),
      state_(other.state_),
      blockData_(other.blockData_),
      light_(other.light_ ? std::make_unique<ChunkLightData>(*other.light_) : nullptr),
      dirty_(other.dirty_),
      revision_(other.revision_),
      meshRevision_(other.meshRevision_),
      terrainVersion_(other.terrainVersion_)
{
}

Chunk& Chunk::operator=(const Chunk& other)
{
    if (this == &other) {
        return *this;
    }
    coord_ = other.coord_;
    state_ = other.state_;
    blockData_ = other.blockData_;
    light_ = other.light_ ? std::make_unique<ChunkLightData>(*other.light_) : nullptr;
    dirty_ = other.dirty_;
    revision_ = other.revision_;
    meshRevision_ = other.meshRevision_;
    terrainVersion_ = other.terrainVersion_;
    return *this;
}

Chunk Chunk::cloneBlocksOnly() const
{
    // Hot-path copy used by the mesh/light dispatcher snapshot. Copies every
    // POD field and the shared_ptr<ChunkBlockData> (ref-inc only, no data
    // duplication), but deliberately leaves `light_` as nullptr so the
    // caller doesn't pay the 32 KB heap-alloc + memcpy + later-free price
    // for light data that the workers won't read anyway.
    Chunk copy(coord_);
    copy.state_ = state_;
    copy.blockData_ = blockData_;          // shared_ptr inc → cheap
    // copy.light_ stays default-null
    // blockEntities_ is per-cell metadata not read by the mesher; default-empty.
    copy.dirty_ = dirty_;
    copy.revision_ = revision_;
    copy.meshRevision_ = meshRevision_;
    copy.terrainVersion_ = terrainVersion_;
    return copy;
}

ChunkCoord Chunk::coord() const noexcept
{
    return coord_;
}

ChunkState Chunk::state() const noexcept
{
    return state_;
}

void Chunk::setState(ChunkState state) noexcept
{
    state_ = state;
}

BlockStateId Chunk::blockAt(int x, int y, int z) const
{
    const auto idx = blockData_->indices.at(index(x, y, z));
    return blockData_->palette.at(static_cast<std::uint16_t>(idx));
}

void Chunk::setBlock(int x, int y, int z, BlockStateId state)
{
    ensureUniqueBlockData();
    const auto paletteIdx = ensurePaletteEntry(state);
    blockData_->indices.set(index(x, y, z), paletteIdx);
    dirty_.blocks = true;
    dirty_.mesh = true;
    dirty_.lighting = true;
    dirty_.save = true;
    dirty_.collision = true;
    ++revision_;
    ++meshRevision_;
}

void Chunk::setBlockSilently(int x, int y, int z, BlockStateId state)
{
    ensureUniqueBlockData();
    const auto paletteIdx = ensurePaletteEntry(state);
    blockData_->indices.set(index(x, y, z), paletteIdx);
}

void Chunk::fillSilently(BlockStateId state)
{
    ensureUniqueBlockData();
    const auto paletteIdx = ensurePaletteEntry(state);
    for (std::size_t i = 0; i < kChunkBlockCount; ++i) {
        blockData_->indices.set(i, paletteIdx);
    }
}

void Chunk::fillColumnRangeSilently(int x, int z, int yBeginInclusive, int yEndInclusive, BlockStateId state)
{
    if (x < 0 || z < 0 || x >= ChunkSize || z >= ChunkSize) {
        throw std::out_of_range("Chunk block coordinate outside 32^3 bounds");
    }
    if (yEndInclusive < 0 || yBeginInclusive >= ChunkSize || yBeginInclusive > yEndInclusive) {
        return;
    }

    const int yBegin = std::max(0, yBeginInclusive);
    const int yEnd = std::min(ChunkSize - 1, yEndInclusive);
    ensureUniqueBlockData();
    const auto paletteIdx = ensurePaletteEntry(state);
    for (int y = yBegin; y <= yEnd; ++y) {
        blockData_->indices.set_unchecked(uncheckedIndex(x, y, z), paletteIdx);
    }
}

Revision Chunk::revision() const noexcept
{
    return revision_;
}

Revision Chunk::meshRevision() const noexcept
{
    return meshRevision_;
}

std::uint64_t Chunk::terrainVersion() const noexcept
{
    return terrainVersion_;
}

void Chunk::setTerrainVersion(std::uint64_t version) noexcept
{
    terrainVersion_ = version;
}

const ChunkDirtyFlags& Chunk::dirty() const noexcept
{
    return dirty_;
}

void Chunk::clearDirty() noexcept
{
    dirty_ = {};
}

void Chunk::clearSaveDirty() noexcept
{
    dirty_.save = false;
}

void Chunk::clearMeshDirtyOnly() noexcept
{
    dirty_.mesh = false;
}

void Chunk::clearLightingDirtyOnly() noexcept
{
    dirty_.lighting = false;
}

void Chunk::markGenerated() noexcept
{
    state_ = ChunkState::Resident;
    dirty_.mesh = true;
    dirty_.lighting = true;
    dirty_.save = false;
    dirty_.collision = true;
    ++revision_;
    ++meshRevision_;
    terrainVersion_ = 0;
}

void Chunk::markLoaded(Revision revision, std::uint64_t terrainVersion) noexcept
{
    state_ = ChunkState::Resident;
    dirty_ = {};
    revision_ = revision;
    meshRevision_ = revision;
    terrainVersion_ = terrainVersion;
}

void Chunk::markGeometryDirty() noexcept
{
    dirty_.mesh = true;
    dirty_.lighting = true;
    dirty_.collision = true;
    ++revision_;
    ++meshRevision_;
}

void Chunk::markMeshDirtyNoRevision() noexcept
{
    // ALWAYS bump meshRevision_, even if dirty.mesh was already set. The
    // previous early-return caused a subtle stale-mesh bug: when a mesh job
    // was in flight and a NEW change happened to this chunk, the second
    // call would see dirty.mesh=true and return without bumping
    // meshRevision_. The dispatched job would then come back as "not stale"
    // (revision matched), install, clearMeshDirtyOnly() — leaving the new
    // change in block data but invisible in the rendered mesh. The fluid
    // sim's horizontal flow chain triggers this exact race on every carve
    // after the first one in a chunk. Always bumping meshRevision_ lets
    // installMeshResults() detect the post-dispatch carve and re-enqueue
    // a fresh mesh build via the meshRevision-aware stale check.
    dirty_.mesh = true;
    ++meshRevision_;
}

void Chunk::markLightingDirtyNoRevision() noexcept
{
    dirty_.lighting = true;
}

void Chunk::resetFromStorage(Palette<BlockStateId, BlockStateIdHash> palette, BitPackedArray indices)
{
    ensureUniqueBlockData();
    blockData_->palette = std::move(palette);
    blockData_->indices = std::move(indices);
}

const ChunkLightData* Chunk::lightData() const noexcept
{
    return light_.get();
}

void Chunk::setLightData(ChunkLightData data)
{
    if (light_) {
        *light_ = std::move(data);
    } else {
        light_ = std::make_unique<ChunkLightData>(std::move(data));
    }
}

void Chunk::clearLightData() noexcept
{
    light_.reset();
}

bool Chunk::hasBlockEntity(std::size_t localIndex) const noexcept
{
    return blockEntities_.contains(localIndex);
}

const BlockEntityData* Chunk::blockEntity(std::size_t localIndex) const noexcept
{
    const auto it = blockEntities_.find(localIndex);
    return it != blockEntities_.end() ? &it->second : nullptr;
}

BlockEntityData& Chunk::createBlockEntity(std::size_t localIndex)
{
    return blockEntities_[localIndex];
}

void Chunk::removeBlockEntity(std::size_t localIndex) noexcept
{
    blockEntities_.erase(localIndex);
}

std::size_t Chunk::blockEntityCount() const noexcept
{
    return blockEntities_.size();
}

void Chunk::ensureUniqueBlockData()
{
    if (blockData_.use_count() > 1) {
        blockData_ = std::make_shared<ChunkBlockData>(*blockData_);
    }
}

std::uint16_t Chunk::ensurePaletteEntry(BlockStateId state)
{
    auto& palette = blockData_->palette;
    auto& indices = blockData_->indices;
    const auto before = palette.size();
    const auto idx = palette.getOrInsert(state);
    if (palette.size() != before) {
        const auto needed = palette.bitsRequired();
        if (needed > indices.bitsPerEntry()) {
            indices.resize(needed);
        }
    }
    return idx;
}

std::size_t Chunk::index(int x, int y, int z)
{
    if (x < 0 || y < 0 || z < 0 || x >= ChunkSize || y >= ChunkSize || z >= ChunkSize) {
        throw std::out_of_range("Chunk block coordinate outside 32^3 bounds");
    }

    return static_cast<std::size_t>(x + (y * ChunkSize) + (z * ChunkSize * ChunkSize));
}



} // namespace voxel::world
