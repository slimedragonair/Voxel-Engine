#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace voxel::world {

struct TerrainSignalRange {
    float min{0.0F};
    float max{0.0F};

    [[nodiscard]] bool contains(float value) const noexcept
    {
        return value >= min && value <= max;
    }
};

struct TerrainHeightProfileDefinition {
    std::string id;
    std::string terrainClass;
    float minHeight{0.0F};
    float maxHeight{0.0F};
    float detailScale{1.0F};
    float ridgeScale{0.0F};
    float terraceSteps{0.0F};
    float terraceStrength{0.0F};
};

struct TerrainBiomeDefinition {
    std::string id;
    std::string displayName;
    std::string heightProfile;
    std::unordered_map<std::string, TerrainSignalRange> conditions;
    std::vector<std::string> surfaceBlocks;
    std::vector<std::string> vegetation;
    float spawnWeight{1.0F};
};

struct TerrainBiomeCandidate {
    const TerrainBiomeDefinition* biome{};
    float score{};
};

struct TerrainBiomeSignalSample {
    float continentalness{};
    float tectonicStress{};
    float erosion{};
    float weirdness{};
    float temperature{};
    float moisture{};
    float volcanism{};
    float manaField{};
    float polarInfluence{};
    float oceanDepthBias{};
    float height{};
    float depth{};
    float oceanDepth{};
};

class TerrainDefinitionRegistry {
public:
    bool registerHeightProfile(TerrainHeightProfileDefinition definition);
    bool registerBiome(TerrainBiomeDefinition definition);

    [[nodiscard]] const TerrainHeightProfileDefinition* findHeightProfile(const std::string& id) const noexcept;
    [[nodiscard]] const TerrainBiomeDefinition* findBiome(const std::string& id) const noexcept;
    [[nodiscard]] const TerrainBiomeDefinition* findBestBiome(
        const TerrainBiomeSignalSample& signals) const noexcept;
    [[nodiscard]] std::vector<TerrainBiomeCandidate> findBiomeCandidates(
        const std::unordered_map<std::string, float>& signals) const;
    [[nodiscard]] std::uint64_t contentHash() const noexcept;

    [[nodiscard]] const std::vector<TerrainHeightProfileDefinition>& heightProfiles() const noexcept;
    [[nodiscard]] const std::vector<TerrainBiomeDefinition>& biomes() const noexcept;

    [[nodiscard]] std::size_t heightProfileCount() const noexcept;
    [[nodiscard]] std::size_t biomeCount() const noexcept;

    void clear();

private:
    std::vector<TerrainHeightProfileDefinition> heightProfiles_;
    std::vector<TerrainBiomeDefinition> biomes_;
    std::unordered_map<std::string, std::size_t> heightProfileIndex_;
    std::unordered_map<std::string, std::size_t> biomeIndex_;
};

struct TerrainDefinitionLoadResult {
    bool loaded{false};
    std::size_t heightProfileCount{0};
    std::size_t biomeCount{0};
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

class TerrainDefinitionLoader {
public:
    TerrainDefinitionLoadResult load(const std::filesystem::path& path, TerrainDefinitionRegistry& registry) const;
};

} // namespace voxel::world
