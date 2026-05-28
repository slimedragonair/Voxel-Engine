#include <voxel/core/Paths.hpp>

#include <optional>
#include <system_error>
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

std::filesystem::path Paths::resolveEngineRoot(const std::filesystem::path& executablePath)
{
    // The sentinel proves we've found a complete asset tree, not just an
    // empty `assets/` directory next to the exe.
    constexpr const char* kSentinel = "assets/data/core/blocks.json";

    const auto walkUp = [&](std::filesystem::path start) -> std::optional<std::filesystem::path> {
        std::error_code ec;
        start = std::filesystem::weakly_canonical(start, ec);
        if (ec) {
            return std::nullopt;
        }
        for (auto cursor = start; !cursor.empty(); cursor = cursor.parent_path()) {
            if (std::filesystem::exists(cursor / kSentinel, ec)) {
                return cursor;
            }
            // parent_path() of a drive root returns itself on Windows; bail
            // when we stop making progress.
            if (cursor.has_parent_path() && cursor.parent_path() == cursor) {
                break;
            }
        }
        return std::nullopt;
    };

    // Prefer the exe's location first — that's the one the Explorer launch
    // and any "copy the build directory somewhere" workflow share.
    if (!executablePath.empty()) {
        auto execDir = executablePath.parent_path();
        if (!execDir.empty()) {
            if (auto hit = walkUp(execDir)) {
                return *hit;
            }
        }
    }

    // Fall back to the current working directory — covers `cmake --build` +
    // `ctest` runs that pass `WORKING_DIRECTORY` at the project root.
    if (auto hit = walkUp(std::filesystem::current_path())) {
        return *hit;
    }

    // Last-ditch fallback: keep the legacy behaviour so we never produce an
    // empty/garbage root. The engine will log "Core data files missing" and
    // fall back to the built-in catalog.
    return std::filesystem::current_path();
}

} // namespace voxel::core
