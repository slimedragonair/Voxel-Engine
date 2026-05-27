#include <voxel/data/RegistryLoader.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include <voxel/core/Logger.hpp>
#include <voxel/data/ItemRegistry.hpp>
#include <voxel/data/RecipeRegistry.hpp>

namespace voxel::data {

namespace {

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

Identifier parseIdentifier(const std::string& value)
{
    const auto separator = value.find(':');
    if (separator == std::string::npos) {
        return {"core", value};
    }
    return {value.substr(0, separator), value.substr(separator + 1)};
}

Identifier parseIdentifier(const nlohmann::json& j, const char* key, const Identifier& fallback = {})
{
    if (!j.contains(key) || !j[key].is_string()) {
        return fallback;
    }
    return parseIdentifier(j[key].get<std::string>());
}

render::meshing::MeshSurface parseRenderSurface(const std::string& value, bool opaque)
{
    if (value == "transparent") {
        return render::meshing::MeshSurface::Transparent;
    }
    if (value == "cutout") {
        return render::meshing::MeshSurface::Cutout;
    }
    return opaque ? render::meshing::MeshSurface::Opaque : render::meshing::MeshSurface::Cutout;
}

automation::KineticRole parseKineticRole(const std::string& value)
{
    if (value == "source") {
        return automation::KineticRole::Source;
    }
    if (value == "consumer") {
        return automation::KineticRole::Consumer;
    }
    return automation::KineticRole::Transfer;
}

automation::KineticBlockDefinition parseKinetic(const nlohmann::json& j)
{
    if (!j.contains("kinetic") || !j["kinetic"].is_object()) {
        return {};
    }
    const auto& k = j["kinetic"];
    automation::KineticBlockDefinition def;
    def.enabled = k.value("enabled", true);
    def.role = parseKineticRole(k.value("role", std::string{"transfer"}));
    def.sourceRpm = k.value("source_rpm", 0.0F);
    def.stressCapacity = k.value("stress_capacity", 0.0F);
    def.stressDemand = k.value("stress_demand", 0.0F);
    def.gearRatio = k.value("gear_ratio", 1.0F);
    def.breaksOnOverload = k.value("breaks_on_overload", true);
    def.axisAware = k.value("axis_aware", false);
    return def;
}

BlockStateSchema parseStateSchema(const nlohmann::json& j)
{
    BlockStateSchema schema;
    if (!j.contains("states") || !j["states"].is_object()) {
        return schema;
    }
    const auto& states = j["states"];
    for (const auto& [key, value] : states.items()) {
        BlockPropertyDef propDef;
        propDef.name = key;
        if (key == "axis") {
            schema.hasAxis = true;
        } else if (key == "powered") {
            schema.hasPowered = true;
        } else if (key == "waterlogged") {
            schema.hasWaterlogged = true;
        }
        if (value.is_array()) {
            for (const auto& v : value) {
                if (v.is_string()) {
                    propDef.values.push_back(v.get<std::string>());
                } else if (v.is_boolean()) {
                    propDef.values.push_back(v.get<bool>() ? "true" : "false");
                }
            }
        }
        schema.properties.push_back(std::move(propDef));
    }
    return schema;
}

std::vector<std::string> parseStringArray(const nlohmann::json& j, const char* key)
{
    std::vector<std::string> result;
    if (!j.contains(key) || !j[key].is_array()) {
        return result;
    }
    for (const auto& item : j[key]) {
        if (item.is_string()) {
            result.push_back(item.get<std::string>());
        }
    }
    return result;
}

CraftingTier parseCraftingTier(const std::string& value)
{
    if (value == "workstation") {
        return CraftingTier::Workstation;
    }
    if (value == "machine") {
        return CraftingTier::Machine;
    }
    return CraftingTier::Hand;
}

} // namespace

LoadResult RegistryLoader::loadCoreData(const std::filesystem::path& root,
                                        BlockRegistry& blocks,
                                        ItemRegistry& items,
                                        RecipeRegistry& recipes)
{
    LoadResult result;
    const auto blocksPath = root / "blocks.json";
    const auto itemsPath = root / "items.json";
    const auto recipesPath = root / "recipes.json";

    auto blockResult = loadBlocks(blocksPath, blocks);
    if (!blockResult.ok()) {
        result.error = blockResult.error;
        return result;
    }
    result.blocksLoaded = true;
    result.blockCount = blockResult.blockCount;

    if (std::filesystem::exists(itemsPath)) {
        auto itemResult = loadItems(itemsPath, items);
        if (!itemResult.ok()) {
            Logger::warn("Item data load failed: " + itemResult.error);
        } else {
            result.itemsLoaded = true;
            result.itemCount = itemResult.itemCount;
        }
    }

    if (std::filesystem::exists(recipesPath)) {
        auto recipeResult = loadRecipes(recipesPath, recipes);
        if (!recipeResult.ok()) {
            Logger::warn("Recipe data load failed: " + recipeResult.error);
        } else {
            result.recipesLoaded = true;
            result.recipeCount = recipeResult.recipeCount;
        }
    }

    return result;
}

LoadResult RegistryLoader::loadBlocks(const std::filesystem::path& path, BlockRegistry& blocks)
{
    LoadResult result;
    const auto text = readFile(path);
    if (text.empty()) {
        result.error = "Failed to read blocks file: " + path.string();
        return result;
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        result.error = "JSON parse error in " + path.string() + ": " + e.what();
        return result;
    }

    if (!doc.contains("registries") || !doc["registries"].is_object()) {
        result.error = "Missing 'registries' object in " + path.string();
        return result;
    }

    const auto& registries = doc["registries"];
    if (!registries.contains("blocks") || !registries["blocks"].is_array()) {
        result.error = "Missing 'blocks' array in " + path.string();
        return result;
    }

    for (const auto& entry : registries["blocks"]) {
        if (!entry.contains("id") || !entry["id"].is_string()) {
            Logger::warn("Skipping block entry with missing id in " + path.string());
            continue;
        }

        BlockDefinition def;
        def.id = parseIdentifier(entry["id"].get<std::string>());
        def.displayName = entry.value("display_name", def.id.path);
        def.solid = entry.value("solid", true);
        def.opaque = entry.value("opaque", true);
        def.renderSurface = parseRenderSurface(entry.value("render_surface", std::string{"opaque"}), def.opaque);
        def.hasBlockEntity = entry.value("has_block_entity", false);
        def.blockEntityType = entry.value("block_entity_type", std::string{});
        def.inventorySlots = entry.value("inventory_slots", 0);
        def.recipeCategory = entry.value("recipe_category", std::string{});
        def.tags = parseStringArray(entry, "tags");
        def.stateSchema = parseStateSchema(entry);
        def.kinetic = parseKinetic(entry);
        def.lightEmission = static_cast<std::uint8_t>(std::clamp(entry.value("light_emission", 0), 0, 15));
        {
            const int defaultAttenuation = def.opaque ? 15 : 0;
            def.lightAttenuation = static_cast<std::uint8_t>(std::clamp(entry.value("light_attenuation", defaultAttenuation), 0, 15));
        }

        blocks.registerBlock(std::move(def));
    }

    result.blocksLoaded = true;
    result.blockCount = blocks.registry().size();
    Logger::info("Loaded " + std::to_string(result.blockCount) + " blocks from " + path.string());
    return result;
}

LoadResult RegistryLoader::loadItems(const std::filesystem::path& path, ItemRegistry& items)
{
    LoadResult result;
    const auto text = readFile(path);
    if (text.empty()) {
        result.error = "Failed to read items file: " + path.string();
        return result;
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        result.error = "JSON parse error in " + path.string() + ": " + e.what();
        return result;
    }

    if (!doc.contains("registries") || !doc["registries"].is_object()) {
        result.error = "Missing 'registries' object in " + path.string();
        return result;
    }

    const auto& registries = doc["registries"];
    if (!registries.contains("items") || !registries["items"].is_array()) {
        result.error = "Missing 'items' array in " + path.string();
        return result;
    }

    for (const auto& entry : registries["items"]) {
        if (!entry.contains("id") || !entry["id"].is_string()) {
            Logger::warn("Skipping item entry with missing id in " + path.string());
            continue;
        }

        ItemDefinition def;
        def.id = parseIdentifier(entry["id"].get<std::string>());
        def.displayName = entry.value("display_name", def.id.path);
        def.maxStackSize = static_cast<std::uint32_t>(std::clamp(entry.value("max_stack_size", 64), 1, 999));
        def.tags = parseStringArray(entry, "tags");
        def.toolType = entry.value("tool_type", std::string{});
        def.durability = static_cast<std::uint16_t>(entry.value("durability", 0));
        def.blockPlacementId = parseIdentifier(entry, "block_placement_id");

        items.registerItem(std::move(def));
    }

    result.itemsLoaded = true;
    result.itemCount = items.registry().size();
    Logger::info("Loaded " + std::to_string(result.itemCount) + " items from " + path.string());
    return result;
}

LoadResult RegistryLoader::loadRecipes(const std::filesystem::path& path, RecipeRegistry& recipes)
{
    LoadResult result;
    const auto text = readFile(path);
    if (text.empty()) {
        result.error = "Failed to read recipes file: " + path.string();
        return result;
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        result.error = "JSON parse error in " + path.string() + ": " + e.what();
        return result;
    }

    if (!doc.contains("registries") || !doc["registries"].is_object()) {
        result.error = "Missing 'registries' object in " + path.string();
        return result;
    }

    const auto& registries = doc["registries"];
    if (!registries.contains("recipes") || !registries["recipes"].is_array()) {
        result.error = "Missing 'recipes' array in " + path.string();
        return result;
    }

    for (const auto& entry : registries["recipes"]) {
        if (!entry.contains("id") || !entry["id"].is_string()) {
            Logger::warn("Skipping recipe entry with missing id in " + path.string());
            continue;
        }

        RecipeDefinition def;
        def.id = parseIdentifier(entry["id"].get<std::string>());
        def.displayName = entry.value("display_name", def.id.path);
        def.durationTicks = static_cast<std::uint32_t>(std::max(entry.value("duration_ticks", 1), 1));
        def.machineCategory = entry.value("machine_category", std::string{});
        def.tier = parseCraftingTier(entry.value("tier", std::string{"hand"}));
        def.hidden = entry.value("hidden", false);

        if (entry.contains("inputs") && entry["inputs"].is_array()) {
            for (const auto& input : entry["inputs"]) {
                RecipeIngredient ingredient;
                ingredient.itemId = parseIdentifier(input, "item");
                ingredient.count = static_cast<std::uint16_t>(std::max(input.value("count", 1), 1));
                def.inputs.push_back(std::move(ingredient));
            }
        }

        if (entry.contains("outputs") && entry["outputs"].is_array()) {
            for (const auto& output : entry["outputs"]) {
                RecipeOutput out;
                out.itemId = parseIdentifier(output, "item");
                out.count = static_cast<std::uint16_t>(std::max(output.value("count", 1), 1));
                def.outputs.push_back(std::move(out));
            }
        }

        recipes.registerRecipe(std::move(def));
    }

    result.recipesLoaded = true;
    result.recipeCount = recipes.registry().size();
    Logger::info("Loaded " + std::to_string(result.recipeCount) + " recipes from " + path.string());
    return result;
}

} // namespace voxel::data
