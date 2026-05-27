#include <voxel/world/TerrainDefinitionRegistry.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <voxel/core/Logger.hpp>

namespace voxel::world {

namespace {

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] bool validId(const std::string& id) noexcept
{
    return !id.empty() && id.find(' ') == std::string::npos;
}

[[nodiscard]] std::optional<TerrainSignalRange> parseRange(const nlohmann::json& value)
{
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
        return std::nullopt;
    }
    TerrainSignalRange range{
        value[0].get<float>(),
        value[1].get<float>(),
    };
    if (range.min > range.max) {
        std::swap(range.min, range.max);
    }
    return range;
}

[[nodiscard]] std::vector<std::string> parseStringArray(const nlohmann::json& j, const char* key)
{
    std::vector<std::string> values;
    if (!j.contains(key) || !j[key].is_array()) {
        return values;
    }
    for (const auto& entry : j[key]) {
        if (entry.is_string()) {
            values.push_back(entry.get<std::string>());
        }
    }
    return values;
}

[[nodiscard]] float rangeScore(float value, TerrainSignalRange range) noexcept
{
    const float width = std::max(0.0001F, range.max - range.min);
    const float center = range.min + width * 0.5F;
    const float normalizedDistance = std::clamp(std::abs(value - center) / (width * 0.5F), 0.0F, 1.0F);
    return 1.0F - normalizedDistance;
}

[[nodiscard]] std::optional<float> signalValue(
    const TerrainBiomeSignalSample& signals,
    const std::string& name) noexcept
{
    if (name == "continentalness") return signals.continentalness;
    if (name == "tectonicStress") return signals.tectonicStress;
    if (name == "erosion") return signals.erosion;
    if (name == "weirdness") return signals.weirdness;
    if (name == "temperature") return signals.temperature;
    if (name == "moisture" || name == "humidity") return signals.moisture;
    if (name == "volcanism") return signals.volcanism;
    if (name == "manaField") return signals.manaField;
    if (name == "polarInfluence") return signals.polarInfluence;
    if (name == "oceanDepthBias") return signals.oceanDepthBias;
    if (name == "height") return signals.height;
    if (name == "depth") return signals.depth;
    if (name == "oceanDepth") return signals.oceanDepth;
    return std::nullopt;
}

[[nodiscard]] float scoreBiome(
    const TerrainBiomeDefinition& biome,
    const TerrainBiomeSignalSample& signals) noexcept
{
    if (biome.spawnWeight <= 0.0F || biome.conditions.empty()) {
        return -1.0F;
    }

    float conditionScore = 0.0F;
    for (const auto& [signalName, range] : biome.conditions) {
        const auto value = signalValue(signals, signalName);
        if (!value.has_value() || !range.contains(*value)) {
            return -1.0F;
        }
        conditionScore += rangeScore(*value, range);
    }

    const float averageConditionScore = conditionScore / static_cast<float>(biome.conditions.size());
    return biome.spawnWeight * (0.5F + averageConditionScore * 0.5F);
}

std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    return seed;
}

std::uint64_t hashString(std::uint64_t seed, const std::string& value) noexcept
{
    for (const char ch : value) {
        seed ^= static_cast<std::uint8_t>(ch);
        seed *= 0x100000001b3ULL;
    }
    return seed;
}

std::uint64_t floatBits(float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

} // namespace

bool TerrainDefinitionRegistry::registerHeightProfile(TerrainHeightProfileDefinition definition)
{
    if (!validId(definition.id) || heightProfileIndex_.contains(definition.id)) {
        return false;
    }
    if (definition.minHeight > definition.maxHeight) {
        std::swap(definition.minHeight, definition.maxHeight);
    }
    const auto index = heightProfiles_.size();
    heightProfileIndex_.emplace(definition.id, index);
    heightProfiles_.push_back(std::move(definition));
    return true;
}

bool TerrainDefinitionRegistry::registerBiome(TerrainBiomeDefinition definition)
{
    if (!validId(definition.id) || biomeIndex_.contains(definition.id)) {
        return false;
    }
    const auto index = biomes_.size();
    biomeIndex_.emplace(definition.id, index);
    biomes_.push_back(std::move(definition));
    return true;
}

const TerrainHeightProfileDefinition* TerrainDefinitionRegistry::findHeightProfile(const std::string& id) const noexcept
{
    const auto it = heightProfileIndex_.find(id);
    if (it == heightProfileIndex_.end()) {
        return nullptr;
    }
    return &heightProfiles_[it->second];
}

const TerrainBiomeDefinition* TerrainDefinitionRegistry::findBiome(const std::string& id) const noexcept
{
    const auto it = biomeIndex_.find(id);
    if (it == biomeIndex_.end()) {
        return nullptr;
    }
    return &biomes_[it->second];
}

const TerrainBiomeDefinition* TerrainDefinitionRegistry::findBestBiome(
    const TerrainBiomeSignalSample& signals) const noexcept
{
    const TerrainBiomeDefinition* best = nullptr;
    float bestScore = -1.0F;
    for (const auto& biome : biomes_) {
        const float score = scoreBiome(biome, signals);
        if (score < 0.0F) {
            continue;
        }
        if (score > bestScore || (score == bestScore && best != nullptr && biome.id < best->id)) {
            best = &biome;
            bestScore = score;
        }
    }
    return best;
}

std::vector<TerrainBiomeCandidate> TerrainDefinitionRegistry::findBiomeCandidates(
    const std::unordered_map<std::string, float>& signals) const
{
    std::vector<TerrainBiomeCandidate> candidates;
    for (const auto& biome : biomes_) {
        if (biome.spawnWeight <= 0.0F || biome.conditions.empty()) {
            continue;
        }

        bool matches = true;
        float conditionScore = 0.0F;
        for (const auto& [signalName, range] : biome.conditions) {
            const auto signal = signals.find(signalName);
            if (signal == signals.end() || !range.contains(signal->second)) {
                matches = false;
                break;
            }
            conditionScore += rangeScore(signal->second, range);
        }

        if (!matches) {
            continue;
        }

        const float averageConditionScore = conditionScore / static_cast<float>(biome.conditions.size());
        candidates.push_back(TerrainBiomeCandidate{
            &biome,
            biome.spawnWeight * (0.5F + averageConditionScore * 0.5F),
        });
    }

    std::sort(candidates.begin(), candidates.end(), [](const TerrainBiomeCandidate& lhs, const TerrainBiomeCandidate& rhs) {
        if (lhs.score == rhs.score) {
            const std::string lhsId = lhs.biome != nullptr ? lhs.biome->id : std::string{};
            const std::string rhsId = rhs.biome != nullptr ? rhs.biome->id : std::string{};
            return lhsId < rhsId;
        }
        return lhs.score > rhs.score;
    });
    return candidates;
}

std::uint64_t TerrainDefinitionRegistry::contentHash() const noexcept
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto& profile : heightProfiles_) {
        hash = hashString(hash, profile.id);
        hash = hashString(hash, profile.terrainClass);
        hash = hashCombine(hash, floatBits(profile.minHeight));
        hash = hashCombine(hash, floatBits(profile.maxHeight));
        hash = hashCombine(hash, floatBits(profile.detailScale));
        hash = hashCombine(hash, floatBits(profile.ridgeScale));
        hash = hashCombine(hash, floatBits(profile.terraceSteps));
        hash = hashCombine(hash, floatBits(profile.terraceStrength));
    }
    for (const auto& biome : biomes_) {
        hash = hashString(hash, biome.id);
        hash = hashString(hash, biome.displayName);
        hash = hashString(hash, biome.heightProfile);
        hash = hashCombine(hash, floatBits(biome.spawnWeight));
        std::vector<const std::pair<const std::string, TerrainSignalRange>*> sortedConditions;
        sortedConditions.reserve(biome.conditions.size());
        for (const auto& condition : biome.conditions) {
            sortedConditions.push_back(&condition);
        }
        std::sort(sortedConditions.begin(), sortedConditions.end(),
            [](const auto* lhs, const auto* rhs) {
                return lhs->first < rhs->first;
            });
        for (const auto* condition : sortedConditions) {
            const auto& name = condition->first;
            const auto& range = condition->second;
            hash = hashString(hash, name);
            hash = hashCombine(hash, floatBits(range.min));
            hash = hashCombine(hash, floatBits(range.max));
        }
        for (const auto& block : biome.surfaceBlocks) {
            hash = hashString(hash, block);
        }
        for (const auto& vegetation : biome.vegetation) {
            hash = hashString(hash, vegetation);
        }
    }
    return hash;
}

const std::vector<TerrainHeightProfileDefinition>& TerrainDefinitionRegistry::heightProfiles() const noexcept
{
    return heightProfiles_;
}

const std::vector<TerrainBiomeDefinition>& TerrainDefinitionRegistry::biomes() const noexcept
{
    return biomes_;
}

std::size_t TerrainDefinitionRegistry::heightProfileCount() const noexcept
{
    return heightProfiles_.size();
}

std::size_t TerrainDefinitionRegistry::biomeCount() const noexcept
{
    return biomes_.size();
}

void TerrainDefinitionRegistry::clear()
{
    heightProfiles_.clear();
    biomes_.clear();
    heightProfileIndex_.clear();
    biomeIndex_.clear();
}

TerrainDefinitionLoadResult TerrainDefinitionLoader::load(
    const std::filesystem::path& path,
    TerrainDefinitionRegistry& registry) const
{
    TerrainDefinitionLoadResult result;
    const auto text = readFile(path);
    if (text.empty()) {
        result.error = "Failed to read terrain definition file: " + path.string();
        return result;
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        result.error = "JSON parse error in " + path.string() + ": " + e.what();
        return result;
    }

    if (!doc.contains("registries") || !doc["registries"].is_object()) {
        result.error = "Missing 'registries' object in " + path.string();
        return result;
    }
    const auto& registries = doc["registries"];
    if (!registries.contains("height_profiles") || !registries["height_profiles"].is_array()) {
        result.error = "Missing 'height_profiles' array in " + path.string();
        return result;
    }
    if (!registries.contains("biomes") || !registries["biomes"].is_array()) {
        result.error = "Missing 'biomes' array in " + path.string();
        return result;
    }

    TerrainDefinitionRegistry parsed;
    for (const auto& entry : registries["height_profiles"]) {
        if (!entry.contains("id") || !entry["id"].is_string()) {
            Logger::warn("Skipping terrain height profile with missing id in " + path.string());
            continue;
        }

        TerrainHeightProfileDefinition profile;
        profile.id = entry["id"].get<std::string>();
        profile.terrainClass = entry.value("terrain_class", std::string{});
        profile.minHeight = entry.value("min_height", 0.0F);
        profile.maxHeight = entry.value("max_height", 0.0F);
        profile.detailScale = entry.value("detail_scale", 1.0F);
        profile.ridgeScale = entry.value("ridge_scale", 0.0F);
        profile.terraceSteps = entry.value("terrace_steps", 0.0F);
        profile.terraceStrength = entry.value("terrace_strength", 0.0F);

        if (!parsed.registerHeightProfile(std::move(profile))) {
            Logger::warn("Skipping duplicate or invalid terrain height profile in " + path.string());
        }
    }

    for (const auto& entry : registries["biomes"]) {
        if (!entry.contains("id") || !entry["id"].is_string()) {
            Logger::warn("Skipping biome with missing id in " + path.string());
            continue;
        }

        TerrainBiomeDefinition biome;
        biome.id = entry["id"].get<std::string>();
        biome.displayName = entry.value("display_name", biome.id);
        biome.heightProfile = entry.value("height_profile", std::string{});
        biome.surfaceBlocks = parseStringArray(entry, "surface_blocks");
        biome.vegetation = parseStringArray(entry, "vegetation");
        biome.spawnWeight = std::max(0.0F, entry.value("spawn_weight", 1.0F));

        if (entry.contains("conditions") && entry["conditions"].is_object()) {
            for (const auto& [key, value] : entry["conditions"].items()) {
                if (auto range = parseRange(value)) {
                    biome.conditions.emplace(key, *range);
                } else {
                    Logger::warn("Skipping invalid condition '" + key + "' for biome " + biome.id);
                }
            }
        }

        if (parsed.findHeightProfile(biome.heightProfile) == nullptr) {
            result.error = "Biome '" + biome.id + "' references missing height profile '" + biome.heightProfile + "'";
            return result;
        }
        if (!parsed.registerBiome(std::move(biome))) {
            Logger::warn("Skipping duplicate or invalid terrain biome in " + path.string());
        }
    }

    registry = std::move(parsed);
    result.loaded = true;
    result.heightProfileCount = registry.heightProfileCount();
    result.biomeCount = registry.biomeCount();
    Logger::info(
        "Loaded " + std::to_string(result.biomeCount)
        + " terrain biomes and " + std::to_string(result.heightProfileCount)
        + " height profiles from " + path.string());
    return result;
}

} // namespace voxel::world
