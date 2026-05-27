#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <voxel/automation/KineticNetwork.hpp>
#include <voxel/core/JobSystem.hpp>
#include <voxel/core/Math.hpp>
#include <voxel/core/MpscQueue.hpp>
#include <voxel/core/Paths.hpp>
#include <voxel/core/RuntimeStats.hpp>
#include <voxel/data/BlockRegistry.hpp>
#include <voxel/data/CoreContentIds.hpp>
#include <voxel/data/ItemRegistry.hpp>
#include <voxel/data/RecipeRegistry.hpp>
#include <voxel/data/RegistryLoader.hpp>
#include <voxel/inventory/Inventory.hpp>
#include <voxel/inventory/InventorySerialization.hpp>
#include <voxel/player/CreativeHotbar.hpp>
#include <voxel/player/PlayerController.hpp>
#include <voxel/player/PlayerSpawnResolver.hpp>
#include <voxel/render/MaterialTable.hpp>
#include <voxel/render/meshing/BlockRenderCatalog.hpp>
#include <voxel/render/meshing/ChunkMeshCache.hpp>
#include <voxel/render/meshing/ClipmapRegionMesher.hpp>
#include <voxel/render/meshing/ClusterMesher.hpp>
#include <voxel/render/meshing/GreedyMesher.hpp>
#include <voxel/save/PlayerInventorySaveService.hpp>
#include <voxel/save/RegionFileStore.hpp>
#include <voxel/save/SaveCoordinator.hpp>
#include <voxel/save/WorldSaveService.hpp>
#include <voxel/world/BitPackedArray.hpp>
#include <voxel/world/BlockEditor.hpp>
#include <voxel/world/BlockEditQueue.hpp>
#include <voxel/world/ChunkDirtyQueue.hpp>
#include <voxel/world/ChunkJobMailbox.hpp>
#include <voxel/world/BlockLightCatalog.hpp>
#include <voxel/world/ChunkLightData.hpp>
#include <voxel/world/LightPropagator.hpp>
#include <voxel/world/Palette.hpp>
#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/ChunkPipeline.hpp>
#include <voxel/world/ChunkStreamer.hpp>
#include <voxel/world/CoordinateUtils.hpp>
#include <voxel/world/FlatTerrainGenerator.hpp>
#include <voxel/world/FluidSystem.hpp>
#include <voxel/world/Lod.hpp>
#include <voxel/world/NoiseTerrainGenerator.hpp>
#include <voxel/world/Raycast.hpp>
#include <voxel/world/SpaceEnvironment.hpp>
#include <voxel/world/TerrainDefinitionRegistry.hpp>
#include <voxel/world/WorldDelta.hpp>

#define VOXEL_CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "check failed: " #condition << " at " << __FILE__ << ':' << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

int main()
{
    // ---- LOD foundation type math --------------------------------------
    // Locks the invariants that the rest of the LOD system depends on:
    //   - chunkToCluster floor-divides correctly across the world origin
    //   - the cluster origin is the (0,0,0) chunk of its containing cluster
    //   - selectLodForChunk uses Chebyshev (cubic) distance and the right
    //     band boundaries
    using voxel::world::ChunkCoord;
    using voxel::world::ClusterCoord;
    using voxel::world::ClusterChunkExtent;
    using voxel::world::LodLevel;
    using voxel::world::LodSettings;
    using voxel::world::RegionCoord;
    VOXEL_CHECK(ClusterChunkExtent == 4); // anchor for the band assertions below

    // Positive coords: straightforward division.
    VOXEL_CHECK(voxel::world::chunkToCluster({0, 0, 0}) == (ClusterCoord{0, 0, 0}));
    VOXEL_CHECK(voxel::world::chunkToCluster({3, 3, 3}) == (ClusterCoord{0, 0, 0}));
    VOXEL_CHECK(voxel::world::chunkToCluster({4, 0, 0}) == (ClusterCoord{1, 0, 0}));
    VOXEL_CHECK(voxel::world::chunkToCluster({7, 5, 9}) == (ClusterCoord{1, 1, 2}));

    // Negative coords: floor division must NOT round toward zero. The
    // straight `/` operator would say -1 / 4 == 0, collapsing chunks
    // -1/-2/-3 onto cluster 0 alongside chunks 0/1/2/3 — wrong.
    VOXEL_CHECK(voxel::world::chunkToCluster({-1, 0, 0}) == (ClusterCoord{-1, 0, 0}));
    VOXEL_CHECK(voxel::world::chunkToCluster({-4, 0, 0}) == (ClusterCoord{-1, 0, 0}));
    VOXEL_CHECK(voxel::world::chunkToCluster({-5, 0, 0}) == (ClusterCoord{-2, 0, 0}));
    VOXEL_CHECK(voxel::world::chunkToCluster({-1, -1, -1}) == (ClusterCoord{-1, -1, -1}));

    // Round-trip: every chunk in cluster C maps back to C, and clusterChunkOrigin(C)
    // produces the lowest chunk in C.
    VOXEL_CHECK(voxel::world::clusterChunkOrigin({3, -2, 0}) == (ChunkCoord{12, -8, 0}));
    for (int dy = 0; dy < ClusterChunkExtent; ++dy) {
        for (int dx = 0; dx < ClusterChunkExtent; ++dx) {
            const ChunkCoord c{12 + dx, -8 + dy, 0};
            VOXEL_CHECK(voxel::world::chunkToCluster(c) == (ClusterCoord{3, -2, 0}));
        }
    }

    {
        const auto targets = voxel::world::lodInvalidationTargetsForEditedChunk({17, -1, 33});
        VOXEL_CHECK(targets.cluster == (ClusterCoord{4, -1, 8}));
        VOXEL_CHECK(targets.region == (RegionCoord{1, -1, 2}));
    }
    {
        const auto targets = voxel::world::lodInvalidationTargetsForEditedChunk({-17, -16, -1});
        VOXEL_CHECK(targets.cluster == (ClusterCoord{-5, -4, -1}));
        VOXEL_CHECK(targets.region == (RegionCoord{-2, -1, -1}));
    }

    // LOD tier selection on default bands {8, 24, 96, 1600}.
    // (Wrapped in a block so the helper names don't leak into the rest of
    // main() — the outer `origin` later is a Chunk, not a ChunkCoord.)
    {
        const LodSettings lod{};
        const ChunkCoord lodCenter{0, 0, 0};
        VOXEL_CHECK(voxel::world::selectLodForChunk({0, 0, 0}, lodCenter, lod) == LodLevel::Active);
        VOXEL_CHECK(voxel::world::selectLodForChunk({7, 0, 0}, lodCenter, lod) == LodLevel::Active);
        VOXEL_CHECK(voxel::world::selectLodForChunk({8, 0, 0}, lodCenter, lod) == LodLevel::Simplified);
        VOXEL_CHECK(voxel::world::selectLodForChunk({23, 0, 0}, lodCenter, lod) == LodLevel::Simplified);
        VOXEL_CHECK(voxel::world::selectLodForChunk({24, 0, 0}, lodCenter, lod) == LodLevel::Cluster);
        VOXEL_CHECK(voxel::world::selectLodForChunk({95, 0, 0}, lodCenter, lod) == LodLevel::Cluster);
        VOXEL_CHECK(voxel::world::selectLodForChunk({96, 0, 0}, lodCenter, lod) == LodLevel::Region);
        VOXEL_CHECK(voxel::world::selectLodForChunk({2000, 0, 0}, lodCenter, lod) == LodLevel::Solar);

        // Chebyshev means the max-of-axes wins; diagonals don't push outward
        // faster than straight axes (unlike Euclidean).
        VOXEL_CHECK(voxel::world::selectLodForChunk({5, 5, 5}, lodCenter, lod) == LodLevel::Active);
        VOXEL_CHECK(voxel::world::selectLodForChunk({7, 7, 7}, lodCenter, lod) == LodLevel::Active);
        VOXEL_CHECK(voxel::world::selectLodForChunk({8, 8, 8}, lodCenter, lod) == LodLevel::Simplified);
    }

    // ---- Phase 1B: ClusterMesher behaviour locks -----------------------
    // Verify the half-resolution meshing pipeline on toy inputs. These
    // tests lock the contract that the cluster mesher follows the same
    // face-index convention as GreedyMesher, that the majority-vote
    // reduction works, that all-air clusters produce empty meshes, and
    // that the revision hash actually changes when chunks change.
    {
        voxel::render::meshing::ClusterMesher clusterMesher;
        // Empty catalog: any block ID returns the default BlockRenderInfo
        // (Opaque, occludes=true, isFluid=false), which is correct for the
        // generic "stone" stand-in we use below. No registry needed here —
        // the cluster mesher only consults the catalog for surface/occludes/
        // isFluid flags, and the defaults are exactly what stone wants.
        voxel::render::meshing::BlockRenderCatalog clusterCatalog;
        const auto stoneState = voxel::world::makeBlockState(voxel::BlockTypeId{2});

        // Case 1: completely empty cluster → empty mesh.
        voxel::render::meshing::ClusterChunkSnapshot emptySnapshot{};
        emptySnapshot.coord = voxel::world::ClusterCoord{0, 0, 0};
        for (int i = 0; i < voxel::world::ClusterChunkVolume; ++i) {
            emptySnapshot.chunks[static_cast<std::size_t>(i)].reset();
        }
        const auto emptyClusterMesh = clusterMesher.build(emptySnapshot, clusterCatalog);
        VOXEL_CHECK(emptyClusterMesh.vertices.empty());
        VOXEL_CHECK(emptyClusterMesh.indices.empty());
        VOXEL_CHECK(emptyClusterMesh.coord == (voxel::world::ClusterCoord{0, 0, 0}));

        // Case 2: a single 2×2×2 block of stone at the cluster's origin.
        // This fills exactly one supervoxel — must produce 6 faces (one per
        // direction) since no neighbouring supervoxel exists to cull against.
        voxel::render::meshing::ClusterChunkSnapshot solidSnapshot{};
        solidSnapshot.coord = voxel::world::ClusterCoord{1, 0, 2};
        // Only chunk (0,0,0) in cluster-local space has any content.
        voxel::world::Chunk solidChunk({4, 0, 8}); // matches cluster (1,0,2) origin
        for (int z = 0; z < 2; ++z) {
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 2; ++x) {
                    solidChunk.setBlock(x, y, z, stoneState);
                }
            }
        }
        solidSnapshot.chunks[voxel::render::meshing::clusterLocalChunkIndex(0, 0, 0)]
            .emplace(solidChunk);
        const auto solidClusterMesh = clusterMesher.build(solidSnapshot, clusterCatalog);
        VOXEL_CHECK(solidClusterMesh.vertices.size() == 24); // 6 faces × 4 corners
        VOXEL_CHECK(solidClusterMesh.indices.size() == 36); // 6 faces × 6 indices
        VOXEL_CHECK(solidClusterMesh.coord == (voxel::world::ClusterCoord{1, 0, 2}));
        // Supervoxel is at cluster-local block (0..2). Outer faces sit at
        // block 0 and block 2.
        bool anyAtBlock0 = false, anyAtBlock2 = false;
        for (const auto& v : solidClusterMesh.vertices) {
            if (v.posX == 0 || v.posY == 0 || v.posZ == 0) anyAtBlock0 = true;
            if (v.posX == 2 || v.posY == 2 || v.posZ == 2) anyAtBlock2 = true;
        }
        VOXEL_CHECK(anyAtBlock0);
        VOXEL_CHECK(anyAtBlock2);

        // Case 3: majority-vote threshold. 4 of 8 sub-blocks = should be
        // air (need strict >half). 5 of 8 = solid.
        voxel::render::meshing::ClusterChunkSnapshot fourOfEightSnapshot{};
        fourOfEightSnapshot.coord = voxel::world::ClusterCoord{0, 0, 0};
        voxel::world::Chunk fourOfEightChunk({0, 0, 0});
        // Put stone in 4 of the first supervoxel's 8 sub-blocks.
        fourOfEightChunk.setBlock(0, 0, 0, stoneState);
        fourOfEightChunk.setBlock(1, 0, 0, stoneState);
        fourOfEightChunk.setBlock(0, 1, 0, stoneState);
        fourOfEightChunk.setBlock(1, 1, 0, stoneState);
        fourOfEightSnapshot.chunks[voxel::render::meshing::clusterLocalChunkIndex(0, 0, 0)]
            .emplace(fourOfEightChunk);
        const auto fourMesh = clusterMesher.build(fourOfEightSnapshot, clusterCatalog);
        // 4 air + 4 stone = airCount=4, NOT > 4 → supervoxel IS solid.
        VOXEL_CHECK(!fourMesh.vertices.empty());

        voxel::render::meshing::ClusterChunkSnapshot threeOfEightSnapshot{};
        threeOfEightSnapshot.coord = voxel::world::ClusterCoord{0, 0, 0};
        voxel::world::Chunk threeOfEightChunk({0, 0, 0});
        threeOfEightChunk.setBlock(0, 0, 0, stoneState);
        threeOfEightChunk.setBlock(1, 0, 0, stoneState);
        threeOfEightChunk.setBlock(0, 1, 0, stoneState);
        threeOfEightSnapshot.chunks[voxel::render::meshing::clusterLocalChunkIndex(0, 0, 0)]
            .emplace(threeOfEightChunk);
        const auto threeMesh = clusterMesher.build(threeOfEightSnapshot, clusterCatalog);
        // 5 air + 3 stone = airCount=5 > 4 → supervoxel is air.
        VOXEL_CHECK(threeMesh.vertices.empty());

        // Case 4: revision hash flips when contained chunk changes.
        // Identical content but different revisions → identical hashes
        // (the chunk's revision is what differs, not the content).
        const auto hashBefore = voxel::render::meshing::hashClusterChunkRevisions(solidSnapshot);
        voxel::render::meshing::ClusterChunkSnapshot dirtiedSnapshot = solidSnapshot;
        // Bump the chunk's revision by making any real block change.
        auto& dirtiedChunk = dirtiedSnapshot.chunks[
            voxel::render::meshing::clusterLocalChunkIndex(0, 0, 0)].value();
        dirtiedChunk.setBlock(5, 5, 5, stoneState); // bumps revision
        const auto hashAfter = voxel::render::meshing::hashClusterChunkRevisions(dirtiedSnapshot);
        VOXEL_CHECK(hashBefore != hashAfter);
    }

    voxel::core::Paths paths;
    VOXEL_CHECK(paths.coreDataRoot().filename() == "core");
    const auto identity = voxel::core::identity();
    const auto combined = voxel::core::multiply(identity, identity);
    VOXEL_CHECK(combined.m[0] == 1.0F);
    VOXEL_CHECK(combined.m[5] == 1.0F);
    VOXEL_CHECK(combined.m[10] == 1.0F);
    VOXEL_CHECK(combined.m[15] == 1.0F);

    voxel::core::RuntimeStats runtimeStats;
    voxel::core::RuntimeCounters runtimeFrame;
    runtimeFrame.frames = 1;
    runtimeFrame.jobSystemPending = 4;
    runtimeFrame.workerCount = 2;
    runtimeFrame.slowFrames = 1;
    voxel::core::recordTimer(runtimeFrame.stageRender, 2500);
    runtimeStats.recordFrame(51.0, runtimeFrame);
    VOXEL_CHECK(runtimeStats.totals().slowFrames == 1);
    VOXEL_CHECK(runtimeStats.totals().jobSystemPending == 4);
    VOXEL_CHECK(runtimeStats.totals().stageRender.count == 1);
    VOXEL_CHECK(runtimeStats.totals().stageRender.maxUs == 2500);
    voxel::core::RuntimeCounters saveOnlyCounters;
    saveOnlyCounters.savesFlushed = 1;
    runtimeStats.recordFrame(0.0, saveOnlyCounters);
    VOXEL_CHECK(runtimeStats.totals().jobSystemPending == 4);

    {
        voxel::world::SpaceEnvironment space;
        const auto ground = space.evaluate(64.0F);
        VOXEL_CHECK(ground.atmosphereDensity > 0.99F);
        VOXEL_CHECK(ground.gravityScale == 1.0F);
        VOXEL_CHECK(!ground.inNearSpace);
        VOXEL_CHECK(!ground.inSpace);

        const auto nearSpace = space.evaluate(9000.0F);
        VOXEL_CHECK(nearSpace.spaceBlend > 0.49F && nearSpace.spaceBlend < 0.51F);
        VOXEL_CHECK(nearSpace.gravityScale > 0.0F && nearSpace.gravityScale < 1.0F);
        VOXEL_CHECK(nearSpace.inNearSpace);
        VOXEL_CHECK(!nearSpace.inSpace);

        const auto fullSpace = space.evaluate(10000.0F);
        VOXEL_CHECK(fullSpace.atmosphereDensity == 0.0F);
        VOXEL_CHECK(fullSpace.spaceBlend == 1.0F);
        VOXEL_CHECK(fullSpace.gravityScale == 0.0F);
        VOXEL_CHECK(fullSpace.inSpace);

        const auto sector = space.sectorFor({-1.0F, 4096.0F, -2049.0F});
        VOXEL_CHECK(sector.x == -1);
        VOXEL_CHECK(sector.y == 2);
        VOXEL_CHECK(sector.z == -2);
        const auto featuresA = space.featuresForSector(sector);
        const auto featuresB = space.featuresForSector(sector);
        VOXEL_CHECK(featuresA.size() == featuresB.size());
        if (!featuresA.empty()) {
            VOXEL_CHECK(featuresA.front().type == featuresB.front().type);
            VOXEL_CHECK(featuresA.front().origin == featuresB.front().origin);
            VOXEL_CHECK(featuresA.front().resourceSeed == featuresB.front().resourceSeed);
            VOXEL_CHECK(featuresA.front().bodyClass == featuresB.front().bodyClass);
            VOXEL_CHECK(featuresA.front().gravityScale == featuresB.front().gravityScale);
            VOXEL_CHECK(featuresA.front().atmosphereDensity == featuresB.front().atmosphereDensity);
            VOXEL_CHECK(featuresA.front().surfaceRoughness == featuresB.front().surfaceRoughness);
            VOXEL_CHECK(featuresA.front().oceanCoverage == featuresB.front().oceanCoverage);
            VOXEL_CHECK(featuresA.front().resourceRichness == featuresB.front().resourceRichness);
            VOXEL_CHECK(featuresA.front().lifeSignal == featuresB.front().lifeSignal);
            VOXEL_CHECK(featuresA.front().landable == featuresB.front().landable);
        }
    }

    {
        voxel::world::FluidSystemSettings fluidSettings{};
        fluidSettings.waterBlockValue = voxel::world::makeBlockState(voxel::BlockTypeId{12}).value;
        fluidSettings.maxActiveWorldY = 9999;
        voxel::world::FluidSystem fluid(fluidSettings);
        voxel::world::ChunkManager fluidChunks;

        const auto spaceLocal = voxel::world::toChunkLocal(0, 10000, 0);
        auto& spaceChunk = fluidChunks.createOrGet(spaceLocal.chunk);
        spaceChunk.setBlockSilently(
            spaceLocal.local.x,
            spaceLocal.local.y,
            spaceLocal.local.z,
            voxel::world::makeBlockState(voxel::BlockTypeId{12}));
        fluid.wake(spaceLocal.chunk, spaceLocal.local);
        VOXEL_CHECK(fluid.queue().size() == 0);
        const auto spaceStats = fluid.tick(fluidChunks, spaceLocal.chunk);
        VOXEL_CHECK(spaceStats.cellsCarved == 0);
        const auto spaceNeighbour = voxel::world::toChunkLocal(1, 10000, 0);
        VOXEL_CHECK(fluidChunks.find(spaceNeighbour.chunk)->blockAt(
            spaceNeighbour.local.x,
            spaceNeighbour.local.y,
            spaceNeighbour.local.z).value == voxel::world::AirBlockState.value);

        const auto groundLocal = voxel::world::toChunkLocal(0, 9999, 0);
        auto& groundChunk = fluidChunks.createOrGet(groundLocal.chunk);
        groundChunk.setBlockSilently(
            groundLocal.local.x,
            groundLocal.local.y,
            groundLocal.local.z,
            voxel::world::makeBlockState(voxel::BlockTypeId{12}));
        // Put a solid block below so this source attempts horizontal spread.
        const auto belowLocal = voxel::world::toChunkLocal(0, 9998, 0);
        fluidChunks.createOrGet(belowLocal.chunk).setBlockSilently(
            belowLocal.local.x,
            belowLocal.local.y,
            belowLocal.local.z,
            voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        fluid.wake(groundLocal.chunk, groundLocal.local);
        const auto groundStats = fluid.tick(fluidChunks, groundLocal.chunk);
        VOXEL_CHECK(groundStats.cellsCarved > 0);
    }

    voxel::data::BlockRegistry blocks;
    blocks.registerCoreBlocks();
    // 19 automation/core + 10 Phase 1 biome + 1 Phase 5 foliage + 4 Space B blocks = 34.
    VOXEL_CHECK(blocks.registry().size() == 34);
    VOXEL_CHECK(blocks.find({"core", "stone"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "dirt"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "grass"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "coal_ore"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "iron_ore"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "water"}) != nullptr);
    // Phase 1 biome blocks — verify both registration and stable typeIds.
    VOXEL_CHECK(blocks.find({"core", "sand"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "sandstone"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "snow"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "basalt"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "ice"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "leaves"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "space_rock"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "rich_metal_ore"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "aether_crystal_ore"}) != nullptr);
    VOXEL_CHECK(blocks.find({"core", "compressed_ice"}) != nullptr);

    voxel::data::BlockRegistry loadedBlocks;
    voxel::data::ItemRegistry loadedItems;
    voxel::data::RecipeRegistry loadedRecipes;
    voxel::data::RegistryLoader loader;
    const auto coreLoadResult = loader.loadCoreData("assets/data/core", loadedBlocks, loadedItems, loadedRecipes);
    VOXEL_CHECK(coreLoadResult.blocksLoaded);
    VOXEL_CHECK(coreLoadResult.itemsLoaded);
    VOXEL_CHECK(coreLoadResult.recipesLoaded);
    // 19 automation/core + 10 Phase 1 biome + 1 Phase 5 + 4 Space B blocks = 34.
    VOXEL_CHECK(loadedBlocks.registry().size() == 34);
    VOXEL_CHECK(loadedBlocks.find({"core", "copper_pipe"}) != nullptr);
    VOXEL_CHECK(loadedBlocks.find({"core", "glass"}) != nullptr);
    VOXEL_CHECK(loadedBlocks.find({"core", "rich_metal_ore"}) != nullptr);
    VOXEL_CHECK(loadedBlocks.find({"core", "aether_crystal_ore"}) != nullptr);
    VOXEL_CHECK(loadedItems.find({"core", "stone"}) != nullptr);
    VOXEL_CHECK(loadedItems.find({"core", "stone"})->maxStackSize == 999);
    VOXEL_CHECK(loadedItems.find({"core", "coal"}) != nullptr);
    VOXEL_CHECK(loadedItems.find({"core", "coal"})->maxStackSize == 500);
    VOXEL_CHECK(loadedItems.find({"core", "space_rock"}) != nullptr);
    VOXEL_CHECK(loadedItems.find({"core", "aether_crystal_ore"}) != nullptr);
    VOXEL_CHECK(loadedItems.find({"core", "aether_crystal"}) != nullptr);
    VOXEL_CHECK(loadedItems.find({"core", "space_alloy_ingot"}) != nullptr);
    VOXEL_CHECK(loadedItems.find({"core", "cryo_ice"}) != nullptr);
    VOXEL_CHECK(loadedRecipes.find({"core", "millstone_process_rich_metal"}) != nullptr);
    VOXEL_CHECK(loadedRecipes.find({"core", "millstone_extract_aether_crystal"}) != nullptr);
    VOXEL_CHECK(loadedRecipes.find({"core", "millstone_crush_compressed_ice"}) != nullptr);
    VOXEL_CHECK(loadedRecipes.find({"core", "press_space_alloy"}) != nullptr);
    const auto millRecipes = loadedRecipes.recipesForMachineCategory("mill");
    const auto pressRecipes = loadedRecipes.recipesForMachineCategory("press");
    VOXEL_CHECK(millRecipes.size() >= 5);
    VOXEL_CHECK(pressRecipes.size() >= 2);
    {
        voxel::data::BlockRegistry reorderedBlocks;
        const auto addBlock = [&reorderedBlocks](voxel::data::Identifier id,
                                                 bool solid,
                                                 bool opaque,
                                                 voxel::render::meshing::MeshSurface surface,
                                                 std::vector<std::string> tags = {}) {
            voxel::data::BlockDefinition def;
            def.id = std::move(id);
            def.displayName = def.id.str();
            def.solid = solid;
            def.opaque = opaque;
            def.renderSurface = surface;
            def.tags = std::move(tags);
            return reorderedBlocks.registerBlock(std::move(def));
        };
        const auto waterType = addBlock({"core", "water"}, false, false,
            voxel::render::meshing::MeshSurface::Transparent, {"fluid", "water"});
        const auto stoneType = addBlock({"core", "stone"}, true, true,
            voxel::render::meshing::MeshSurface::Opaque, {"terrain", "solid"});
        const auto grassType = addBlock({"core", "grass"}, true, true,
            voxel::render::meshing::MeshSurface::Opaque, {"terrain", "surface"});
        const auto dirtType = addBlock({"core", "dirt"}, true, true,
            voxel::render::meshing::MeshSurface::Opaque, {"terrain", "soil"});
        const auto ids = voxel::data::resolveCoreBlockIds(reorderedBlocks);
        VOXEL_CHECK(ids.waterType.value == waterType.value);
        VOXEL_CHECK(ids.stoneType.value == stoneType.value);
        VOXEL_CHECK(ids.grassType.value == grassType.value);
        VOXEL_CHECK(ids.dirtType.value == dirtType.value);
        VOXEL_CHECK(ids.water.value == voxel::world::makeBlockState(waterType).value);
        VOXEL_CHECK(ids.water.value != voxel::world::makeBlockState(voxel::BlockTypeId{12}).value);

        voxel::world::NoiseTerrainSettings resolvedSettings{};
        voxel::data::applyCoreBlockIds(resolvedSettings, ids);
        VOXEL_CHECK(resolvedSettings.waterBlock.value == ids.water.value);
        VOXEL_CHECK(resolvedSettings.stoneBlock.value == ids.stone.value);
        VOXEL_CHECK(resolvedSettings.grassBlock.value == ids.grass.value);

        const auto collision = reorderedBlocks.buildCollisionCatalog();
        VOXEL_CHECK(!collision.isSolid(ids.water));
        VOXEL_CHECK(collision.isSolid(ids.stone));
        VOXEL_CHECK(voxel::world::blockMatchesRaycastMask(
            ids.water,
            voxel::world::RaycastMask::FluidsOnly,
            ids.waterType));
        VOXEL_CHECK(!voxel::world::blockMatchesRaycastMask(
            voxel::world::makeBlockState(voxel::BlockTypeId{12}),
            voxel::world::RaycastMask::FluidsOnly,
            ids.waterType));
        voxel::world::VoxelRaycaster reorderedRaycaster;
        reorderedRaycaster.setFluidBlockType(ids.waterType);
        VOXEL_CHECK(reorderedRaycaster.fluidBlockType().value == ids.waterType.value);
        VOXEL_CHECK(!voxel::player::PlayerController::blockIsSolid(voxel::world::makeBlockState(voxel::BlockTypeId{12})));
        VOXEL_CHECK(!voxel::player::PlayerController::blockIsSolid(
            voxel::world::makeBlockState(voxel::BlockTypeId{12}, 7)));

        voxel::player::CreativeHotbar reorderedHotbar(ids);
        VOXEL_CHECK(reorderedHotbar.slot(0).block.value == ids.stone.value);
        VOXEL_CHECK(reorderedHotbar.slot(4).block.value == ids.water.value);

        const auto materials = voxel::render::MaterialTableBuilder::build(reorderedBlocks);
        VOXEL_CHECK(materials[waterType.value].noiseParams[3] == 5.0F);
        VOXEL_CHECK(materials[stoneType.value].baseColor[0] == 0.50F);
    }
    voxel::world::TerrainDefinitionRegistry terrainDefinitions;
    voxel::world::TerrainDefinitionLoader terrainDefinitionLoader;
    const auto terrainDefinitionResult = terrainDefinitionLoader.load("assets/data/core/terrain.json", terrainDefinitions);
    VOXEL_CHECK(terrainDefinitionResult.loaded);
    VOXEL_CHECK(terrainDefinitionResult.heightProfileCount >= 10);
    VOXEL_CHECK(terrainDefinitionResult.biomeCount == 21);
    VOXEL_CHECK(terrainDefinitions.findHeightProfile("tectonic_mountains") != nullptr);
    VOXEL_CHECK(terrainDefinitions.findHeightProfile("abyss_trench") != nullptr);
    const auto* magicForest = terrainDefinitions.findBiome("magic_forest");
    VOXEL_CHECK(magicForest != nullptr);
    VOXEL_CHECK(magicForest->heightProfile == "rolling_plains");
    VOXEL_CHECK(magicForest->conditions.contains("manaField"));
    VOXEL_CHECK(magicForest->conditions.at("manaField").contains(0.75F));
    VOXEL_CHECK(!magicForest->conditions.at("manaField").contains(0.30F));
    const auto* abyss = terrainDefinitions.findBiome("ocean_abyss");
    VOXEL_CHECK(abyss != nullptr);
    VOXEL_CHECK(abyss->heightProfile == "abyss_trench");
    const auto magicCandidates = terrainDefinitions.findBiomeCandidates({
        {"temperature", 0.50F},
        {"moisture", 0.75F},
        {"manaField", 0.82F},
        {"continentalness", 0.45F},
        {"height", 96.0F},
        {"tectonicStress", 0.20F},
        {"weirdness", 0.10F},
    });
    bool sawMagicCandidate = false;
    for (const auto& candidate : magicCandidates) {
        if (candidate.biome != nullptr && candidate.biome->id == "magic_forest" && candidate.score > 0.0F) {
            sawMagicCandidate = true;
        }
    }
    VOXEL_CHECK(sawMagicCandidate);
    voxel::world::TerrainBiomeSignalSample magicSignalSample{};
    magicSignalSample.temperature = 0.45F;
    magicSignalSample.moisture = 0.75F;
    magicSignalSample.manaField = 0.82F;
    magicSignalSample.continentalness = 0.45F;
    magicSignalSample.height = 96.0F;
    magicSignalSample.tectonicStress = 0.20F;
    magicSignalSample.weirdness = 0.10F;
    const auto* bestMagicBiome = terrainDefinitions.findBestBiome(magicSignalSample);
    VOXEL_CHECK(bestMagicBiome != nullptr);
    VOXEL_CHECK(bestMagicBiome->id == "magic_forest");
    voxel::world::NoiseTerrainGenerator registryVersionGen;
    const auto terrainVersionWithoutDefinitions = registryVersionGen.terrainVersion();
    registryVersionGen.setTerrainDefinitions(&terrainDefinitions);
    VOXEL_CHECK(registryVersionGen.terrainVersion() != terrainVersionWithoutDefinitions);
    voxel::world::TerrainDefinitionRegistry customTerrainDefinitions;
    voxel::world::TerrainHeightProfileDefinition forcedHighProfile;
    forcedHighProfile.id = "forced_high";
    forcedHighProfile.terrainClass = "Plains";
    forcedHighProfile.minHeight = 640.0F;
    forcedHighProfile.maxHeight = 640.0F;
    forcedHighProfile.detailScale = 0.0F;
    VOXEL_CHECK(customTerrainDefinitions.registerHeightProfile(std::move(forcedHighProfile)));
    voxel::world::TerrainBiomeDefinition forcedPlainsBiome;
    forcedPlainsBiome.id = "plains";
    forcedPlainsBiome.displayName = "Forced Plains";
    forcedPlainsBiome.heightProfile = "forced_high";
    forcedPlainsBiome.conditions.emplace("continentalness", voxel::world::TerrainSignalRange{-1.0F, 1.0F});
    forcedPlainsBiome.spawnWeight = 10.0F;
    VOXEL_CHECK(customTerrainDefinitions.registerBiome(std::move(forcedPlainsBiome)));
    voxel::world::NoiseTerrainGenerator baseHeightProfileGen;
    voxel::world::NoiseTerrainGenerator forcedHeightProfileGen;
    forcedHeightProfileGen.setTerrainDefinitions(&customTerrainDefinitions);
    const auto baseProfileColumn = baseHeightProfileGen.sampleColumnAt(72.0F, 88.0F);
    const auto forcedProfileColumn = forcedHeightProfileGen.sampleColumnAt(72.0F, 88.0F);
    VOXEL_CHECK(forcedProfileColumn.surfaceY > baseProfileColumn.surfaceY + 200);
    VOXEL_CHECK(forcedProfileColumn.biome == voxel::world::TerrainBiomeId::Plains);
    const auto renderCatalog = loadedBlocks.buildRenderCatalog();
    VOXEL_CHECK(renderCatalog.get(voxel::world::makeBlockState(voxel::BlockTypeId{3})).surface == voxel::render::meshing::MeshSurface::Transparent);
    VOXEL_CHECK(!renderCatalog.get(voxel::world::makeBlockState(voxel::BlockTypeId{3})).occludesNeighborFaces);
    VOXEL_CHECK(renderCatalog.get(voxel::world::makeBlockState(voxel::BlockTypeId{4})).surface == voxel::render::meshing::MeshSurface::Cutout);
    const auto kineticCatalog = loadedBlocks.buildKineticCatalog();
    VOXEL_CHECK(kineticCatalog.get(voxel::world::makeBlockState(voxel::BlockTypeId{5})).role == voxel::automation::KineticRole::Source);
    VOXEL_CHECK(kineticCatalog.get(voxel::world::makeBlockState(voxel::BlockTypeId{6})).role == voxel::automation::KineticRole::Transfer);
    VOXEL_CHECK(kineticCatalog.get(voxel::world::makeBlockState(voxel::BlockTypeId{7})).role == voxel::automation::KineticRole::Consumer);

    voxel::world::ChunkManager chunks;
    auto& origin = chunks.createOrGet({0, 0, 0});
    VOXEL_CHECK(origin.blockAt(0, 0, 0).value == 0);

    voxel::world::FlatTerrainGenerator generator;
    generator.generate(origin);
    VOXEL_CHECK(origin.state() == voxel::world::ChunkState::Resident);
    VOXEL_CHECK(voxel::world::blockTypeOf(origin.blockAt(0, 0, 0)).value == 2);

    voxel::world::WorldDeltaBatch batch;
    batch.push_back(voxel::world::BlockDelta{
        {{0}, {0, 0, 0}, {0, 0, 0}, {1, 2, 3}},
        voxel::world::makeBlockState(voxel::BlockTypeId{2}),
        voxel::BlockStateId{0}
    });
    voxel::world::WorldDeltaApplier{}.apply(chunks, batch);
    VOXEL_CHECK(origin.blockAt(1, 2, 3).value == 0);

    // ------------------------------------------------------------------
    // Generous player inventory backend foundation.
    // ------------------------------------------------------------------
    {
        const auto stoneItem = loadedItems.findRuntimeId({"core", "stone"});
        const auto coalItem = loadedItems.findRuntimeId({"core", "coal"});
        VOXEL_CHECK(stoneItem.value != 0);
        VOXEL_CHECK(coalItem.value != 0);

        voxel::inventory::PlayerInventory inventory;
        VOXEL_CHECK(voxel::inventory::PlayerInventory::kMainSlots == 120);
        VOXEL_CHECK(voxel::inventory::PlayerInventory::kHotbarSlots == 12);
        VOXEL_CHECK(voxel::inventory::PlayerInventory::kAccessorySlots == 6);
        VOXEL_CHECK(inventory.mainInventory().slotCount() == 120);
        VOXEL_CHECK(inventory.hotbarInventory().slotCount() == 12);
        VOXEL_CHECK(inventory.accessoryInventory().slotKind(0) == voxel::inventory::SlotKind::Accessory);
        VOXEL_CHECK(inventory.equipmentInventory().slotKind(5) == voxel::inventory::SlotKind::Back);

        auto overflow = inventory.hotbarInventory().insertAt(0, {stoneItem, 1200, 0}, loadedItems);
        VOXEL_CHECK(inventory.hotbarInventory().slot(0).count == 999);
        VOXEL_CHECK(overflow.count == 201);
        inventory.selectHotbarSlot(0);
        VOXEL_CHECK(inventory.selectedHotbarItem().itemId == stoneItem);

        voxel::inventory::Inventory filtered(2);
        filtered.setSlotKind(0, voxel::inventory::SlotKind::Fuel);
        filtered.setSlotKind(1, voxel::inventory::SlotKind::Helmet);
        VOXEL_CHECK(filtered.canInsertAt(0, {coalItem, 1, 0}, loadedItems));
        VOXEL_CHECK(!filtered.canInsertAt(0, {stoneItem, 1, 0}, loadedItems));
        VOXEL_CHECK(!filtered.canInsertAt(1, {stoneItem, 1, 0}, loadedItems));

        voxel::inventory::Inventory locked(1);
        locked.setSlotLocked(0, true);
        auto lockedRemainder = locked.insertAt(0, {stoneItem, 1, 0}, loadedItems);
        VOXEL_CHECK(lockedRemainder.count == 1);
        locked.setSlotFavorite(0, true);
        VOXEL_CHECK(locked.slotFavorite(0));

        voxel::inventory::Inventory swapFiltered(2);
        swapFiltered.setSlotKind(0, voxel::inventory::SlotKind::Helmet);
        swapFiltered.setSlotKind(1, voxel::inventory::SlotKind::Generic);
        VOXEL_CHECK(swapFiltered.insertAt(1, {stoneItem, 1, 0}, loadedItems).empty());
        VOXEL_CHECK(!swapFiltered.swapSlots(0, 1, loadedItems));

        voxel::inventory::Inventory movement(3);
        VOXEL_CHECK(movement.insertAt(0, {stoneItem, 500, 0}, loadedItems).empty());
        voxel::inventory::ItemStack carried{stoneItem, 600, 0};
        VOXEL_CHECK(movement.tryMergeIntoSlot(0, carried, loadedItems));
        VOXEL_CHECK(movement.slot(0).count == 999);
        VOXEL_CHECK(carried.count == 101);
        VOXEL_CHECK(movement.placeOneIntoSlot(1, carried, loadedItems));
        VOXEL_CHECK(movement.slot(1).count == 1);
        VOXEL_CHECK(carried.count == 100);
        const auto half = movement.takeHalf(0);
        VOXEL_CHECK(half.has_value());
        VOXEL_CHECK(half->count == 500);
        VOXEL_CHECK(movement.slot(0).count == 499);

        voxel::inventory::Inventory destination(1);
        VOXEL_CHECK(movement.moveSlotTo(1, destination, loadedItems));
        VOXEL_CHECK(movement.slot(1).empty());
        VOXEL_CHECK(destination.slot(0).count == 1);

        voxel::inventory::PlayerInventory serialInventory;
        VOXEL_CHECK(serialInventory.hotbarInventory().insertAt(0, {stoneItem, 42, 0}, loadedItems).empty());
        VOXEL_CHECK(serialInventory.mainInventory().insertAt(7, {coalItem, 17, 0}, loadedItems).empty());
        serialInventory.hotbarInventory().setSlotLocked(0, true);
        serialInventory.hotbarInventory().setSlotFavorite(0, true);
        serialInventory.selectHotbarSlot(11);

        const auto json = voxel::inventory::serializePlayerInventoryJson(serialInventory, loadedItems);
        VOXEL_CHECK(json.find("core:stone") != std::string::npos);
        voxel::inventory::PlayerInventory roundTrip;
        VOXEL_CHECK(voxel::inventory::deserializePlayerInventoryJson(json, roundTrip, loadedItems));
        VOXEL_CHECK(roundTrip.selectedHotbarSlot() == 11);
        VOXEL_CHECK(roundTrip.hotbarInventory().slot(0).itemId == stoneItem);
        VOXEL_CHECK(roundTrip.hotbarInventory().slot(0).count == 42);
        VOXEL_CHECK(roundTrip.hotbarInventory().slotLocked(0));
        VOXEL_CHECK(roundTrip.hotbarInventory().slotFavorite(0));
        VOXEL_CHECK(roundTrip.mainInventory().slot(7).itemId == coalItem);
        VOXEL_CHECK(roundTrip.mainInventory().slot(7).count == 17);

        const std::filesystem::path playerSaveRoot = "build/test_saves/player_inventory";
        std::filesystem::remove_all(playerSaveRoot);
        voxel::save::PlayerInventorySaveService playerInventorySave;
        VOXEL_CHECK(playerInventorySave.inventoryPath(playerSaveRoot).filename() == "inventory.json");
        VOXEL_CHECK(playerInventorySave.save(playerSaveRoot, serialInventory, loadedItems));
        VOXEL_CHECK(std::filesystem::exists(playerInventorySave.inventoryPath(playerSaveRoot)));
        voxel::inventory::PlayerInventory loadedInventory;
        VOXEL_CHECK(playerInventorySave.load(playerSaveRoot, loadedInventory, loadedItems));
        VOXEL_CHECK(loadedInventory.selectedHotbarSlot() == 11);
        VOXEL_CHECK(loadedInventory.hotbarInventory().slot(0).itemId == stoneItem);
        VOXEL_CHECK(loadedInventory.hotbarInventory().slot(0).count == 42);
        VOXEL_CHECK(loadedInventory.hotbarInventory().slotLocked(0));
        VOXEL_CHECK(loadedInventory.mainInventory().slot(7).itemId == coalItem);
        std::filesystem::remove_all(playerSaveRoot);
    }

    voxel::world::VoxelRaycaster raycaster;
    const auto hit = raycaster.cast(chunks, {{0.5F, 5.0F, 0.5F}, {0.0F, -1.0F, 0.0F}, 16.0F});
    VOXEL_CHECK(hit.has_value());
    const voxel::world::ChunkCoord expectedHitChunk{0, 0, 0};
    VOXEL_CHECK(hit->position.chunk == expectedHitChunk);
    VOXEL_CHECK(hit->position.block.x == 0);
    VOXEL_CHECK(hit->position.block.y == 0);
    VOXEL_CHECK(hit->position.block.z == 0);
    VOXEL_CHECK(hit->normal.y == 1);

    voxel::world::BlockEditor editor(blocks);
    const auto breakResult = editor.breakBlock(chunks, hit->position);
    VOXEL_CHECK(breakResult.changed);
    VOXEL_CHECK(breakResult.deltas.size() == 1);
    VOXEL_CHECK(origin.blockAt(0, 0, 0).value == voxel::world::AirBlockState.value);
    const auto placeResult = editor.placeBlock(chunks, hit->position, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
    VOXEL_CHECK(placeResult.changed);
    VOXEL_CHECK(voxel::world::blockTypeOf(origin.blockAt(0, 0, 0)).value == 2);

    // ------------------------------------------------------------------
    // First-person player controller foundation (Phase H1-H5)
    // ------------------------------------------------------------------
    {
        voxel::world::ChunkManager playerChunks;
        auto& ground = playerChunks.createOrGet({0, 0, 0});
        ground.markLoaded(1);
        for (int z = 0; z < voxel::world::ChunkSize; ++z) {
            for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                ground.setBlockSilently(x, 0, z, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
            }
        }

        voxel::player::PlayerController player({.height = 1.8F, .radius = 0.3F});
        player.setPosition(voxel::core::Vec3{4.5F, 4.0F, 4.5F});
        for (int i = 0; i < 240 && !player.grounded(); ++i) {
            player.tick(playerChunks, {}, 1.0F / 60.0F);
        }
        VOXEL_CHECK(player.grounded());
        VOXEL_CHECK(player.position().y >= 0.99F && player.position().y <= 1.05F);

        player.setPosition(voxel::core::Vec3{8.5F, 4.0F, 8.5F});
        player.tick(playerChunks, {.jump = true}, 1.0F / 60.0F);
        VOXEL_CHECK(player.velocity().y <= 0.0F);
        for (int i = 0; i < 240 && !player.grounded(); ++i) {
            player.tick(playerChunks, {}, 1.0F / 60.0F);
        }
        player.tick(playerChunks, {.jump = true}, 1.0F / 60.0F);
        VOXEL_CHECK(player.velocity().y > 0.0F);

        auto& wallChunk = playerChunks.createOrGet({1, 0, 0});
        wallChunk.markLoaded(1);
        wallChunk.setBlockSilently(0, 1, 4, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        player.setPosition(voxel::core::Vec3{31.5F, 1.0F, 4.5F});
        player.setLook(0.0F, 0.0F);
        for (int i = 0; i < 60; ++i) {
            player.tick(playerChunks, {.right = true}, 1.0F / 60.0F);
        }
        VOXEL_CHECK(player.position().x < 31.72F);

        const voxel::world::ChunkCoord overlapChunk{0, 0, 0};
        const voxel::world::BlockCoord overlapBlock{4, 1, 4};
        player.setPosition(voxel::core::Vec3{4.5F, 1.0F, 4.5F});
        VOXEL_CHECK(player.overlapsBlock(overlapChunk, overlapBlock));
        const bool appWouldRejectPlace = player.overlapsBlock(overlapChunk, overlapBlock);
        VOXEL_CHECK(appWouldRejectPlace);

        voxel::world::ChunkManager rayChunks;
        auto& rayChunk = rayChunks.createOrGet({0, 0, 0});
        rayChunk.markLoaded(1);
        rayChunk.setBlockSilently(7, 1, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        voxel::world::VoxelRaycaster reachRaycaster;
        const auto tooFar = reachRaycaster.cast(rayChunks, {{0.5F, 1.5F, 0.5F}, {1.0F, 0.0F, 0.0F}, 4.0F});
        const auto inReach = reachRaycaster.cast(rayChunks, {{0.5F, 1.5F, 0.5F}, {1.0F, 0.0F, 0.0F}, 8.0F});
        VOXEL_CHECK(!tooFar.has_value());
        VOXEL_CHECK(inReach.has_value());

        voxel::player::CreativeHotbar hotbar;
        VOXEL_CHECK(voxel::player::CreativeHotbar::SlotCount == 12);
        for (std::size_t i = 0; i < voxel::player::CreativeHotbar::SlotCount; ++i) {
            VOXEL_CHECK(hotbar.validSlot(i));
            VOXEL_CHECK(hotbar.select(i));
            VOXEL_CHECK(hotbar.selected().block.value != voxel::world::AirBlockState.value);
        }
        VOXEL_CHECK(!hotbar.select(voxel::player::CreativeHotbar::SlotCount));

        voxel::player::PlayerSpawnResolver spawnResolver({.minWorldY = -16, .maxWorldY = 16});
        const auto spawn = spawnResolver.resolve(playerChunks, 4.5F, 4.5F);
        VOXEL_CHECK(spawn.has_value());
        VOXEL_CHECK(spawn->y >= 1.0F && spawn->y < 2.0F);
    }

    voxel::world::Chunk dirtyFlagChunk({19, 0, 0});
    dirtyFlagChunk.markLoaded(7);
    dirtyFlagChunk.clearDirty();
    const auto dirtyBaselineRevision = dirtyFlagChunk.revision();
    dirtyFlagChunk.markMeshDirtyNoRevision();
    VOXEL_CHECK(dirtyFlagChunk.dirty().mesh);
    VOXEL_CHECK(!dirtyFlagChunk.dirty().save);
    VOXEL_CHECK(dirtyFlagChunk.revision() == dirtyBaselineRevision);
    dirtyFlagChunk.clearMeshDirtyOnly();
    dirtyFlagChunk.markLightingDirtyNoRevision();
    VOXEL_CHECK(dirtyFlagChunk.dirty().lighting);
    VOXEL_CHECK(!dirtyFlagChunk.dirty().save);
    VOXEL_CHECK(dirtyFlagChunk.revision() == dirtyBaselineRevision);
    dirtyFlagChunk.clearLightingDirtyOnly();
    dirtyFlagChunk.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
    VOXEL_CHECK(dirtyFlagChunk.dirty().mesh);
    VOXEL_CHECK(dirtyFlagChunk.dirty().lighting);
    VOXEL_CHECK(dirtyFlagChunk.dirty().save);
    VOXEL_CHECK(dirtyFlagChunk.revision() == dirtyBaselineRevision + 1);

    {
        voxel::world::Chunk original({18, 0, 0});
        original.setBlockSilently(1, 1, 1, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        voxel::world::Chunk snapshot = original;
        original.setBlockSilently(1, 1, 1, voxel::world::makeBlockState(voxel::BlockTypeId{3}));
        VOXEL_CHECK(voxel::world::blockTypeOf(snapshot.blockAt(1, 1, 1)).value == 2);
        VOXEL_CHECK(voxel::world::blockTypeOf(original.blockAt(1, 1, 1)).value == 3);

        snapshot.fillColumnRangeSilently(2, 2, 0, 3, voxel::world::makeBlockState(voxel::BlockTypeId{4}));
        VOXEL_CHECK(original.blockAt(2, 0, 2).value == voxel::world::AirBlockState.value);
        VOXEL_CHECK(voxel::world::blockTypeOf(snapshot.blockAt(2, 0, 2)).value == 4);
    }

    auto& borderChunk = chunks.createOrGet({20, 0, 0});
    auto& neighborChunk = chunks.createOrGet({21, 0, 0});
    borderChunk.markLoaded(1);
    neighborChunk.markLoaded(1);
    borderChunk.setBlock(31, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
    neighborChunk.clearDirty();
    const auto borderBreak = editor.breakBlock(
        chunks,
        {{0}, {}, {20, 0, 0}, {31, 0, 0}});
    VOXEL_CHECK(borderBreak.changed);
    VOXEL_CHECK(neighborChunk.dirty().mesh);
    VOXEL_CHECK(neighborChunk.dirty().lighting);
    VOXEL_CHECK(!neighborChunk.dirty().save);
    VOXEL_CHECK(neighborChunk.revision() == 1);

    const auto crossChunkPlace = editor.placeBlock(
        chunks,
        {{0}, {}, {30, 0, 0}, {32, 0, 0}},
        voxel::world::makeBlockState(voxel::BlockTypeId{2}));
    VOXEL_CHECK(crossChunkPlace.changed);
    const voxel::world::ChunkCoord placedNeighborCoord{31, 0, 0};
    auto* placedNeighbor = chunks.find(placedNeighborCoord);
    VOXEL_CHECK(placedNeighbor != nullptr);
    VOXEL_CHECK(voxel::world::blockTypeOf(placedNeighbor->blockAt(0, 0, 0)).value == 2);

    {
        voxel::world::ChunkManager queuedChunks;
        auto& queued = queuedChunks.createOrGet({0, 0, 0});
        queued.markLoaded(1);
        queued.setBlock(1, 1, 1, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        queued.setBlock(2, 1, 1, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        queued.clearDirty();

        voxel::world::BlockEditQueue queue;
        queue.enqueueBreak({0, {}, {0, 0, 0}, {1, 1, 1}});
        queue.enqueueBreak({0, {}, {0, 0, 0}, {2, 1, 1}});
        const auto queueStats = queue.flush(queuedChunks, editor);
        VOXEL_CHECK(queueStats.accepted == 2);
        VOXEL_CHECK(queueStats.rejected == 0);
        VOXEL_CHECK(queueStats.dirtyMeshQueued == 1);
        VOXEL_CHECK(queueStats.dirtyMeshCoalesced == 1);
        VOXEL_CHECK(queued.dirty().save);
    }

    {
        voxel::world::ChunkDirtyQueue dirtyQueue;
        const voxel::world::ChunkCoord center{0, 0, 0};
        VOXEL_CHECK(dirtyQueue.enqueue({4, 0, 0}, 1, 1, 10.0F));
        VOXEL_CHECK(!dirtyQueue.enqueue({4, 0, 0}, 2, 2, -5.0F));
        VOXEL_CHECK(dirtyQueue.enqueue({1, 0, 0}, 1, 1));
        VOXEL_CHECK(dirtyQueue.enqueue({0, 3, 0}, 1, 1));
        VOXEL_CHECK(dirtyQueue.size() == 3);

        const auto ordered = dirtyQueue.popClosest(center, 3);
        VOXEL_CHECK(ordered.size() == 3);
        VOXEL_CHECK(ordered[0].coord.x == 4 && ordered[0].coord.y == 0 && ordered[0].coord.z == 0);
        VOXEL_CHECK(ordered[0].revision == 2);
        VOXEL_CHECK(ordered[0].priority == -5.0F);
        VOXEL_CHECK(ordered[1].coord.x == 1 && ordered[1].coord.y == 0 && ordered[1].coord.z == 0);
        VOXEL_CHECK(ordered[2].coord.x == 0 && ordered[2].coord.y == 3 && ordered[2].coord.z == 0);
        VOXEL_CHECK(dirtyQueue.size() == 0);
    }

    voxel::world::ChunkManager kineticChunks;
    auto& kineticChunk = kineticChunks.createOrGet({0, 0, 0});
    kineticChunk.markLoaded(1);
    kineticChunk.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{5}));
    kineticChunk.setBlock(1, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{6}));
    kineticChunk.setBlock(2, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{7}));
    const auto kineticResult = voxel::automation::KineticNetworkSolver{}.solve(kineticChunks, kineticCatalog);
    VOXEL_CHECK(kineticResult.networks.size() == 1);
    VOXEL_CHECK(kineticResult.networks.front().sourceCount == 1);
    VOXEL_CHECK(kineticResult.networks.front().consumerCount == 1);
    VOXEL_CHECK(!kineticResult.networks.front().overloaded);
    VOXEL_CHECK(kineticResult.networks.front().rpm == 32.0F);

    kineticChunk.setBlock(3, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{7}));
    kineticChunk.setBlock(4, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{7}));
    kineticChunk.setBlock(5, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{7}));
    const auto overloadedResult = voxel::automation::KineticNetworkSolver{}.solve(kineticChunks, kineticCatalog);
    VOXEL_CHECK(overloadedResult.networks.size() == 1);
    VOXEL_CHECK(overloadedResult.networks.front().overloaded);
    VOXEL_CHECK(overloadedResult.networks.front().rpm == 0.0F);
    VOXEL_CHECK(overloadedResult.overloadedNetworks == 1);

    // F6: nodes carry their network id, and the network exposes a representative
    // node coordinate that callers (e.g. the debug log) can render.
    VOXEL_CHECK(overloadedResult.nodes.size() == overloadedResult.networks.front().nodeCount);
    for (const auto& node : overloadedResult.nodes) {
        VOXEL_CHECK(node.networkId == overloadedResult.networks.front().id);
    }
    const auto& repNet = overloadedResult.networks.front();
    const auto& rep = repNet.representativeNode;
    const voxel::world::ChunkCoord originChunk{0, 0, 0};
    VOXEL_CHECK(rep.chunk == originChunk);
    VOXEL_CHECK(rep.block.x >= 0 && rep.block.x < voxel::world::ChunkSize);

    voxel::world::ChunkStreamer streamer(chunks);
    streamer.pump({0, 0, 0}, {.renderDistanceChunks = 4, .simulationDistanceChunks = 1, .physicsDistanceChunks = 1});
    const voxel::world::ChunkCoord minStreamChunk{-1, -1, -1};
    const voxel::world::ChunkCoord maxStreamChunk{1, 1, 1};
    VOXEL_CHECK(chunks.find(minStreamChunk) != nullptr);
    VOXEL_CHECK(chunks.find(maxStreamChunk) != nullptr);
    VOXEL_CHECK(chunks.residentCount() >= 27);

    voxel::world::StreamingSettings pinnedSeaSettings{};
    pinnedSeaSettings.renderDistanceChunks = 2;
    pinnedSeaSettings.verticalRenderDistanceChunks = 1;
    pinnedSeaSettings.pinnedVerticalChunkY = 0;
    const auto highRequests = streamer.planRequests({0, 8, 0}, pinnedSeaSettings);
    bool containsPinnedSeaLayer = false;
    bool containsNormalVerticalBand = false;
    for (const auto& request : highRequests) {
        containsPinnedSeaLayer = containsPinnedSeaLayer || request.coord == voxel::world::ChunkCoord{0, 0, 0};
        containsNormalVerticalBand = containsNormalVerticalBand || request.coord == voxel::world::ChunkCoord{0, 8, 0};
    }
    VOXEL_CHECK(containsPinnedSeaLayer);
    VOXEL_CHECK(containsNormalVerticalBand);
    VOXEL_CHECK(highRequests.size() == static_cast<std::size_t>((2 * 2 + 1) * (2 * 1 + 1) * (2 * 2 + 1) + (2 * 2 + 1) * (2 * 2 + 1) * 3));

    const auto nearSeaRequests = streamer.planRequests({0, 0, 0}, pinnedSeaSettings);
    VOXEL_CHECK(nearSeaRequests.size() == static_cast<std::size_t>((2 * 2 + 1) * (2 * 1 + 1) * (2 * 2 + 1)));

    voxel::render::meshing::GreedyMesher mesher;
    const auto sameMesh = [](const voxel::render::meshing::ChunkMesh& lhs,
                             const voxel::render::meshing::ChunkMesh& rhs) {
        if (lhs.vertices.size() != rhs.vertices.size()
            || lhs.indices != rhs.indices
            || lhs.drawRanges.size() != rhs.drawRanges.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.vertices.size(); ++i) {
            if (lhs.vertices[i].packedPos != rhs.vertices[i].packedPos
                || lhs.vertices[i].packedUv != rhs.vertices[i].packedUv
                || lhs.vertices[i].packedLight != rhs.vertices[i].packedLight
                || lhs.vertices[i].packedMaterial != rhs.vertices[i].packedMaterial) {
                return false;
            }
        }
        for (std::size_t i = 0; i < lhs.drawRanges.size(); ++i) {
            if (lhs.drawRanges[i].surface != rhs.drawRanges[i].surface
                || lhs.drawRanges[i].materialId != rhs.drawRanges[i].materialId
                || lhs.drawRanges[i].indexOffset != rhs.drawRanges[i].indexOffset
                || lhs.drawRanges[i].indexCount != rhs.drawRanges[i].indexCount) {
                return false;
            }
        }
        return lhs.sections[0].opaqueIndexCount == rhs.sections[0].opaqueIndexCount
            && lhs.sections[0].cutoutIndexCount == rhs.sections[0].cutoutIndexCount
            && lhs.sections[0].transparentIndexCount == rhs.sections[0].transparentIndexCount;
    };
    const auto mesh = mesher.build(origin);
    VOXEL_CHECK(mesh.sourceRevision == origin.revision());
    VOXEL_CHECK(!mesh.vertices.empty());
    VOXEL_CHECK(!mesh.indices.empty());

    voxel::world::Chunk singleBlockChunk({10, 0, 0});
    singleBlockChunk.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
    const auto singleBlockMesh = mesher.build(singleBlockChunk);
    const auto singleBlockFaces = mesher.classifyVisibleFaces(
        singleBlockChunk, renderCatalog, nullptr, voxel::render::meshing::ChunkNeighborhood{});
    const auto singleBlockHybridMesh = mesher.buildFromVisibleFaces(singleBlockChunk, singleBlockFaces);
    VOXEL_CHECK(singleBlockFaces.size() == 6);
    VOXEL_CHECK(sameMesh(singleBlockMesh, singleBlockHybridMesh));
    VOXEL_CHECK(singleBlockMesh.vertices.size() == 24);
    VOXEL_CHECK(singleBlockMesh.indices.size() == 36);
    VOXEL_CHECK(singleBlockMesh.drawRanges.size() == 1);
    VOXEL_CHECK(singleBlockMesh.drawRanges.front().materialId == voxel::world::makeBlockState(voxel::BlockTypeId{2}).value);

    voxel::world::Chunk twoBlockChunk({11, 0, 0});
    twoBlockChunk.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
    twoBlockChunk.setBlock(1, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
    const auto mergedMesh = mesher.build(twoBlockChunk);
    const auto twoBlockFaces = mesher.classifyVisibleFaces(
        twoBlockChunk, renderCatalog, nullptr, voxel::render::meshing::ChunkNeighborhood{});
    VOXEL_CHECK(twoBlockFaces.size() == 10);
    VOXEL_CHECK(sameMesh(mergedMesh, mesher.buildFromVisibleFaces(twoBlockChunk, twoBlockFaces)));
    VOXEL_CHECK(mergedMesh.vertices.size() == 24);
    VOXEL_CHECK(mergedMesh.indices.size() == 36);
    VOXEL_CHECK(mergedMesh.drawRanges.size() == 1);

    voxel::world::Chunk materialSplitChunk({12, 0, 0});
    materialSplitChunk.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
    materialSplitChunk.setBlock(1, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{3}));
    const auto splitMesh = mesher.build(materialSplitChunk);
    const auto splitCatalogMesh = mesher.build(materialSplitChunk, renderCatalog);
    const auto splitFaces = mesher.classifyVisibleFaces(
        materialSplitChunk, renderCatalog, nullptr, voxel::render::meshing::ChunkNeighborhood{});
    VOXEL_CHECK(sameMesh(splitCatalogMesh, mesher.buildFromVisibleFaces(materialSplitChunk, splitFaces)));
    VOXEL_CHECK(splitMesh.vertices.size() == 40);
    VOXEL_CHECK(splitMesh.indices.size() == 60);
    VOXEL_CHECK(splitMesh.drawRanges.size() == 2);

    voxel::world::Chunk transparentChunk({13, 0, 0});
    transparentChunk.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{3}));
    transparentChunk.setBlock(1, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{3}));
    const auto transparentMesh = mesher.build(transparentChunk, renderCatalog);
    VOXEL_CHECK(transparentMesh.drawRanges.size() == 1);
    VOXEL_CHECK(transparentMesh.drawRanges.front().surface == voxel::render::meshing::MeshSurface::Transparent);
    VOXEL_CHECK(transparentMesh.vertices.front().packedLight != 0);

    voxel::world::Chunk waterPairChunk({14, 0, 0});
    waterPairChunk.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{12}));
    waterPairChunk.setBlock(1, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{12}));
    const auto waterPairMesh = mesher.build(waterPairChunk, renderCatalog);
    VOXEL_CHECK(waterPairMesh.vertices.size() == 24);
    VOXEL_CHECK(waterPairMesh.indices.size() == 36);
    VOXEL_CHECK(waterPairMesh.drawRanges.size() == 1);
    VOXEL_CHECK(waterPairMesh.drawRanges.front().surface == voxel::render::meshing::MeshSurface::Transparent);

    voxel::world::Chunk waterLeft({15, 0, 0});
    voxel::world::Chunk waterRight({16, 0, 0});
    waterLeft.setBlock(voxel::world::ChunkSize - 1, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{12}));
    waterRight.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{12}));
    voxel::render::meshing::ChunkNeighborhood waterNeighborhood{};
    waterNeighborhood.posX = &waterRight;
    const auto waterSeamMesh = mesher.build(waterLeft, renderCatalog, nullptr, waterNeighborhood);
    bool hasPositiveXSeamFace = false;
    for (const auto& vertex : waterSeamMesh.vertices) {
        const auto x = static_cast<int>(vertex.packedPos & 63U);
        const auto face = (vertex.packedPos >> 18U) & 7U;
        if (x == voxel::world::ChunkSize && face == 0U) {
            hasPositiveXSeamFace = true;
            break;
        }
    }
    VOXEL_CHECK(!hasPositiveXSeamFace);

    // Hybrid-path parity check across a chunk seam — classifyVisibleFaces
    // + buildFromVisibleFaces with a non-null neighbour must match the
    // regular build path. If this diverges, the bug that's manifesting as
    // missing faces in-game lives in classifyVisibleFaces (CPU side) and
    // not in the GPU classifier shader.
    const auto waterSeamFaces = mesher.classifyVisibleFaces(
        waterLeft, renderCatalog, nullptr, waterNeighborhood);
    VOXEL_CHECK(sameMesh(waterSeamMesh, mesher.buildFromVisibleFaces(waterLeft, waterSeamFaces)));

    // Boundary solid block with a solid neighbour: the +X face of the
    // edge cell must be culled (touches solid neighbour), every other
    // exposed face must survive. This is the exact shape of the bug the
    // user reported — a solid block with one face seemingly missing.
    const auto hybridStoneState = voxel::world::makeBlockState(voxel::BlockTypeId{2});
    voxel::world::Chunk stoneLeft({22, 0, 0});
    voxel::world::Chunk stoneRight({23, 0, 0});
    stoneLeft.setBlock(voxel::world::ChunkSize - 1, 5, 5, hybridStoneState);
    stoneRight.setBlock(0, 5, 5, hybridStoneState);
    voxel::render::meshing::ChunkNeighborhood stoneNeighborhood{};
    stoneNeighborhood.posX = &stoneRight;
    const auto stoneSeamMesh = mesher.build(stoneLeft, renderCatalog, nullptr, stoneNeighborhood);
    const auto stoneSeamFaces = mesher.classifyVisibleFaces(
        stoneLeft, renderCatalog, nullptr, stoneNeighborhood);
    VOXEL_CHECK(sameMesh(stoneSeamMesh, mesher.buildFromVisibleFaces(stoneLeft, stoneSeamFaces)));

    // Mid-chunk solid block with all 6 face neighbours = air. All 6 faces
    // must be present. This is the simplest case where missing faces
    // would be immediately obvious. (Same content as singleBlockChunk but
    // positioned mid-chunk to exercise the bounded-loop expansion.)
    voxel::world::Chunk midBlockChunk({24, 0, 0});
    midBlockChunk.setBlock(15, 15, 15, hybridStoneState);
    const auto midBlockMesh = mesher.build(midBlockChunk, renderCatalog);
    const auto midBlockFaces = mesher.classifyVisibleFaces(
        midBlockChunk, renderCatalog, nullptr, voxel::render::meshing::ChunkNeighborhood{});
    VOXEL_CHECK(midBlockFaces.size() == 6);
    VOXEL_CHECK(sameMesh(midBlockMesh, mesher.buildFromVisibleFaces(midBlockChunk, midBlockFaces)));

    // Solid block at the chunk's lower-Y boundary with no -Y neighbour
    // chunk loaded. Conservative behaviour: -Y face must be visible.
    voxel::world::Chunk bottomBoundaryChunk({25, 0, 0});
    bottomBoundaryChunk.setBlock(5, 0, 5, hybridStoneState);
    const auto bottomMesh = mesher.build(bottomBoundaryChunk, renderCatalog);
    const auto bottomFaces = mesher.classifyVisibleFaces(
        bottomBoundaryChunk, renderCatalog, nullptr, voxel::render::meshing::ChunkNeighborhood{});
    VOXEL_CHECK(bottomFaces.size() == 6);
    VOXEL_CHECK(sameMesh(bottomMesh, mesher.buildFromVisibleFaces(bottomBoundaryChunk, bottomFaces)));

    // Solid block at the chunk's lower-Y boundary with the -Y neighbour
    // loaded and ITS top cell solid: the block's -Y face must be culled,
    // and the regular build / hybrid build must agree.
    voxel::world::Chunk bottomNeighbourChunk({25, -1, 0});
    bottomNeighbourChunk.setBlock(5, voxel::world::ChunkSize - 1, 5, hybridStoneState);
    voxel::render::meshing::ChunkNeighborhood bottomNeighborhood{};
    bottomNeighborhood.negY = &bottomNeighbourChunk;
    const auto bottomCulledMesh =
        mesher.build(bottomBoundaryChunk, renderCatalog, nullptr, bottomNeighborhood);
    const auto bottomCulledFaces = mesher.classifyVisibleFaces(
        bottomBoundaryChunk, renderCatalog, nullptr, bottomNeighborhood);
    VOXEL_CHECK(bottomCulledFaces.size() == 5); // 6 faces minus the culled -Y
    VOXEL_CHECK(sameMesh(bottomCulledMesh,
        mesher.buildFromVisibleFaces(bottomBoundaryChunk, bottomCulledFaces)));

    voxel::world::Chunk waterAgainstStoneChunk({19, 0, 0});
    const auto waterState = voxel::world::makeBlockState(voxel::BlockTypeId{12});
    const auto stoneState = voxel::world::makeBlockState(voxel::BlockTypeId{2});
    waterAgainstStoneChunk.setBlock(0, 0, 0, waterState);
    waterAgainstStoneChunk.setBlock(1, 0, 0, stoneState);
    const auto waterAgainstStoneMesh = mesher.build(waterAgainstStoneChunk, renderCatalog);
    bool hasWaterFaceAgainstStone = false;
    bool hasStoneFaceAgainstWater = false;
    for (const auto& vertex : waterAgainstStoneMesh.vertices) {
        const auto x = static_cast<int>(vertex.packedPos & 63U);
        const auto face = (vertex.packedPos >> 18U) & 7U;
        if (vertex.packedMaterial == waterState.value && x == 1 && face == 0U) {
            hasWaterFaceAgainstStone = true;
        }
        if (vertex.packedMaterial == stoneState.value && x == 1 && face == 1U) {
            hasStoneFaceAgainstWater = true;
        }
    }
    VOXEL_CHECK(!hasWaterFaceAgainstStone);
    VOXEL_CHECK(hasStoneFaceAgainstWater);

    voxel::render::meshing::MeshingOptions waterOptions{};
    waterOptions.staticWaterSurfaceY = 0;

    voxel::world::Chunk lowerWaterChunk({17, -1, 0});
    lowerWaterChunk.setBlock(0, voxel::world::ChunkSize - 1, 0, voxel::world::makeBlockState(voxel::BlockTypeId{12}));
    const auto lowerWaterMesh = mesher.build(lowerWaterChunk, renderCatalog, nullptr,
                                             voxel::render::meshing::ChunkNeighborhood{}, waterOptions);
    bool hasFalseLowerWaterCap = false;
    for (const auto& vertex : lowerWaterMesh.vertices) {
        const auto y = static_cast<int>((vertex.packedPos >> 6U) & 63U);
        const auto face = (vertex.packedPos >> 18U) & 7U;
        if (y == voxel::world::ChunkSize && face == 2U) {
            hasFalseLowerWaterCap = true;
            break;
        }
    }
    VOXEL_CHECK(!hasFalseLowerWaterCap);

    voxel::world::Chunk lowerWaterCavityChunk({20, -1, 0});
    lowerWaterCavityChunk.setBlock(0, 16, 0, voxel::world::makeBlockState(voxel::BlockTypeId{12}));
    const auto lowerWaterCavityMesh = mesher.build(lowerWaterCavityChunk, renderCatalog, nullptr,
                                                   voxel::render::meshing::ChunkNeighborhood{}, waterOptions);
    bool hasSubsurfaceWaterCap = false;
    for (const auto& vertex : lowerWaterCavityMesh.vertices) {
        const auto y = static_cast<int>((vertex.packedPos >> 6U) & 63U);
        const auto face = (vertex.packedPos >> 18U) & 7U;
        if (y == 17 && face == 2U) {
            hasSubsurfaceWaterCap = true;
            break;
        }
    }
    VOXEL_CHECK(!hasSubsurfaceWaterCap);

    voxel::world::Chunk surfaceWaterChunk({18, 0, 0});
    surfaceWaterChunk.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{12}));
    const auto surfaceWaterMesh = mesher.build(surfaceWaterChunk, renderCatalog, nullptr,
                                               voxel::render::meshing::ChunkNeighborhood{}, waterOptions);
    bool hasSurfaceWaterCap = false;
    for (const auto& vertex : surfaceWaterMesh.vertices) {
        const auto y = static_cast<int>((vertex.packedPos >> 6U) & 63U);
        const auto face = (vertex.packedPos >> 18U) & 7U;
        if (y == 1 && face == 2U) {
            hasSurfaceWaterCap = true;
            break;
        }
    }
    VOXEL_CHECK(hasSurfaceWaterCap);

    auto streamingEdgeWaterChunk = std::make_unique<voxel::world::Chunk>(voxel::world::ChunkCoord{21, 0, 0});
    streamingEdgeWaterChunk->setBlock(voxel::world::ChunkSize - 1, 0, 0,
                                      voxel::world::makeBlockState(voxel::BlockTypeId{12}));
    const auto streamingEdgeWaterMesh = mesher.build(*streamingEdgeWaterChunk, renderCatalog, nullptr,
                                                     voxel::render::meshing::ChunkNeighborhood{}, waterOptions);
    bool hasMissingNeighborWaterWall = false;
    for (const auto& vertex : streamingEdgeWaterMesh.vertices) {
        const auto x = static_cast<int>(vertex.packedPos & 63U);
        const auto face = (vertex.packedPos >> 18U) & 7U;
        if (x == voxel::world::ChunkSize && face == 0U) {
            hasMissingNeighborWaterWall = true;
            break;
        }
    }
    VOXEL_CHECK(!hasMissingNeighborWaterWall);

    const auto unboundedEdgeWaterMesh = mesher.build(*streamingEdgeWaterChunk, renderCatalog);
    bool hasConservativeWaterWallWithoutStaticSea = false;
    for (const auto& vertex : unboundedEdgeWaterMesh.vertices) {
        const auto x = static_cast<int>(vertex.packedPos & 63U);
        const auto face = (vertex.packedPos >> 18U) & 7U;
        if (x == voxel::world::ChunkSize && face == 0U) {
            hasConservativeWaterWallWithoutStaticSea = true;
            break;
        }
    }
    VOXEL_CHECK(hasConservativeWaterWallWithoutStaticSea);

    voxel::render::meshing::ChunkMeshCache meshCache;
    meshCache.store(singleBlockChunk.coord(), singleBlockMesh);
    VOXEL_CHECK(meshCache.size() == 1);
    VOXEL_CHECK(meshCache.isCurrent(singleBlockChunk.coord(), singleBlockChunk.revision()));
    meshCache.store({100, 0, 100}, singleBlockMesh);
    const auto evictedMeshes = meshCache.removeOutsideRadius({10, 0, 0}, 8, 1);
    VOXEL_CHECK(evictedMeshes.size() == 1);
    VOXEL_CHECK(meshCache.size() == 1);

    voxel::save::RegionFileStore saveStore("build/test_saves/smoke_world");
    const auto savedDirty = voxel::save::WorldSaveService{}.saveDirtyChunks(chunks, saveStore);
    VOXEL_CHECK(savedDirty >= 1);
    VOXEL_CHECK(!origin.dirty().save);
    VOXEL_CHECK(origin.dirty().mesh);
    saveStore.saveChunk(origin);
    const auto loaded = saveStore.loadChunk(origin.coord());
    VOXEL_CHECK(loaded.has_value());
    VOXEL_CHECK(loaded->coord() == origin.coord());
    VOXEL_CHECK(loaded->revision() == origin.revision());
    VOXEL_CHECK(loaded->blockAt(0, 0, 0).value == origin.blockAt(0, 0, 0).value);
    VOXEL_CHECK(loaded->blockAt(1, 2, 3).value == origin.blockAt(1, 2, 3).value);

    {
        voxel::world::ChunkManager saveBudgetChunks;
        auto& a = saveBudgetChunks.createOrGet({200, 0, 0});
        auto& b = saveBudgetChunks.createOrGet({201, 0, 0});
        a.markLoaded(1);
        b.markLoaded(1);
        a.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        b.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        voxel::save::WorldSaveService saveService;
        VOXEL_CHECK(saveService.dirtyChunkCount(saveBudgetChunks) == 2);
        const auto savedOne = saveService.saveDirtyChunks(saveBudgetChunks, saveStore, 1);
        VOXEL_CHECK(savedOne == 1);
        VOXEL_CHECK(saveService.dirtyChunkCount(saveBudgetChunks) == 1);
        const auto savedRest = saveService.saveDirtyChunks(saveBudgetChunks, saveStore, 8);
        VOXEL_CHECK(savedRest == 1);
        VOXEL_CHECK(saveService.dirtyChunkCount(saveBudgetChunks) == 0);
    }
    {
        voxel::world::ChunkManager asyncSaveChunks;
        const voxel::world::ChunkCoord firstCoord{220, 0, 0};
        const voxel::world::ChunkCoord secondCoord{221, 0, 0};
        auto& first = asyncSaveChunks.createOrGet(firstCoord);
        auto& second = asyncSaveChunks.createOrGet(secondCoord);
        first.markLoaded(1);
        second.markLoaded(1);
        first.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        second.setBlock(1, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));

        voxel::save::WorldSaveService saveService;
        voxel::save::SaveCoordinator coordinator;
        voxel::core::JobSystem saveJobs;
        saveJobs.start(1);
        voxel::save::SaveCoordinatorSettings saveSettings{};
        saveSettings.saveRoot = "build/test_saves/save_coordinator";
        saveSettings.maxSavesPerFlush = 1;
        saveSettings.saveFlushIntervalFrames = 120;

        const auto idleStats = coordinator.flushPending(
            false, asyncSaveChunks, saveService, saveJobs, saveSettings, 1);
        VOXEL_CHECK(idleStats.saveQueueLength == 2);
        VOXEL_CHECK(coordinator.pendingJobCount() == 0);
        VOXEL_CHECK(saveService.dirtyChunkCount(asyncSaveChunks) == 2);

        const auto firstFlush = coordinator.flushPending(
            true, asyncSaveChunks, saveService, saveJobs, saveSettings, 1);
        VOXEL_CHECK(firstFlush.saveBudgetSaturated == 1);
        VOXEL_CHECK(coordinator.pendingJobCount() == 1);
        VOXEL_CHECK(saveService.dirtyChunkCount(asyncSaveChunks) == 1);
        saveJobs.waitAll();
        VOXEL_CHECK(coordinator.drainCompleted(true) == 1);

        const auto secondFlush = coordinator.flushPending(
            true, asyncSaveChunks, saveService, saveJobs, saveSettings, 1);
        VOXEL_CHECK(secondFlush.saveQueueLength == 1);
        saveJobs.waitAll();
        VOXEL_CHECK(coordinator.drainCompleted(true) == 1);
        VOXEL_CHECK(coordinator.pendingJobCount() == 0);
        VOXEL_CHECK(saveService.dirtyChunkCount(asyncSaveChunks) == 0);

        voxel::save::RegionFileStore coordinatorStore(saveSettings.saveRoot);
        VOXEL_CHECK(coordinatorStore.loadChunk(firstCoord).has_value());
        VOXEL_CHECK(coordinatorStore.loadChunk(secondCoord).has_value());
        saveJobs.stop();
    }

    voxel::world::ChunkManager pipelineChunks;
    voxel::world::ChunkStreamer pipelineStreamer(pipelineChunks);
    const auto pipelineRequests = pipelineStreamer.planRequests({0, 0, 0}, {.renderDistanceChunks = 2, .simulationDistanceChunks = 0, .physicsDistanceChunks = 0});
    const voxel::world::ChunkCoord originCoord{0, 0, 0};
    VOXEL_CHECK(pipelineRequests.front().coord == originCoord);
    const auto forwardRequests = pipelineStreamer.planRequests(
        {0, 0, 0},
        {.renderDistanceChunks = 2, .verticalRenderDistanceChunks = 0, .simulationDistanceChunks = 0, .physicsDistanceChunks = 0},
        {1.0F, 0.0F, 0.0F});
    auto plusOneIt = std::find_if(forwardRequests.begin(), forwardRequests.end(), [](const voxel::world::ChunkRequest& request) {
        const voxel::world::ChunkCoord wanted{1, 0, 0};
        return request.coord == wanted;
    });
    auto minusOneIt = std::find_if(forwardRequests.begin(), forwardRequests.end(), [](const voxel::world::ChunkRequest& request) {
        const voxel::world::ChunkCoord wanted{-1, 0, 0};
        return request.coord == wanted;
    });
    VOXEL_CHECK(plusOneIt != forwardRequests.end());
    VOXEL_CHECK(minusOneIt != forwardRequests.end());
    VOXEL_CHECK(plusOneIt < minusOneIt);
    const auto pinnedSeaRequests = pipelineStreamer.planRequests(
        {0, 8, 0},
        {.renderDistanceChunks = 2, .verticalRenderDistanceChunks = 0,
         .simulationDistanceChunks = 0, .physicsDistanceChunks = 0,
         .pinnedVerticalChunkY = 0, .pinnedVerticalChunkRadius = 1});
    auto seaCenterIt = std::find_if(pinnedSeaRequests.begin(), pinnedSeaRequests.end(), [](const voxel::world::ChunkRequest& request) {
        return request.coord == voxel::world::ChunkCoord{0, 0, 0};
    });
    auto seaBelowIt = std::find_if(pinnedSeaRequests.begin(), pinnedSeaRequests.end(), [](const voxel::world::ChunkRequest& request) {
        return request.coord == voxel::world::ChunkCoord{0, -1, 0};
    });
    auto normalEdgeIt = std::find_if(pinnedSeaRequests.begin(), pinnedSeaRequests.end(), [](const voxel::world::ChunkRequest& request) {
        return request.coord == voxel::world::ChunkCoord{2, 8, 0};
    });
    VOXEL_CHECK(seaCenterIt != pinnedSeaRequests.end());
    VOXEL_CHECK(seaBelowIt != pinnedSeaRequests.end());
    VOXEL_CHECK(normalEdgeIt != pinnedSeaRequests.end());
    VOXEL_CHECK(seaCenterIt < normalEdgeIt);
    voxel::world::ChunkPipeline pipeline;
    const auto pipelineStats = pipeline.processRequests(
        pipelineChunks,
        saveStore,
        generator,
        pipelineRequests,
        {.maxLoadsOrGenerationsPerTick = 1});
    VOXEL_CHECK(pipelineStats.loaded + pipelineStats.generated == 1);
    VOXEL_CHECK(pipelineChunks.residentCount() == 1);
    VOXEL_CHECK(pipelineChunks.find({0, 0, 0}) != nullptr);

    // Installing a newly generated neighbor dirties adjacent mesh/lighting only;
    // it must not bump revisions or mark those chunks as needing save.
    {
        voxel::world::ChunkManager neighborChunks;
        auto& existing = neighborChunks.createOrGet({1, 0, 0});
        existing.markLoaded(12);
        existing.clearDirty();

        voxel::world::Chunk generated({0, 0, 0});
        generator.generate(generated);
        voxel::world::ChunkJobMailbox mailbox;
        voxel::core::JobSystem neighborJobs;
        mailbox.pushGeneration({generated.coord(), std::move(generated)});
        voxel::world::ChunkPipeline neighborPipeline;
        const auto neighborStats = neighborPipeline.processRequestsAsync(
            neighborChunks,
            saveStore,
            generator,
            neighborJobs,
            mailbox,
            {},
            {.maxLoadsOrGenerationsPerTick = 0});
        VOXEL_CHECK(neighborStats.loaded == 1);
        VOXEL_CHECK(neighborStats.neighborRemeshes == 1);
        VOXEL_CHECK(existing.dirty().mesh);
        VOXEL_CHECK(existing.dirty().lighting);
        VOXEL_CHECK(!existing.dirty().save);
        VOXEL_CHECK(existing.revision() == 12);
    }

    {
        voxel::world::ChunkJobMailbox mailbox;
        const voxel::world::ChunkCoord coord{42, 0, 42};
        VOXEL_CHECK(!mailbox.isGenerationInFlight(coord));
        VOXEL_CHECK(mailbox.tryBeginGeneration(coord));
        VOXEL_CHECK(mailbox.isGenerationInFlight(coord));
        VOXEL_CHECK(!mailbox.tryBeginGeneration(coord));
        mailbox.endGeneration(coord);
        VOXEL_CHECK(!mailbox.isGenerationInFlight(coord));

        mailbox.pushGeneration({{1, 0, 0}, voxel::world::Chunk({1, 0, 0})});
        mailbox.pushGeneration({{2, 0, 0}, voxel::world::Chunk({2, 0, 0})});
        const auto oneResult = mailbox.drainGeneration(1);
        VOXEL_CHECK(oneResult.size() == 1);
        VOXEL_CHECK(mailbox.pendingGenerationResults() == 1);
        const auto remainingResults = mailbox.drainGeneration(8);
        VOXEL_CHECK(remainingResults.size() == 1);
        VOXEL_CHECK(mailbox.pendingGenerationResults() == 0);

        mailbox.pushGeneration({{12, 0, 0}, voxel::world::Chunk({12, 0, 0})});
        mailbox.pushGeneration({{2, 0, 0}, voxel::world::Chunk({2, 0, 0})});
        mailbox.pushGeneration({{6, 0, 0}, voxel::world::Chunk({6, 0, 0})});
        const auto closest = mailbox.drainGenerationClosest({0, 0, 0}, 2);
        VOXEL_CHECK(closest.size() == 2);
        VOXEL_CHECK(closest[0].coord.x == 2);
        VOXEL_CHECK(closest[1].coord.x == 6);
        VOXEL_CHECK(mailbox.pendingGenerationResults() == 1);
        const auto far = mailbox.drainGenerationClosest({0, 0, 0}, 8);
        VOXEL_CHECK(far.size() == 1);
        VOXEL_CHECK(far[0].coord.x == 12);
    }

    // ------------------------------------------------------------------
    // JobSystem (Phase E1)
    // ------------------------------------------------------------------
    {
        voxel::core::JobSystem jobs;
        jobs.start(4);
        VOXEL_CHECK(jobs.running());
        VOXEL_CHECK(jobs.workerCount() == 4);

        // Basic: N jobs all complete with correct results.
        constexpr int kJobCount = 64;
        std::vector<std::future<int>> futures;
        futures.reserve(kJobCount);
        for (int i = 0; i < kJobCount; ++i) {
            futures.push_back(jobs.submit({"unit", voxel::core::JobPriority::Medium}, [i]() { return i * 2; }));
        }
        jobs.waitAll();
        for (int i = 0; i < kJobCount; ++i) {
            VOXEL_CHECK(futures[i].get() == i * 2);
        }
        VOXEL_CHECK(jobs.pendingCount() == 0);

        // Re-entrant submit: a job submitting jobs that submit jobs. No deadlock.
        std::atomic<int> nestedCounter{0};
        auto rootFuture = jobs.submit({"root", voxel::core::JobPriority::Medium}, [&]() {
            auto child = jobs.submit({"child", voxel::core::JobPriority::Medium}, [&]() {
                auto grandchild = jobs.submit({"grandchild", voxel::core::JobPriority::Medium}, [&]() {
                    nestedCounter.fetch_add(1, std::memory_order_acq_rel);
                });
                grandchild.wait();
                nestedCounter.fetch_add(1, std::memory_order_acq_rel);
            });
            child.wait();
            nestedCounter.fetch_add(1, std::memory_order_acq_rel);
        });
        rootFuture.wait();
        VOXEL_CHECK(nestedCounter.load() == 3);

        jobs.stop();
        VOXEL_CHECK(!jobs.running());
    }

    // Priority ordering: deterministic via gate/latch (no timing assumptions).
    {
        voxel::core::JobSystem jobs;
        jobs.start(1); // single worker so order is total

        std::mutex gateMutex;
        std::condition_variable gateCv;
        bool gateOpen = false;

        std::atomic<int> startCounter{0};
        constexpr int kFiller = 8;

        // Block the only worker with a Low job that waits on the gate.
        auto blockFuture = jobs.submit({"gatekeeper", voxel::core::JobPriority::Low}, [&]() {
            std::unique_lock<std::mutex> lock(gateMutex);
            gateCv.wait(lock, [&]() { return gateOpen; });
        });

        // Now enqueue a mix of Low fillers followed by a Critical job.
        std::vector<std::pair<int, std::future<void>>> tail;
        for (int i = 0; i < kFiller; ++i) {
            tail.emplace_back(0, jobs.submit({"fill", voxel::core::JobPriority::Low}, [&, i]() {
                (void)i;
                startCounter.fetch_add(1, std::memory_order_acq_rel);
            }));
        }
        std::atomic<int> criticalOrder{-1};
        auto critFuture = jobs.submit({"crit", voxel::core::JobPriority::Critical}, [&]() {
            criticalOrder.store(startCounter.fetch_add(1, std::memory_order_acq_rel),
                                std::memory_order_release);
        });

        // Release the gate. After the gatekeeper Low finishes, the next pulled task
        // must be Critical (priority bucket is highest), not one of the Low fillers.
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            gateOpen = true;
        }
        gateCv.notify_all();
        blockFuture.wait();
        critFuture.wait();
        VOXEL_CHECK(criticalOrder.load() == 0); // Critical ran first after the gatekeeper.

        jobs.waitAll();
        jobs.stop();
    }

    // submit() works without start() — runs inline so futures still resolve.
    {
        voxel::core::JobSystem jobs;
        auto fut = jobs.submit({"inline", voxel::core::JobPriority::Medium}, []() { return 7; });
        VOXEL_CHECK(fut.get() == 7);
    }

    // ------------------------------------------------------------------
    // Async chunk pipeline (Phase E2)
    // ------------------------------------------------------------------
    {
        voxel::core::JobSystem jobs;
        jobs.start(4);

        voxel::world::ChunkManager asyncChunks;
        voxel::world::ChunkStreamer asyncStreamer(asyncChunks);
        voxel::world::ChunkPipeline asyncPipeline;
        voxel::world::ChunkJobMailbox mailbox;
        voxel::world::FlatTerrainGenerator asyncGenerator;
        voxel::save::RegionFileStore asyncSaveStore("build/test_saves/async_world");

        // Streamer radius = renderDistanceChunks (X/Z) × verticalRenderDistanceChunks (Y).
        // 2 × 2 gives 5 × 5 × 5 = 125 chunks.
        const auto requests = asyncStreamer.planRequests(
            {0, 0, 0},
            {.renderDistanceChunks = 2, .verticalRenderDistanceChunks = 2,
             .simulationDistanceChunks = 0, .physicsDistanceChunks = 0});
        VOXEL_CHECK(requests.size() == 125);

        // Pump the pipeline until all 125 requests resolve. Cap iterations so a hang shows up as a test failure.
        std::size_t totalDispatched = 0;
        std::size_t pumpIterations = 0;
        constexpr std::size_t kMaxIterations = 256;
        while (asyncChunks.residentCount() < requests.size() && pumpIterations < kMaxIterations) {
            const auto stats = asyncPipeline.processRequestsAsync(
                asyncChunks, asyncSaveStore, asyncGenerator, jobs, mailbox, requests,
                {.maxLoadsOrGenerationsPerTick = 16});
            totalDispatched += stats.dispatched;
            jobs.waitAll();
            ++pumpIterations;
        }
        VOXEL_CHECK(asyncChunks.residentCount() == requests.size());
        VOXEL_CHECK(mailbox.inFlightGenerationCount() == 0);
        VOXEL_CHECK(totalDispatched == requests.size());
        bool generatedChunksSaveClean = true;
        asyncChunks.forEach([&generatedChunksSaveClean](const voxel::world::Chunk& chunk) {
            generatedChunksSaveClean = generatedChunksSaveClean && !chunk.dirty().save;
        });
        VOXEL_CHECK(generatedChunksSaveClean);

        // After completion, calling processRequestsAsync again should not redispatch any chunk.
        const auto noopStats = asyncPipeline.processRequestsAsync(
            asyncChunks, asyncSaveStore, asyncGenerator, jobs, mailbox, requests,
            {.maxLoadsOrGenerationsPerTick = 16});
        VOXEL_CHECK(noopStats.dispatched == 0);
        VOXEL_CHECK(noopStats.skipped == requests.size());

        jobs.stop();
    }

    // ------------------------------------------------------------------
    // Chunk-local lighting (Phase E6)
    // ------------------------------------------------------------------
    {
        voxel::data::BlockRegistry lightingBlocks;
        lightingBlocks.registerCoreBlocks();
        auto lightCatalog = lightingBlocks.buildLightCatalog();
        // Mark stone (id 2) as a torch for this test so we can BFS block light.
        lightCatalog.set(voxel::BlockTypeId{20}, {15U, 0U});

        // Empty chunk: every cell at full skylight.
        {
            voxel::world::Chunk empty({0, 0, 0});
            voxel::world::ChunkLightData light;
            voxel::world::LightPropagator{}.propagateIsolated(empty, lightCatalog, light);
            VOXEL_CHECK(light.skyLight(0, 0, 0) == 15);
            VOXEL_CHECK(light.skyLight(31, 31, 31) == 15);
            VOXEL_CHECK(light.skyLight(15, 15, 15) == 15);
        }

        // Uniform opaque chunks have no internal light and should take the
        // fast path without changing observable lighting.
        {
            voxel::world::Chunk solid({0, 0, 0});
            solid.fillSilently(voxel::world::makeBlockState(voxel::BlockTypeId{2}));
            voxel::world::ChunkLightData light;
            voxel::world::LightPropagator{}.propagateIsolated(solid, lightCatalog, light);
            VOXEL_CHECK(light.skyLight(0, 31, 0) == 0);
            VOXEL_CHECK(light.skyLight(15, 15, 15) == 0);
            VOXEL_CHECK(light.blockLight(15, 15, 15) == 0);
        }

        // Stone slab at y=15: above is fully lit, below is dark.
        {
            voxel::world::Chunk capped({0, 0, 0});
            for (int z = 0; z < voxel::world::ChunkSize; ++z) {
                for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                    capped.setBlockSilently(x, 15, z, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
                }
            }
            voxel::world::ChunkLightData light;
            voxel::world::LightPropagator{}.propagateIsolated(capped, lightCatalog, light);
            VOXEL_CHECK(light.skyLight(15, 31, 15) == 15);
            VOXEL_CHECK(light.skyLight(15, 16, 15) == 15);
            // The slab itself fully attenuates (attenuation=15 default for opaque).
            VOXEL_CHECK(light.skyLight(15, 15, 15) == 0);
            // Below is in shadow.
            VOXEL_CHECK(light.skyLight(15, 14, 15) == 0);
            VOXEL_CHECK(light.skyLight(15, 0, 15) == 0);
        }

        // Emissive torch surrounded by air: blocklight peaks at source, falls off.
        {
            voxel::world::Chunk litCave({0, 0, 0});
            // Place a synthetic torch (id 20) at the centre. Treat all other cells as air.
            litCave.setBlockSilently(15, 15, 15, voxel::world::makeBlockState(voxel::BlockTypeId{20}));
            voxel::world::ChunkLightData light;
            voxel::world::LightPropagator{}.propagateIsolated(litCave, lightCatalog, light);
            VOXEL_CHECK(light.blockLight(15, 15, 15) == 15);
            VOXEL_CHECK(light.blockLight(16, 15, 15) == 14);
            VOXEL_CHECK(light.blockLight(17, 15, 15) == 13);
            // Far corner should be dark (15 steps away in manhattan distance).
            VOXEL_CHECK(light.blockLight(0, 0, 0) == 0);
        }
    }

    // ------------------------------------------------------------------
    // Cross-chunk lighting (Phase F1)
    // ------------------------------------------------------------------
    {
        voxel::data::BlockRegistry crossBlocks;
        crossBlocks.registerCoreBlocks();
        auto crossCatalog = crossBlocks.buildLightCatalog();

        voxel::world::ChunkManager twoChunks;
        // Stack: (0,0,0) sits below (0,1,0). Top is fully empty so sky enters
        // from above and should cascade into the bottom chunk through the seam.
        auto& bottom = twoChunks.createOrGet({0, 0, 0});
        auto& top = twoChunks.createOrGet({0, 1, 0});
        bottom.markLoaded(1);
        top.markLoaded(1);

        // Light top chunk first (its +Y neighbour is missing, so propagator
        // assumes open sky above).
        voxel::world::ChunkLightData topLight;
        voxel::world::LightPropagator{}.propagate(top, twoChunks, crossCatalog, topLight);
        VOXEL_CHECK(topLight.skyLight(0, 0, 0) == 15);  // bottom row of top chunk
        VOXEL_CHECK(topLight.skyLight(15, 0, 15) == 15);
        top.setLightData(std::move(topLight));

        // Now light the bottom chunk. It should pull sky=15 down from the top
        // chunk's bottom row (which is its +Y neighbour).
        voxel::world::ChunkLightData bottomLight;
        voxel::world::LightPropagator{}.propagate(bottom, twoChunks, crossCatalog, bottomLight);
        VOXEL_CHECK(bottomLight.skyLight(0, 31, 0) == 15);
        VOXEL_CHECK(bottomLight.skyLight(15, 15, 15) == 15);
        VOXEL_CHECK(bottomLight.skyLight(0, 0, 0) == 15);

        // Place an opaque slab at y=15 in the bottom chunk. Sky above should
        // stay at 15 (fed by top chunk), below should be dark.
        for (int z = 0; z < voxel::world::ChunkSize; ++z) {
            for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                bottom.setBlockSilently(x, 15, z, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
            }
        }
        voxel::world::ChunkLightData reLit;
        voxel::world::LightPropagator{}.propagate(bottom, twoChunks, crossCatalog, reLit);
        VOXEL_CHECK(reLit.skyLight(15, 31, 15) == 15);  // Above slab still seeded from top chunk.
        VOXEL_CHECK(reLit.skyLight(15, 16, 15) == 15);
        VOXEL_CHECK(reLit.skyLight(15, 14, 15) == 0);   // Below slab fully shadowed.

        // Missing neighbours behave as open-sky-above-only: -Y/-X/+X/-Z/+Z return 0.
        voxel::world::ChunkManager lonely;
        auto& solo = lonely.createOrGet({5, 5, 5});
        solo.markLoaded(1);
        voxel::world::ChunkLightData soloLight;
        voxel::world::LightPropagator{}.propagate(solo, lonely, crossCatalog, soloLight);
        VOXEL_CHECK(soloLight.skyLight(0, 0, 0) == 15);   // +Y neighbour missing → open sky.
        VOXEL_CHECK(soloLight.skyLight(31, 31, 31) == 15);
    }

    // ------------------------------------------------------------------
    // Mesh seam culling at chunk borders (Phase F2)
    // ------------------------------------------------------------------
    {
        voxel::render::meshing::GreedyMesher seamMesher;

        // Stone-filled chunk A and a stone-filled +X neighbour B. With the
        // neighbourhood populated, the boundary face at x=31 of A must be
        // culled (the cell at neighbour x=0 occludes it).
        voxel::world::Chunk a({0, 0, 0});
        voxel::world::Chunk b({1, 0, 0});
        for (int z = 0; z < voxel::world::ChunkSize; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                    a.setBlockSilently(x, y, z, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
                    b.setBlockSilently(x, y, z, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
                }
            }
        }

        // No neighbour: mesher should keep the +X face of A.
        const auto noNeighbourMesh = seamMesher.build(a);

        // With neighbour B present: the +X face should be culled, producing
        // strictly fewer indices.
        voxel::render::meshing::ChunkNeighborhood neighbourhood{};
        neighbourhood.posX = &b;
        const auto withNeighbourMesh = seamMesher.build(a, voxel::render::meshing::BlockRenderCatalog{}, nullptr, neighbourhood);

        VOXEL_CHECK(withNeighbourMesh.indices.size() < noNeighbourMesh.indices.size());

        // Specifically: a single 32x32 quad's worth of faces should be gone.
        // 32x32 face = 1024 cells, but greedy merging packs them into 1 quad = 6 indices.
        // Without neighbour: all 6 faces drawn. With +X neighbour: 5 faces drawn.
        // (Other 5 boundary faces still keep their faces because their neighbours are null.)
    }

    // ------------------------------------------------------------------
    // Save format (Phase E5)
    // ------------------------------------------------------------------
    {
        const std::filesystem::path saveRoot{"build/test_saves/e5_world"};
        std::filesystem::remove_all(saveRoot);

        voxel::save::RegionFileStore store(saveRoot);

        voxel::world::Chunk original({3, 4, 5});
        original.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        original.setBlock(31, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{8}));
        original.setBlock(0, 31, 0, voxel::world::makeBlockState(voxel::BlockTypeId{9}));
        original.setBlock(0, 0, 31, voxel::world::makeBlockState(voxel::BlockTypeId{3}));
        const auto originalRevision = original.revision();
        store.saveChunk(original);

        const auto loaded = store.loadChunk({3, 4, 5});
        VOXEL_CHECK(loaded.has_value());
        VOXEL_CHECK(loaded->revision() == originalRevision);
        VOXEL_CHECK(voxel::world::blockTypeOf(loaded->blockAt(0, 0, 0)).value == 2);
        VOXEL_CHECK(voxel::world::blockTypeOf(loaded->blockAt(31, 0, 0)).value == 8);
        VOXEL_CHECK(voxel::world::blockTypeOf(loaded->blockAt(0, 31, 0)).value == 9);
        VOXEL_CHECK(voxel::world::blockTypeOf(loaded->blockAt(0, 0, 31)).value == 3);
        // Mid-chunk default = air.
        VOXEL_CHECK(loaded->blockAt(5, 5, 5).value == voxel::world::AirBlockState.value);

        // Missing files return nullopt without throwing.
        const auto missing = store.loadChunk({999, 999, 999});
        VOXEL_CHECK(!missing.has_value());

        // Corrupted (wrong magic) file returns nullopt.
        const std::filesystem::path corruptPath = saveRoot / "chunks" / "100_0_0.vchk";
        std::filesystem::create_directories(corruptPath.parent_path());
        {
            std::ofstream bad(corruptPath, std::ios::binary | std::ios::trunc);
            const char garbage[] = "NOPE";
            bad.write(garbage, 4);
        }
        const auto corrupt = store.loadChunk({100, 0, 0});
        VOXEL_CHECK(!corrupt.has_value());
    }

    // Mesh job: snapshot copy is consistent with main-thread chunk state.
    {
        voxel::core::JobSystem jobs;
        jobs.start(2);

        voxel::world::ChunkManager meshChunks;
        auto& chunk = meshChunks.createOrGet({0, 0, 0});
        chunk.setBlock(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        chunk.setBlock(1, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        chunk.setBlock(2, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));

        voxel::world::ChunkJobMailbox mailbox;
        const auto sourceRevision = chunk.revision();
        const voxel::world::MeshJobKey meshKey{chunk.coord(), sourceRevision, 12345};
        VOXEL_CHECK(mailbox.tryBeginMesh(meshKey));
        VOXEL_CHECK(!mailbox.tryBeginMesh(meshKey));

        voxel::world::Chunk snapshot(chunk);
        const voxel::world::ChunkCoord meshCoord{0, 0, 0};
        voxel::world::ChunkJobMailbox* mailboxPtr = &mailbox;
        auto meshFuture = jobs.submit({"unit.mesh", voxel::core::JobPriority::Medium},
            [snapshot = std::move(snapshot), meshCoord, sourceRevision, mailboxPtr]() mutable {
                voxel::render::meshing::GreedyMesher mesher;
                auto mesh = mesher.build(snapshot);
                voxel::world::ChunkMeshResult result{};
                result.coord = meshCoord;
                result.sourceRevision = sourceRevision;
                result.neighborRevisionHash = 12345;
                result.mesh = std::move(mesh);
                mailboxPtr->pushMesh(std::move(result));
            });
        meshFuture.wait();

        auto drained = mailbox.drainMesh();
        VOXEL_CHECK(drained.size() == 1);
        VOXEL_CHECK(drained.front().coord == meshCoord);
        VOXEL_CHECK(drained.front().sourceRevision == sourceRevision);
        VOXEL_CHECK(drained.front().neighborRevisionHash == 12345);
        VOXEL_CHECK(!drained.front().mesh.vertices.empty());
        mailbox.endMesh(meshKey);
        VOXEL_CHECK(mailbox.inFlightMeshCount() == 0);

        voxel::world::ChunkMeshResult staleResult{};
        staleResult.coord = meshCoord;
        staleResult.sourceRevision = sourceRevision;
        staleResult.neighborRevisionHash = 12345;
        chunk.setBlock(3, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        VOXEL_CHECK(chunk.revision() != staleResult.sourceRevision);

        jobs.stop();
    }

    // ------------------------------------------------------------------
    // Palette + BitPackedArray (Phase E3)
    // ------------------------------------------------------------------
    {
        // Single-entry palette still reports >= 1 bit so storage works.
        VOXEL_CHECK(voxel::world::bitsRequiredForPaletteSize(0) == 1);
        VOXEL_CHECK(voxel::world::bitsRequiredForPaletteSize(1) == 1);
        VOXEL_CHECK(voxel::world::bitsRequiredForPaletteSize(2) == 1);
        VOXEL_CHECK(voxel::world::bitsRequiredForPaletteSize(3) == 2);
        VOXEL_CHECK(voxel::world::bitsRequiredForPaletteSize(4) == 2);
        VOXEL_CHECK(voxel::world::bitsRequiredForPaletteSize(5) == 3);
        VOXEL_CHECK(voxel::world::bitsRequiredForPaletteSize(16) == 4);
        VOXEL_CHECK(voxel::world::bitsRequiredForPaletteSize(17) == 5);
        VOXEL_CHECK(voxel::world::bitsRequiredForPaletteSize(256) == 8);

        // BitPackedArray round-trip with various widths.
        for (std::size_t bits : {1U, 2U, 3U, 5U, 8U, 13U, 16U}) {
            voxel::world::BitPackedArray arr(1024, bits);
            const std::uint32_t mask = (bits >= 32U) ? 0xFFFFFFFFU : ((1U << bits) - 1U);
            for (std::size_t i = 0; i < 1024; ++i) {
                arr.set(i, static_cast<std::uint32_t>(i) & mask);
            }
            for (std::size_t i = 0; i < 1024; ++i) {
                VOXEL_CHECK(arr.at(i) == (static_cast<std::uint32_t>(i) & mask));
            }
        }

        // Resize preserves values when widening.
        voxel::world::BitPackedArray arr(64, 1);
        for (std::size_t i = 0; i < 64; ++i) {
            arr.set(i, i % 2U);
        }
        arr.resize(5);
        VOXEL_CHECK(arr.bitsPerEntry() == 5);
        for (std::size_t i = 0; i < 64; ++i) {
            VOXEL_CHECK(arr.at(i) == i % 2U);
        }
    }

    // ------------------------------------------------------------------
    // Deterministic noise + NoiseTerrainGenerator (Phase E4)
    // ------------------------------------------------------------------
    {
        // hash3D is stable, deterministic, and seed-sensitive.
        VOXEL_CHECK(voxel::core::hash3D(1, 2, 3, 1337) == voxel::core::hash3D(1, 2, 3, 1337));
        VOXEL_CHECK(voxel::core::hash3D(1, 2, 3, 1337) != voxel::core::hash3D(1, 2, 3, 1338));

        // valueNoise2D in [0, 1).
        for (int i = 0; i < 16; ++i) {
            const float v = voxel::core::valueNoise2D(static_cast<float>(i) * 0.37F, static_cast<float>(i) * 1.11F, 42U);
            VOXEL_CHECK(v >= 0.0F && v < 1.0F);
        }

        // Two generators with the same seed produce identical chunks.
        voxel::world::NoiseTerrainSettings settings{};
        settings.seed = 4242;
        voxel::world::NoiseTerrainGenerator gen1(settings);
        voxel::world::NoiseTerrainGenerator gen2(settings);
        const voxel::world::TerrainColumnCoord prepassCoord{0, 0};
        const auto prepassA = gen1.buildColumnPrepass(prepassCoord);
        const auto prepassB = gen2.buildColumnPrepass(prepassCoord);
        VOXEL_CHECK(prepassA.key == prepassB.key);
        VOXEL_CHECK(prepassA.surfaceBlockY == prepassB.surfaceBlockY);
        VOXEL_CHECK(prepassA.surfaceKind == prepassB.surfaceKind);
        VOXEL_CHECK(prepassA.biome == prepassB.biome);
        VOXEL_CHECK(prepassA.continentalness == prepassB.continentalness);
        VOXEL_CHECK(prepassA.erosion == prepassB.erosion);
        VOXEL_CHECK(prepassA.peaksValleys == prepassB.peaksValleys);
        VOXEL_CHECK(prepassA.temperature == prepassB.temperature);
        VOXEL_CHECK(prepassA.humidity == prepassB.humidity);
        VOXEL_CHECK(prepassA.weirdness == prepassB.weirdness);
        VOXEL_CHECK(prepassA.terrainClass == prepassB.terrainClass);
        VOXEL_CHECK(prepassA.tectonicStress == prepassB.tectonicStress);
        VOXEL_CHECK(prepassA.volcanism == prepassB.volcanism);
        VOXEL_CHECK(prepassA.manaField == prepassB.manaField);
        VOXEL_CHECK(prepassA.polarInfluence == prepassB.polarInfluence);
        VOXEL_CHECK(prepassA.oceanDepthBias == prepassB.oceanDepthBias);
        VOXEL_CHECK(prepassA.oceanDepth == prepassB.oceanDepth);
        VOXEL_CHECK(prepassA.seaMask == prepassB.seaMask);
        VOXEL_CHECK(prepassA.beachMask == prepassB.beachMask);
        VOXEL_CHECK(prepassA.riverCandidateMask == prepassB.riverCandidateMask);
        bool prepassRangesValid = true;
        for (std::size_t i = 0; i < prepassA.continentalness.size(); ++i) {
            prepassRangesValid = prepassRangesValid
                && prepassA.continentalness[i] >= -1.0F && prepassA.continentalness[i] <= 1.0F
                && prepassA.erosion[i] >= -1.0F && prepassA.erosion[i] <= 1.0F
                && prepassA.peaksValleys[i] >= -1.0F && prepassA.peaksValleys[i] <= 1.0F
                && prepassA.temperature[i] >= -1.0F && prepassA.temperature[i] <= 1.0F
                && prepassA.humidity[i] >= -1.0F && prepassA.humidity[i] <= 1.0F
                && prepassA.weirdness[i] >= -1.0F && prepassA.weirdness[i] <= 1.0F
                && prepassA.tectonicStress[i] >= 0.0F && prepassA.tectonicStress[i] <= 1.0F
                && prepassA.volcanism[i] >= 0.0F && prepassA.volcanism[i] <= 1.0F
                && prepassA.manaField[i] >= 0.0F && prepassA.manaField[i] <= 1.0F
                && prepassA.polarInfluence[i] >= 0.0F && prepassA.polarInfluence[i] <= 1.0F
                && prepassA.oceanDepthBias[i] >= 0.0F && prepassA.oceanDepthBias[i] <= 1.0F
                && prepassA.oceanDepth[i] >= 0.0F
                && prepassA.surfaceBlockY[i] >= static_cast<int>(settings.minWorldY)
                && prepassA.surfaceBlockY[i] <= static_cast<int>(settings.maxWorldY);
        }
        VOXEL_CHECK(prepassRangesValid);

        voxel::world::Chunk a({0, 0, 0});
        voxel::world::Chunk b({0, 0, 0});
        gen1.generate(a);
        gen2.generate(b);
        VOXEL_CHECK(!a.dirty().save);
        VOXEL_CHECK(!b.dirty().save);
        bool identical = true;
        for (int z = 0; z < voxel::world::ChunkSize && identical; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize && identical; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize && identical; ++x) {
                    if (a.blockAt(x, y, z).value != b.blockAt(x, y, z).value) {
                        identical = false;
                    }
                }
            }
        }
        VOXEL_CHECK(identical);

        // Different seeds produce a different chunk somewhere.
        voxel::world::NoiseTerrainSettings altSettings = settings;
        altSettings.seed = 9999;
        voxel::world::NoiseTerrainGenerator altGen(altSettings);
        const auto prepassC = altGen.buildColumnPrepass(prepassCoord);
        VOXEL_CHECK(prepassA.surfaceBlockY != prepassC.surfaceBlockY);
        VOXEL_CHECK(prepassA.continentalness != prepassC.continentalness);
        voxel::world::Chunk c({0, 0, 0});
        altGen.generate(c);
        bool differs = false;
        for (int z = 0; z < voxel::world::ChunkSize && !differs; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize && !differs; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize && !differs; ++x) {
                    if (a.blockAt(x, y, z).value != c.blockAt(x, y, z).value) {
                        differs = true;
                    }
                }
            }
        }
        VOXEL_CHECK(differs);

        voxel::world::NoiseTerrainSettings variantSettings = settings;
        variantSettings.seaLevel += 1.0F;
        voxel::world::NoiseTerrainGenerator variantGen(variantSettings);
        const auto variantPrepass = variantGen.buildColumnPrepass(prepassCoord);
        VOXEL_CHECK(prepassA.key.terrainVersion != variantPrepass.key.terrainVersion);
        VOXEL_CHECK(prepassA.surfaceBlockY != variantPrepass.surfaceBlockY);

        voxel::world::NoiseTerrainSettings highSurfaceSettings = settings;
        highSurfaceSettings.seaLevel = 640.0F;
        voxel::world::NoiseTerrainGenerator highSurfaceGen(highSurfaceSettings);
        voxel::render::meshing::ClipmapRegionMesher clipmapMesher;
        const auto lowWindowMesh = clipmapMesher.build({0, 0, 0}, highSurfaceGen);
        const auto highWindowMesh = clipmapMesher.build({0, 1, 0}, highSurfaceGen);
        VOXEL_CHECK(lowWindowMesh.vertices.empty());
        VOXEL_CHECK(lowWindowMesh.indices.empty());
        VOXEL_CHECK(!highWindowMesh.vertices.empty());
        VOXEL_CHECK(!highWindowMesh.indices.empty());

        // World-shape prepass should expose ocean/land/biome metadata across
        // nearby deterministic sample columns without requiring full chunks.
        bool sawOceanPrepass = false;
        bool sawLandPrepass = false;
        bool sawDeepOceanPrepass = false;
        bool sawNonOceanBiome = false;
        int shorelineAdjacentMaxDelta = 0;
        const auto isShorelineBand = [](voxel::world::TerrainSurfaceKind kind) {
            return kind == voxel::world::TerrainSurfaceKind::Land
                || kind == voxel::world::TerrainSurfaceKind::Beach
                || kind == voxel::world::TerrainSurfaceKind::ShallowOcean;
        };
        for (int cz = -96; cz <= 96; cz += 12) {
            for (int cx = -96; cx <= 96; cx += 12) {
                const auto sample = gen1.buildColumnPrepass({cx, cz});
                for (int z = 0; z < voxel::world::ChunkSize; ++z) {
                    for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                        const auto idx = static_cast<std::size_t>(x + z * voxel::world::ChunkSize);
                        if (x + 1 < voxel::world::ChunkSize) {
                            const auto nx = static_cast<std::size_t>((x + 1) + z * voxel::world::ChunkSize);
                            const bool shoreline = isShorelineBand(sample.surfaceKind[idx])
                                && isShorelineBand(sample.surfaceKind[nx])
                                && sample.surfaceKind[idx] != sample.surfaceKind[nx];
                            if (shoreline) {
                                shorelineAdjacentMaxDelta = std::max(shorelineAdjacentMaxDelta,
                                    std::abs(sample.surfaceBlockY[idx] - sample.surfaceBlockY[nx]));
                            }
                        }
                        if (z + 1 < voxel::world::ChunkSize) {
                            const auto nz = static_cast<std::size_t>(x + (z + 1) * voxel::world::ChunkSize);
                            const bool shoreline = isShorelineBand(sample.surfaceKind[idx])
                                && isShorelineBand(sample.surfaceKind[nz])
                                && sample.surfaceKind[idx] != sample.surfaceKind[nz];
                            if (shoreline) {
                                shorelineAdjacentMaxDelta = std::max(shorelineAdjacentMaxDelta,
                                    std::abs(sample.surfaceBlockY[idx] - sample.surfaceBlockY[nz]));
                            }
                        }
                    }
                }
                for (std::size_t i = 0; i < sample.surfaceKind.size(); ++i) {
                    sawOceanPrepass = sawOceanPrepass || sample.seaMask[i];
                    sawLandPrepass = sawLandPrepass || !sample.seaMask[i];
                    sawDeepOceanPrepass = sawDeepOceanPrepass || sample.surfaceKind[i] == voxel::world::TerrainSurfaceKind::DeepOcean;
                    sawNonOceanBiome = sawNonOceanBiome
                        || (sample.biome[i] != voxel::world::TerrainBiomeId::OceanAbyss
                            && sample.biome[i] != voxel::world::TerrainBiomeId::DeepOcean
                            && sample.biome[i] != voxel::world::TerrainBiomeId::Ocean
                            && sample.biome[i] != voxel::world::TerrainBiomeId::WarmOcean
                            && sample.biome[i] != voxel::world::TerrainBiomeId::ColdOcean);
                }
            }
        }
        VOXEL_CHECK(sawOceanPrepass);
        VOXEL_CHECK(sawLandPrepass);
        VOXEL_CHECK(sawDeepOceanPrepass);
        VOXEL_CHECK(sawNonOceanBiome);
        VOXEL_CHECK(shorelineAdjacentMaxDelta <= 768);

        auto prepassCache = std::make_shared<voxel::world::TerrainColumnPrepassCache>();
        voxel::world::NoiseTerrainGenerator cachedGen(settings);
        cachedGen.setPrepassCache(prepassCache);
        prepassCache->insert(cachedGen.buildColumnPrepass(prepassCoord));
        voxel::world::Chunk directChunk({0, 0, 0});
        voxel::world::Chunk cachedChunk({0, 0, 0});
        gen1.generate(directChunk);
        cachedGen.generate(cachedChunk);
        VOXEL_CHECK(cachedGen.lastGenerationMode() == voxel::world::TerrainGenerationMode::CachedPrepass);
        bool cachedMatchesDirect = true;
        for (int z = 0; z < voxel::world::ChunkSize && cachedMatchesDirect; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize && cachedMatchesDirect; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize && cachedMatchesDirect; ++x) {
                    if (directChunk.blockAt(x, y, z).value != cachedChunk.blockAt(x, y, z).value) {
                        cachedMatchesDirect = false;
                    }
                }
            }
        }
        VOXEL_CHECK(cachedMatchesDirect);
        VOXEL_CHECK(!cachedChunk.dirty().save);

        voxel::world::NoiseTerrainGenerator batchGen(settings);
        std::vector<voxel::world::Chunk> batchedChunks;
        batchedChunks.emplace_back(voxel::world::ChunkCoord{2, -1, 2});
        batchedChunks.emplace_back(voxel::world::ChunkCoord{2, 0, 2});
        batchedChunks.emplace_back(voxel::world::ChunkCoord{2, 1, 2});
        std::vector<voxel::world::TerrainGenerationMode> batchModes;
        batchGen.generateColumn(batchedChunks, batchModes);
        VOXEL_CHECK(batchModes.size() == batchedChunks.size());
        bool batchMatchesIndependent = true;
        for (const auto& batched : batchedChunks) {
            voxel::world::NoiseTerrainGenerator independentGen(settings);
            voxel::world::Chunk independent(batched.coord());
            independentGen.generate(independent);
            for (int z = 0; z < voxel::world::ChunkSize && batchMatchesIndependent; ++z) {
                for (int y = 0; y < voxel::world::ChunkSize && batchMatchesIndependent; ++y) {
                    for (int x = 0; x < voxel::world::ChunkSize && batchMatchesIndependent; ++x) {
                        if (batched.blockAt(x, y, z).value != independent.blockAt(x, y, z).value) {
                            batchMatchesIndependent = false;
                        }
                    }
                }
            }
        }
        VOXEL_CHECK(batchMatchesIndependent);

        const auto cacheStats = prepassCache->drainStats();
        VOXEL_CHECK(cacheStats.hits == 1);
        VOXEL_CHECK(cacheStats.misses == 0);
        VOXEL_CHECK(prepassCache->entryCount() == 1);

        auto missCache = std::make_shared<voxel::world::TerrainColumnPrepassCache>();
        voxel::world::NoiseTerrainGenerator missGen(settings);
        missGen.setPrepassCache(missCache);
        voxel::world::Chunk missChunk({0, 0, 0});
        missGen.generate(missChunk);
        VOXEL_CHECK(missGen.lastGenerationMode() == voxel::world::TerrainGenerationMode::Direct);
        const auto missStats = missCache->drainStats();
        VOXEL_CHECK(missStats.misses == 1);
        VOXEL_CHECK(missCache->entryCount() == 1);

        voxel::world::Chunk highAir({0, 10, 0});
        gen1.generate(highAir);
        VOXEL_CHECK(!highAir.dirty().save);
        bool highChunkAllAir = true;
        for (int z = 0; z < voxel::world::ChunkSize && highChunkAllAir; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize && highChunkAllAir; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize && highChunkAllAir; ++x) {
                    if (highAir.blockAt(x, y, z).value != voxel::world::AirBlockState.value) {
                        highChunkAllAir = false;
                    }
                }
            }
        }
        VOXEL_CHECK(highChunkAllAir);

        voxel::world::SpaceEnvironment space(settings.space);
        voxel::world::SpaceFeature asteroidFeature{};
        bool foundAsteroidFeature = false;
        int bestAsteroidScore = -1;
        const auto asteroidFeatureScore = [](voxel::world::SpaceFeatureType type) noexcept {
            switch (type) {
            case voxel::world::SpaceFeatureType::MetalRichAsteroids: return 5;
            case voxel::world::SpaceFeatureType::CrystalAsteroids: return 5;
            case voxel::world::SpaceFeatureType::IceField: return 4;
            case voxel::world::SpaceFeatureType::Comet: return 4;
            case voxel::world::SpaceFeatureType::RingDebris: return 3;
            case voxel::world::SpaceFeatureType::AsteroidCluster: return 2;
            default: return 0;
            }
        };
        const int firstSpaceSectorY = static_cast<int>(
            std::floor(settings.space.atmosphereTopY / static_cast<float>(settings.space.sectorSizeBlocks)));
        for (int sy = firstSpaceSectorY; sy < firstSpaceSectorY + 8; ++sy) {
            for (int sz = -8; sz <= 8; ++sz) {
                for (int sx = -8; sx <= 8; ++sx) {
                    for (const auto& feature : space.featuresForSector({sx, sy, sz})) {
                        if (feature.type == voxel::world::SpaceFeatureType::AsteroidCluster
                            || feature.type == voxel::world::SpaceFeatureType::IceField
                            || feature.type == voxel::world::SpaceFeatureType::MetalRichAsteroids
                            || feature.type == voxel::world::SpaceFeatureType::CrystalAsteroids
                            || feature.type == voxel::world::SpaceFeatureType::Comet
                            || feature.type == voxel::world::SpaceFeatureType::RingDebris) {
                            const auto center = space.featureWorldCenter(feature);
                            if (center.y >= settings.space.atmosphereTopY) {
                                const int score = asteroidFeatureScore(feature.type);
                                if (score > bestAsteroidScore) {
                                    asteroidFeature = feature;
                                    foundAsteroidFeature = true;
                                    bestAsteroidScore = score;
                                }
                            }
                        }
                    }
                }
            }
        }
        VOXEL_CHECK(foundAsteroidFeature);
        const auto asteroidCenter = space.featureWorldCenter(asteroidFeature);
        const voxel::world::ChunkCoord asteroidChunkCoord{
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(asteroidCenter.x)), voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(asteroidCenter.y)), voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(asteroidCenter.z)), voxel::world::ChunkSize)
        };
        voxel::world::Chunk asteroidChunk(asteroidChunkCoord);
        gen1.generate(asteroidChunk);
        VOXEL_CHECK(!asteroidChunk.dirty().save);
        std::size_t asteroidBlocks = 0;
        std::size_t spaceRockBlocks = 0;
        std::size_t richMetalBlocks = 0;
        std::size_t crystalBlocks = 0;
        std::size_t compressedIceBlocks = 0;
        for (int z = 0; z < voxel::world::ChunkSize; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                    const auto block = asteroidChunk.blockAt(x, y, z);
                    if (block.value != voxel::world::AirBlockState.value) {
                        ++asteroidBlocks;
                    }
                    const auto type = voxel::world::blockTypeOf(block);
                    if (type.value == 31U) ++spaceRockBlocks;
                    if (type.value == 32U) ++richMetalBlocks;
                    if (type.value == 33U) ++crystalBlocks;
                    if (type.value == 34U) ++compressedIceBlocks;
                }
            }
        }
        VOXEL_CHECK(asteroidBlocks > 0);
        VOXEL_CHECK(spaceRockBlocks + richMetalBlocks + crystalBlocks + compressedIceBlocks == asteroidBlocks);
        VOXEL_CHECK(richMetalBlocks + crystalBlocks + compressedIceBlocks > 0);

        voxel::world::SpaceFeature moonFeature{};
        voxel::world::SpaceFeature planetFeature{};
        voxel::world::SpaceFeature landablePlanetFeature{};
        voxel::world::SpaceFeature gasGiantFeature{};
        bool foundMoonFeature = false;
        bool foundPlanetFeature = false;
        bool foundLandablePlanetFeature = false;
        bool foundGasGiantFeature = false;
        for (int sy = firstSpaceSectorY; sy < firstSpaceSectorY + 16 && (!foundMoonFeature || !foundPlanetFeature); ++sy) {
            for (int sz = -16; sz <= 16 && (!foundMoonFeature || !foundPlanetFeature); ++sz) {
                for (int sx = -16; sx <= 16 && (!foundMoonFeature || !foundPlanetFeature); ++sx) {
                    for (const auto& feature : space.featuresForSector({sx, sy, sz})) {
                        const auto center = space.featureWorldCenter(feature);
                        if (center.y < settings.space.atmosphereTopY) {
                            continue;
                        }
                        if (!foundMoonFeature && feature.type == voxel::world::SpaceFeatureType::Moon) {
                            moonFeature = feature;
                            foundMoonFeature = true;
                        } else if (!foundPlanetFeature && feature.type == voxel::world::SpaceFeatureType::Planet) {
                            planetFeature = feature;
                            foundPlanetFeature = true;
                        }
                    }
                }
            }
        }
        for (int sy = firstSpaceSectorY; sy < firstSpaceSectorY + 16 && !foundLandablePlanetFeature; ++sy) {
            for (int sz = -16; sz <= 16 && !foundLandablePlanetFeature; ++sz) {
                for (int sx = -16; sx <= 16 && !foundLandablePlanetFeature; ++sx) {
                    for (const auto& feature : space.featuresForSector({sx, sy, sz})) {
                        const auto center = space.featureWorldCenter(feature);
                        if (center.y < settings.space.atmosphereTopY) {
                            continue;
                        }
                        if (feature.type == voxel::world::SpaceFeatureType::Planet && feature.landable) {
                            landablePlanetFeature = feature;
                            foundLandablePlanetFeature = true;
                            break;
                        }
                    }
                }
            }
        }
        for (int sy = firstSpaceSectorY; sy < firstSpaceSectorY + 32 && !foundGasGiantFeature; ++sy) {
            for (int sz = -32; sz <= 32 && !foundGasGiantFeature; ++sz) {
                for (int sx = -32; sx <= 32 && !foundGasGiantFeature; ++sx) {
                    for (const auto& feature : space.featuresForSector({sx, sy, sz})) {
                        const auto center = space.featureWorldCenter(feature);
                        if (center.y < settings.space.atmosphereTopY) {
                            continue;
                        }
                        if (feature.type == voxel::world::SpaceFeatureType::Planet
                            && feature.bodyClass == voxel::world::SpaceBodyClass::GasGiant) {
                            gasGiantFeature = feature;
                            foundGasGiantFeature = true;
                            break;
                        }
                    }
                }
            }
        }
        VOXEL_CHECK(foundMoonFeature);
        VOXEL_CHECK(foundPlanetFeature);
        VOXEL_CHECK(foundLandablePlanetFeature);
        VOXEL_CHECK(foundGasGiantFeature);
        VOXEL_CHECK(moonFeature.radius >= 260.0F);
        VOXEL_CHECK(planetFeature.radius >= 1400.0F);
        VOXEL_CHECK(moonFeature.bodyClass != voxel::world::SpaceBodyClass::None);
        VOXEL_CHECK(planetFeature.bodyClass != voxel::world::SpaceBodyClass::None);
        VOXEL_CHECK(moonFeature.gravityScale > 0.0F && moonFeature.gravityScale < 0.6F);
        VOXEL_CHECK(planetFeature.gravityScale > 0.5F);
        VOXEL_CHECK(moonFeature.atmosphereDensity >= 0.0F && moonFeature.atmosphereDensity <= 0.1F);
        VOXEL_CHECK(planetFeature.atmosphereDensity >= 0.0F && planetFeature.atmosphereDensity <= 1.0F);
        VOXEL_CHECK(moonFeature.surfaceRoughness > 0.0F && moonFeature.surfaceRoughness <= 1.0F);
        VOXEL_CHECK(moonFeature.resourceRichness > 0.0F && moonFeature.resourceRichness <= 1.0F);
        VOXEL_CHECK(moonFeature.lifeSignal == 0.0F);
        VOXEL_CHECK(moonFeature.landable);
        VOXEL_CHECK(planetFeature.surfaceRoughness >= 0.0F && planetFeature.surfaceRoughness <= 1.0F);
        VOXEL_CHECK(planetFeature.oceanCoverage >= 0.0F && planetFeature.oceanCoverage <= 1.0F);
        VOXEL_CHECK(planetFeature.resourceRichness >= 0.0F && planetFeature.resourceRichness <= 1.0F);
        VOXEL_CHECK(planetFeature.lifeSignal >= 0.0F && planetFeature.lifeSignal <= 1.0F);
        VOXEL_CHECK(planetFeature.landable == (planetFeature.bodyClass != voxel::world::SpaceBodyClass::GasGiant));
        const auto planetSectorFeatures = space.featuresForSector(planetFeature.sector);
        std::size_t planetCountInSector = 0;
        std::size_t moonCountInPlanetSector = 0;
        std::size_t ringDebrisCountInPlanetSector = 0;
        for (const auto& feature : planetSectorFeatures) {
            if (feature.type == voxel::world::SpaceFeatureType::Planet) {
                ++planetCountInSector;
            } else if (feature.type == voxel::world::SpaceFeatureType::Moon) {
                ++moonCountInPlanetSector;
                VOXEL_CHECK(feature.origin == voxel::world::FeatureOrigin::Natural);
                VOXEL_CHECK(feature.radius >= 220.0F && feature.radius <= 580.0F);
                VOXEL_CHECK(feature.bodyClass != voxel::world::SpaceBodyClass::None);
                VOXEL_CHECK(feature.gravityScale > 0.0F && feature.gravityScale < 0.6F);
                VOXEL_CHECK(feature.landable);
                VOXEL_CHECK(feature.lifeSignal == 0.0F);
            } else if (feature.type == voxel::world::SpaceFeatureType::RingDebris) {
                ++ringDebrisCountInPlanetSector;
                VOXEL_CHECK(feature.bodyClass == voxel::world::SpaceBodyClass::None);
                VOXEL_CHECK(!feature.landable);
                VOXEL_CHECK(feature.surfaceRoughness == 0.0F);
                VOXEL_CHECK(feature.resourceRichness == 0.0F);
            }
        }
        VOXEL_CHECK(planetCountInSector == 1);
        VOXEL_CHECK(moonCountInPlanetSector >= 1);
        VOXEL_CHECK(ringDebrisCountInPlanetSector <= 1);

        const auto landablePlanetCenter = space.featureWorldCenter(landablePlanetFeature);
        const voxel::world::ChunkCoord planetChunkCoord{
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(landablePlanetCenter.x)), voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(landablePlanetCenter.y)), voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(landablePlanetCenter.z)), voxel::world::ChunkSize)
        };
        voxel::world::Chunk planetChunk(planetChunkCoord);
        gen1.generate(planetChunk);
        std::size_t planetBlocks = 0;
        std::size_t planetResourceBlocks = 0;
        std::size_t planetWaterBlocks = 0;
        for (int z = 0; z < voxel::world::ChunkSize; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                    const auto block = planetChunk.blockAt(x, y, z);
                    if (block.value == voxel::world::AirBlockState.value) {
                        continue;
                    }
                    ++planetBlocks;
                    const auto type = voxel::world::blockTypeOf(block);
                    if (type.value == 32U || type.value == 33U || type.value == 34U) {
                        ++planetResourceBlocks;
                    }
                    if (type.value == 12U) {
                        ++planetWaterBlocks;
                    }
                }
            }
        }
        VOXEL_CHECK(planetBlocks > 0);
        VOXEL_CHECK(landablePlanetFeature.bodyClass != voxel::world::SpaceBodyClass::GasGiant);
        VOXEL_CHECK(planetResourceBlocks <= planetBlocks);
        VOXEL_CHECK(planetWaterBlocks == 0);

        const voxel::world::ChunkCoord planetSurfaceChunkCoord{
            voxel::world::floorDiv(
                static_cast<std::int64_t>(std::floor(landablePlanetCenter.x + landablePlanetFeature.radius * 0.98F)),
                voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(landablePlanetCenter.y)), voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(landablePlanetCenter.z)), voxel::world::ChunkSize)
        };
        voxel::world::Chunk planetSurfaceChunk(planetSurfaceChunkCoord);
        gen1.generate(planetSurfaceChunk);
        std::size_t planetSurfaceBlocks = 0;
        std::size_t planetSurfaceWaterBlocks = 0;
        for (int z = 0; z < voxel::world::ChunkSize; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                    const auto block = planetSurfaceChunk.blockAt(x, y, z);
                    if (block.value == voxel::world::AirBlockState.value) {
                        continue;
                    }
                    ++planetSurfaceBlocks;
                    if (voxel::world::blockTypeOf(block).value == 12U) {
                        ++planetSurfaceWaterBlocks;
                    }
                }
            }
        }
        VOXEL_CHECK(planetSurfaceBlocks > 0);
        VOXEL_CHECK(planetSurfaceWaterBlocks == 0);

        const auto gasGiantCenter = space.featureWorldCenter(gasGiantFeature);
        const voxel::world::ChunkCoord gasGiantChunkCoord{
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(gasGiantCenter.x)), voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(gasGiantCenter.y)), voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(gasGiantCenter.z)), voxel::world::ChunkSize)
        };
        voxel::world::Chunk gasGiantChunk(gasGiantChunkCoord);
        gen1.generate(gasGiantChunk);
        bool gasGiantCenterAllAir = true;
        for (int z = 0; z < voxel::world::ChunkSize && gasGiantCenterAllAir; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize && gasGiantCenterAllAir; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize && gasGiantCenterAllAir; ++x) {
                    if (gasGiantChunk.blockAt(x, y, z).value != voxel::world::AirBlockState.value) {
                        gasGiantCenterAllAir = false;
                    }
                }
            }
        }
        VOXEL_CHECK(gasGiantFeature.landable == false);
        VOXEL_CHECK(gasGiantCenterAllAir);

        const auto moonCenter = space.featureWorldCenter(moonFeature);
        const voxel::world::ChunkCoord moonChunkCoord{
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(moonCenter.x)), voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(moonCenter.y)), voxel::world::ChunkSize),
            voxel::world::floorDiv(static_cast<std::int64_t>(std::floor(moonCenter.z)), voxel::world::ChunkSize)
        };
        voxel::world::Chunk moonChunk(moonChunkCoord);
        gen1.generate(moonChunk);
        std::size_t moonBlocks = 0;
        std::size_t moonResourceBlocks = 0;
        for (int z = 0; z < voxel::world::ChunkSize; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                    const auto block = moonChunk.blockAt(x, y, z);
                    if (block.value == voxel::world::AirBlockState.value) {
                        continue;
                    }
                    ++moonBlocks;
                    const auto type = voxel::world::blockTypeOf(block);
                    if (type.value == 32U || type.value == 33U || type.value == 34U) {
                        ++moonResourceBlocks;
                    }
                    VOXEL_CHECK(type.value >= 31U && type.value <= 34U);
                }
            }
        }
        VOXEL_CHECK(moonBlocks > 0);
        VOXEL_CHECK(moonResourceBlocks > 0);

        voxel::world::NoiseTerrainSettings noSpaceSettings = settings;
        noSpaceSettings.enableSpaceAsteroids = false;
        voxel::world::NoiseTerrainGenerator noSpaceGen(noSpaceSettings);
        voxel::world::Chunk disabledSpaceChunk(asteroidChunkCoord);
        noSpaceGen.generate(disabledSpaceChunk);
        bool disabledSpaceAllAir = true;
        for (int z = 0; z < voxel::world::ChunkSize && disabledSpaceAllAir; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize && disabledSpaceAllAir; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize && disabledSpaceAllAir; ++x) {
                    if (disabledSpaceChunk.blockAt(x, y, z).value != voxel::world::AirBlockState.value) {
                        disabledSpaceAllAir = false;
                    }
                }
            }
        }
        VOXEL_CHECK(disabledSpaceAllAir);

        const auto prepassKey = gen1.prepassKey(prepassCoord);
        voxel::world::TerrainColumnPrepassCache duplicateCache;
        VOXEL_CHECK(duplicateCache.tryBeginJob(prepassKey));
        VOXEL_CHECK(!duplicateCache.tryBeginJob(prepassKey));
        duplicateCache.endJobWithoutStore(prepassKey);
        VOXEL_CHECK(duplicateCache.inFlightCount() == 0);

        // Sanity: a chunk straddling the surface has both stone and air.
        voxel::world::Chunk straddler({0, 0, 0});
        gen1.generate(straddler);
        std::size_t airCount = 0;
        std::size_t solidCount = 0;
        for (int z = 0; z < voxel::world::ChunkSize; ++z) {
            for (int y = 0; y < voxel::world::ChunkSize; ++y) {
                for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                    if (straddler.blockAt(x, y, z).value == 0) {
                        ++airCount;
                    } else {
                        ++solidCount;
                    }
                }
            }
        }
        VOXEL_CHECK(airCount > 0);
        VOXEL_CHECK(solidCount > 0);

        // FlatTerrainGenerator is still available and behaves the same as before.
        voxel::world::FlatTerrainGenerator flat;
        voxel::world::Chunk flatChunk({0, 0, 0});
        flat.generate(flatChunk);
        VOXEL_CHECK(voxel::world::blockTypeOf(flatChunk.blockAt(0, 0, 0)).value == 2);
    }

    // ------------------------------------------------------------------
    // Terrain polish: ores + water (Phase F5)
    // ------------------------------------------------------------------
    {
        voxel::world::NoiseTerrainSettings polishSettings{};
        polishSettings.seed = 12345;
        polishSettings.seaLevel = 0.0F;
        voxel::world::NoiseTerrainGenerator polishGen(polishSettings);

        // Sample vertical chunks around sea level and look for ores + water.
        // Surface blocks are checked separately from the prepass because ATGS
        // v1 can place common land well above the old +127 smoke-test band.
        bool sawCoal = false;
        bool sawIron = false;
        bool sawWater = false;
        bool sawPaintedSurface = false;
        bool sawStone = false;
        for (int cz = -8; cz <= 8 && !(sawCoal && sawIron && sawWater && sawStone); cz += 4) {
            for (int cx = -8; cx <= 8 && !(sawCoal && sawIron && sawWater && sawStone); cx += 4) {
                for (int cy = -4; cy <= 3 && !(sawCoal && sawIron && sawWater && sawStone); ++cy) {
                    voxel::world::Chunk strip({cx, cy, cz});
                    polishGen.generate(strip);
                    VOXEL_CHECK(!strip.dirty().save);
                    for (int z = 0; z < voxel::world::ChunkSize; ++z) {
                        for (int y = 0; y < voxel::world::ChunkSize; ++y) {
                            for (int x = 0; x < voxel::world::ChunkSize; ++x) {
                                const auto blockType = voxel::world::blockTypeOf(strip.blockAt(x, y, z)).value;
                                if (blockType == 2) sawStone = true;
                                else if (blockType == 10) sawCoal = true;
                                else if (blockType == 11) sawIron = true;
                                else if (blockType == 12) sawWater = true;
                            }
                        }
                    }
                }
            }
        }

        for (int cz = -16; cz <= 16 && !sawPaintedSurface; cz += 4) {
            for (int cx = -16; cx <= 16 && !sawPaintedSurface; cx += 4) {
                const auto prepass = polishGen.buildColumnPrepass({cx, cz});
                for (int z = 0; z < voxel::world::ChunkSize && !sawPaintedSurface; z += 4) {
                    for (int x = 0; x < voxel::world::ChunkSize && !sawPaintedSurface; x += 4) {
                        const auto idx = static_cast<std::size_t>(x + z * voxel::world::ChunkSize);
                        if (prepass.surfaceKind[idx] != voxel::world::TerrainSurfaceKind::Land) {
                            continue;
                        }

                        const int surfaceY = prepass.surfaceBlockY[idx];
                        const int surfaceChunkY = static_cast<int>(voxel::world::floorDiv(surfaceY, voxel::world::ChunkSize));
                        voxel::world::Chunk surfaceChunk({cx, surfaceChunkY, cz});
                        polishGen.generate(surfaceChunk);
                        VOXEL_CHECK(!surfaceChunk.dirty().save);
                        for (int y = 0; y < voxel::world::ChunkSize && !sawPaintedSurface; ++y) {
                            const auto blockType = voxel::world::blockTypeOf(surfaceChunk.blockAt(x, y, z)).value;
                            if (blockType == 9 || blockType == 20 || blockType == 22 || blockType == 23
                                || blockType == 26 || blockType == 27 || blockType == 28 || blockType == 29) {
                                sawPaintedSurface = true;
                            }
                        }
                    }
                }
            }
        }
        VOXEL_CHECK(sawCoal);
        VOXEL_CHECK(sawIron);
        VOXEL_CHECK(sawWater);
        VOXEL_CHECK(sawPaintedSurface);
        VOXEL_CHECK(sawStone);

        // Regression: deep ocean must fill every vertical water-only chunk up
        // to the global sea level. A previous ActualTopMap initialization bug
        // left water-only chunks empty, so the visible water top followed the
        // seabed chunk and stepped down in 32-block slabs.
        bool foundDeepOceanColumn = false;
        voxel::world::ChunkCoord oceanCoord{};
        int oceanLocalX = 0;
        int oceanLocalZ = 0;
        for (int cz = -128; cz <= 128 && !foundDeepOceanColumn; cz += 8) {
            for (int cx = -128; cx <= 128 && !foundDeepOceanColumn; cx += 8) {
                const auto prepass = polishGen.buildColumnPrepass({cx, cz});
                for (int z = 0; z < voxel::world::ChunkSize && !foundDeepOceanColumn; ++z) {
                    for (int x = 0; x < voxel::world::ChunkSize && !foundDeepOceanColumn; ++x) {
                        const auto idx = static_cast<std::size_t>(x + z * voxel::world::ChunkSize);
                        if (prepass.surfaceKind[idx] == voxel::world::TerrainSurfaceKind::DeepOcean
                            && prepass.surfaceBlockY[idx] <= -64) {
                            foundDeepOceanColumn = true;
                            oceanCoord = {cx, 0, cz};
                            oceanLocalX = x;
                            oceanLocalZ = z;
                        }
                    }
                }
            }
        }
        VOXEL_CHECK(foundDeepOceanColumn);
        if (foundDeepOceanColumn) {
            voxel::world::Chunk seaLevelChunk({oceanCoord.x, 0, oceanCoord.z});
            voxel::world::Chunk belowSeaChunk({oceanCoord.x, -1, oceanCoord.z});
            polishGen.generate(seaLevelChunk);
            polishGen.generate(belowSeaChunk);
            VOXEL_CHECK(voxel::world::blockTypeOf(seaLevelChunk.blockAt(oceanLocalX, 0, oceanLocalZ)).value == 12);
            VOXEL_CHECK(voxel::world::blockTypeOf(seaLevelChunk.blockAt(oceanLocalX, 1, oceanLocalZ)).value == 0);
            VOXEL_CHECK(voxel::world::blockTypeOf(belowSeaChunk.blockAt(oceanLocalX, 0, oceanLocalZ)).value == 12);
            VOXEL_CHECK(voxel::world::blockTypeOf(belowSeaChunk.blockAt(oceanLocalX, voxel::world::ChunkSize - 1, oceanLocalZ)).value == 12);
        }
    }

    // Chunk palette growth: index width tracks unique block count.
    {
        voxel::world::Chunk chunk({99, 0, 0});
        VOXEL_CHECK(chunk.blockData().palette.size() == 1); // air seeded
        VOXEL_CHECK(chunk.blockData().indices.bitsPerEntry() == 1);

        // Two materials: still 1 bit (air + stone).
        chunk.setBlockSilently(0, 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        VOXEL_CHECK(chunk.blockData().palette.size() == 2);
        VOXEL_CHECK(chunk.blockData().indices.bitsPerEntry() == 1);
        VOXEL_CHECK(voxel::world::blockTypeOf(chunk.blockAt(0, 0, 0)).value == 2);
        VOXEL_CHECK(chunk.blockAt(1, 0, 0).value == 0); // air default

        // Add 16 more materials → 18 entries → bits = 5.
        for (std::uint32_t i = 3; i <= 18; ++i) {
            chunk.setBlockSilently(static_cast<int>(i - 3), 0, 0, voxel::world::makeBlockState(voxel::BlockTypeId{i}));
        }
        VOXEL_CHECK(chunk.blockData().palette.size() == 18);
        VOXEL_CHECK(chunk.blockData().indices.bitsPerEntry() == 5);
        // Earlier writes survived the resize.
        VOXEL_CHECK(voxel::world::blockTypeOf(chunk.blockAt(0, 0, 0)).value == 3);
        VOXEL_CHECK(voxel::world::blockTypeOf(chunk.blockAt(15, 0, 0)).value == 18);

        chunk.fillColumnRangeSilently(2, 2, 4, 9, voxel::world::makeBlockState(voxel::BlockTypeId{2}));
        VOXEL_CHECK(voxel::world::blockTypeOf(chunk.blockAt(2, 4, 2)).value == 2);
        VOXEL_CHECK(voxel::world::blockTypeOf(chunk.blockAt(2, 9, 2)).value == 2);
        VOXEL_CHECK(chunk.blockAt(2, 10, 2).value == 0);
    }

    // ------------------------------------------------------------------
    // Frustum culling (Phase H2 prep)
    // ------------------------------------------------------------------
    {
        // Build a viewProjection looking from (0,0,8) toward -Z. The frustum
        // covers a cone in front of the eye. AABBs in front should pass; AABBs
        // behind the camera should be rejected.
        const auto proj = voxel::core::perspectiveVulkan(1.0F, 16.0F / 9.0F, 0.1F, 256.0F);
        const auto view = voxel::core::lookAt(
            voxel::core::Vec3{0.0F, 0.0F, 8.0F},
            voxel::core::Vec3{0.0F, 0.0F, 0.0F},
            voxel::core::Vec3{0.0F, 1.0F, 0.0F});
        const auto vp = voxel::core::multiply(proj, view);
        const auto frustum = voxel::core::extractFrustumPlanes(vp);

        // In front of the camera, at the origin: inside.
        VOXEL_CHECK(voxel::core::aabbIntersectsFrustum(
            frustum,
            voxel::core::Vec3{-1.0F, -1.0F, -1.0F},
            voxel::core::Vec3{1.0F, 1.0F, 1.0F}));

        // Far behind the camera (z = +200, camera looks toward -Z): outside.
        VOXEL_CHECK(!voxel::core::aabbIntersectsFrustum(
            frustum,
            voxel::core::Vec3{-1.0F, -1.0F, 200.0F},
            voxel::core::Vec3{1.0F, 1.0F, 202.0F}));

        // Way off to the side (x = +500): outside.
        VOXEL_CHECK(!voxel::core::aabbIntersectsFrustum(
            frustum,
            voxel::core::Vec3{500.0F, -1.0F, 0.0F},
            voxel::core::Vec3{502.0F, 1.0F, 2.0F}));
    }

    // ---- MpscQueue lock-free correctness -------------------------------
    // Validates that the Vyukov-style MPSC queue produces strict FIFO
    // under concurrent producers, doesn't lose or duplicate items, and
    // handles the empty/drain/single-item edge cases. Without these
    // tests, a subtle ordering bug in the head_/tail_ atomic ops could
    // sit dormant for months until a low-traffic queue scenario hits
    // it during gameplay.
    {
        // 1. Single-threaded push/pop preserves FIFO.
        voxel::core::MpscQueue<int> q;
        VOXEL_CHECK(q.empty());

        int popped = -1;
        VOXEL_CHECK(!q.try_pop(popped));   // empty queue → no pop
        VOXEL_CHECK(popped == -1);

        for (int i = 0; i < 100; ++i) {
            q.push(i);
        }
        VOXEL_CHECK(!q.empty());
        for (int i = 0; i < 100; ++i) {
            VOXEL_CHECK(q.try_pop(popped));
            VOXEL_CHECK(popped == i);
        }
        VOXEL_CHECK(q.empty());
        VOXEL_CHECK(!q.try_pop(popped));   // empty again
    }
    {
        // 2. drain() returns everything currently enqueued in order.
        voxel::core::MpscQueue<int> q;
        for (int i = 0; i < 50; ++i) {
            q.push(i * 2);
        }
        auto drained = q.drain();
        VOXEL_CHECK(drained.size() == 50);
        for (std::size_t i = 0; i < drained.size(); ++i) {
            VOXEL_CHECK(drained[i] == static_cast<int>(i) * 2);
        }
        VOXEL_CHECK(q.empty());
    }
    {
        // 3. drain(maxItems) bounds the batch and leaves leftovers.
        voxel::core::MpscQueue<int> q;
        for (int i = 0; i < 20; ++i) {
            q.push(i);
        }
        auto first = q.drain(8);
        VOXEL_CHECK(first.size() == 8);
        for (std::size_t i = 0; i < first.size(); ++i) {
            VOXEL_CHECK(first[i] == static_cast<int>(i));
        }
        auto rest = q.drain();
        VOXEL_CHECK(rest.size() == 12);
        for (std::size_t i = 0; i < rest.size(); ++i) {
            VOXEL_CHECK(rest[i] == static_cast<int>(i) + 8);
        }
    }
    {
        // 4. Multi-producer correctness: 8 producer threads each push
        //    1000 distinct values, single consumer drains and verifies
        //    no item is lost or duplicated. Ordering across producers is
        //    NOT guaranteed (MPSC is FIFO per-producer, not globally
        //    ordered when producers race), so we sort + compare.
        constexpr int kProducers = 8;
        constexpr int kItemsPerProducer = 1000;
        constexpr int kTotal = kProducers * kItemsPerProducer;

        voxel::core::MpscQueue<int> q;
        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        std::atomic<int> startGate{0};
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&q, &startGate, p]() {
                // Spin briefly so all producers start the push burst
                // roughly together, maximizing contention coverage.
                while (startGate.load(std::memory_order_acquire) == 0) {
                    std::this_thread::yield();
                }
                const int base = p * kItemsPerProducer;
                for (int i = 0; i < kItemsPerProducer; ++i) {
                    q.push(base + i);
                }
            });
        }
        startGate.store(1, std::memory_order_release);

        // Consumer: drain until we've seen all kTotal items. Bounded
        // by a generous deadline so a real bug doesn't hang the test.
        std::vector<int> received;
        received.reserve(kTotal);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int item = 0;
        while (static_cast<int>(received.size()) < kTotal) {
            if (q.try_pop(item)) {
                received.push_back(item);
                continue;
            }
            VOXEL_CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        for (auto& t : producers) {
            t.join();
        }
        // Verify: exactly kTotal items, all values from [0, kTotal) seen
        // exactly once.
        VOXEL_CHECK(received.size() == static_cast<std::size_t>(kTotal));
        std::sort(received.begin(), received.end());
        for (int i = 0; i < kTotal; ++i) {
            VOXEL_CHECK(received[i] == i);
        }
        VOXEL_CHECK(q.empty());
    }
    {
        // 5. Move-only value type works (move-construct, no copy).
        //    Pushes a unique_ptr to validate the move-only path.
        voxel::core::MpscQueue<std::unique_ptr<int>> q;
        q.push(std::make_unique<int>(42));
        q.push(std::make_unique<int>(7));
        std::unique_ptr<int> got;
        VOXEL_CHECK(q.try_pop(got));
        VOXEL_CHECK(got && *got == 42);
        VOXEL_CHECK(q.try_pop(got));
        VOXEL_CHECK(got && *got == 7);
        VOXEL_CHECK(!q.try_pop(got));
    }

    std::cout << "voxel smoke tests passed\n";
    return 0;
}
