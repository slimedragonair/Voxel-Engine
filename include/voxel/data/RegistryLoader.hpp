#pragma once

#include <filesystem>
#include <string>

#include <voxel/data/BlockRegistry.hpp>

namespace voxel::data {

struct LoadResult {
    bool blocksLoaded{false};
    bool itemsLoaded{false};
    bool recipesLoaded{false};
    std::size_t blockCount{0};
    std::size_t itemCount{0};
    std::size_t recipeCount{0};
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

class ItemRegistry;
class RecipeRegistry;

class RegistryLoader {
public:
    LoadResult loadCoreData(const std::filesystem::path& root,
                            BlockRegistry& blocks,
                            ItemRegistry& items,
                            RecipeRegistry& recipes);

    LoadResult loadBlocks(const std::filesystem::path& path, BlockRegistry& blocks);
    LoadResult loadItems(const std::filesystem::path& path, ItemRegistry& items);
    LoadResult loadRecipes(const std::filesystem::path& path, RecipeRegistry& recipes);
};

} // namespace voxel::data
