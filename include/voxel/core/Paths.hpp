#pragma once

#include <filesystem>
#include <string>

namespace voxel::core {

class Paths {
public:
    explicit Paths(std::filesystem::path root = std::filesystem::current_path());

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::filesystem::path assetsRoot() const;
    [[nodiscard]] std::filesystem::path coreDataRoot() const;
    [[nodiscard]] std::filesystem::path savesDirectory() const;
    [[nodiscard]] std::filesystem::path saveRoot() const;

    void setActiveWorldDirectory(std::string directoryName);
    [[nodiscard]] const std::string& activeWorldDirectory() const noexcept { return activeWorldDir_; }
    void setActiveWorldRoot(std::filesystem::path absoluteRoot);

    // Resolve the engine root by walking upward from the executable's
    // directory (and from the current working directory as a fallback)
    // looking for `assets/data/core/blocks.json`. Returns the deepest
    // ancestor that contains it, or — if no sentinel is found — falls
    // back to `current_path()`. Lets the exe run identically from
    // Explorer (CWD = exe dir), from a shell at the project root, or
    // from anywhere the assets/ tree was copied.
    [[nodiscard]] static std::filesystem::path resolveEngineRoot(const std::filesystem::path& executablePath);

private:
    std::filesystem::path root_;
    std::string activeWorldDir_{"dev_world"};
    std::filesystem::path activeWorldRootOverride_{};
};

} // namespace voxel::core
