#include <voxel/data/RecipeRegistry.hpp>

namespace voxel::data {

RecipeId RecipeRegistry::registerRecipe(RecipeDefinition definition)
{
    return RecipeId{registry_.add(std::move(definition))};
}

const RecipeDefinition* RecipeRegistry::find(const Identifier& id) const
{
    return registry_.find(id);
}

const Registry<RecipeDefinition>& RecipeRegistry::registry() const noexcept
{
    return registry_;
}

std::vector<const RecipeDefinition*> RecipeRegistry::recipesForMachineCategory(const std::string& category) const
{
    std::vector<const RecipeDefinition*> result;
    for (const auto& recipe : registry_.entries()) {
        if (recipe.machineCategory == category) {
            result.push_back(&recipe);
        }
    }
    return result;
}

std::vector<const RecipeDefinition*> RecipeRegistry::recipesForTier(CraftingTier tier) const
{
    std::vector<const RecipeDefinition*> result;
    for (const auto& recipe : registry_.entries()) {
        if (recipe.tier == tier) {
            result.push_back(&recipe);
        }
    }
    return result;
}

} // namespace voxel::data
