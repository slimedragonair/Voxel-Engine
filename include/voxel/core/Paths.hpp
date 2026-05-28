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

private:
    std::filesystem::path root_;
    std::string activeWorldDir_{"dev_world"};
    std::filesystem::path activeWorldRootOverride_{};
};

} // namespace voxel::core
