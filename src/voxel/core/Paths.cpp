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

std::filesystem::path Paths::savesDirectory() const
{
    return root_ / "saves";
}

std::filesystem::path Paths::saveRoot() const
{
    if (!activeWorldRootOverride_.empty()) {
        return activeWorldRootOverride_;
    }
    return savesDirectory() / activeWorldDir_;
}

void Paths::setActiveWorldDirectory(std::string directoryName)
{
    activeWorldDir_ = std::move(directoryName);
    activeWorldRootOverride_.clear();
}

void Paths::setActiveWorldRoot(std::filesystem::path absoluteRoot)
{
    activeWorldRootOverride_ = std::move(absoluteRoot);
}

} // namespace voxel::core
