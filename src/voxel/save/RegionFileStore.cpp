#include <voxel/save/RegionFileStore.hpp>

#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <voxel/core/Logger.hpp>
#include <voxel/world/BitPackedArray.hpp>
#include <voxel/world/Palette.hpp>

#if defined(VOXEL_HAS_ZSTD)
#include <zstd.h>
#endif

namespace voxel::save {

namespace {

constexpr std::array<char, 4> kChunkMagic{'V', 'X', 'C', '2'};

enum FormatFlag : std::uint16_t {
    FlagZstdCompressed = 1U << 0U,
};

std::filesystem::path chunkDirectory(const std::filesystem::path& root)
{
    return root / "chunks";
}

std::filesystem::path chunkPath(const std::filesystem::path& root, world::ChunkCoord coord)
{
    const auto name = std::to_string(coord.x) + "_" + std::to_string(coord.y) + "_" + std::to_string(coord.z) + ".vchk";
    return chunkDirectory(root) / name;
}

std::optional<world::ChunkCoord> coordFromChunkFilename(const std::filesystem::path& path)
{
    if (path.extension() != ".vchk") {
        return std::nullopt;
    }
    const auto stem = path.stem().string();
    const auto first = stem.find('_');
    const auto second = first == std::string::npos ? std::string::npos : stem.find('_', first + 1U);
    if (first == std::string::npos || second == std::string::npos) {
        return std::nullopt;
    }

    world::ChunkCoord coord{};
    const auto parsePart = [](std::string_view text, std::int64_t& out) {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    };
    if (!parsePart(std::string_view{stem}.substr(0, first), coord.x)
        || !parsePart(std::string_view{stem}.substr(first + 1U, second - first - 1U), coord.y)
        || !parsePart(std::string_view{stem}.substr(second + 1U), coord.z)) {
        return std::nullopt;
    }
    return coord;
}

template <typename T>
void appendValue(std::vector<std::byte>& buffer, const T& value)
{
    const auto* src = reinterpret_cast<const std::byte*>(&value);
    buffer.insert(buffer.end(), src, src + sizeof(T));
}

template <typename T>
bool readValue(const std::vector<std::byte>& buffer, std::size_t& cursor, T& value)
{
    if (cursor + sizeof(T) > buffer.size()) {
        return false;
    }
    std::memcpy(&value, buffer.data() + cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

std::vector<std::byte> encodeBody(const world::Chunk& chunk)
{
    const auto& blockData = chunk.blockData();
    const auto& palette = blockData.palette;
    const auto& indices = blockData.indices;
    const auto& words = indices.words();

    std::vector<std::byte> body;
    body.reserve(2U + palette.size() * sizeof(std::uint32_t) + 1U + 4U + words.size() * sizeof(std::uint64_t));

    const auto paletteCount = static_cast<std::uint16_t>(palette.size());
    appendValue(body, paletteCount);
    for (const auto& entry : palette.entries()) {
        appendValue(body, entry.value);
    }

    const auto bits = static_cast<std::uint8_t>(indices.bitsPerEntry());
    appendValue(body, bits);
    const auto wordCount = static_cast<std::uint32_t>(words.size());
    appendValue(body, wordCount);
    for (const auto& word : words) {
        appendValue(body, word);
    }
    return body;
}

bool decodeBody(const std::vector<std::byte>& body, world::Chunk& chunk)
{
    std::size_t cursor = 0;
    std::uint16_t paletteCount = 0;
    if (!readValue(body, cursor, paletteCount)) {
        return false;
    }
    std::vector<BlockStateId> entries;
    entries.reserve(paletteCount);
    for (std::uint16_t i = 0; i < paletteCount; ++i) {
        std::uint32_t value = 0;
        if (!readValue(body, cursor, value)) {
            return false;
        }
        if (value < 65536U && value != 0) {
            value = world::makeBlockState(BlockTypeId{value}).value;
        }
        entries.push_back(BlockStateId{value});
    }

    std::uint8_t bits = 0;
    std::uint32_t wordCount = 0;
    if (!readValue(body, cursor, bits) || !readValue(body, cursor, wordCount)) {
        return false;
    }
    std::vector<std::uint64_t> words(wordCount);
    for (std::uint32_t i = 0; i < wordCount; ++i) {
        if (!readValue(body, cursor, words[i])) {
            return false;
        }
    }

    world::Palette<BlockStateId, world::BlockStateIdHash> palette;
    palette.resetWith(std::move(entries));

    world::BitPackedArray indices;
    indices.resetWith(static_cast<std::size_t>(world::ChunkVolume), bits, std::move(words));

    chunk.resetFromStorage(std::move(palette), std::move(indices));
    return true;
}

} // namespace

RegionFileStore::RegionFileStore(std::filesystem::path root)
    : root_(std::move(root))
{
    rebuildChunkIndex();
}

bool RegionFileStore::zstdEnabled() noexcept
{
#if defined(VOXEL_HAS_ZSTD)
    return true;
#else
    return false;
#endif
}

void RegionFileStore::saveChunk(const world::Chunk& chunk)
{
    std::filesystem::create_directories(chunkDirectory(root_));

    const auto path = chunkPath(root_, chunk.coord());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open chunk save file: " + path.string());
    }

    auto body = encodeBody(chunk);
    std::uint16_t flags = 0;

#if defined(VOXEL_HAS_ZSTD)
    std::vector<std::byte> compressed(ZSTD_compressBound(body.size()));
    const auto compressedSize = ZSTD_compress(
        compressed.data(), compressed.size(),
        body.data(), body.size(), 3);
    if (!ZSTD_isError(compressedSize) && compressedSize < body.size()) {
        compressed.resize(compressedSize);
        body = std::move(compressed);
        flags |= FormatFlag::FlagZstdCompressed;
    }
#endif

    const auto coord = chunk.coord();
    const auto revision = chunk.revision();
    const auto terrainVersion = chunk.terrainVersion();
    const std::uint16_t version = kSaveFormatVersion;
    const auto bodySize = static_cast<std::uint32_t>(body.size());

    output.write(kChunkMagic.data(), static_cast<std::streamsize>(kChunkMagic.size()));
    output.write(reinterpret_cast<const char*>(&version), sizeof(version));
    output.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    output.write(reinterpret_cast<const char*>(&coord.x), sizeof(coord.x));
    output.write(reinterpret_cast<const char*>(&coord.y), sizeof(coord.y));
    output.write(reinterpret_cast<const char*>(&coord.z), sizeof(coord.z));
    output.write(reinterpret_cast<const char*>(&revision), sizeof(revision));
    output.write(reinterpret_cast<const char*>(&terrainVersion), sizeof(terrainVersion));
    output.write(reinterpret_cast<const char*>(&bodySize), sizeof(bodySize));
    output.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    knownChunkFiles_.insert(chunk.coord());
}

std::optional<world::Chunk> RegionFileStore::loadChunk(world::ChunkCoord coord)
{
    if (knownChunkFiles_.find(coord) == knownChunkFiles_.end()) {
        return std::nullopt;
    }

    return loadChunkFromRoot(root_, coord);
}

std::optional<world::Chunk> RegionFileStore::loadChunkFromRoot(const std::filesystem::path& root, world::ChunkCoord coord)
{
    const auto path = chunkPath(root, coord);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    std::array<char, 4> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (magic != kChunkMagic) {
        Logger::warn("Save format mismatch (magic) for chunk " + path.string() + "; ignoring file.");
        return std::nullopt;
    }

    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    world::ChunkCoord storedCoord{};
    Revision revision = 0;
    std::uint64_t terrainVersion = 0;
    std::uint32_t bodySize = 0;

    input.read(reinterpret_cast<char*>(&version), sizeof(version));
    input.read(reinterpret_cast<char*>(&flags), sizeof(flags));
    input.read(reinterpret_cast<char*>(&storedCoord.x), sizeof(storedCoord.x));
    input.read(reinterpret_cast<char*>(&storedCoord.y), sizeof(storedCoord.y));
    input.read(reinterpret_cast<char*>(&storedCoord.z), sizeof(storedCoord.z));
    input.read(reinterpret_cast<char*>(&revision), sizeof(revision));
    if (!input) {
        return std::nullopt;
    }
    if (version < 1 || version > kSaveFormatVersion) {
        Logger::warn("Save format version " + std::to_string(version) + " is incompatible (supported 1.."
                     + std::to_string(kSaveFormatVersion) + "); ignoring file.");
        return std::nullopt;
    }
    if (!(storedCoord == coord)) {
        return std::nullopt;
    }
    if (version >= 2) {
        input.read(reinterpret_cast<char*>(&terrainVersion), sizeof(terrainVersion));
        if (!input) {
            return std::nullopt;
        }
    }
    input.read(reinterpret_cast<char*>(&bodySize), sizeof(bodySize));
    if (!input) {
        return std::nullopt;
    }

    std::vector<std::byte> body(bodySize);
    input.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(body.size()));
    if (!input) {
        return std::nullopt;
    }

    if (flags & FormatFlag::FlagZstdCompressed) {
#if defined(VOXEL_HAS_ZSTD)
        const auto rawSize = ZSTD_getFrameContentSize(body.data(), body.size());
        if (rawSize == ZSTD_CONTENTSIZE_ERROR || rawSize == ZSTD_CONTENTSIZE_UNKNOWN) {
            return std::nullopt;
        }
        std::vector<std::byte> raw(rawSize);
        const auto produced = ZSTD_decompress(raw.data(), raw.size(), body.data(), body.size());
        if (ZSTD_isError(produced)) {
            return std::nullopt;
        }
        body = std::move(raw);
#else
        Logger::warn("Chunk file is Zstd-compressed but engine built without Zstd support: " + path.string());
        return std::nullopt;
#endif
    }

    world::Chunk chunk(coord);
    if (!decodeBody(body, chunk)) {
        return std::nullopt;
    }
    chunk.markLoaded(revision, terrainVersion);
    return chunk;
}

void RegionFileStore::rebuildChunkIndex()
{
    knownChunkFiles_.clear();
    const auto dir = chunkDirectory(root_);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            continue;
        }
        if (const auto coord = coordFromChunkFilename(entry.path())) {
            knownChunkFiles_.insert(*coord);
        }
    }
}

} // namespace voxel::save
