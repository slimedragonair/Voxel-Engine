#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/data/Identifier.hpp>
#include <voxel/data/Registry.hpp>

namespace voxel::data {

enum class CraftingTier : std::uint8_t {
    Hand,
    Workstation,
    Machine
};

struct RecipeIngredient {
    Identifier itemId;
    std::uint16_t count{1};
};

struct RecipeOutput {
    Identifier itemId;
    std::uint16_t count{1};
};

struct RecipeDefinition {
    Identifier id;
    std::string displayName;
    std::vector<RecipeIngredient> inputs;
    std::vector<RecipeOutput> outputs;
    std::uint32_t durationTicks{1};
    std::string machineCategory;
    CraftingTier tier{CraftingTier::Hand};
    bool hidden{false};
};

class RecipeRegistry {
public:
    RecipeId registerRecipe(RecipeDefinition definition);
    [[nodiscard]] const RecipeDefinition* find(const Identifier& id) const;
    [[nodiscard]] const Registry<RecipeDefinition>& registry() const noexcept;
    [[nodiscard]] std::vector<const RecipeDefinition*> recipesForMachineCategory(const std::string& category) const;
    [[nodiscard]] std::vector<const RecipeDefinition*> recipesForTier(CraftingTier tier) const;

private:
    Registry<RecipeDefinition> registry_;
};

} // namespace voxel::data
