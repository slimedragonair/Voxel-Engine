#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <voxel/core/RuntimeStats.hpp>
#include <voxel/world/ChunkConstants.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::world {

struct TerrainColumnCoord {
    std::int64_t x{};
    std::int64_t z{};

    [[nodiscard]] friend bool operator==(TerrainColumnCoord lhs, TerrainColumnCoord rhs) noexcept
    {
        return lhs.x == rhs.x && lhs.z == rhs.z;
    }
};

struct TerrainColumnCoordHash {
    [[nodiscard]] std::size_t operator()(TerrainColumnCoord coord) const noexcept
    {
        const auto x = static_cast<std::uint64_t>(coord.x);
        const auto z = static_cast<std::uint64_t>(coord.z);
        return static_cast<std::size_t>((x * 73856093ULL) ^ (z * 83492791ULL));
    }
};

struct TerrainColumnKey {
    TerrainColumnCoord coord{};
    std::uint32_t seed{};
    std::uint64_t terrainVersion{};

    [[nodiscard]] friend bool operator==(const TerrainColumnKey& lhs, const TerrainColumnKey& rhs) noexcept
    {
        return lhs.coord == rhs.coord
            && lhs.seed == rhs.seed
            && lhs.terrainVersion == rhs.terrainVersion;
    }
};

struct TerrainColumnKeyHash {
    [[nodiscard]] std::size_t operator()(const TerrainColumnKey& key) const noexcept
    {
        std::size_t seed = TerrainColumnCoordHash{}(key.coord);
        seed ^= std::hash<std::uint32_t>{}(key.seed + 0x9e3779b9U + static_cast<std::uint32_t>(seed << 6U) + static_cast<std::uint32_t>(seed >> 2U));
        seed ^= std::hash<std::uint64_t>{}(key.terrainVersion + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
        return seed;
    }
};

enum class TerrainSurfaceKind : std::uint8_t {
    Land,
    Beach,
    ShallowOcean,
    Ocean,
    DeepOcean
};

enum class TerrainClass : std::uint8_t {
    OceanShelf,
    DeepOcean,
    Abyss,
    Plains,
    Highlands,
    Mountains,
    Plateau,
    Volcanic,
    Polar,
    Magic,
    ArcaneFracture
};

enum class TerrainBiomeId : std::uint8_t {
    DeepOcean,
    OceanAbyss,
    Ocean,
    WarmOcean,
    ColdOcean,
    Beach,
    Plains,
    Forest,
    DenseForest,
    RedwoodForest,
    LushHighlandsValley,
    Swamp,
    Desert,
    Savanna,
    Jungle,
    Badlands,
    Mountains,
    SnowyMountains,
    Tundra,
    IceCaps,
    Taiga,
    MagicalGrove,
    FloatingIslands,
    ElementalCrystalCave,
    VolcanicWastes,
    ArcaneFractureZone
};

struct ColumnWorldgenData {
    float continentalness{};
    float tectonicStress{};
    float erosion{};
    float peaksValleys{};
    float temperature{};
    float humidity{};
    float weirdness{};
    float volcanism{};
    float manaField{};
    float polarInfluence{};
    float oceanDepthBias{};
    float oceanDepth{};
    float biomeBlend{};
    std::int32_t surfaceY{};
    std::int32_t seaLevel{};
    TerrainBiomeId biome{TerrainBiomeId::Plains};
    TerrainClass terrainClass{TerrainClass::Plains};
    TerrainSurfaceKind surfaceKind{TerrainSurfaceKind::Land};
    bool isOcean{};
    bool isBeach{};
    bool isRiverCandidate{};
};

struct TerrainColumnPrepass {
    TerrainColumnKey key{};
    std::array<float, ChunkSize * ChunkSize> surfaceY{};
    std::array<std::int32_t, ChunkSize * ChunkSize> surfaceBlockY{};
    std::array<TerrainSurfaceKind, ChunkSize * ChunkSize> surfaceKind{};
    std::array<TerrainBiomeId, ChunkSize * ChunkSize> biome{};
    std::array<TerrainClass, ChunkSize * ChunkSize> terrainClass{};
    std::array<float, ChunkSize * ChunkSize> continentalness{};
    std::array<float, ChunkSize * ChunkSize> tectonicStress{};
    std::array<float, ChunkSize * ChunkSize> erosion{};
    std::array<float, ChunkSize * ChunkSize> peaksValleys{};
    std::array<float, ChunkSize * ChunkSize> temperature{};
    std::array<float, ChunkSize * ChunkSize> humidity{};
    std::array<float, ChunkSize * ChunkSize> weirdness{};
    std::array<float, ChunkSize * ChunkSize> volcanism{};
    std::array<float, ChunkSize * ChunkSize> manaField{};
    std::array<float, ChunkSize * ChunkSize> polarInfluence{};
    std::array<float, ChunkSize * ChunkSize> oceanDepthBias{};
    std::array<float, ChunkSize * ChunkSize> oceanDepth{};
    std::array<float, ChunkSize * ChunkSize> biomeBlend{};
    std::array<bool, ChunkSize * ChunkSize> seaMask{};
    std::array<bool, ChunkSize * ChunkSize> beachMask{};
    std::array<bool, ChunkSize * ChunkSize> riverCandidateMask{};
};

struct TerrainColumnPrepassCacheStats {
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t jobsCompleted{};
    core::RuntimeCounters::Timer prepassBuildTime{};
};

class TerrainColumnPrepassCache {
public:
    [[nodiscard]] std::optional<TerrainColumnPrepass> find(const TerrainColumnKey& key);
    [[nodiscard]] bool contains(const TerrainColumnKey& key) const;
    void insert(TerrainColumnPrepass prepass);

    [[nodiscard]] bool tryBeginJob(const TerrainColumnKey& key);
    void completeJob(TerrainColumnPrepass prepass, std::uint64_t buildTimeUs);
    void endJobWithoutStore(const TerrainColumnKey& key);

    [[nodiscard]] std::size_t entryCount() const;
    [[nodiscard]] std::size_t inFlightCount() const noexcept;
    [[nodiscard]] TerrainColumnPrepassCacheStats drainStats();

private:
    mutable std::mutex mutex_;
    std::unordered_map<TerrainColumnKey, TerrainColumnPrepass, TerrainColumnKeyHash> entries_;
    std::unordered_set<TerrainColumnKey, TerrainColumnKeyHash> inFlight_;
    std::atomic<std::size_t> inFlightCount_{0};
    TerrainColumnPrepassCacheStats stats_{};
};

} // namespace voxel::world
