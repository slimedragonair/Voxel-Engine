#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <voxel/world/WorldDescriptor.hpp>

namespace voxel::save {

// One enumerated world on disk. `root` is the directory we hand to the
// chunk/inventory/player-state save services.
struct WorldEntry {
    std::filesystem::path root;
    world::WorldDescriptor descriptor;
};

// Enumerates `<savesDirectory>/*/world.json` and creates new worlds in the
// same hierarchy. Pure file IO — no Vulkan or threading concerns.
//
// Naming: world directories are derived from the world's display name by
// lowercasing + replacing anything outside [a-z0-9_-] with `_`. Two worlds
// with names that would collapse to the same slug get suffixed `_2`, `_3`…
class WorldRegistry {
public:
    explicit WorldRegistry(std::filesystem::path savesDirectory);

    // Best-effort load. Skips directories with malformed/absent world.json.
    [[nodiscard]] std::vector<WorldEntry> listWorlds() const;
    [[nodiscard]] std::optional<WorldEntry> findByName(std::string_view name) const;
    [[nodiscard]] std::optional<WorldEntry> findByDirectory(const std::filesystem::path& root) const;

    // Creates a new world directory + descriptor and writes it to disk.
    // `seed = 0` means "pick a random seed via std::random_device". Returns
    // std::nullopt if the directory could not be created.
    [[nodiscard]] std::optional<WorldEntry> createWorld(std::string displayName, std::uint64_t seed);

    // Reads `<root>/world.json`. Returns nullopt on any error.
    [[nodiscard]] std::optional<world::WorldDescriptor> readDescriptor(const std::filesystem::path& root) const;

    // Writes `<root>/world.json`. Returns true on success.
    [[nodiscard]] bool writeDescriptor(const std::filesystem::path& root,
                                       const world::WorldDescriptor& descriptor) const;

    // Updates only the human-readable name in `<root>/world.json`. The
    // directory slug is intentionally *not* changed so existing chunk save
    // paths (deep under root) stay valid and the player doesn't lose their
    // world if they edit the name. Returns the updated entry on success.
    [[nodiscard]] std::optional<WorldEntry> renameWorld(const WorldEntry& entry,
                                                        std::string newDisplayName) const;

    // Recursively removes the world directory. Returns true if either the
    // directory was removed or it never existed.
    [[nodiscard]] bool deleteWorld(const WorldEntry& entry) const;

    [[nodiscard]] const std::filesystem::path& savesDirectory() const noexcept { return savesDirectory_; }

    // Helper: turn a display name into a filesystem-safe slug. Public so the
    // main menu / tests can use the same rules.
    [[nodiscard]] static std::string slugify(std::string_view name);

    // Helper: random uint64 from std::random_device, avoiding 0 (reserved as
    // "pick a random seed" sentinel by createWorld).
    [[nodiscard]] static std::uint64_t generateRandomSeed();

    // Helper: current Unix time in milliseconds.
    [[nodiscard]] static std::int64_t nowUnixMs();

private:
    std::filesystem::path savesDirectory_;
};

} // namespace voxel::save
