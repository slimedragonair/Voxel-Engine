#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace voxel::core {

class Paths {
public:
    explicit Paths(std::filesystem::path root = std::filesystem::current_path());

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::filesystem::path assetsRoot() const;
    [[nodiscard]] std::filesystem::path coreDataRoot() const;

    // Parent directory holding every world's subdirectory.
    [[nodiscard]] std::filesystem::path savesDirectory() const;

    // Active world's root. Defaults to `<root>/saves/dev_world` so any caller
    // that doesn't know about multiple worlds (existing tests, simple
    // headless runs) still works the same as before.
    [[nodiscard]] std::filesystem::path saveRoot() const;

    // Override the active world. Pass the *directory name* (not the
    // human-readable name) — typically a slug produced by WorldRegistry.
    void setActiveWorldDirectory(std::string directoryName);
    [[nodiscard]] const std::string& activeWorldDirectory() const noexcept { return activeWorldDir_; }

    // Override with an absolute / arbitrary path. Used when the menu hands
    // us a world root that doesn't live directly under `savesDirectory()`.
    void setActiveWorldRoot(std::filesystem::path absoluteRoot);

private:
    std::filesystem::path root_;
    std::string activeWorldDir_{"dev_world"};
    std::filesystem::path activeWorldRootOverride_{};
};

} // namespace voxel::core

