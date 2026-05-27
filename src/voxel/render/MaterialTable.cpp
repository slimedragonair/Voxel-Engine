#include <voxel/render/MaterialTable.hpp>

#include <voxel/data/BlockRegistry.hpp>

namespace voxel::render {

namespace {

enum PatternType : std::uint32_t {
    Noise = 0,
    OreSpots = 1,
    Grass = 2,
    WoodGrain = 3,
    EdgeHighlight = 4,
    Water = 5
};

MaterialGpuData makeMaterial(float r, float g, float b,
                             float sr, float sg, float sb,
                             float noiseScale, float noiseStrength,
                             float edgeWidth, PatternType pattern,
                             float emission = 0.0F,
                             float alpha = 1.0F)
{
    MaterialGpuData m{};
    m.baseColor[0] = r;
    m.baseColor[1] = g;
    m.baseColor[2] = b;
    m.baseColor[3] = alpha;
    m.secondaryColor[0] = sr;
    m.secondaryColor[1] = sg;
    m.secondaryColor[2] = sb;
    m.secondaryColor[3] = 0.0F;
    m.noiseParams[0] = noiseScale;
    m.noiseParams[1] = noiseStrength;
    m.noiseParams[2] = edgeWidth;
    m.noiseParams[3] = static_cast<float>(pattern);
    m.textureParams[0] = 0.0F;
    m.textureParams[1] = emission;
    m.textureParams[2] = 0.0F;
    m.textureParams[3] = 0.0F;
    return m;
}

} // namespace

std::vector<MaterialGpuData> MaterialTableBuilder::build(const data::BlockRegistry& registry)
{
    std::vector<MaterialGpuData> table(kMaxMaterials, MaterialGpuData{});

    // Index 0: debug fallback (magenta — should never be visible)
    table[0] = makeMaterial(1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F, 1.0F, 0.05F, Noise);

    // TypeId 1: air (never rendered, but fill for safety)
    table[1] = table[0];

    // TypeId 2: stone — subtle gray noise
    table[2] = makeMaterial(0.50F, 0.50F, 0.52F, 0.42F, 0.42F, 0.44F, 3.0F, 0.6F, 0.04F, Noise);

    // TypeId 3: glass — edge highlight, slightly blue-tinted
    table[3] = makeMaterial(0.85F, 0.92F, 0.95F, 1.0F, 1.0F, 1.0F, 1.0F, 0.1F, 0.08F, EdgeHighlight);

    // TypeId 4: copper_pipe — metallic edge highlight
    table[4] = makeMaterial(0.72F, 0.45F, 0.20F, 0.55F, 0.35F, 0.15F, 2.0F, 0.3F, 0.06F, EdgeHighlight);

    // TypeId 5: creative_motor — industrial noise
    table[5] = makeMaterial(0.35F, 0.35F, 0.40F, 0.80F, 0.60F, 0.10F, 4.0F, 0.4F, 0.05F, Noise);

    // TypeId 6: wooden_gear — wood grain
    table[6] = makeMaterial(0.55F, 0.38F, 0.20F, 0.45F, 0.30F, 0.15F, 2.0F, 0.5F, 0.03F, WoodGrain);

    // TypeId 7: mechanical_press — iron plate edge highlight
    table[7] = makeMaterial(0.52F, 0.52F, 0.55F, 0.35F, 0.35F, 0.38F, 2.0F, 0.3F, 0.06F, EdgeHighlight);

    // TypeId 8: dirt — clumpy brown noise
    table[8] = makeMaterial(0.45F, 0.30F, 0.18F, 0.38F, 0.24F, 0.14F, 4.0F, 0.7F, 0.03F, Noise);

    // TypeId 9: grass — face-dependent green top / dirt sides
    table[9] = makeMaterial(0.34F, 0.63F, 0.20F, 0.48F, 0.32F, 0.18F, 4.0F, 0.5F, 0.03F, Grass);

    // TypeId 10: coal_ore — stone base with dark spots
    table[10] = makeMaterial(0.50F, 0.50F, 0.52F, 0.08F, 0.08F, 0.08F, 5.0F, 0.8F, 0.03F, OreSpots);

    // TypeId 11: iron_ore — stone base with tan/rust spots
    table[11] = makeMaterial(0.50F, 0.50F, 0.52F, 0.72F, 0.55F, 0.40F, 5.0F, 0.7F, 0.03F, OreSpots);

    // TypeId 12: water — W1 tuning. baseColor is the "deep" water tone,
    // secondaryColor the "shallow / sky-reflecting" highlight. The shader's
    // ripple noise blends between them, so a bigger gap = more visible
    // animation. Alpha 0.74 keeps the water translucent enough to read
    // underwater terrain.
    table[12] = makeMaterial(
        /*base   */ 0.05F, 0.20F, 0.48F,
        /*second */ 0.42F, 0.68F, 0.84F,
        /*scale  */ 0.4F, /*strength*/ 0.08F, /*edgeWidth*/ 0.03F,
        Water,
        /*emission*/ 0.0F, /*alpha*/ 0.74F);

    // TypeId 13: oak_log — wood grain with bark
    table[13] = makeMaterial(0.58F, 0.40F, 0.22F, 0.35F, 0.25F, 0.12F, 2.0F, 0.5F, 0.03F, WoodGrain);

    // TypeId 14: crafting_table — warm wood noise
    table[14] = makeMaterial(0.58F, 0.40F, 0.22F, 0.65F, 0.45F, 0.25F, 3.0F, 0.4F, 0.05F, Noise);

    // TypeId 15: torch — emissive warm glow
    table[15] = makeMaterial(0.90F, 0.70F, 0.20F, 0.95F, 0.80F, 0.30F, 2.0F, 0.3F, 0.03F, Noise, 0.9F);

    // ---- TypeIds 16-19: automation blocks (use stone-like defaults) ----
    table[16] = makeMaterial(0.50F, 0.50F, 0.52F, 0.42F, 0.42F, 0.44F, 3.0F, 0.6F, 0.04F, Noise); // belt
    table[17] = makeMaterial(0.45F, 0.45F, 0.48F, 0.30F, 0.30F, 0.33F, 3.0F, 0.5F, 0.05F, EdgeHighlight); // gearbox
    table[18] = makeMaterial(0.55F, 0.45F, 0.30F, 0.40F, 0.30F, 0.20F, 3.0F, 0.4F, 0.05F, EdgeHighlight); // clutch
    table[19] = makeMaterial(0.42F, 0.42F, 0.44F, 0.30F, 0.30F, 0.32F, 3.0F, 0.6F, 0.05F, Noise); // millstone

    // ---- TypeIds 20-29: biome surface blocks ----
    // TypeId 20: sand — pale tan, fine grain noise. World-space tiling makes
    // greedy-merged desert dunes look continuous.
    table[20] = makeMaterial(0.86F, 0.78F, 0.55F, 0.72F, 0.62F, 0.40F, 6.0F, 0.4F, 0.03F, Noise);

    // TypeId 21: sandstone — warm tan with stronger layered noise (looks banded).
    table[21] = makeMaterial(0.78F, 0.68F, 0.45F, 0.58F, 0.48F, 0.30F, 2.5F, 0.6F, 0.04F, Noise);

    // TypeId 22: snow — bright white with very subtle blue-shadow noise.
    table[22] = makeMaterial(0.94F, 0.95F, 0.97F, 0.82F, 0.85F, 0.92F, 8.0F, 0.2F, 0.02F, Noise);

    // TypeId 23: red_sand — rusty orange-red badlands surface.
    table[23] = makeMaterial(0.78F, 0.42F, 0.22F, 0.62F, 0.30F, 0.15F, 6.0F, 0.45F, 0.03F, Noise);

    // TypeId 24: terracotta — layered red/orange. Higher noise strength to suggest strata.
    table[24] = makeMaterial(0.70F, 0.38F, 0.22F, 0.55F, 0.25F, 0.12F, 1.5F, 0.7F, 0.05F, Noise);

    // TypeId 25: gravel — gray with chaotic stone-like noise. Higher contrast than stone.
    table[25] = makeMaterial(0.48F, 0.47F, 0.46F, 0.32F, 0.32F, 0.32F, 4.5F, 0.8F, 0.04F, Noise);

    // TypeId 26: basalt — very dark gray with hex-like edge highlights for volcanic look.
    table[26] = makeMaterial(0.20F, 0.18F, 0.20F, 0.12F, 0.10F, 0.12F, 3.0F, 0.5F, 0.07F, EdgeHighlight);

    // TypeId 27: podzol — dark brown forest soil with deep-noise variation.
    table[27] = makeMaterial(0.32F, 0.22F, 0.12F, 0.20F, 0.14F, 0.08F, 4.0F, 0.7F, 0.03F, Noise);

    // TypeId 28: mossy_stone — stone tone tinted green for swamp/overgrown areas.
    table[28] = makeMaterial(0.42F, 0.50F, 0.35F, 0.30F, 0.42F, 0.25F, 3.0F, 0.6F, 0.04F, Noise);

    // TypeId 29: ice — pale cyan with edge highlight for translucent look.
    table[29] = makeMaterial(0.78F, 0.88F, 0.95F, 0.60F, 0.75F, 0.90F, 1.5F, 0.2F, 0.10F, EdgeHighlight);

    // ---- TypeId 30: leaves — broken green canopy with high-frequency noise. ----
    table[30] = makeMaterial(0.18F, 0.54F, 0.16F, 0.42F, 0.68F, 0.22F, 8.0F, 0.7F, 0.04F, Noise);

    // ---- TypeIds 31-34: Space Phase B asteroid mining resources. ----
    table[31] = makeMaterial(0.34F, 0.35F, 0.38F, 0.22F, 0.23F, 0.26F, 5.5F, 0.75F, 0.05F, Noise); // space_rock
    table[32] = makeMaterial(0.42F, 0.43F, 0.45F, 0.78F, 0.62F, 0.42F, 5.5F, 0.85F, 0.04F, OreSpots); // rich_metal_ore
    table[33] = makeMaterial(0.26F, 0.46F, 0.58F, 0.42F, 0.92F, 0.95F, 2.0F, 0.45F, 0.08F, EdgeHighlight, 0.18F); // aether_crystal_ore
    table[34] = makeMaterial(0.58F, 0.78F, 0.90F, 0.82F, 0.94F, 1.0F, 1.2F, 0.25F, 0.09F, EdgeHighlight); // compressed_ice

    (void)registry;
    return table;
}

} // namespace voxel::render
