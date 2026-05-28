#pragma once

#include <cstdint>
#include <memory>

#include <voxel/world/BlockState.hpp>
#include <voxel/world/IChunkGenerator.hpp>
#include <voxel/world/SpaceEnvironment.hpp>
#include <voxel/world/TerrainColumnPrepass.hpp>

namespace voxel::world {

class TerrainDefinitionRegistry;

// Layered terrain with a 2D world-shape prepass and 3D cave/ore density.
// Block IDs match the JSON registry: 2=stone, 8=dirt, 9=grass. If those IDs
// aren't loaded, the generator still produces the same block layout — the
// renderer will simply fall back to its default material.
struct NoiseTerrainSettings {
    std::uint32_t seed{1337};
    // ATGS v1 vertical budget. Sea level remains 0; ordinary land lives
    // around 20..140, mega mountains can reach ~520, and ocean trenches can
    // drop toward -800 while leaving headroom for caves below.
    float minWorldY{-1024.0F};
    float maxWorldY{768.0F};
    float surfaceFrequency{1.0F / 96.0F}; // world-units; smaller = larger features
    float surfaceAmplitude{92.0F};        // ordinary land displacement in blocks
    int   surfaceOctaves{4};
    float seaLevel{0.0F};

    float continentFrequency{1.0F / 2800.0F};
    float tectonicFrequency{1.0F / 1800.0F};
    float erosionFrequency{1.0F / 900.0F};
    float peaksFrequency{1.0F / 1100.0F};
    float climateFrequency{1.0F / 1800.0F};
    float weirdnessFrequency{1.0F / 1250.0F};
    float volcanismFrequency{1.0F / 1500.0F};
    float manaFrequency{1.0F / 1700.0F};
    float macroWarpFrequency{1.0F / 1900.0F};
    float macroWarpStrength{560.0F};
    float polarScaleBlocks{9000.0F};
    float deepOceanMinDepth{180.0F};
    float deepOceanMaxDepth{450.0F};
    float abyssMinDepth{450.0F};
    float abyssMaxDepth{820.0F};
    float shelfMinDepth{20.0F};
    float shelfMaxDepth{150.0F};
    float mountainBoost{390.0F};
    float hillBoost{62.0F};
    float plateauHeight{210.0F};

    // ---- Cheese caves (existing layer) — large open blob caverns. ----
    float caveFrequency{1.0F / 22.0F};    // F5: a bit lower-freq = bigger caves
    int   caveOctaves{3};
    float caveThreshold{0.60F};           // F5: slightly lower = more open caves
    float caveCeilingOffset{4.0F};        // never cut caves into the top dirt layer

    // ---- Spaghetti caves (Phase 3) — thin meandering tunnels formed by
    // the intersection of two 3D noise iso-surfaces. Lower `bandHalfWidth`
    // = thinner tunnels; higher frequency = more twisty.
    float spaghettiFrequency{1.0F / 30.0F};
    int   spaghettiOctaves{2};
    float spaghettiBandHalfWidth{0.06F};
    // Depth gating: spaghetti tunnels only exist between these depths-from-surface.
    int   spaghettiMinDepth{8};
    int   spaghettiMaxDepth{96};

    // ---- Ravine caves (Phase 3) — vertical slabs/chasms. Ridged 2D noise
    // in XZ + a Y-window mask. Thin ravineBandHalfWidth = sharper slabs.
    float ravineFrequency{1.0F / 90.0F};
    float ravineBandHalfWidth{0.04F};
    int   ravineMinDepth{12};   // top of ravine (blocks below surface)
    int   ravineMaxDepth{72};   // bottom of ravine

    // ---- Overhangs (Phase 7) — 3D density distortion of the heightmap.
    // Each column's surface is the iso-line of `surfaceY(x,z) + noise*amp - y`.
    // When noise is large enough relative to local slope, the surface folds
    // back on itself → overhangs, arches, eroded badlands cliffs.
    //
    // `overhangAmplitude` is the MAXIMUM possible shift. The actual per-
    // column amplitude is a smooth function of `peaksValleys` and `erosion`
    // (see TerrainPipeline.cpp::terrainAmplitude), so flat terrain gets 0
    // and only genuine mountain columns approach the cap. This prevents
    // cliff seams at biome borders that a hard biome-lookup would create.
    float overhangFrequency{1.0F / 32.0F};
    int   overhangOctaves{3};
    float overhangAmplitude{5.0F};   // max blocks the surface can shift
    float overhangBaseAmp{0.0F};     // floor amplitude for ALL biomes (0 = off)

    // ---- Rivers (Phase 4) — meandering channels carved across the land.
    // The ridge band of a 2D fbm produces continuous 1D river curves. The
    // climate gate (humidity/erosion/weirdness on the per-column data)
    // restricts rivers to plausible hydrological regions, so rivers appear
    // as patches inside humid temperate lowlands rather than everywhere.
    float riverFrequency{1.0F / 320.0F};  // lower freq = longer river runs
    float riverBandHalfWidth{0.045F};      // |ridge - 0.5| < this = carve
    int   riverMaxDepth{4};                // how far below baseline to carve
    float riverHumidityMin{0.30F};         // climate gate (relaxed from 0.45)
    float riverErosionMax{-0.05F};
    float riverWeirdnessAbsMax{0.45F};

    // F5: ores. Only spawn below `oreMinDepth` blocks from the surface.
    float coalFrequency{1.0F / 9.0F};
    float coalThreshold{0.72F};
    int   coalMinDepth{4};
    float ironFrequency{1.0F / 11.0F};
    float ironThreshold{0.78F};
    int   ironMinDepth{12};

    BlockStateId stoneBlock{makeBlockState(BlockTypeId{2})};
    BlockStateId dirtBlock{makeBlockState(BlockTypeId{8})};
    BlockStateId grassBlock{makeBlockState(BlockTypeId{9})};
    BlockStateId coalOreBlock{makeBlockState(BlockTypeId{10})};
    BlockStateId ironOreBlock{makeBlockState(BlockTypeId{11})};
    BlockStateId waterBlock{makeBlockState(BlockTypeId{12})};

    // Phase 1 biome surface blocks. Hardcoded TypeIds match the registration
    // order in BlockRegistry::registerCoreBlocks() and assets/data/core/blocks.json.
    BlockStateId sandBlock{makeBlockState(BlockTypeId{20})};
    BlockStateId sandstoneBlock{makeBlockState(BlockTypeId{21})};
    BlockStateId snowBlock{makeBlockState(BlockTypeId{22})};
    BlockStateId redSandBlock{makeBlockState(BlockTypeId{23})};
    BlockStateId terracottaBlock{makeBlockState(BlockTypeId{24})};
    BlockStateId gravelBlock{makeBlockState(BlockTypeId{25})};
    BlockStateId basaltBlock{makeBlockState(BlockTypeId{26})};
    BlockStateId podzolBlock{makeBlockState(BlockTypeId{27})};
    BlockStateId mossyStoneBlock{makeBlockState(BlockTypeId{28})};
    BlockStateId iceBlock{makeBlockState(BlockTypeId{29})};

    // Phase 5 foliage blocks.
    BlockStateId oakLogBlock{makeBlockState(BlockTypeId{13})};
    BlockStateId leavesBlock{makeBlockState(BlockTypeId{30})};

    // Revised Space Phase A/B: natural space features. Above atmosphereTopY,
    // terrain chunks skip planetary terrain and can contain sparse mineable
    // asteroid material from deterministic space sectors.
    bool enableSpaceAsteroids{true};
    SpaceSettings space{};
    BlockStateId spaceStoneBlock{makeBlockState(BlockTypeId{31})};
    BlockStateId spaceMetalOreBlock{makeBlockState(BlockTypeId{32})};
    BlockStateId spaceCrystalBlock{makeBlockState(BlockTypeId{33})};
    BlockStateId spaceIceBlock{makeBlockState(BlockTypeId{34})};
};

class NoiseTerrainGenerator final : public IChunkGenerator {
public:
    NoiseTerrainGenerator() = default;
    explicit NoiseTerrainGenerator(NoiseTerrainSettings settings);

    void generate(Chunk& chunk) override;
    void generateColumn(std::vector<Chunk>& chunks, std::vector<TerrainGenerationMode>& modes) override;
    [[nodiscard]] TerrainGenerationMode lastGenerationMode() const noexcept override;

    void setPrepassCache(std::shared_ptr<TerrainColumnPrepassCache> cache) noexcept;
    void setTerrainDefinitions(const TerrainDefinitionRegistry* terrainDefinitions) noexcept;
    [[nodiscard]] TerrainColumnPrepass buildColumnPrepass(TerrainColumnCoord coord) const;
    [[nodiscard]] TerrainColumnKey prepassKey(TerrainColumnCoord coord) const noexcept;
    [[nodiscard]] std::uint64_t terrainVersion() const noexcept override;

    // Phase 5: sample full column data (biome, surface height, river status,
    // etc.) at an arbitrary world position. Used by the foliage stage to look
    // up surface heights for trees whose center is in a neighboring chunk.
    // Cost: one set of fbm samples — about 1µs per call. Stateless.
    [[nodiscard]] ColumnWorldgenData sampleColumnAt(float worldX, float worldZ) const;

    [[nodiscard]] const NoiseTerrainSettings& settings() const noexcept { return settings_; }

private:
    void generateWithPrepass(Chunk& chunk, const TerrainColumnPrepass& prepass, TerrainGenerationMode mode);
    [[nodiscard]] bool generateSpaceChunk(Chunk& chunk);
    [[nodiscard]] TerrainColumnPrepass resolvePrepass(TerrainColumnCoord columnCoord, TerrainGenerationMode& mode);

    NoiseTerrainSettings settings_{};
    std::shared_ptr<TerrainColumnPrepassCache> prepassCache_;
    const TerrainDefinitionRegistry* terrainDefinitions_{nullptr};
};

} // namespace voxel::world
