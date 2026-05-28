#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <voxel/world/WorldDescriptor.hpp>

namespace voxel::save {

struct WorldEntry {
    std::filesystem::path root;
    world::WorldDescriptor descriptor;
};

// Enumerates `<savesDirectory>/*/world.json` and creates new worlds in the
// same hierarchy. Pure file IO — no Vulkan or threading concerns.
class WorldRegistry {
public:
    explicit WorldRegistry(std::filesystem::path savesDirectory);

    [[nodiscard]] std::vector<WorldEntry> listWorlds() const;
    [[nodiscard]] std::optional<WorldEntry> findByName(std::string_view name) const;
    [[nodiscard]] std::optional<WorldEntry> findByDirectory(const std::filesystem::path& root) const;

    // `seed = 0` means "pick a random uint64 via std::random_device".
    [[nodiscard]] std::optional<WorldEntry> createWorld(std::string displayName, std::uint64_t seed);

    [[nodiscard]] std::optional<world::WorldDescriptor> readDescriptor(const std::filesystem::path& root) const;
    [[nodiscard]] bool writeDescriptor(const std::filesystem::path& root,
                                       const world::WorldDescriptor& descriptor) const;

    // M1: directory slug stays stable so chunk save paths aren't invalidated;
    // only the display name in world.json changes.
    [[nodiscard]] std::optional<WorldEntry> renameWorld(const WorldEntry& entry,
                                                        std::string newDisplayName) const;
    [[nodiscard]] bool deleteWorld(const WorldEntry& entry) const;

    [[nodiscard]] const std::filesystem::path& savesDirectory() const noexcept { return savesDirectory_; }

    [[nodiscard]] static std::string slugify(std::string_view name);
    [[nodiscard]] static std::uint64_t generateRandomSeed();
    [[nodiscard]] static std::int64_t nowUnixMs();

private:
    std::filesystem::path savesDirectory_;
};

} // namespace voxel::save
