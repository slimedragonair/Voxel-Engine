#include <voxel/save/WorldRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
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
        out = nlohmann::json::parse(buffer.str(), nullptr, /*allow_exceptions*/ true, /*ignore_comments*/ true);
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
    // Trim trailing underscores so we don't end up with "my_world____".
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
    // std::random_device gives 32 bits on most implementations; mix two reads
    // so the seed exercises the full 64-bit space.
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

    world::WorldDescriptor descriptor{};
    if (auto it = doc.find("name"); it != doc.end() && it->is_string()) {
        descriptor.name = it->get<std::string>();
    } else {
        // Fall back to directory name so an orphaned world.json without a
        // "name" field still shows up as something the user can pick.
        descriptor.name = root.filename().string();
    }
    if (auto it = doc.find("seed"); it != doc.end() && it->is_number_unsigned()) {
        descriptor.seed = it->get<std::uint64_t>();
    } else if (auto it2 = doc.find("seed"); it2 != doc.end() && it2->is_number_integer()) {
        // JSON parsers sometimes treat large literals as signed; coerce.
        descriptor.seed = static_cast<std::uint64_t>(it2->get<std::int64_t>());
    }
    if (auto it = doc.find("createdAtMs"); it != doc.end() && it->is_number_integer()) {
        descriptor.createdAtMs = it->get<std::int64_t>();
    }
    if (auto it = doc.find("lastPlayedAtMs"); it != doc.end() && it->is_number_integer()) {
        descriptor.lastPlayedAtMs = it->get<std::int64_t>();
    }
    if (auto it = doc.find("formatVersion"); it != doc.end() && it->is_number_unsigned()) {
        descriptor.formatVersion = it->get<std::uint32_t>();
    }
    return descriptor;
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
        WorldEntry entry;
        entry.root = dirEntry.path();
        entry.descriptor = std::move(*descriptor);
        entries.push_back(std::move(entry));
    }
    // Most-recently-played first so the menu's top option is the natural pick.
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
    WorldEntry entry;
    entry.root = root;
    entry.descriptor = std::move(*descriptor);
    return entry;
}

std::optional<WorldEntry> WorldRegistry::renameWorld(const WorldEntry& entry,
                                                      std::string newDisplayName) const
{
    if (newDisplayName.empty()) {
        return std::nullopt;
    }
    // Read the on-disk descriptor first so we don't clobber fields the
    // caller's WorldEntry might be stale on (e.g. lastPlayedAtMs touched
    // by a concurrent shutdown of another instance).
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
    // remove_all returns the number of files removed; an error_code is set
    // on failure. We deliberately don't check the count — an empty
    // directory legitimately returns 0.
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

    // Pick a directory name that doesn't collide with an existing world.
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

    WorldEntry entry;
    entry.root = chosenDir;
    entry.descriptor.name = std::move(displayName);
    entry.descriptor.seed = seed;
    entry.descriptor.createdAtMs = nowUnixMs();
    entry.descriptor.lastPlayedAtMs = entry.descriptor.createdAtMs;
    entry.descriptor.formatVersion = world::WorldDescriptor::kFormatVersion;
    if (!writeDescriptor(entry.root, entry.descriptor)) {
        Logger::error("WorldRegistry: failed to write world.json under " + entry.root.string());
        return std::nullopt;
    }
    return entry;
}

} // namespace voxel::save
