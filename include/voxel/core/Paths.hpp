#pragma once

#include <filesystem>

namespace voxel::core {

class Paths {
public:
    explicit Paths(std::filesystem::path root = std::filesystem::current_path());

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::filesystem::path assetsRoot() const;
    [[nodiscard]] std::filesystem::path coreDataRoot() const;
    [[nodiscard]] std::filesystem::path saveRoot() const;

private:
    std::filesystem::path root_;
};

} // namespace voxel::core

