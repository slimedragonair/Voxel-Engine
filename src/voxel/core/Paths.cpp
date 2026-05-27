#include <voxel/core/Paths.hpp>

#include <utility>

namespace voxel::core {

Paths::Paths(std::filesystem::path root)
    : root_(std::move(root))
{
}

const std::filesystem::path& Paths::root() const noexcept
{
    return root_;
}

std::filesystem::path Paths::assetsRoot() const
{
    return root_ / "assets";
}

std::filesystem::path Paths::coreDataRoot() const
{
    return assetsRoot() / "data" / "core";
}

std::filesystem::path Paths::saveRoot() const
{
    return root_ / "saves" / "dev_world";
}

} // namespace voxel::core

