#include <voxel/save/WorldRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include <voxel/core/Logger.hpp>

namespace voxel::save {

using ::voxel::Logger;

namespace {

constexpr const char* kDescriptorFile = "world.json";

bool readJsonFile(const std::filesystem::path& path, nlohmann::json& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    try {
        out = nlohmann::json::parse(buffer.str(), nullptr, true, /*ignore_comments*/ true);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    return true;
}

} // namespace

WorldRegistry::WorldRegistry(std::filesystem::path savesDirectory)
    : savesDirectory_(std::move(savesDirectory))
{
}

std::string WorldRegistry::slugify(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        } else if (c == '-' || c == '_') {
            out.push_back(c);
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "world";
    }
    return out;
}

std::uint64_t WorldRegistry::generateRandomSeed()
{
    std::random_device rd;
    std::uniform_int_distribution<std::uint64_t> dist(1ULL, std::numeric_limits<std::uint64_t>::max());
    const std::uint64_t hi = dist(rd);
    const std::uint64_t lo = dist(rd);
    return (hi << 32U) ^ lo;
}

std::int64_t WorldRegistry::nowUnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::optional<world::WorldDescriptor> WorldRegistry::readDescriptor(const std::filesystem::path& root) const
{
    nlohmann::json doc;
    if (!readJsonFile(root / kDescriptorFile, doc)) {
        return std::nullopt;
    }
    if (!doc.is_object()) {
        return std::nullopt;
    }
    world::WorldDescriptor d{};
    if (auto it = doc.find("name"); it != doc.end() && it->is_string()) {
        d.name = it->get<std::string>();
    } else {
        d.name = root.filename().string();
    }
    if (auto it = doc.find("seed"); it != doc.end()) {
        if (it->is_number_unsigned()) {
            d.seed = it->get<std::uint64_t>();
        } else if (it->is_number_integer()) {
            d.seed = static_cast<std::uint64_t>(it->get<std::int64_t>());
        }
    }
    if (auto it = doc.find("createdAtMs"); it != doc.end() && it->is_number_integer()) {
        d.createdAtMs = it->get<std::int64_t>();
    }
    if (auto it = doc.find("lastPlayedAtMs"); it != doc.end() && it->is_number_integer()) {
        d.lastPlayedAtMs = it->get<std::int64_t>();
    }
    if (auto it = doc.find("formatVersion"); it != doc.end() && it->is_number_unsigned()) {
        d.formatVersion = it->get<std::uint32_t>();
    }
    return d;
}

bool WorldRegistry::writeDescriptor(const std::filesystem::path& root,
                                    const world::WorldDescriptor& descriptor) const
{
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return false;
    }
    nlohmann::json doc{
        {"name", descriptor.name},
        {"seed", descriptor.seed},
        {"createdAtMs", descriptor.createdAtMs},
        {"lastPlayedAtMs", descriptor.lastPlayedAtMs},
        {"formatVersion", descriptor.formatVersion},
    };
    std::ofstream out(root / kDescriptorFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << doc.dump(2);
    return static_cast<bool>(out);
}

std::vector<WorldEntry> WorldRegistry::listWorlds() const
{
    std::vector<WorldEntry> entries;
    std::error_code ec;
    if (!std::filesystem::exists(savesDirectory_, ec) || !std::filesystem::is_directory(savesDirectory_, ec)) {
        return entries;
    }
    for (const auto& dirEntry : std::filesystem::directory_iterator(savesDirectory_, ec)) {
        if (ec) {
            break;
        }
        if (!dirEntry.is_directory()) {
            continue;
        }
        auto descriptor = readDescriptor(dirEntry.path());
        if (!descriptor.has_value()) {
            continue;
        }
        WorldEntry e;
        e.root = dirEntry.path();
        e.descriptor = std::move(*descriptor);
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(), [](const WorldEntry& a, const WorldEntry& b) {
        return a.descriptor.lastPlayedAtMs > b.descriptor.lastPlayedAtMs;
    });
    return entries;
}

std::optional<WorldEntry> WorldRegistry::findByName(std::string_view name) const
{
    for (auto& entry : listWorlds()) {
        if (entry.descriptor.name == name) {
            return entry;
        }
    }
    return std::nullopt;
}

std::optional<WorldEntry> WorldRegistry::findByDirectory(const std::filesystem::path& root) const
{
    auto descriptor = readDescriptor(root);
    if (!descriptor.has_value()) {
        return std::nullopt;
    }
    WorldEntry e;
    e.root = root;
    e.descriptor = std::move(*descriptor);
    return e;
}

std::optional<WorldEntry> WorldRegistry::renameWorld(const WorldEntry& entry,
                                                      std::string newDisplayName) const
{
    if (newDisplayName.empty()) {
        return std::nullopt;
    }
    auto descriptor = readDescriptor(entry.root);
    if (!descriptor.has_value()) {
        return std::nullopt;
    }
    descriptor->name = std::move(newDisplayName);
    if (!writeDescriptor(entry.root, *descriptor)) {
        return std::nullopt;
    }
    WorldEntry updated;
    updated.root = entry.root;
    updated.descriptor = std::move(*descriptor);
    return updated;
}

bool WorldRegistry::deleteWorld(const WorldEntry& entry) const
{
    std::error_code ec;
    if (!std::filesystem::exists(entry.root, ec)) {
        return true;
    }
    std::filesystem::remove_all(entry.root, ec);
    if (ec) {
        Logger::error("WorldRegistry: failed to delete \"" + entry.root.string() + "\": " + ec.message());
        return false;
    }
    return true;
}

std::optional<WorldEntry> WorldRegistry::createWorld(std::string displayName, std::uint64_t seed)
{
    if (displayName.empty()) {
        displayName = "New World";
    }
    if (seed == 0) {
        seed = generateRandomSeed();
    }
    const auto baseSlug = slugify(displayName);
    auto chosenDir = savesDirectory_ / baseSlug;
    std::error_code ec;
    for (int suffix = 2; std::filesystem::exists(chosenDir, ec); ++suffix) {
        chosenDir = savesDirectory_ / (baseSlug + "_" + std::to_string(suffix));
        if (suffix > 1000) {
            Logger::error("WorldRegistry: too many world directories share slug \"" + baseSlug + "\"");
            return std::nullopt;
        }
    }
    if (!std::filesystem::create_directories(chosenDir, ec) || ec) {
        if (!std::filesystem::exists(chosenDir, ec)) {
            Logger::error("WorldRegistry: failed to create world directory: " + chosenDir.string());
            return std::nullopt;
        }
    }
    WorldEntry e;
    e.root = chosenDir;
    e.descriptor.name = std::move(displayName);
    e.descriptor.seed = seed;
    e.descriptor.createdAtMs = nowUnixMs();
    e.descriptor.lastPlayedAtMs = e.descriptor.createdAtMs;
    e.descriptor.formatVersion = world::WorldDescriptor::kFormatVersion;
    if (!writeDescriptor(e.root, e.descriptor)) {
        return std::nullopt;
    }
    return e;
}

} // namespace voxel::save
