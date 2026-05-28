#include <voxel/core/Application.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

#include <imgui.h>

#include <voxel/core/Logger.hpp>
#include <voxel/data/CoreContentIds.hpp>
#include <voxel/data/RegistryLoader.hpp>
#include <voxel/render/MaterialTable.hpp>
#include <voxel/render/meshing/ClipmapRegionMesher.hpp>
#include <voxel/render/meshing/ClusterMesher.hpp>
#include <voxel/render/meshing/RegionMesher.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/CoordinateUtils.hpp>
#include <voxel/world/LightPropagator.hpp>
#include <voxel/world/WorldDelta.hpp>

namespace voxel {

namespace {

world::PlanetCoord adjacentPosition(world::PlanetCoord position, world::BlockCoord normal)
{
    position.block.x += normal.x;
    position.block.y += normal.y;
    position.block.z += normal.z;
    return position;
}

voxel::BlockStateId resolvePlacementState(voxel::BlockStateId base, world::BlockCoord hitNormal, const data::BlockRegistry& registry)
{
    const auto typeId = world::blockTypeOf(base);
    const auto* def = registry.registry().byRuntimeId(typeId.value);
    if (def == nullptr || !def->stateSchema.hasAxis) {
        return base;
    }

    world::BlockAxis axis = world::BlockAxis::Y;
    if (hitNormal.x != 0) {
        axis = world::BlockAxis::X;
    } else if (hitNormal.z != 0) {
        axis = world::BlockAxis::Z;
    }
    return world::withAxis(base, axis);
}

std::uint64_t elapsedUs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

double timerAverageMs(const core::RuntimeCounters::Timer& timer)
{
    return timer.count == 0 ? 0.0 : (static_cast<double>(timer.totalUs) / static_cast<double>(timer.count)) / 1000.0;
}

double timerMaxMs(const core::RuntimeCounters::Timer& timer)
{
    return static_cast<double>(timer.maxUs) / 1000.0;
}

std::uint64_t meshBytes(const render::meshing::ChunkMesh& mesh)
{
    return static_cast<std::uint64_t>(mesh.vertices.size() * sizeof(render::meshing::VoxelVertex))
        + static_cast<std::uint64_t>(mesh.indices.size() * sizeof(std::uint32_t));
}

ItemTypeId itemForPlacedBlock(BlockStateId blockState, const data::BlockRegistry& blocks, const data::ItemRegistry& items)
{
    const auto blockType = world::blockTypeOf(blockState);
    const auto* blockDef = blocks.registry().byRuntimeId(blockType.value);
    if (blockDef == nullptr) {
        return {};
    }

    for (const auto& itemDef : items.registry().entries()) {
        if (itemDef.hasBlockPlacement() && itemDef.blockPlacementId == blockDef->id) {
            return items.findRuntimeId(itemDef.id);
        }
    }
    return {};
}

std::int64_t chunkDistanceScore(world::ChunkCoord coord, world::ChunkCoord center) noexcept
{
    const auto dx = coord.x - center.x;
    const auto dy = coord.y - center.y;
    const auto dz = coord.z - center.z;
    return (dx * dx) + (dy * dy * 4) + (dz * dz);
}

std::int64_t intervalDistanceToPoint(
    std::int64_t minValue,
    std::int64_t maxValue,
    std::int64_t point) noexcept
{
    if (point < minValue) {
        return minValue - point;
    }
    if (point > maxValue) {
        return point - maxValue;
    }
    return 0;
}

std::int64_t clusterHorizontalDistanceChunks(
    world::ClusterCoord cluster,
    world::ChunkCoord center) noexcept
{
    const auto origin = world::clusterChunkOrigin(cluster);
    const auto dx = intervalDistanceToPoint(
        origin.x,
        origin.x + world::ClusterChunkExtent - 1,
        center.x);
    const auto dz = intervalDistanceToPoint(
        origin.z,
        origin.z + world::ClusterChunkExtent - 1,
        center.z);
    return std::max(dx, dz);
}

std::int64_t regionHorizontalDistanceChunks(
    world::RegionCoord region,
    world::ChunkCoord center) noexcept
{
    const auto origin = world::regionChunkOrigin(region);
    const auto dx = intervalDistanceToPoint(
        origin.x,
        origin.x + world::RegionChunkExtent - 1,
        center.x);
    const auto dz = intervalDistanceToPoint(
        origin.z,
        origin.z + world::RegionChunkExtent - 1,
        center.z);
    return std::max(dx, dz);
}

std::size_t estimateStreamingChunkCapacity(const world::StreamingSettings& settings) noexcept
{
    const auto horizontalDiameter = static_cast<std::size_t>(std::max(0, settings.renderDistanceChunks) * 2 + 1);
    const auto verticalDiameter = static_cast<std::size_t>(std::max(0, settings.verticalRenderDistanceChunks) * 2 + 1);
    return (horizontalDiameter * horizontalDiameter * verticalDiameter) + horizontalDiameter * horizontalDiameter;
}

world::NoiseTerrainSettings terrainSettingsForConfig(const ApplicationConfig& config,
                                                     const data::CoreBlockIds& coreBlocks = {})
{
    world::NoiseTerrainSettings settings{};
    data::applyCoreBlockIds(settings, coreBlocks);
    settings.enableSpaceAsteroids = config.enableSpacePhaseA;
    settings.space = config.space;
    return settings;
}

std::size_t detectedThreadCount() noexcept
{
    return std::max<std::size_t>(1U, static_cast<std::size_t>(std::thread::hardware_concurrency()));
}

std::size_t workerCountForPercent(std::uint32_t percent) noexcept
{
    return std::max<std::size_t>(1U, (detectedThreadCount() * percent) / 100U);
}

void applyRuntimeWorkProfile(ApplicationConfig& config, std::size_t workerBudget, bool aggressive)
{
    workerBudget = std::max<std::size_t>(workerBudget, 1U);
    config.chunkPipeline.maxLoadsOrGenerationsPerTick = std::clamp<std::size_t>(
        workerBudget * (aggressive ? 3U : 2U), 8U, aggressive ? 96U : 64U);
    config.chunkPipeline.maxGenerationInstallsPerTick = std::clamp<std::size_t>(
        workerBudget * (aggressive ? 3U : 2U), 8U, aggressive ? 128U : 96U);
    config.chunkPipeline.minGenerationInstallsPerTick = std::clamp<std::size_t>(workerBudget / 2U, 4U, 16U);
    config.chunkPipeline.maxInFlightGeneration = std::clamp<std::size_t>(
        workerBudget * (aggressive ? 12U : 10U), 64U, aggressive ? 512U : 384U);
    config.maxChunkMeshesPerTick = std::clamp<std::size_t>(
        workerBudget * (aggressive ? 5U : 4U), 16U, aggressive ? 256U : 192U);
    config.workBudget.maxMeshJobsPerTick = config.maxChunkMeshesPerTick;
    config.workBudget.maxMeshInstallsPerTick = std::clamp<std::size_t>(
        workerBudget * (aggressive ? 4U : 3U), 16U, aggressive ? 256U : 192U);
    config.workBudget.maxGpuUploadBytesPerTick = 64U * 1024U * 1024U;
    config.workBudget.maxUploadSubmitMsPerFrame = aggressive ? 10.0 : 6.0;
}

float lerpFloat(float a, float b, float t) noexcept
{
    return a + (b - a) * std::clamp(t, 0.0F, 1.0F);
}

float sanitizedFarPlane(float value, float fallback) noexcept
{
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, 128.0F, 524288.0F);
}

std::uint64_t mixStarBits(std::uint64_t value) noexcept
{
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

float starUnitFloat(std::uint64_t& state) noexcept
{
    state = mixStarBits(state + 0x9e3779b97f4a7c15ULL);
    return static_cast<float>((state >> 40U) & 0xFFFFFFU) / static_cast<float>(0xFFFFFFU);
}

bool sameVisibleFaceRecords(std::vector<render::meshing::VisibleFaceRecord> lhs,
                            std::vector<render::meshing::VisibleFaceRecord> rhs)
{
    const auto less = [](const auto& a, const auto& b) {
        if (a.localIndex != b.localIndex) return a.localIndex < b.localIndex;
        if (a.faceIndex != b.faceIndex) return a.faceIndex < b.faceIndex;
        if (a.materialId != b.materialId) return a.materialId < b.materialId;
        if (a.packedLight != b.packedLight) return a.packedLight < b.packedLight;
        return static_cast<int>(a.surface) < static_cast<int>(b.surface);
    };
    std::sort(lhs.begin(), lhs.end(), less);
    std::sort(rhs.begin(), rhs.end(), less);
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].localIndex != rhs[i].localIndex
            || lhs[i].faceIndex != rhs[i].faceIndex
            || lhs[i].surface != rhs[i].surface
            || lhs[i].materialId != rhs[i].materialId
            || lhs[i].packedLight != rhs[i].packedLight) {
            return false;
        }
    }
    return true;
}

} // namespace

Application::Application(ApplicationConfig config)
    : config_(std::move(config)),
      chunks_(),
      streamer_(chunks_),
      terrainGenerator_(terrainSettingsForConfig(config_)),
      spaceEnvironment_(config_.space),
      saveStore_(config_.paths.saveRoot()),
      // M2: WorldRegistry indexes the parent saves/ directory so the World
      // Manager overlay can enumerate sibling worlds (not just the active one).
      worldRegistry_(config_.paths.savesDirectory()),
      blockEditor_(blocks_)
{
}

Application::~Application()
{
    shutdown();
}

int Application::run()
{
    initialize();

    std::size_t frame = 0;
    while (window_ != nullptr && !window_->shouldClose()) {
        window_->pollEvents();
        tick();
        ++frame;

        if (returnToTitleRequested_) {
            // M2/N3: World Manager / Title Screen asked the launcher to
            // loop back. We still let shutdown() run so player state +
            // inventory + world descriptor get persisted before we exit.
            break;
        }
        if (config_.maxFrames > 0 && frame >= config_.maxFrames) {
            break;
        }
    }

    shutdown();
    return returnToTitleRequested_ ? kReturnToTitle : kExitNormal;
}

void Application::initialize()
{
    if (initialized_) {
        return;
    }

    Logger::info("Initializing engine modules");
    window_ = std::make_unique<platform::GlfwWindow>(platform::WindowConfig{config_.name, config_.width, config_.height});
    window_->setCursorCaptured(true);
    jobs_.start(config_.workerCount);
    const auto chunkCapacity = estimateStreamingChunkCapacity(config_.streaming);
    chunks_.reserve(chunkCapacity);
    meshQueue_.reserve(chunkCapacity);
    lightingQueue_.reserve(chunkCapacity);
    data::RegistryLoader loader;
    auto loadResult = loader.loadCoreData(config_.paths.coreDataRoot(), blocks_, items_, recipes_);
    if (!loadResult.blocksLoaded) {
        Logger::warn("Core data files missing; falling back to built-in block definitions");
        blocks_.registerCoreBlocks();
    }
    if (loadResult.itemsLoaded) {
        Logger::info("Loaded " + std::to_string(loadResult.itemCount) + " items");
    }
    if (loadResult.recipesLoaded) {
        Logger::info("Loaded " + std::to_string(loadResult.recipeCount) + " recipes");
    }
    coreBlocks_ = data::resolveCoreBlockIds(blocks_);
    if (!coreBlocks_.allRequiredResolved) {
        Logger::warn("Core block ID resolution fell back for one or more entries; check core block data.");
    }
    raycaster_.setFluidBlockType(coreBlocks_.waterType);
    terrainGenerator_ = world::NoiseTerrainGenerator(terrainSettingsForConfig(config_, coreBlocks_));
    {
        const auto terrainPath = config_.paths.coreDataRoot() / "terrain.json";
        if (std::filesystem::exists(terrainPath)) {
            world::TerrainDefinitionLoader terrainLoader;
            auto terrainResult = terrainLoader.load(terrainPath, terrainDefinitions_);
            if (!terrainResult.ok()) {
                Logger::warn("Terrain definition load failed: " + terrainResult.error);
            } else if (terrainResult.loaded) {
                terrainGenerator_.setTerrainDefinitions(&terrainDefinitions_);
            }
        } else {
            Logger::warn("Terrain definition file missing: " + terrainPath.string());
        }
    }
    hotbar_.reset(coreBlocks_);
    selectedPlaceBlock_ = hotbar_.selected().block;
    seedCreativeInventory();
    blockRenderCatalog_ = blocks_.buildRenderCatalog();
    blockCollisionCatalog_ = blocks_.buildCollisionCatalog();
    player_.setCollisionCatalog(blockCollisionCatalog_);
    spawnResolver_.setCollisionCatalog(blockCollisionCatalog_);
    blockLightCatalog_ = blocks_.buildLightCatalog();
    kineticCatalog_ = blocks_.buildKineticCatalog();
    player_.setWalkSpeed(config_.playerWalkSpeed);
    player_.setFlySpeed(config_.playerFlySpeed);
    if (config_.titleScreenMode) {
        // N1: title screen mode never touches a world. Skip player state +
        // inventory loads entirely; their save paths point at whatever
        // default world the launcher built with, which we'd rather not
        // accidentally clobber.
        Logger::info("Application running in title-screen mode (no world loaded).");
    } else if (config_.debugStartPosition.has_value()) {
        player_.setPosition(*config_.debugStartPosition);
        player_.setNoclip(config_.debugStartNoclip);
        playerSpawnResolved_ = true;
        Logger::info(
            "Debug start position x=" + std::to_string(config_.debugStartPosition->x)
            + " y=" + std::to_string(config_.debugStartPosition->y)
            + " z=" + std::to_string(config_.debugStartPosition->z));
    } else if (auto saved = playerStateSaveService_.load(config_.paths.saveRoot()); saved.has_value()) {
        // L2: restore the player exactly where they logged out, including
        // their look direction and noclip toggle. A saved state overrides
        // the spawn-resolver path that otherwise hunts for the surface.
        player_.setPosition(saved->position);
        player_.setLook(saved->yawRadians, saved->pitchRadians);
        player_.setNoclip(saved->noclip);
        playerSpawnResolved_ = true;
        Logger::info(
            "Loaded player state x=" + std::to_string(saved->position.x)
            + " y=" + std::to_string(saved->position.y)
            + " z=" + std::to_string(saved->position.z));
    }
    // L2: restore inventory if a saved one exists. The creative-seed path
    // above already populated the default contents; loading overwrites those
    // slots with whatever the player had last session.
    if (!config_.titleScreenMode
        && playerInventorySaveService_.load(config_.paths.saveRoot(), playerInventory_, items_)) {
        Logger::info("Loaded player inventory from " + config_.paths.saveRoot().string());
    }
    maxLightPropagationsPerTick_ = config_.workBudget.maxLightingPerTick;
    physics_.initialize();
    magic_.initialize();
    network_.initialize();
    simulation_.initialize();
    world::registerCoreBlockEntityTypes(blockEntityTypes_);
    blockEntityScheduler_.initialize(blocks_, blockEntityTypes_);
    terrainGenerator_.setPrepassCache(terrainPrepassCache_);
    renderer_.initialize({
        config_.name,
        config_.width,
        config_.height,
        window_.get(),
        config_.enableGpuCulling,
        config_.compareGpuCulling});
    renderer_.uploadMaterialTable(render::MaterialTableBuilder::build(blocks_));
    // Initialize the ImGui debug overlay after the renderer's render pass +
    // swapchain are ready. The window provides the GLFW handle for ImGui's
    // GLFW backend (mouse / keyboard / clipboard).
    if (window_ != nullptr) {
        renderer_.initializeImGui(*window_);
    }
    // W2: tell the fluid system which block id represents water so its hot
    // loops don't have to query the registry per cell.
    {
        world::FluidSystemSettings fs = fluidSystem_.settings();
        fs.waterBlockValue = terrainGenerator_.settings().waterBlock.value;
        if (config_.enableSpacePhaseA) {
            fs.maxActiveWorldY = static_cast<std::int32_t>(std::floor(config_.space.atmosphereTopY)) - 1;
        } else {
            fs.maxActiveWorldY.reset();
        }
        fluidSystem_.setSettings(fs);
    }
    // EXPERIMENTAL: opt-in GPU compute fluid sim. Phase 1 validates that the
    // Vulkan resources (slot pool, block bits, events + readback, descriptor
    // set) allocate cleanly. tick() is still a CPU no-op until Phases 2-3
    // wire the compute shader.
    if (config_.useGpuFluidSim) {
        world::FluidSystemSettings fs = fluidSystem_.settings();
        fluidGpuSystem_ = std::make_unique<world::FluidGpuSystem>(renderer_, fs);
        if (!fluidGpuSystem_->initialize()) {
            Logger::error("Application: FluidGpuSystem initialization failed; "
                          "falling back to CPU FluidSystem.");
            fluidGpuSystem_.reset();
        }
    }
    if (config_.useGpuHybridMeshing) {
        hybridMeshingGpuSystem_ = std::make_unique<render::meshing::HybridMeshingGpuSystem>(renderer_);
        if (!hybridMeshingGpuSystem_->initialize()) {
            Logger::error("Application: HybridMeshingGpuSystem initialization failed; "
                          "falling back to CPU GreedyMesher.");
            hybridMeshingGpuSystem_.reset();
        } else {
            std::size_t selfTestsPassed = 0;
            std::size_t selfTestsFailed = 0;
            std::size_t selfTestFaces = 0;
            const auto stone = coreBlocks_.stone;
            const auto glass = coreBlocks_.glass;
            const auto water = terrainGenerator_.settings().waterBlock;

            const auto checkHybridCase = [&](std::string_view name,
                                             const world::Chunk& chunk,
                                             const render::meshing::ChunkNeighborhood& neighborhood,
                                             render::meshing::MeshingOptions options) {
                const auto gpuFaces = hybridMeshingGpuSystem_->classifyBlocking(
                    chunk, blockRenderCatalog_, nullptr, neighborhood, options);
                const auto cpuFaces = mesher_.classifyVisibleFaces(
                    chunk, blockRenderCatalog_, nullptr, neighborhood, options);
                if (!sameVisibleFaceRecords(gpuFaces, cpuFaces)) {
                    ++selfTestsFailed;
                    Logger::warn(std::string{"Application: HybridMeshingGpuSystem self-test mismatch for "}
                                 + std::string{name}
                                 + "; gpu_faces=" + std::to_string(gpuFaces.size())
                                 + " cpu_faces=" + std::to_string(cpuFaces.size()));
                    return;
                }
                ++selfTestsPassed;
                selfTestFaces += gpuFaces.size();
            };

            world::Chunk singleBlock{{0, 0, 0}};
            singleBlock.setBlock(0, 0, 0, stone);
            checkHybridCase("single_block", singleBlock, {}, {});

            world::Chunk mergedBlocks{{1, 0, 0}};
            mergedBlocks.setBlock(0, 0, 0, stone);
            mergedBlocks.setBlock(1, 0, 0, stone);
            checkHybridCase("merged_blocks", mergedBlocks, {}, {});

            world::Chunk materialSplit{{2, 0, 0}};
            materialSplit.setBlock(0, 0, 0, stone);
            materialSplit.setBlock(1, 0, 0, glass);
            checkHybridCase("material_split", materialSplit, {}, {});

            world::Chunk waterPair{{3, 0, 0}};
            waterPair.setBlock(0, 0, 0, water);
            waterPair.setBlock(1, 0, 0, water);
            checkHybridCase("water_pair", waterPair, {}, {});

            world::Chunk waterLeft{{4, 0, 0}};
            world::Chunk waterRight{{5, 0, 0}};
            waterLeft.setBlock(world::ChunkSize - 1, 0, 0, water);
            waterRight.setBlock(0, 0, 0, water);
            render::meshing::ChunkNeighborhood waterNeighborhood{};
            waterNeighborhood.posX = &waterRight;
            checkHybridCase("water_cross_chunk", waterLeft, waterNeighborhood, {});

            render::meshing::MeshingOptions staticWaterOptions{};
            staticWaterOptions.staticWaterSurfaceY = static_cast<int>(std::floor(terrainGenerator_.settings().seaLevel));
            world::Chunk staticEdgeWater{{6, 0, 0}};
            staticEdgeWater.setBlock(world::ChunkSize - 1, 0, 0, water);
            checkHybridCase("static_water_missing_neighbor", staticEdgeWater, {}, staticWaterOptions);

            if (selfTestsFailed != 0) {
                Logger::warn("Application: HybridMeshingGpuSystem startup self-tests failed; passed="
                             + std::to_string(selfTestsPassed)
                             + " failed=" + std::to_string(selfTestsFailed));
            } else {
                Logger::info("Application: HybridMeshingGpuSystem startup self-tests passed; cases="
                             + std::to_string(selfTestsPassed)
                             + " faces=" + std::to_string(selfTestFaces));
            }
        }
    }
    if (config_.useClusterLod) {
        clusterRenderer_ = std::make_unique<render::ClusterRenderer>(renderer_);
        if (!clusterRenderer_->initialize()) {
            Logger::error("Application: ClusterRenderer initialization failed; "
                          "disabling LOD2 cluster rendering for this session.");
            clusterRenderer_.reset();
        } else {
            // Phase 1C-4b: wire the cluster draw recording into the
            // renderer's per-frame external-draw hook. The hook runs
            // inside the same render pass as chunk draws, after chunks
            // but before debug-line overlays — so cluster meshes share
            // the depth buffer with chunks (chunks naturally occlude
            // the LOD2 mesh where both exist, per persistence policy).
            auto* clusters = clusterRenderer_.get();
            renderer_.setExternalDrawHook(
                [clusters](const render::VulkanRenderer::ExternalDrawContext& ctx) {
                    clusters->recordDraws(
                        ctx.commandBuffer,
                        reinterpret_cast<const float*>(&ctx.viewProjection),
                        ctx.lightParams,
                        ctx.cameraParams,
                        ctx.cameraWorldParams,
                        ctx.atmosphereParams);
                });
            renderer_.setSwapchainRecreatedHook([this]() {
                if (clusterRenderer_ != nullptr && !clusterRenderer_->rebuildSwapchainResources()) {
                    Logger::warn("Application: ClusterRenderer pipeline rebuild after swapchain recreation failed; disabling LOD rendering.");
                    config_.useClusterLod = false;
                }
            });

            // Phase 1D-2: bring up the GPU classifier. Failure here just
            // falls back to the CPU mesher path in tickClusterMaintenance —
            // LOD still works, just synchronously and more expensively.
            clusterGpuSystem_ =
                std::make_unique<render::meshing::ClusterGpuMeshing>(renderer_);
            if (!clusterGpuSystem_->initialize()) {
                Logger::warn("Application: ClusterGpuMeshing initialization "
                             "failed; LOD2 cluster builds will run synchronously "
                             "on the CPU main thread.");
                clusterGpuSystem_.reset();
            }

            // Phase 1D-2b: bring up the on-disk LOD2 mesh cache. The
            // cache lives alongside the save data at
            // `{saveRoot}/lod2_cache/`. Init failure just disables the
            // cache — the engine still builds clusters from scratch
            // each session in that case.
            (void)clusterMeshCache_.initialize(
                config_.paths.saveRoot() / "lod2_cache");
            // Phase 1D-3b: parallel cache for LOD3 region meshes.
            // Separate directory means LOD2 ↔ LOD3 coord aliases
            // (e.g., LOD2 cluster (4,0,0) and LOD3 region (1,0,0)
            // both serialize to key "4_0_0") never collide.
            (void)regionMeshCache_.initialize(
                config_.paths.saveRoot() / "lod3_cache");
        }
    }
    runtimeStats_.reset();
    initialized_ = true;
}



void Application::tick()
{
    if (!renderer_.beginFrame()) {
        // No swapchain image was acquired, usually because the window is
        // minimized or resize recreation is waiting for a non-zero extent.
        // Do not build ImGui draw data, upload meshes, or call endFrame()
        // without a live image index for this tick.
        return;
    }
    // Open the ImGui frame and build the debug overlay using the previous
    // frame's recorded counters. The 1-frame lag is invisible at 60+ FPS and
    // avoids the chicken-and-egg of "ImGui::Render must happen before world
    // rendering, but some counters aren't available until after."
    // N1: title-screen mode skips all world systems and only drives the
    // ImGui menu + presents. This keeps a single Application path (window,
    // Vulkan, ImGui all live) instead of forking into a "menu app" /
    // "game app" split. tickTitleScreen() handles its own beginFrame.
    if (config_.titleScreenMode) {
        tickTitleScreen();
        return;
    }

    renderer_.beginImGuiFrame();
    debugOverlay_.draw(debugOverlayVisible_);
    drawRuntimeSettingsOverlay();
    renderer_.endImGuiFrame();
    constexpr Tick tickIndex = 1;
    const auto frameStart = std::chrono::steady_clock::now();
    auto stageStart = frameStart;
    core::RuntimeCounters frameCounters{};
    frameCounters.frames = 1;
    const auto markStage = [&](core::RuntimeCounters::Timer& timer) {
        const auto stageEnd = std::chrono::steady_clock::now();
        core::recordTimer(timer, elapsedUs(stageStart, stageEnd));
        stageStart = stageEnd;
    };
    const auto markSubStage = [](core::RuntimeCounters::Timer& timer, std::chrono::steady_clock::time_point start) {
        core::recordTimer(timer, elapsedUs(start, std::chrono::steady_clock::now()));
    };

    // 1. Install any chunks generated by workers in previous ticks (drains mailbox).
    // 2. Plan + dispatch generation jobs for new requests.
    auto streamSubStageStart = std::chrono::steady_clock::now();
    const auto& requests = streamingRequestsForFrame();
    markSubStage(frameCounters.stageStreamPlan, streamSubStageStart);
    streamSubStageStart = std::chrono::steady_clock::now();
    const auto& dispatchRequests = streamingDispatchRequestsForFrame(requests);
    markSubStage(frameCounters.stageStreamDispatch, streamSubStageStart);
    frameCounters.chunkRequestsPlanned = requests.size();
    streamSubStageStart = std::chrono::steady_clock::now();
    frameCounters.terrainPrepassJobsSubmitted = dispatchTerrainPrepassJobs(requests);
    markSubStage(frameCounters.stageStreamPrepass, streamSubStageStart);
    auto chunkPipelineSettings = config_.chunkPipeline;
    chunkPipelineSettings.installPriorityCenter = streamingCenter();
    chunkPipelineSettings.workerLoadRoot = config_.paths.saveRoot();
    streamSubStageStart = std::chrono::steady_clock::now();
    const auto stats = chunkPipeline_.processRequestsAsync(
        chunks_, saveStore_, terrainGenerator_, jobs_, chunkJobMailbox_, dispatchRequests, chunkPipelineSettings);
    markSubStage(frameCounters.stageStreamPipeline, streamSubStageStart);
    frameCounters.generationJobsSubmitted = stats.dispatched;
    frameCounters.generationJobsCompleted = stats.loaded;
    frameCounters.remeshesCausedByNeighborInstall = stats.neighborRemeshes;
    frameCounters.terrainGeneration = stats.generationTime;
    frameCounters.terrainGenerationFromPrepass = stats.generationFromPrepassTime;
    frameCounters.terrainGenerationDirect = stats.generationDirectTime;
    core::mergeTimer(frameCounters.queueWait, stats.queueWaitTime);
    frameCounters.load = stats.loadTime;
    streamSubStageStart = std::chrono::steady_clock::now();
    enqueueInstalledChunkWork(stats);
    (void)enqueueVisibleMeshWork(requests);
    markSubStage(frameCounters.stageStreamEnqueue, streamSubStageStart);
    markStage(frameCounters.stageStreaming);

    if (window_ != nullptr) {
        const bool escapeDown = window_->keyDown(platform::Key::Escape);
        if (escapeDown && !escapeToggleLatch_) {
            if (inventoryOpen_) {
                if (!cursorStack_.empty()) {
                    cursorStack_ = playerInventory_.insert(cursorStack_, items_);
                    cursorStack_.clear();
                }
                inventoryOpen_ = false;
                window_->setCursorCaptured(true);
                hasPlayerCursor_ = false;
                Logger::info("Inventory overlay: closed");
            } else if (window_->cursorCaptured()) {
                window_->setCursorCaptured(false);
                hasPlayerCursor_ = false;
                Logger::info("Mouse released; click the sandbox window to capture again.");
            }
        }
        escapeToggleLatch_ = escapeDown;

        const bool anyMouseDown = window_->mouseButtonDown(platform::MouseButton::Left)
            || window_->mouseButtonDown(platform::MouseButton::Right);
        if (!inventoryOpen_ && !window_->cursorCaptured() && anyMouseDown) {
            window_->setCursorCaptured(true);
            hasPlayerCursor_ = false;
            Logger::info("Mouse captured.");
        }

        const bool freecamDown = window_->keyDown(platform::Key::C);
        if (freecamDown && !freecamToggleLatch_) {
            freecam_ = !freecam_;
            hasPlayerCursor_ = false;
            if (freecam_) {
                renderer_.setDebugCameraPose(player_.dEyePosition(), player_.yawRadians(), player_.pitchRadians());
            }
            Logger::info(std::string{"Camera mode: "} + (freecam_ ? "freecam" : "player"));
        }
        freecamToggleLatch_ = freecamDown;

        const bool inventoryDown = window_->keyDown(platform::Key::I);
        if (inventoryDown && !inventoryToggleLatch_) {
            const bool wasOpen = inventoryOpen_;
            inventoryOpen_ = !inventoryOpen_;
            if (wasOpen && !cursorStack_.empty()) {
                cursorStack_ = playerInventory_.insert(cursorStack_, items_);
                cursorStack_.clear();
            }
            window_->setCursorCaptured(!inventoryOpen_);
            hasPlayerCursor_ = false;
            Logger::info(std::string{"Inventory overlay: "} + (inventoryOpen_ ? "open" : "closed"));
        }
        inventoryToggleLatch_ = inventoryDown;

        const bool noclipDown = window_->keyDown(platform::Key::V);
        if (noclipDown && !noclipToggleLatch_) {
            player_.toggleNoclip();
            Logger::info(std::string{"Player noclip: "} + (player_.noclip() ? "on" : "off"));
        }
        noclipToggleLatch_ = noclipDown;

        // F3 toggles the ImGui debug overlay (per-pipeline-stage timings,
        // frame-time graph, copy-to-clipboard button).
        const bool overlayKeyDown = window_->keyDown(platform::Key::F3);
        if (overlayKeyDown && !debugOverlayToggleLatch_) {
            debugOverlayVisible_ = !debugOverlayVisible_;
            Logger::info(std::string{"Debug overlay: "} + (debugOverlayVisible_ ? "on" : "off"));
        }
        debugOverlayToggleLatch_ = overlayKeyDown;

        // M2: F9 toggles the in-game World Manager. Free the cursor on open
        // so the player can immediately click ImGui buttons.
        const bool worldMgrKeyDown = window_->keyDown(platform::Key::F9);
        if (worldMgrKeyDown && !worldManagerToggleLatch_) {
            worldManagerOpen_ = !worldManagerOpen_;
            if (worldManagerOpen_) {
                window_->setCursorCaptured(false);
            }
            Logger::info(std::string{"World manager: "} + (worldManagerOpen_ ? "open" : "closed"));
        }
        worldManagerToggleLatch_ = worldMgrKeyDown;

        updateSpaceEnvironment(freecam_ ? renderer_.debugCameraDPosition() : player_.dEyePosition());

        const bool shouldAttemptSpawnResolve = !playerSpawnResolved_
            && (frameIndex_ < 5 || frameIndex_ - lastSpawnResolveAttemptFrame_ >= 10);
        if (shouldAttemptSpawnResolve) {
            lastSpawnResolveAttemptFrame_ = frameIndex_;
            playerSpawnResolved_ = tryResolvePlayerSpawn();
        }

        if (inventoryOpen_) {
            renderer_.setDebugCameraPose(player_.dEyePosition(), player_.yawRadians(), player_.pitchRadians());
        } else if (freecam_) {
            renderer_.updateDebugCamera(*window_, 1.0F / 60.0F);
        } else if (!playerSpawnResolved_) {
            renderer_.setDebugCameraPose(player_.dEyePosition(), player_.yawRadians(), player_.pitchRadians());
        } else {
            player_.tick(chunks_, gatherPlayerInput(), 1.0F / 60.0F);
            renderer_.setDebugCameraPose(player_.dEyePosition(), player_.yawRadians(), player_.pitchRadians());
        }
        updateSpaceEnvironment(freecam_ ? renderer_.debugCameraDPosition() : player_.dEyePosition());
        // W0: tell the renderer whether the camera is currently in a water block.
        // Drives the underwater fog effect. Uses the same camera selection as
        // raycast (freecam vs player) so flying through water in freecam shows fog.
        {
            const auto cameraPos = freecam_ ? renderer_.debugCameraPosition() : player_.eyePosition();
            const auto local = world::toChunkLocal(
                static_cast<std::int64_t>(std::floor(cameraPos.x)),
                static_cast<std::int64_t>(std::floor(cameraPos.y)),
                static_cast<std::int64_t>(std::floor(cameraPos.z)));
            float underwater = 0.0F;
            if (const auto* chunk = chunks_.find(local.chunk)) {
                const auto block = chunk->blockAt(local.local.x, local.local.y, local.local.z);
                if (coreBlocks_.isWater(block)) {
                    underwater = 1.0F;
                }
            }
            renderer_.setCameraUnderwater(underwater);
        }
        updateSelectedBlock();
        updateSpellCasting();
        const bool uiCapturesMouse = debugOverlay_.wantsMouseCapture() || runtimeSettingsMouseCapture_;
        const auto interactionStats = (window_->cursorCaptured() && !inventoryOpen_ && !uiCapturesMouse && !spellCastingMode_)
            ? handleWorldInteraction()
            : core::RuntimeCounters{};
        frameCounters.blockEditsAccepted += interactionStats.blockEditsAccepted;
        frameCounters.blockEditsRejected += interactionStats.blockEditsRejected;
        frameCounters.dirtyMeshChunksQueued += interactionStats.dirtyMeshChunksQueued;
        frameCounters.dirtyMeshChunksCoalesced += interactionStats.dirtyMeshChunksCoalesced;
        frameCounters.dirtyLightingChunksQueued += interactionStats.dirtyLightingChunksQueued;
        frameCounters.dirtyLightingChunksCoalesced += interactionStats.dirtyLightingChunksCoalesced;
        if (inventoryOpen_) {
            handleInventoryInteraction();
        }
    }
    markStage(frameCounters.stagePlayer);

    // 3. Install mesh results from prior ticks, then dispatch new mesh jobs.
    //    The default shader-lighting path skips CPU propagation entirely; the
    //    optional CPU path still runs after meshing so visibility is not blocked.
    const auto meshInstallStats = installMeshResults();
    markStage(frameCounters.stageMeshInstall);
    const auto meshDispatchStats = dispatchMeshJobs();
    markStage(frameCounters.stageMeshDispatch);
    // tickClusterMaintenance MOVED — see end of tick (after stageSimulation
    // markStage). Was running here AND again before the simulation
    // markStage, doubling the work and attributing ~15ms of LOD cost to
    // light_ms/sim_ms. Phase 1D-2 will move the build itself to workers.
    const auto lightingStats = config_.useGpuShaderLighting ? LightingStats{} : propagateLightingForDirtyChunks();
    markStage(frameCounters.stageLighting);
    frameCounters.meshJobsCompleted = meshInstallStats.completed;
    frameCounters.meshJobsDiscardedStale = meshInstallStats.staleDiscarded;
    frameCounters.lightingRecomputes = lightingStats.recomputed;
    frameCounters.lightingJobsSubmitted = lightingStats.submitted;
    frameCounters.lightingJobsCompleted = lightingStats.completed;
    frameCounters.lightingJobsDiscardedStale = lightingStats.staleDiscarded;
    frameCounters.lightingPropagation = lightingStats.propagationTime;
    core::mergeTimer(frameCounters.queueWait, lightingStats.queueWaitTime);
    frameCounters.meshJobsSubmitted = meshDispatchStats.submitted;
    frameCounters.meshSnapshot = meshDispatchStats.snapshotTime;
  
    frameCounters.meshBuild = meshInstallStats.meshBuildTime;
    core::mergeTimer(frameCounters.queueWait, meshInstallStats.queueWaitTime);
    frameCounters.uploadBudgetDeferrals = meshInstallStats.uploadBudgetDeferrals;
    if (!config_.useGpuShaderLighting && lightingStats.recomputed >= maxLightPropagationsPerTick_) {
        frameCounters.lightingBudgetSaturated = 1;
    }
    if (meshInstallStats.uploadBudgetDeferrals > 0) {
        frameCounters.meshInstallBudgetSaturated = 1;
    }

    const bool saveInterval = frameIndex_ > 0 && config_.workBudget.saveFlushIntervalFrames > 0
        && (frameIndex_ % config_.workBudget.saveFlushIntervalFrames) == 0;
    const auto saveStats = saveCoordinator_.flushPending(
        saveInterval,
        chunks_,
        worldSaveService_,
        jobs_,
        save::SaveCoordinatorSettings{
            config_.paths.saveRoot(),
            config_.workBudget.maxSavesPerFlush,
            config_.workBudget.saveFlushIntervalFrames},
        frameIndex_);
    core::mergeTimer(frameCounters.save, saveStats.save);
    frameCounters.saveQueueLength = saveStats.saveQueueLength;
    frameCounters.savesFlushed = saveStats.savesFlushed;
    frameCounters.saveBudgetSaturated = saveStats.saveBudgetSaturated;
    frameCounters.dirtyLightingQueueLength = lightingQueue_.size();
    frameCounters.dirtyMeshQueueLength = meshQueue_.size();
    markStage(frameCounters.stageSave);

    physics_.tick(tickIndex);
    magic_.tick(tickIndex);
    network_.tick(tickIndex);
    simulation_.tick(gameRegistry_, 1.0F / 60.0F);
    {
        world::BlockEntityTickContext beContext{items_, recipes_, blocks_, machineInventories_, kineticSolver_, chunks_, {}, 1.0F / 60.0F, tickIndex};
        const auto beStats = blockEntityScheduler_.tick(beContext, chunks_, blocks_,
            config_.workBudget.maxLightingMsPerFrame,
            // R0-LOD plan: LOD1 sim throttle. Only tick block entities
            // for chunks inside the active sim band (LOD0). Chunks in
            // LOD1 (between simulationDistance and renderDistance) keep
            // their state but pause their entity ticks until the player
            // moves closer.
            [this](world::ChunkCoord c) { return isChunkInActiveSim(c); });
        (void)beStats;
    }
    // W2: tick the fluid simulation. Queue is empty until a player edit
    // wakes it, so this costs ~0 in steady state.
    {
        // When useGpuFluidSim is enabled, route ticks through the GPU sim.
        // The CPU FluidSystem stays alive as the fallback / queue owner —
        // its `wake()` helpers are still used by the rest of the engine
        // (e.g. when a block edit adjacent to water wakes the queue). The
        // queue ownership is unified into FluidGpuSystem when active.
        const auto fluidStats = (fluidGpuSystem_ != nullptr)
            ? fluidGpuSystem_->tick(chunks_, streamingCenter())
            : fluidSystem_.tick(chunks_, streamingCenter());
        (void)fluidStats;  // future: expose in DebugOverlay
    }
    updateAutomationDebug();
    updateVisualOverlay();
    markStage(frameCounters.stageSimulation);

    // Phase 1D-1: build one LOD2 cluster mesh per tick (closest to the
    // player first). No-op when clusterRenderer_ is null. Sits AFTER the
    // simulation markStage so the build cost (~5-15ms in Release, will
    // be moved to workers in Phase 1D-2) doesn't pollute sim_ms. Without
    // its own markStage it currently spills into render_ms, which is
    // wrong but at least visibly so — we can see the cost rather than
    // having it hide inside lighting/sim.
    tickClusterMaintenance();
    // Phase 1D-3: LOD3 regions. Runs after cluster maintenance so the
    // skip-region logic sees fresh cluster coverage. Same adaptive
    // throttle (8 ms) gates new work when the engine is busy, same
    // single-job-at-a-time discipline keeps the worker pool from
    // saturating with region builds at the expense of chunk meshes.
    tickRegionMaintenance();

    renderer_.endFrame();
    const auto renderStats = renderer_.drainFrameStats();
    frameCounters.gpuUploads = renderStats.gpuUploads;
    frameCounters.duplicateGpuUploadSkips = renderStats.duplicateUploadSkips;
    frameCounters.stagingUploadBytes = renderStats.stagingUploadBytes;
    frameCounters.gpuUpload.count = renderStats.gpuUploads;
    frameCounters.gpuUpload.totalUs = renderStats.gpuUploadTimeUs;
    frameCounters.gpuUpload.maxUs = renderStats.gpuUploadMaxUs;
    frameCounters.uploadBatchCount = renderStats.uploadBatchCount;
    frameCounters.uploadBatchBytes = renderStats.uploadBatchBytes;
    frameCounters.uploadQueueLength = renderStats.uploadQueueLength;
    frameCounters.chunksMadeDrawable = renderStats.chunksMadeDrawable;
    frameCounters.gpuCullDispatches = renderStats.gpuCullDispatches;
    frameCounters.gpuCullSections = renderStats.gpuCullSections;
    frameCounters.gpuCullVisible = renderStats.gpuCullVisible;
    frameCounters.gpuCullCpuVisible = renderStats.gpuCullCpuVisible;
    frameCounters.gpuCullMismatches = renderStats.gpuCullMismatches;
    frameCounters.gpuCullDrawCommands = renderStats.gpuCullDrawCommands;
    frameCounters.sceneEntriesSynced = renderStats.sceneEntriesSynced;
    frameCounters.sceneFullSyncs = renderStats.sceneFullSyncs;
    frameCounters.rendererFenceWait.count = 1;
    frameCounters.rendererFenceWait.totalUs = renderStats.rendererFenceWaitUs;
    frameCounters.rendererFenceWait.maxUs = renderStats.rendererFenceWaitMaxUs;
    frameCounters.chunksDrawn = renderStats.chunksDrawn;
    frameCounters.chunksCulled = renderStats.chunksCulled;

    const auto prepassStats = terrainPrepassCache_->drainStats();
    frameCounters.terrainPrepassJobsCompleted = prepassStats.jobsCompleted;
    frameCounters.terrainPrepassCacheHits = prepassStats.hits;
    frameCounters.terrainPrepassCacheMisses = prepassStats.misses;
    frameCounters.terrainPrepass = prepassStats.prepassBuildTime;
    frameCounters.terrainPrepassCacheEntries = terrainPrepassCache_->entryCount();
    markStage(frameCounters.stageRender);

    const auto now = std::chrono::steady_clock::now();
    const auto frameMs = std::chrono::duration<double, std::milli>(now - frameStart).count();
    frameCounters.jobSystemPending = jobs_.pendingCount();
    frameCounters.workerCount = jobs_.workerCount();
    frameCounters.pendingGenerationResults = chunkJobMailbox_.pendingGenerationResults();
    frameCounters.pendingMeshResults = chunkJobMailbox_.pendingMeshResults() + pendingMeshResults_.size();
    frameCounters.pendingLightingResults = chunkJobMailbox_.pendingLightingResults();
    frameCounters.residentChunks = chunks_.residentCount();
    frameCounters.meshCacheEntries = meshCache_.size();
    frameCounters.inFlightGeneration = chunkJobMailbox_.inFlightGenerationCount();
    frameCounters.inFlightMesh = chunkJobMailbox_.inFlightMeshCount();
    frameCounters.inFlightLighting = chunkJobMailbox_.inFlightLightingCount();
    if (frameMs >= config_.slowFrameLogThresholdMs) {
        frameCounters.slowFrames = 1;
        if (frameIndex_ < 5 || frameIndex_ - lastSlowFrameLogFrame_ >= 30
            || frameMs >= config_.slowFrameLogThresholdMs * 3.0) {
            logSlowFrame(frameMs, frameCounters);
            lastSlowFrameLogFrame_ = frameIndex_;
        }
    }
    runtimeStats_.recordFrame(frameMs, frameCounters);
    // Push this frame's counters to the debug overlay so the next frame's
    // .draw() shows them. The draw call already happened earlier in this
    // tick using the previous frame's data — overall the UI lags by one
    // frame, which is imperceptible at >30 fps.
    debugOverlay_.record(frameMs, frameCounters);
    // Deferred mesh-data frees from the install loop. Doing them here keeps
    // the per-stage budgets accurate (the install loop only pays for the
    // upload itself, not for the subsequent allocator cleanup).
    meshDataToFree_.clear();
    const bool reportInterval = runtimeStats_.shouldReport(now);
    if (frameIndex_ < 5 || reportInterval) {
        logRuntimeStats("Runtime stats", runtimeStats_.interval(), runtimeStats_.intervalAverageFrameMs());
    }
    if (reportInterval) {
        runtimeStats_.resetInterval(now);
    }
    if (frameIndex_ % 15 == 0) {
        updateWindowTitle();
    }
    if (frameIndex_ % 120 == 0) {
        evictFarMeshCache();
    }
    ++frameIndex_;
}

world::ChunkCoord Application::streamingCenter() const noexcept
{
    const auto pos = player_.dPosition();
    return {
        world::floorDiv(static_cast<std::int64_t>(std::floor(pos.x)), world::ChunkSize),
        world::floorDiv(static_cast<std::int64_t>(std::floor(pos.y)), world::ChunkSize),
        world::floorDiv(static_cast<std::int64_t>(std::floor(pos.z)), world::ChunkSize)
    };
}

bool Application::isChunkInActiveSim(world::ChunkCoord coord) const noexcept
{
    // LOD0 sim band: chunks within `simulationDistance` (horizontal)
    // and `verticalRenderDistance` (vertical) of the streaming center.
    // Using verticalRenderDistance for the Y axis since the streamer
    // doesn't expose a separate vertical sim distance — chunks outside
    // vertical render aren't loaded anyway.
    const auto center = streamingCenter();
    const std::int64_t horizR = config_.streaming.simulationDistanceChunks;
    const std::int64_t vertR  = config_.streaming.verticalRenderDistanceChunks;
    const auto dx = coord.x - center.x;
    const auto dy = coord.y - center.y;
    const auto dz = coord.z - center.z;
    const auto adx = dx >= 0 ? dx : -dx;
    const auto ady = dy >= 0 ? dy : -dy;
    const auto adz = dz >= 0 ? dz : -dz;
    return adx <= horizR && adz <= horizR && ady <= vertR;
}

player::PlayerInput Application::gatherPlayerInput()
{
    player::PlayerInput input{};
    input.forward = window_->keyDown(platform::Key::W);
    input.backward = window_->keyDown(platform::Key::S);
    input.left = window_->keyDown(platform::Key::A);
    input.right = window_->keyDown(platform::Key::D);
    input.jump = window_->keyDown(platform::Key::Space);
    input.flyUp = window_->keyDown(platform::Key::E);
    input.flyDown = window_->keyDown(platform::Key::Q);
    input.fast = window_->keyDown(platform::Key::LeftShift);

    if (!window_->cursorCaptured()) {
        hasPlayerCursor_ = false;
        return input;
    }

    const auto cursor = window_->cursorPosition();
    if (hasPlayerCursor_) {
        const auto dx = std::clamp(cursor.x - lastPlayerCursorX_, -250.0, 250.0);
        const auto dy = std::clamp(cursor.y - lastPlayerCursorY_, -250.0, 250.0);
        input.yawDelta = static_cast<float>(-dx) * config_.mouseSensitivity;
        input.pitchDelta = static_cast<float>(-dy) * config_.mouseSensitivity;
    }
    lastPlayerCursorX_ = cursor.x;
    lastPlayerCursorY_ = cursor.y;
    hasPlayerCursor_ = true;
    return input;
}

void Application::updateSelectedBlock()
{
    const auto selectIfDown = [this](platform::Key key, std::size_t slot) {
        return window_->keyDown(key) && hotbar_.select(slot);
    };

    (void)(selectIfDown(platform::Key::Digit1, 0)
        || selectIfDown(platform::Key::Digit2, 1)
        || selectIfDown(platform::Key::Digit3, 2)
        || selectIfDown(platform::Key::Digit4, 3)
        || selectIfDown(platform::Key::Digit5, 4)
        || selectIfDown(platform::Key::Digit6, 5)
        || selectIfDown(platform::Key::Digit7, 6)
        || selectIfDown(platform::Key::Digit8, 7)
        || selectIfDown(platform::Key::Digit9, 8)
        || selectIfDown(platform::Key::Digit0, 9)
        || selectIfDown(platform::Key::Minus, 10)
        || selectIfDown(platform::Key::Equal, 11));

    const auto slot = hotbar_.selected();
    if (slot.block.value != selectedPlaceBlock_.value) {
        selectedPlaceBlock_ = slot.block;
        Logger::info(
            "Selected hotbar[" + std::to_string(hotbar_.selectedSlot() + 1U)
            + "] " + std::string{slot.name}
            + " state=" + std::to_string(selectedPlaceBlock_.value));
    }
}

void Application::updateSpellCasting()
{
    if (window_ == nullptr) {
        return;
    }

    const bool rDown = window_->keyDown(platform::Key::R);
    auto& magicState = magic_.playerMagic();
    const bool wasCasting = magicState.castingMode;

    magicState.castingMode = rDown;

    if (magicState.castingMode && !wasCasting) {
        spellCastingMode_ = true;
        prevSpellSlot_ = magicState.hotbar.selectedSlot();
        Logger::info("Spell casting mode: ON");
    }
    if (!magicState.castingMode && wasCasting) {
        spellCastingMode_ = false;
        Logger::info("Spell casting mode: OFF");
    }

    if (!spellCastingMode_) {
        return;
    }

    const auto selectSpellIfDown = [this](platform::Key key, std::size_t slot) {
        return window_->keyDown(key) && magic_.playerMagic().hotbar.select(slot);
    };

    const bool slotChanged = selectSpellIfDown(platform::Key::Digit1, 0)
        || selectSpellIfDown(platform::Key::Digit2, 1)
        || selectSpellIfDown(platform::Key::Digit3, 2)
        || selectSpellIfDown(platform::Key::Digit4, 3)
        || selectSpellIfDown(platform::Key::Digit5, 4)
        || selectSpellIfDown(platform::Key::Digit6, 5)
        || selectSpellIfDown(platform::Key::Digit7, 6)
        || selectSpellIfDown(platform::Key::Digit8, 7)
        || selectSpellIfDown(platform::Key::Digit9, 8);

    if (slotChanged) {
        const auto& selected = magicState.hotbar.slot(magicState.hotbar.selectedSlot());
        const auto* id = selected.builtInId();
        Logger::info("Spell slot " + std::to_string(magicState.hotbar.selectedSlot() + 1) + ": " + (id ? id->str() : "(empty)"));
    }

    const double scroll = window_->scrollOffset();
    if (scroll != 0.0) {
        const int direction = scroll > 0.0 ? 1 : -1;
        magicState.hotbar.scroll(direction);
        const auto& selected = magicState.hotbar.slot(magicState.hotbar.selectedSlot());
        const auto* id = selected.builtInId();
        Logger::info("Spell slot " + std::to_string(magicState.hotbar.selectedSlot() + 1) + ": " + (id ? id->str() : "(empty)"));
    }
    window_->clearScrollOffset();

    const bool leftDown = window_->mouseButtonDown(platform::MouseButton::Left);
    const bool rightDown = window_->mouseButtonDown(platform::MouseButton::Right);
    const bool leftPressed = leftDown && !leftMouseWasDown_;
    const bool rightPressed = rightDown && !rightMouseWasDown_;
    leftMouseWasDown_ = leftDown;
    rightMouseWasDown_ = rightDown;

    if (leftPressed || rightPressed) {
        const auto rayOrigin = freecam_ ? renderer_.debugCameraPosition() : player_.eyePosition();
        const auto rayForward = freecam_ ? renderer_.debugCameraForward() : player_.forwardVector();
        constexpr float kReachDistance = 6.0F;
        const auto hit = raycaster_.cast(chunks_, {rayOrigin, rayForward, kReachDistance, 0});

        magic::SpellCastRequest request;
        request.button = leftPressed ? magic::CastButton::Primary : magic::CastButton::Secondary;
        request.castOrigin = rayOrigin;
        request.castDirection = rayForward;
        if (hit.has_value()) {
            request.targetBlock = hit->position;
            request.hasTarget = true;
        }

        const auto& selected = magicState.hotbar.slot(magicState.hotbar.selectedSlot());
        const auto* spellId = selected.builtInId();
        if (spellId != nullptr) {
            request.spellId = SpellId{static_cast<std::uint32_t>(magicState.hotbar.selectedSlot() + 1)};
            auto result = magic_.spellExecutor().execute(request, magicState, 1.0F / 60.0F);
            if (!result.success) {
                Logger::info("Spell cast failed");
            }
        }
    }
}

void Application::drawRuntimeSettingsOverlay()
{
    runtimeSettingsMouseCapture_ = false;
    if (!debugOverlayVisible_) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(390, 390), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(544, 12), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Runtime Settings")) {
        ImGui::End();
        return;
    }
    runtimeSettingsMouseCapture_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    ImGui::Text("Streaming");
    int renderDistance = config_.streaming.renderDistanceChunks;
    if (ImGui::SliderInt("Render distance", &renderDistance, 1, 64)) {
        config_.streaming.renderDistanceChunks = std::clamp(renderDistance, 1, 64);
    }
    int verticalDistance = config_.streaming.verticalRenderDistanceChunks;
    if (ImGui::SliderInt("Vertical distance", &verticalDistance, 0, 12)) {
        config_.streaming.verticalRenderDistanceChunks = std::clamp(verticalDistance, 0, 12);
    }
    int simulationDistance = config_.streaming.simulationDistanceChunks;
    if (ImGui::SliderInt("Simulation distance", &simulationDistance, 1, 32)) {
        config_.streaming.simulationDistanceChunks = std::clamp(simulationDistance, 1, 32);
    }
    int physicsDistance = config_.streaming.physicsDistanceChunks;
    if (ImGui::SliderInt("Physics distance", &physicsDistance, 1, 32)) {
        config_.streaming.physicsDistanceChunks = std::clamp(physicsDistance, 1, 32);
    }
    // R0-LOD plan: separate knob from render distance. Bumps LOD3
    // reach without growing the LOD0 chunk count. Low values keep
    // LOD3 tight (fewer regions to build, less memory); high values
    // extend the horizon dramatically. Default 4 is conservative.
    int farTerrainQ = static_cast<int>(config_.farTerrainQualityMultiplier);
    if (ImGui::SliderInt("Far terrain quality", &farTerrainQ, 1, 16)) {
        config_.farTerrainQualityMultiplier =
            std::clamp<std::int64_t>(farTerrainQ, 1, 16);
    }

    // Live readout of the computed LOD bands. As the user drags the
    // Render distance and Far terrain quality sliders, these numbers
    // update so the otherwise-invisible scaling is visible. The bands
    // are deterministic from (R0, farQ) via computeLodBands.
    {
        const auto bands = world::computeLodBands(
            config_.streaming.renderDistanceChunks,
            config_.farTerrainQualityMultiplier);
        ImGui::TextDisabled("LOD bands (chunks):");
        ImGui::Text("  LOD0 (full):     0 - %lld",
            static_cast<long long>(bands.lod0End));
        ImGui::Text("  LOD1 (reduced):  %lld - %lld",
            static_cast<long long>(bands.lod0End + 1),
            static_cast<long long>(bands.lod1End));
        ImGui::Text("  LOD2 (cluster):  %lld - %lld",
            static_cast<long long>(bands.lod1End + 1),
            static_cast<long long>(bands.lod2End));
        ImGui::Text("  LOD3 (region):   %lld - %lld",
            static_cast<long long>(bands.lod2End + 1),
            static_cast<long long>(bands.lod3End));
    }

    ImGui::Separator();
    ImGui::Text("Space view");
    int spaceFarPlaneK = static_cast<int>(std::round(config_.spaceCameraFarPlane / 1024.0F));
    if (ImGui::SliderInt("Space far plane (k blocks)", &spaceFarPlaneK, 16, 512)) {
        config_.spaceCameraFarPlane = sanitizedFarPlane(static_cast<float>(spaceFarPlaneK) * 1024.0F, 81920.0F);
        config_.spaceCameraFarPlane = std::max(config_.spaceCameraFarPlane, config_.normalCameraFarPlane);
    }
    ImGui::Text("Current view plane: %.0f blocks",
        lerpFloat(config_.normalCameraFarPlane, config_.spaceCameraFarPlane, currentSpaceState_.spaceBlend));

    ImGui::Separator();
    ImGui::Text("Atmosphere");
    ImGui::SliderFloat("Fog near", &config_.atmosphere.fogNear, 0.0F, 512.0F, "%.0f");
    config_.atmosphere.fogNear = std::clamp(config_.atmosphere.fogNear, 0.0F, 8192.0F);
    ImGui::SliderFloat("Fog far", &config_.atmosphere.fogFar, 512.0F, 12000.0F, "%.0f");
    config_.atmosphere.fogFar = std::clamp(config_.atmosphere.fogFar, config_.atmosphere.fogNear + 1.0F, 65536.0F);
    ImGui::SliderFloat("Haze strength", &config_.atmosphere.fogStrength, 0.0F, 1.2F, "%.2f");
    config_.atmosphere.fogStrength = std::clamp(config_.atmosphere.fogStrength, 0.0F, 1.5F);
    ImGui::SliderFloat("Far light lift", &config_.atmosphere.farLightLift, 0.0F, 1.0F, "%.2f");
    config_.atmosphere.farLightLift = std::clamp(config_.atmosphere.farLightLift, 0.0F, 1.0F);
    if (ImGui::Button("Concept daylight")) {
        config_.atmosphere = render::VulkanRenderer::AtmosphereSettings{};
        config_.sky = render::VulkanRenderer::SkySettings{};
    }

    ImGui::Text("Sky");
    ImGui::SliderFloat("Sky horizon", &config_.sky.horizonLift, -0.20F, 0.60F, "%.2f");
    config_.sky.horizonLift = std::clamp(config_.sky.horizonLift, -0.40F, 0.80F);
    ImGui::SliderFloat("Sky saturation", &config_.sky.saturation, 0.50F, 1.80F, "%.2f");
    config_.sky.saturation = std::clamp(config_.sky.saturation, 0.0F, 2.0F);
    ImGui::SliderFloat("Cloud strength", &config_.sky.cloudStrength, 0.0F, 1.10F, "%.2f");
    config_.sky.cloudStrength = std::clamp(config_.sky.cloudStrength, 0.0F, 1.25F);
    ImGui::SliderFloat("Sky brightness", &config_.sky.brightness, 0.50F, 1.60F, "%.2f");
    config_.sky.brightness = std::clamp(config_.sky.brightness, 0.25F, 2.0F);

    ImGui::Separator();
    ImGui::Text("Work budgets");
    if (ImGui::Button("Balanced 65% CPU")) {
        const auto targetWorkers = workerCountForPercent(65U);
        resizeWorkerPool(targetWorkers);
        applyRuntimeWorkProfile(config_, targetWorkers, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Aggressive 70% CPU")) {
        const auto targetWorkers = workerCountForPercent(70U);
        resizeWorkerPool(targetWorkers);
        applyRuntimeWorkProfile(config_, targetWorkers, true);
    }
    int generationPerTick = static_cast<int>(config_.chunkPipeline.maxLoadsOrGenerationsPerTick);
    if (ImGui::SliderInt("Generation dispatch", &generationPerTick, 1, 128)) {
        config_.chunkPipeline.maxLoadsOrGenerationsPerTick = static_cast<std::size_t>(std::clamp(generationPerTick, 1, 128));
    }
    int meshJobs = static_cast<int>(config_.workBudget.maxMeshJobsPerTick);
    if (ImGui::SliderInt("Mesh jobs", &meshJobs, 1, 256)) {
        config_.workBudget.maxMeshJobsPerTick = static_cast<std::size_t>(std::clamp(meshJobs, 1, 256));
        config_.maxChunkMeshesPerTick = config_.workBudget.maxMeshJobsPerTick;
    }
    int meshInstalls = static_cast<int>(config_.workBudget.maxMeshInstallsPerTick);
    if (ImGui::SliderInt("Mesh installs", &meshInstalls, 1, 256)) {
        config_.workBudget.maxMeshInstallsPerTick = static_cast<std::size_t>(std::clamp(meshInstalls, 1, 256));
    }
    int uploadMb = static_cast<int>(config_.workBudget.maxGpuUploadBytesPerTick / (1024U * 1024U));
    if (ImGui::SliderInt("GPU upload MB", &uploadMb, 4, 64)) {
        config_.workBudget.maxGpuUploadBytesPerTick =
            static_cast<std::size_t>(std::clamp(uploadMb, 4, 64)) * 1024U * 1024U;
    }

    ImGui::Separator();
    int workers = static_cast<int>(jobs_.workerCount());
    const int maxWorkers = static_cast<int>(std::min<std::size_t>(detectedThreadCount(), 128U));
    const bool workersChanged = ImGui::SliderInt("Worker threads", &workers, 1, maxWorkers);
    if (workersChanged && ImGui::IsItemDeactivatedAfterEdit()) {
        resizeWorkerPool(static_cast<std::size_t>(std::clamp(workers, 1, maxWorkers)));
    }
    ImGui::Text("Workers: %llu active / %llu hardware",
        static_cast<unsigned long long>(jobs_.workerCount()),
        static_cast<unsigned long long>(detectedThreadCount()));
    ImGui::Text("GPU cull: %s   hybrid meshing: %s",
        config_.enableGpuCulling ? "on" : "off",
        hybridMeshingGpuSystem_ != nullptr ? "on" : "fallback");

    // Phase 1D-2 toggle — kept disabled (greyed) when the LOD pipeline
    // didn't initialize so users don't think a tick of the box will do
    // anything. The actual draw / build short-circuit is in
    // tickClusterMaintenance.setEnabled / recordDraws.
    const bool clusterAvailable = (clusterRenderer_ != nullptr);
    if (!clusterAvailable) {
        ImGui::BeginDisabled();
    }
    bool useClusterLod = config_.useClusterLod;
    if (ImGui::Checkbox("LOD2 cluster rendering", &useClusterLod)) {
        config_.useClusterLod = useClusterLod;
    }
    ImGui::SameLine();
    if (clusterAvailable) {
        ImGui::Text("(%llu resident, %s)",
            static_cast<unsigned long long>(clusterRenderer_->residentClusterCount()),
            clusterGpuSystem_ ? "GPU classify" : "CPU classify");
    } else {
        ImGui::TextDisabled("(disabled at startup — pass without --no-cluster-lod)");
    }
    if (!clusterAvailable) {
        ImGui::EndDisabled();
    }

    // LOD diagnostics panel. Surfaces otherwise-invisible state so the
    // user can verify the LOD pipeline is doing what they expect at any
    // given moment — how many clusters/regions are resident, how many
    // are currently in-flight at each pipeline stage, etc.
    if (clusterAvailable) {
        ImGui::Separator();
        ImGui::Text("LOD diagnostics");

        // LOD2 cluster pipeline state.
        const auto clusterResident = clusterRenderer_->residentClusterCount();
        const auto clusterTracked = builtClusters_.size();
        const char* clusterStage = "idle";
        if (pendingClusterJob_.has_value())      clusterStage = "GPU classifying";
        else if (pendingMergeJob_.has_value())   clusterStage = "worker merging";
        ImGui::Text("  LOD2: %llu drawn  %llu tracked  (%s)",
            static_cast<unsigned long long>(clusterResident),
            static_cast<unsigned long long>(clusterTracked),
            clusterStage);

        // LOD3 region pipeline state. Three pipeline stages means
        // up to three distinct in-flight markers.
        const auto regionResident = clusterRenderer_->residentRegionCount();
        const auto regionTracked = builtRegions_.size();
        const char* regionStage = "idle";
        if (pendingRegionPaddedJob_.has_value())      regionStage = "worker reducing";
        else if (pendingRegionGpuJob_.has_value())    regionStage = "GPU classifying";
        else if (pendingRegionMergeJob_.has_value())  regionStage = "worker merging";
        ImGui::Text("  LOD3: %llu drawn  %llu tracked  (%s)",
            static_cast<unsigned long long>(regionResident),
            static_cast<unsigned long long>(regionTracked),
            regionStage);

        // Chunk-data + LOD0 cache sizes for context. `mesh_cache`
        // tracks built chunk meshes (subset of resident chunks);
        // `chunks_` is the master block-data store.
        ImGui::Text("  LOD0: %llu chunks resident, %llu chunk meshes built",
            static_cast<unsigned long long>(chunks_.residentCount()),
            static_cast<unsigned long long>(meshCache_.size()));
    }

    ImGui::Text("Space far plane affects visibility only; streaming radius stays separate.");

    ImGui::End();
}

void Application::resizeWorkerPool(std::size_t workerCount)
{
    workerCount = std::max<std::size_t>(1U, workerCount);
    if (jobs_.workerCount() == workerCount) {
        config_.workerCount = workerCount;
        return;
    }

    Logger::info("Runtime settings: resizing worker pool to " + std::to_string(workerCount));
    jobs_.waitAll();
    if (pendingReplan_.has_value()
        && pendingReplan_->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        cachedStreamingRequests_ = pendingReplan_->future.get();
        cachedStreamingCenter_ = pendingReplan_->center;
        cachedStreamingSettings_ = pendingReplan_->settings;
        cachedStreamingForwardBucket_ = pendingReplan_->forwardBucket;
        cachedStreamingRequestFrame_ = frameIndex_;
        streamingRequestCursor_ = 0;
        streamingRequestCacheValid_ = true;
        streamingDispatchIdle_ = false;
        pendingReplan_.reset();
    }
    jobs_.stop();
    jobs_.start(workerCount);
    config_.workerCount = workerCount;
}

void Application::drawWorldManagerOverlay()
{
    worldManagerCaptureMouse_ = false;
    if (!worldManagerOpen_) {
        return;
    }
    const auto worlds = worldRegistry_.listWorlds();
    if (worldRenameBuffers_.size() != worlds.size()) {
        worldRenameBuffers_.assign(worlds.size(), std::string{});
    }
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        if (worldRenameBuffers_[i].empty()) {
            worldRenameBuffers_[i] = worlds[i].descriptor.name;
        }
    }
    ImGui::SetNextWindowSize(ImVec2(560, 440), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("World Manager (F9)", &worldManagerOpen_)) {
        ImGui::End();
        return;
    }
    worldManagerCaptureMouse_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    if (!config_.worldDisplayName.empty()) {
        ImGui::TextDisabled("Currently playing: %s (seed=%llu)",
            config_.worldDisplayName.c_str(),
            static_cast<unsigned long long>(config_.worldSeed));
    }
    ImGui::Separator();
    ImGui::Text("Worlds in %s", worldRegistry_.savesDirectory().string().c_str());
    if (worlds.empty()) {
        ImGui::TextDisabled("  (no saved worlds yet)");
    }
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        const auto& entry = worlds[i];
        ImGui::PushID(static_cast<int>(i));
        const bool isActive = entry.root == config_.paths.saveRoot();
        if (isActive) {
            ImGui::TextColored(ImVec4(0.65F, 0.95F, 0.55F, 1.0F),
                "[active] %s", entry.descriptor.name.c_str());
        } else {
            ImGui::Text("%s", entry.descriptor.name.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled(" seed=%llu",
            static_cast<unsigned long long>(entry.descriptor.seed));
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s", worldRenameBuffers_[i].c_str());
        ImGui::SetNextItemWidth(280.0F);
        if (ImGui::InputText("##rename", buffer, sizeof(buffer))) {
            worldRenameBuffers_[i] = buffer;
        }
        ImGui::SameLine();
        const bool nameChanged = worldRenameBuffers_[i] != entry.descriptor.name
            && !worldRenameBuffers_[i].empty();
        ImGui::BeginDisabled(!nameChanged);
        if (ImGui::Button("Apply rename")) {
            (void)worldRegistry_.renameWorld(entry, worldRenameBuffers_[i]);
            worldRenameBuffers_[i].clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(isActive);
        if (ImGui::Button("Switch")) {
            nextWorldRequest_ = entry;
            returnToTitleRequested_ = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(isActive);
        if (ImGui::Button("Delete")) {
            pendingDeleteIndex_ = static_cast<int>(i);
            ImGui::OpenPopup("Confirm delete");
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopupModal("Confirm delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete world \"%s\"?", entry.descriptor.name.c_str());
            ImGui::Text("This removes all chunks, player state, and inventory.");
            if (ImGui::Button("Yes, delete", ImVec2(120, 0))) {
                if (pendingDeleteIndex_ == static_cast<int>(i)) {
                    (void)worldRegistry_.deleteWorld(entry);
                    worldRenameBuffers_.clear();
                }
                pendingDeleteIndex_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                pendingDeleteIndex_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (ImGui::Button("Save & Quit to Title")) {
        nextWorldRequest_.reset();
        returnToTitleRequested_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(returns to the launcher menu)");
    ImGui::End();
}

void Application::tickTitleScreen()
{
    // N1: beginFrame() was already called by tick(). Drive only ImGui + present.
    window_->setCursorCaptured(false);
    renderer_.beginImGuiFrame();
    drawTitleScreen();
    renderer_.endImGuiFrame();
    renderer_.endFrame();
    ++frameIndex_;
}

void Application::drawTitleScreen()
{
    const auto worlds = worldRegistry_.listWorlds();
    if (worldRenameBuffers_.size() != worlds.size()) {
        worldRenameBuffers_.assign(worlds.size(), std::string{});
    }
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        if (worldRenameBuffers_[i].empty()) {
            worldRenameBuffers_[i] = worlds[i].descriptor.name;
        }
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 center = viewport->GetCenter();
    constexpr float kPanelWidth = 680.0F;
    constexpr float kPanelHeight = 540.0F;
    ImGui::SetNextWindowPos(ImVec2(center.x, center.y), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, kPanelHeight), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoTitleBar;
    if (!ImGui::Begin("##TitleScreen", nullptr, flags)) {
        ImGui::End();
        return;
    }
    const char* kTitle = "AETHERFORGE: INFINITE CREATION";
    const char* kSubtitle = "A voxel sandbox.";
    {
        ImGui::SetWindowFontScale(2.0F);
        const auto titleSize = ImGui::CalcTextSize(kTitle);
        ImGui::SetCursorPosX((kPanelWidth - titleSize.x) * 0.5F);
        ImGui::TextColored(ImVec4(0.85F, 0.95F, 1.0F, 1.0F), "%s", kTitle);
        ImGui::SetWindowFontScale(1.0F);
        const auto subSize = ImGui::CalcTextSize(kSubtitle);
        ImGui::SetCursorPosX((kPanelWidth - subSize.x) * 0.5F);
        ImGui::TextDisabled("%s", kSubtitle);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }
    ImGui::Text("Saved Worlds");
    ImGui::BeginChild("##WorldList", ImVec2(0, 220), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (worlds.empty()) {
        ImGui::TextDisabled("  (no worlds yet — create one below)");
    }
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        const auto& entry = worlds[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string label = entry.descriptor.name
            + "  (seed=" + std::to_string(entry.descriptor.seed) + ")";
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                nextWorldRequest_ = entry;
                returnToTitleRequested_ = true;
            }
        }
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s", worldRenameBuffers_[i].c_str());
        ImGui::SetNextItemWidth(220.0F);
        if (ImGui::InputText("##rename", buffer, sizeof(buffer))) {
            worldRenameBuffers_[i] = buffer;
        }
        ImGui::SameLine();
        const bool nameChanged = worldRenameBuffers_[i] != entry.descriptor.name
            && !worldRenameBuffers_[i].empty();
        ImGui::BeginDisabled(!nameChanged);
        if (ImGui::Button("Apply")) {
            (void)worldRegistry_.renameWorld(entry, worldRenameBuffers_[i]);
            worldRenameBuffers_[i].clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Play")) {
            nextWorldRequest_ = entry;
            returnToTitleRequested_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            titleDeleteIndex_ = static_cast<int>(i);
            ImGui::OpenPopup("Confirm delete");
        }
        if (ImGui::BeginPopupModal("Confirm delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete world \"%s\"?", entry.descriptor.name.c_str());
            ImGui::Text("This removes all chunks, player state, and inventory.");
            if (ImGui::Button("Yes, delete", ImVec2(120, 0))) {
                if (titleDeleteIndex_ == static_cast<int>(i)) {
                    (void)worldRegistry_.deleteWorld(entry);
                    worldRenameBuffers_.clear();
                }
                titleDeleteIndex_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                titleDeleteIndex_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::Text("Create New World");
    {
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", titleNewWorldName_.c_str());
        ImGui::SetNextItemWidth(300.0F);
        if (ImGui::InputTextWithHint("##name", "World name (blank = \"New World\")",
                                     nameBuf, sizeof(nameBuf))) {
            titleNewWorldName_ = nameBuf;
        }
        ImGui::SameLine();
        char seedBuf[64];
        std::snprintf(seedBuf, sizeof(seedBuf), "%s", titleNewWorldSeedText_.c_str());
        ImGui::SetNextItemWidth(160.0F);
        if (ImGui::InputTextWithHint("##seed", "seed (blank = random)",
                                     seedBuf, sizeof(seedBuf),
                                     ImGuiInputTextFlags_CharsDecimal)) {
            titleNewWorldSeedText_ = seedBuf;
        }
        ImGui::SameLine();
        if (ImGui::Button("Create & Play")) {
            std::string name = titleNewWorldName_;
            if (name.empty()) {
                name = "New World";
            }
            std::uint64_t seed = 0;
            if (!titleNewWorldSeedText_.empty()) {
                const auto& s = titleNewWorldSeedText_;
                const auto result = std::from_chars(s.data(), s.data() + s.size(), seed);
                if (result.ec != std::errc{}) {
                    seed = 0;
                }
            }
            auto created = worldRegistry_.createWorld(std::move(name), seed);
            if (created.has_value()) {
                nextWorldRequest_ = std::move(*created);
                returnToTitleRequested_ = true;
                titleNewWorldName_.clear();
                titleNewWorldSeedText_.clear();
                worldRenameBuffers_.clear();
            }
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Quit", ImVec2(120, 0))) {
        nextWorldRequest_.reset();
        returnToTitleRequested_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  Double-click a world to enter it, or click Play.");
    ImGui::End();
}

void Application::seedCreativeInventory()
{
    for (std::size_t i = 0; i < player::CreativeHotbar::SlotCount; ++i) {
        const auto slot = hotbar_.slot(i);
        if (!slot.block) {
            continue;
        }
        const auto itemId = itemForPlacedBlock(slot.block, blocks_, items_);
        const auto* itemDef = items_.registry().byRuntimeId(itemId.value);
        if (itemDef == nullptr) {
            continue;
        }
        (void)playerInventory_.hotbarInventory().insertAt(i, {itemId, itemDef->maxStackSize, 0}, items_);
    }

    const std::array<data::Identifier, 8> starterBackpackItems{{
        {"core", "coal"},
        {"core", "iron_ingot"},
        {"core", "copper_ingot"},
        {"core", "wooden_gear_item"},
        {"core", "belt_item"},
        {"core", "gearbox_item"},
        {"core", "clutch_item"},
        {"core", "millstone_item"},
    }};
    for (std::size_t i = 0; i < starterBackpackItems.size(); ++i) {
        const auto itemId = items_.findRuntimeId(starterBackpackItems[i]);
        const auto* itemDef = items_.registry().byRuntimeId(itemId.value);
        if (itemDef == nullptr) {
            continue;
        }
        const std::uint32_t count = std::min<std::uint32_t>(itemDef->maxStackSize, itemDef->maxStackSize >= 500 ? 128U : 32U);
        (void)playerInventory_.mainInventory().insertAt(i, {itemId, count, 0}, items_);
    }
}

core::RuntimeCounters Application::handleWorldInteraction()
{
    core::RuntimeCounters stats{};
    const bool leftDown = window_->mouseButtonDown(platform::MouseButton::Left);
    const bool rightDown = window_->mouseButtonDown(platform::MouseButton::Right);
    const bool leftPressed = leftDown && !leftMouseWasDown_;
    const bool rightPressed = rightDown && !rightMouseWasDown_;
    leftMouseWasDown_ = leftDown;
    rightMouseWasDown_ = rightDown;

    const auto rayOrigin = freecam_ ? renderer_.debugCameraPosition() : player_.eyePosition();
    const auto rayForward = freecam_ ? renderer_.debugCameraForward() : player_.forwardVector();
    constexpr float kReachDistance = 6.0F;
    const auto hit = raycaster_.cast(chunks_, {rayOrigin, rayForward, kReachDistance, 0});
    renderer_.setDebugBlockOutline(hit ? std::optional<world::PlanetCoord>{hit->position} : std::nullopt);

    if (!leftPressed && !rightPressed) {
        return stats;
    }

    if (!hit.has_value()) {
        ++stats.blockEditsRejected;
        return stats;
    }

    if (leftPressed) {
        blockEditQueue_.enqueueBreak(hit->position);
    } else if (rightPressed) {
        const auto placePosition = adjacentPosition(hit->position, hit->normal);
        if (!freecam_ && player_.overlapsBlock(placePosition.chunk, placePosition.block)) {
            Logger::info("Block place rejected: target overlaps player");
            ++stats.blockEditsRejected;
            return stats;
        }
        blockEditQueue_.enqueuePlace(placePosition, resolvePlacementState(selectedPlaceBlock_, hit->normal, blocks_));
    }

    const auto edit = blockEditQueue_.flush(chunks_, blockEditor_);
    stats.blockEditsAccepted += edit.accepted;
    stats.blockEditsRejected += edit.rejected;
    stats.dirtyMeshChunksQueued += edit.dirtyMeshQueued;
    stats.dirtyMeshChunksCoalesced += edit.dirtyMeshCoalesced;
    stats.dirtyLightingChunksQueued += edit.dirtyLightingQueued;
    stats.dirtyLightingChunksCoalesced += edit.dirtyLightingCoalesced;

    if (edit.accepted == 0) {
        return stats;
    }
    for (const auto coord : edit.dirtyChunks) {
        enqueueLightingIfNeeded(coord);
        enqueueMeshIfNeeded(coord);
        kineticSolver_.markDirty(coord);
    }

    for (const auto& delta : edit.deltas) {
        if (auto* blockDelta = std::get_if<world::BlockDelta>(&delta)) {
            kineticSolver_.markDirty(blockDelta->position);
            blockEntityScheduler_.markDirty(blockDelta->position.chunk);
            invalidateLodForEditedChunk(blockDelta->position.chunk);

            // W2: if the player just broke a block (now-air) next to a water
            // cell, BFS-activate that water so it can start flowing into the
            // newly opened space. Bounded radius keeps the cost predictable
            // when exposing the ocean.
            const bool nowAir = blockDelta->next.value == world::AirBlockState.value;
            const bool wasWater = coreBlocks_.isWater(blockDelta->previous);
            if (nowAir && !wasWater) {
                // Check the 6 neighbours; if any is water, kick off activation
                // from that neighbour's cell with a small radius.
                constexpr std::array<world::BlockCoord, 6> kDeltas{{
                    { 1, 0, 0}, {-1, 0, 0},
                    { 0, 1, 0}, { 0,-1, 0},
                    { 0, 0, 1}, { 0, 0,-1},
                }};
                for (const auto& d : kDeltas) {
                    const auto neighbourWorld = world::BlockCoord{
                        world::toWorldBlock(blockDelta->position.chunk, blockDelta->position.block).x + d.x,
                        world::toWorldBlock(blockDelta->position.chunk, blockDelta->position.block).y + d.y,
                        world::toWorldBlock(blockDelta->position.chunk, blockDelta->position.block).z + d.z,
                    };
                    const auto neighbourLocal = world::toChunkLocal(
                        neighbourWorld.x, neighbourWorld.y, neighbourWorld.z);
                    if (auto* neighbourChunk = chunks_.find(neighbourLocal.chunk)) {
                        const auto block = neighbourChunk->blockAt(
                            neighbourLocal.local.x, neighbourLocal.local.y, neighbourLocal.local.z);
                        if (coreBlocks_.isWater(block)) {
                            // 8-cell BFS unlock — bounded cost per edit.
                            // Route to whichever sim owns the queue this run.
                            // PHASE 5 TODO: the GPU sim's activateOceanEdge is
                            // currently a no-op stub; when wired it will share
                            // the queue with the CPU sim or run the BFS itself.
                            if (fluidGpuSystem_ != nullptr) {
                                (void)fluidGpuSystem_->activateOceanEdge(
                                    chunks_, neighbourLocal.chunk, neighbourLocal.local, 8);
                            } else {
                                (void)fluidSystem_.activateOceanEdge(
                                    chunks_, neighbourLocal.chunk, neighbourLocal.local, 8);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    Logger::info(
        "Block edit queued: accepted=" + std::to_string(edit.accepted)
        + " rejected=" + std::to_string(edit.rejected)
        + " deltas=" + std::to_string(edit.deltas.size())
        + " dirty_mesh_chunks=" + std::to_string(edit.dirtyMeshQueued)
        + " save_queue=" + std::to_string(worldSaveService_.dirtyChunkCount(chunks_)));
    return stats;
}

void Application::invalidateLodForEditedChunk(world::ChunkCoord coord)
{
    if (clusterRenderer_ == nullptr) {
        return;
    }

    const auto targets = world::lodInvalidationTargetsForEditedChunk(coord);
    const auto erased = builtClusters_.erase(targets.cluster);
    clusterRenderer_->removeClusterMesh(targets.cluster);
    if (erased != 0) {
        Logger::info(
            "LOD2 cluster invalidated by block edit: "
            + std::to_string(targets.cluster.x) + ","
            + std::to_string(targets.cluster.y) + ","
            + std::to_string(targets.cluster.z));
    }

    // LOD3 is currently a procedural clipmap sampled from NoiseTerrainGenerator,
    // not an edit-aware voxel aggregate. Player edits are represented by LOD0
    // chunks and edit-invalidated LOD2 clusters. `targets.region` names the
    // future edit-aware clipmap target; it is intentionally not invalidated yet.
}

bool Application::tryResolvePlayerSpawn()
{
    const auto current = player_.position();
    auto spawn = spawnResolver_.resolve(chunks_, current.x, current.z);
    if (!spawn.has_value()) {
        return false;
    }

    player_.setPosition(*spawn);
    Logger::info(
        "Player spawn resolved at x=" + std::to_string(spawn->x)
        + " y=" + std::to_string(spawn->y)
        + " z=" + std::to_string(spawn->z));
    return true;
}

void Application::updateSpaceEnvironment(core::DVec3 cameraPos)
{
    if (!config_.enableSpacePhaseA) {
        currentSpaceState_ = {};
        player_.setGravityScale(1.0F);
        renderer_.setClearColor(0.50F, 0.68F, 0.92F, 1.0F);
        renderer_.setCameraFarPlane(sanitizedFarPlane(config_.normalCameraFarPlane, 1024.0F));
        renderer_.setAtmosphereSettings(config_.atmosphere);
        renderer_.setSkySettings(config_.sky);
        return;
    }

    currentSpaceState_ = spaceEnvironment_.evaluate(static_cast<float>(cameraPos.y));
    player_.setGravityScale(currentSpaceState_.gravityScale);

    const float t = currentSpaceState_.spaceBlend;
    const float normalFarPlane = sanitizedFarPlane(config_.normalCameraFarPlane, 1024.0F);
    const float spaceFarPlane = std::max(sanitizedFarPlane(config_.spaceCameraFarPlane, 81920.0F), normalFarPlane);
    renderer_.setClearColor(
        lerpFloat(0.50F, 0.004F, t),
        lerpFloat(0.68F, 0.006F, t),
        lerpFloat(0.92F, 0.018F, t),
        1.0F);
    renderer_.setCameraFarPlane(lerpFloat(normalFarPlane, spaceFarPlane, t));

    auto atmosphere = config_.atmosphere;
    atmosphere.fogFar = lerpFloat(atmosphere.fogFar, std::max(atmosphere.fogFar, 12000.0F), t);
    atmosphere.fogStrength *= (1.0F - t * 0.85F);
    atmosphere.farLightLift *= (1.0F - t * 0.50F);
    renderer_.setAtmosphereSettings(atmosphere);
    renderer_.setSkySettings(config_.sky);
}

void Application::updateWindowTitle()
{
    if (window_ == nullptr) {
        return;
    }
    const auto slot = hotbar_.selected();
    std::ostringstream title;
    title << config_.name
          << " | " << (freecam_ ? "freecam" : "player")
          << (player_.noclip() ? " noclip" : "")
          << (player_.grounded() ? " grounded" : "")
          << (window_->cursorCaptured() ? " mouse-locked" : " mouse-free");
    if (spellCastingMode_) {
        const auto& spellSlot = magic_.playerMagic().hotbar.slot(magic_.playerMagic().hotbar.selectedSlot());
        const auto* spellId = spellSlot.builtInId();
        title << " | SPELL [" << (magic_.playerMagic().hotbar.selectedSlot() + 1) << "] "
              << (spellId ? spellId->path : "?")
              << " mana=" << std::fixed << std::setprecision(0) << magic_.playerMagic().mana.current;
    } else {
        title << " | [" << (hotbar_.selectedSlot() == 9 ? 0 : hotbar_.selectedSlot() + 1) << "] " << slot.name;
    }
    title << " | " << std::fixed << std::setprecision(1) << runtimeStats_.lastFrameMs() << " ms"
          << " | chunks " << chunks_.residentCount()
          << " mesh " << meshCache_.size();
    if (config_.enableSpacePhaseA && currentSpaceState_.spaceBlend > 0.01F) {
        const auto sector = spaceEnvironment_.sectorFor(freecam_ ? renderer_.debugCameraPosition() : player_.eyePosition());
        title << " | space " << std::setprecision(0) << (currentSpaceState_.spaceBlend * 100.0F) << "%"
              << " sector " << sector.x << "," << sector.y << "," << sector.z;
    }
    window_->setTitle(title.str());
}

void Application::handleInventoryInteraction()
{
    if (!inventoryOpen_ || window_ == nullptr) {
        invLeftMouseWasDown_ = false;
        invRightMouseWasDown_ = false;
        return;
    }

    const bool leftDown = window_->mouseButtonDown(platform::MouseButton::Left);
    const bool rightDown = window_->mouseButtonDown(platform::MouseButton::Right);
    const bool leftClicked = leftDown && !invLeftMouseWasDown_;
    const bool rightClicked = rightDown && !invRightMouseWasDown_;
    invLeftMouseWasDown_ = leftDown;
    invRightMouseWasDown_ = rightDown;

    if (!leftClicked && !rightClicked) {
        return;
    }

    const auto cursor = window_->cursorPosition();
    const float mx = static_cast<float>(cursor.x);
    const float my = static_cast<float>(cursor.y);

    const SlotHitRect* hit = nullptr;
    for (const auto& sr : inventorySlotRects_) {
        if (mx >= sr.x && mx < sr.x + sr.w && my >= sr.y && my < sr.y + sr.h) {
            hit = &sr;
            break;
        }
    }
    if (hit == nullptr) {
        return;
    }

    const auto inventoryForRegion = [this](SlotHitRect::Region region) -> inventory::Inventory& {
        switch (region) {
        case SlotHitRect::Region::Hotbar:     return playerInventory_.hotbarInventory();
        case SlotHitRect::Region::Backpack:   return playerInventory_.mainInventory();
        case SlotHitRect::Region::Equipment:  return playerInventory_.equipmentInventory();
        case SlotHitRect::Region::Accessory:  return playerInventory_.accessoryInventory();
        }
        return playerInventory_.mainInventory();
    };

    auto& inv = inventoryForRegion(hit->region);
    const auto idx = hit->index;
    const auto& registry = items_;
    const bool shift = window_->keyDown(platform::Key::LeftShift);

    if (leftClicked) {
        if (shift) {
            switch (hit->region) {
            case SlotHitRect::Region::Hotbar:
                (void)inv.moveSlotTo(idx, playerInventory_.mainInventory(), registry);
                break;
            case SlotHitRect::Region::Backpack:
                (void)inv.moveSlotTo(idx, playerInventory_.hotbarInventory(), registry);
                break;
            case SlotHitRect::Region::Equipment:
            case SlotHitRect::Region::Accessory:
                (void)inv.moveSlotTo(idx, playerInventory_.mainInventory(), registry);
                break;
            }
        } else if (cursorStack_.empty()) {
            auto taken = inv.extract(idx, std::numeric_limits<std::uint32_t>::max());
            if (taken) {
                cursorStack_ = *taken;
            }
        } else {
            const auto& slotStack = inv.slot(idx);
            if (slotStack.empty()) {
                cursorStack_ = inv.insertAt(idx, cursorStack_, registry);
            } else if (slotStack.itemId.value == cursorStack_.itemId.value) {
                (void)inv.tryMergeIntoSlot(idx, cursorStack_, registry);
            } else {
                auto taken = inv.extract(idx, std::numeric_limits<std::uint32_t>::max());
                auto remainder = inv.insertAt(idx, cursorStack_, registry);
                if (remainder.empty()) {
                    cursorStack_ = taken.value_or(inventory::ItemStack{});
                } else {
                    if (taken) {
                        (void)inv.insertAt(idx, *taken, registry);
                    }
                }
            }
        }
    } else if (rightClicked) {
        if (cursorStack_.empty()) {
            auto half = inv.takeHalf(idx);
            if (half) {
                cursorStack_ = *half;
            }
        } else {
            (void)inv.placeOneIntoSlot(idx, cursorStack_, registry);
        }
    }
}

void Application::updateVisualOverlay()
{
    if (window_ == nullptr) {
        renderer_.setUiOverlay({});
        return;
    }

    const bool showStars = config_.enableSpacePhaseA && currentSpaceState_.spaceBlend > 0.02F;
    std::vector<render::VulkanRenderer::UiRect> rects;
    rects.reserve((inventoryOpen_ ? 180 : 64) + (showStars ? 160 : 0));

    const auto addRect = [&rects](float x, float y, float w, float h, float r, float g, float b, float a) {
        rects.push_back({x, y, w, h, r, g, b, a});
    };
    const auto addBorder = [&addRect](float x, float y, float w, float h, float t, float r, float g, float b, float a) {
        addRect(x, y, w, t, r, g, b, a);
        addRect(x, y + h - t, w, t, r, g, b, a);
        addRect(x, y, t, h, r, g, b, a);
        addRect(x + w - t, y, t, h, r, g, b, a);
    };
    const auto addMiniDigit = [&addRect](float x, float y, std::size_t digit, float r, float g, float b, float a) {
        constexpr std::array<std::uint8_t, 10> masks{
            0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
            0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111
        };
        const std::uint8_t mask = masks[digit % 10U];
        constexpr float w = 10.0F;
        constexpr float h = 16.0F;
        constexpr float t = 2.0F;
        if ((mask & (1U << 0U)) != 0) addRect(x + t, y, w - 2.0F * t, t, r, g, b, a);
        if ((mask & (1U << 1U)) != 0) addRect(x + w - t, y + t, t, h * 0.5F - t, r, g, b, a);
        if ((mask & (1U << 2U)) != 0) addRect(x + w - t, y + h * 0.5F, t, h * 0.5F - t, r, g, b, a);
        if ((mask & (1U << 3U)) != 0) addRect(x + t, y + h - t, w - 2.0F * t, t, r, g, b, a);
        if ((mask & (1U << 4U)) != 0) addRect(x, y + h * 0.5F, t, h * 0.5F - t, r, g, b, a);
        if ((mask & (1U << 5U)) != 0) addRect(x, y + t, t, h * 0.5F - t, r, g, b, a);
        if ((mask & (1U << 6U)) != 0) addRect(x + t, y + h * 0.5F - t * 0.5F, w - 2.0F * t, t, r, g, b, a);
    };
    const auto blockColor = [](std::size_t slot) {
        switch (slot) {
        case 0: return std::array<float, 3>{0.52F, 0.54F, 0.55F};
        case 1: return std::array<float, 3>{0.34F, 0.24F, 0.16F};
        case 2: return std::array<float, 3>{0.30F, 0.48F, 0.20F};
        case 3: return std::array<float, 3>{0.68F, 0.88F, 0.95F};
        case 4: return std::array<float, 3>{0.18F, 0.36F, 0.72F};
        case 5: return std::array<float, 3>{0.36F, 0.55F, 0.28F};
        case 6: return std::array<float, 3>{0.64F, 0.48F, 0.28F};
        case 7: return std::array<float, 3>{0.42F, 0.36F, 0.24F};
        case 8: return std::array<float, 3>{0.58F, 0.52F, 0.42F};
        case 9: return std::array<float, 3>{0.62F, 0.46F, 0.34F};
        case 10: return std::array<float, 3>{0.42F, 0.42F, 0.40F};
        case 11: return std::array<float, 3>{0.50F, 0.44F, 0.34F};
        default: return std::array<float, 3>{0.10F, 0.12F, 0.13F};
        }
    };
    const auto itemColor = [this, &blockColor](const inventory::ItemStack& stack, std::size_t fallback) {
        const auto* item = items_.registry().byRuntimeId(stack.itemId.value);
        if (item == nullptr) {
            return blockColor(fallback);
        }
        const auto hasTag = [item](std::string_view tag) {
            return std::any_of(item->tags.begin(), item->tags.end(), [tag](const std::string& candidate) {
                return candidate == tag;
            });
        };
        if (hasTag("water")) return std::array<float, 3>{0.18F, 0.36F, 0.72F};
        if (hasTag("glass") || hasTag("transparent")) return std::array<float, 3>{0.68F, 0.88F, 0.95F};
        if (hasTag("surface")) return std::array<float, 3>{0.30F, 0.48F, 0.20F};
        if (hasTag("soil")) return std::array<float, 3>{0.34F, 0.24F, 0.16F};
        if (hasTag("wood")) return std::array<float, 3>{0.43F, 0.28F, 0.18F};
        if (hasTag("ore")) return std::array<float, 3>{0.45F, 0.43F, 0.40F};
        if (hasTag("fuel")) return std::array<float, 3>{0.10F, 0.10F, 0.09F};
        if (hasTag("metal")) return std::array<float, 3>{0.66F, 0.55F, 0.42F};
        if (hasTag("kinetic") || hasTag("automation") || hasTag("machine") || hasTag("logistics")) {
            return std::array<float, 3>{0.58F, 0.48F, 0.28F};
        }
        if (hasTag("light")) return std::array<float, 3>{0.95F, 0.74F, 0.24F};
        if (hasTag("terrain")) return std::array<float, 3>{0.52F, 0.54F, 0.55F};
        return blockColor(fallback);
    };
    const auto stackFraction = [this](const inventory::ItemStack& stack) {
        const auto* item = items_.registry().byRuntimeId(stack.itemId.value);
        const float maxStack = item != nullptr ? static_cast<float>(std::max<std::uint32_t>(item->maxStackSize, 1U)) : 999.0F;
        return std::clamp(static_cast<float>(stack.count) / maxStack, 0.05F, 1.0F);
    };
    const auto addMiniDigitScaled = [&addRect](float x, float y, std::size_t digit, float scale, float r, float g, float b, float a) {
        constexpr std::array<std::uint8_t, 10> masks{
            0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
            0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111
        };
        const std::uint8_t mask = masks[digit % 10U];
        const float w = 10.0F * scale;
        const float h = 16.0F * scale;
        const float t = std::max(1.0F, 2.0F * scale);
        if ((mask & (1U << 0U)) != 0) addRect(x + t, y, w - 2.0F * t, t, r, g, b, a);
        if ((mask & (1U << 1U)) != 0) addRect(x + w - t, y + t, t, h * 0.5F - t, r, g, b, a);
        if ((mask & (1U << 2U)) != 0) addRect(x + w - t, y + h * 0.5F, t, h * 0.5F - t, r, g, b, a);
        if ((mask & (1U << 3U)) != 0) addRect(x + t, y + h - t, w - 2.0F * t, t, r, g, b, a);
        if ((mask & (1U << 4U)) != 0) addRect(x, y + h * 0.5F, t, h * 0.5F - t, r, g, b, a);
        if ((mask & (1U << 5U)) != 0) addRect(x, y + t, t, h * 0.5F - t, r, g, b, a);
        if ((mask & (1U << 6U)) != 0) addRect(x + t, y + h * 0.5F - t * 0.5F, w - 2.0F * t, t, r, g, b, a);
    };
    const auto addSlotIcon = [&addRect](float x, float y, float size, inventory::SlotKind kind) {
        const float p = size * 0.22F;
        const float q = size * 0.12F;
        const float cx = x + size * 0.5F;
        const float cy = y + size * 0.5F;
        const float r = 0.68F;
        const float g = 0.64F;
        const float b = 0.54F;
        const float a = 0.38F;
        switch (kind) {
        case inventory::SlotKind::Helmet:
            addRect(x + p, y + p, size - 2.0F * p, q, r, g, b, a);
            addRect(x + p, y + p, q, size * 0.32F, r, g, b, a);
            addRect(x + size - p - q, y + p, q, size * 0.32F, r, g, b, a);
            break;
        case inventory::SlotKind::Chest:
            addRect(cx - q, y + p, q * 2.0F, size * 0.52F, r, g, b, a);
            addRect(x + p, y + p + q, size - 2.0F * p, q, r, g, b, a);
            addRect(x + p, y + size - p - q, size - 2.0F * p, q, r, g, b, a);
            break;
        case inventory::SlotKind::Legs:
            addRect(cx - q * 1.5F, y + p, q, size - 2.0F * p, r, g, b, a);
            addRect(cx + q * 0.5F, y + p, q, size - 2.0F * p, r, g, b, a);
            addRect(cx - q * 1.5F, y + p, q * 3.0F, q, r, g, b, a);
            break;
        case inventory::SlotKind::Boots:
            addRect(x + p, cy, q, size * 0.28F, r, g, b, a);
            addRect(cx, cy, q, size * 0.28F, r, g, b, a);
            addRect(x + p, y + size - p - q, size * 0.28F, q, r, g, b, a);
            addRect(cx, y + size - p - q, size * 0.28F, q, r, g, b, a);
            break;
        case inventory::SlotKind::Gloves:
            addRect(x + p, cy - q, size - 2.0F * p, q * 2.0F, r, g, b, a);
            addRect(x + p, cy - q * 2.0F, q, q, r, g, b, a);
            addRect(x + size - p - q, cy - q * 2.0F, q, q, r, g, b, a);
            break;
        case inventory::SlotKind::Back:
            addRect(x + p, y + p, size - 2.0F * p, size - 2.0F * p, r, g, b, a);
            addRect(cx - q * 0.5F, y + p, q, size - 2.0F * p, 0.18F, 0.18F, 0.16F, a);
            break;
        case inventory::SlotKind::MainHand:
            addRect(cx - q * 0.5F, y + p, q, size - 2.0F * p, r, g, b, a);
            addRect(cx - size * 0.22F, cy - q * 0.5F, size * 0.44F, q, r, g, b, a);
            break;
        case inventory::SlotKind::OffHand:
            addRect(x + p, y + p, size - 2.0F * p, size - 2.0F * p, r, g, b, a);
            addRect(x + p + q, y + p + q, size - 2.0F * (p + q), size - 2.0F * (p + q), 0.18F, 0.18F, 0.16F, a);
            break;
        case inventory::SlotKind::Accessory:
            addRect(cx - q * 0.5F, y + p, q, size * 0.20F, r, g, b, a);
            addRect(cx - q * 1.5F, y + p + size * 0.18F, q * 3.0F, q, r, g, b, a);
            addRect(cx - q, cy, q * 2.0F, q * 2.0F, r, g, b, a);
            break;
        default:
            addRect(x + p, y + p, size - 2.0F * p, size - 2.0F * p, r, g, b, a);
            break;
        }
    };

    inventorySlotRects_.clear();

    const auto extent = renderer_.drawableExtent();
    const float windowWidth = static_cast<float>(std::max<std::uint32_t>(extent.width, 1U));
    const float windowHeight = static_cast<float>(std::max<std::uint32_t>(extent.height, 1U));
    float uiScale = std::clamp(std::min(windowWidth / 1280.0F, windowHeight / 720.0F), 0.70F, 1.20F);

    const auto addTinyGlyph = [&addRect](float x, float y, char ch, float scale, float r, float g, float b, float a) {
        std::array<std::uint8_t, 5> rows{};
        switch (ch) {
        case 'A': rows = {0b010, 0b101, 0b111, 0b101, 0b101}; break;
        case 'C': rows = {0b111, 0b100, 0b100, 0b100, 0b111}; break;
        case 'E': rows = {0b111, 0b100, 0b110, 0b100, 0b111}; break;
        case 'N': rows = {0b101, 0b111, 0b111, 0b111, 0b101}; break;
        case 'P': rows = {0b110, 0b101, 0b110, 0b100, 0b100}; break;
        case 'R': rows = {0b110, 0b101, 0b110, 0b101, 0b101}; break;
        case 'S': rows = {0b111, 0b100, 0b111, 0b001, 0b111}; break;
        default: return;
        }
        const float cell = 3.0F * scale;
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (int col = 0; col < 3; ++col) {
                if ((rows[row] & (1U << (2 - col))) != 0U) {
                    addRect(x + static_cast<float>(col) * cell, y + static_cast<float>(row) * cell, cell, cell, r, g, b, a);
                }
            }
        }
    };
    const auto addTinyText = [&addTinyGlyph](float x, float y, std::string_view text, float scale, float r, float g, float b, float a) {
        float cursor = x;
        for (char ch : text) {
            if (ch == ' ') {
                cursor += 5.0F * scale;
                continue;
            }
            addTinyGlyph(cursor, y, ch, scale, r, g, b, a);
            cursor += 11.0F * scale;
        }
    };

    if (showStars) {
        const auto cameraPos = freecam_ ? renderer_.debugCameraPosition() : player_.eyePosition();
        const auto sector = spaceEnvironment_.sectorFor(cameraPos);
        std::uint64_t rng = config_.space.seed;
        rng ^= mixStarBits(static_cast<std::uint64_t>(sector.x) + 0x9e3779b97f4a7c15ULL);
        rng ^= mixStarBits(static_cast<std::uint64_t>(sector.y) + 0xbf58476d1ce4e5b9ULL);
        rng ^= mixStarBits(static_cast<std::uint64_t>(sector.z) + 0x94d049bb133111ebULL);
        rng = mixStarBits(rng);

        const float alphaScale = std::clamp(currentSpaceState_.spaceBlend, 0.0F, 1.0F);
        for (std::size_t i = 0; i < 144; ++i) {
            const float x = starUnitFloat(rng) * windowWidth;
            const float y = starUnitFloat(rng) * windowHeight;
            const float bright = 0.45F + starUnitFloat(rng) * 0.55F;
            const float size = starUnitFloat(rng) > 0.88F ? 2.0F : 1.0F;
            const float alpha = (0.16F + starUnitFloat(rng) * 0.36F) * alphaScale;
            addRect(x, y, size, size, 0.72F * bright, 0.78F * bright, 0.90F * bright, alpha);
        }
    }

    if (config_.enableSpacePhaseA && currentSpaceState_.altitudeY >= config_.space.nearSpaceStartY - 512.0F) {
        const float panelW = 176.0F * uiScale;
        const float panelH = 28.0F * uiScale;
        const float panelX = (windowWidth - panelW) * 0.5F;
        const float panelY = 14.0F * uiScale;
        const float t = std::clamp(currentSpaceState_.spaceBlend, 0.0F, 1.0F);
        const bool inSpace = currentSpaceState_.inSpace;
        const float cr = inSpace ? 0.34F : 0.95F;
        const float cg = inSpace ? 0.86F : 0.72F;
        const float cb = inSpace ? 1.0F : 0.28F;
        addRect(panelX, panelY, panelW, panelH, 0.01F, 0.015F, 0.022F, 0.70F);
        addBorder(panelX, panelY, panelW, panelH, std::max(1.0F, uiScale), cr, cg, cb, 0.82F);
        addTinyText(panelX + 10.0F * uiScale, panelY + 6.0F * uiScale, inSpace ? "SPACE" : "NEAR", uiScale, cr, cg, cb, 0.90F);
        const float barX = panelX + 72.0F * uiScale;
        const float barY = panelY + 10.0F * uiScale;
        const float barW = 88.0F * uiScale;
        const float barH = 7.0F * uiScale;
        addRect(barX, barY, barW, barH, 0.08F, 0.10F, 0.13F, 0.88F);
        addRect(barX, barY, barW * t, barH, cr, cg, cb, 0.92F);
        for (int i = 0; i <= 4; ++i) {
            const float px = barX + static_cast<float>(i) * (barW * 0.25F);
            addRect(px, barY - 3.0F * uiScale, std::max(1.0F, uiScale), barH + 6.0F * uiScale, cr, cg, cb, 0.45F);
        }
    }

    // Crosshair.
    addRect(windowWidth * 0.5F - 8.0F * uiScale, windowHeight * 0.5F - 1.0F, 16.0F * uiScale, 2.0F, 0.94F, 0.90F, 0.70F, 0.80F);
    addRect(windowWidth * 0.5F - 1.0F, windowHeight * 0.5F - 8.0F * uiScale, 2.0F, 16.0F * uiScale, 0.94F, 0.90F, 0.70F, 0.80F);

    const float slotSize = 42.0F * uiScale;
    const float gap = 5.0F * uiScale;
    constexpr float hotbarSlots = 12.0F;
    const float hotbarWidth = hotbarSlots * slotSize + (hotbarSlots - 1.0F) * gap;
    const float hotbarX = (windowWidth - hotbarWidth) * 0.5F;
    const float hotbarY = windowHeight - slotSize - 18.0F * uiScale;

    addRect(hotbarX - 10.0F * uiScale, hotbarY - 10.0F * uiScale, hotbarWidth + 20.0F * uiScale, slotSize + 20.0F * uiScale, 0.02F, 0.025F, 0.028F, 0.72F);
    for (std::size_t i = 0; i < 12; ++i) {
        const float x = hotbarX + static_cast<float>(i) * (slotSize + gap);
        const bool selected = i == hotbar_.selectedSlot();
        addRect(x, hotbarY, slotSize, slotSize, 0.08F, 0.095F, 0.10F, 0.90F);
        addBorder(x, hotbarY, slotSize, slotSize, selected ? 3.0F * uiScale : std::max(1.0F, uiScale),
            selected ? 1.0F : 0.22F, selected ? 0.82F : 0.24F, selected ? 0.20F : 0.25F, selected ? 1.0F : 0.70F);
        inventorySlotRects_.push_back({x, hotbarY, slotSize, slotSize, SlotHitRect::Region::Hotbar, i});
        const auto& hotbarStack = playerInventory_.hotbarInventory().slot(i);
        if (!hotbarStack.empty()) {
            const auto c = itemColor(hotbarStack, i);
            addRect(x + 9.0F * uiScale, hotbarY + 9.0F * uiScale, slotSize - 18.0F * uiScale, slotSize - 18.0F * uiScale, c[0], c[1], c[2], 0.95F);
            addRect(x + 8.0F * uiScale, hotbarY + slotSize - 8.0F * uiScale, (slotSize - 16.0F * uiScale) * stackFraction(hotbarStack), 3.0F * uiScale,
                0.95F, 0.80F, 0.28F, 0.85F);
        } else if (i < player::CreativeHotbar::SlotCount && hotbar_.validSlot(i)) {
            const auto c = blockColor(i);
            addRect(x + 11.0F * uiScale, hotbarY + 11.0F * uiScale, slotSize - 22.0F * uiScale, slotSize - 22.0F * uiScale, c[0], c[1], c[2], 0.45F);
        }
        if (i < 10) {
            addMiniDigitScaled(x + 4.0F * uiScale, hotbarY + 4.0F * uiScale, i == 9 ? 0U : i + 1U, uiScale, 0.88F, 0.84F, 0.66F, 0.72F);
        } else {
            addRect(x + 5.0F * uiScale, hotbarY + 11.0F * uiScale, 10.0F * uiScale, 2.0F * uiScale, 0.88F, 0.84F, 0.66F, 0.72F);
            if (i == 11) {
                addRect(x + 9.0F * uiScale, hotbarY + 7.0F * uiScale, 2.0F * uiScale, 10.0F * uiScale, 0.88F, 0.84F, 0.66F, 0.72F);
            }
        }
    }

    if (inventoryOpen_) {
        const float availableHeight = std::max(260.0F, hotbarY - 26.0F * uiScale);
        const float maxInventoryScale = std::clamp(availableHeight / 438.0F, 0.58F, uiScale);
        uiScale = std::min(uiScale, maxInventoryScale);

        const float margin = 20.0F * uiScale;
        const float invSlot = 28.0F * uiScale;
        const float invGap = 4.0F * uiScale;
        const float equipSlot = 36.0F * uiScale;
        const float equipGap = 8.0F * uiScale;
        const float gridW = static_cast<float>(inventory::PlayerInventory::kBackpackColumns) * invSlot
            + static_cast<float>(inventory::PlayerInventory::kBackpackColumns - 1U) * invGap;
        const float gridH = static_cast<float>(inventory::PlayerInventory::kBackpackRows) * invSlot
            + static_cast<float>(inventory::PlayerInventory::kBackpackRows - 1U) * invGap;
        const float equipW = 2.0F * equipSlot + equipGap;
        const float panelW = margin * 3.0F + equipW + gridW;
        const float panelH = margin * 2.0F + gridH;
        const float panelX = std::max(10.0F, (windowWidth - panelW) * 0.5F);
        const float panelY = std::max(10.0F, std::min((availableHeight - panelH) * 0.5F + 8.0F * uiScale, hotbarY - panelH - 14.0F * uiScale));
        addRect(panelX, panelY, panelW, panelH, 0.025F, 0.030F, 0.034F, 0.92F);
        addBorder(panelX, panelY, panelW, panelH, std::max(1.0F, 2.0F * uiScale), 0.45F, 0.36F, 0.20F, 0.95F);

        const float equipmentX = panelX + margin;
        const float equipmentY = panelY + margin;
        for (std::size_t i = 0; i < inventory::PlayerInventory::kEquipmentSlots; ++i) {
            const float y = equipmentY + static_cast<float>(i % 4) * (equipSlot + equipGap);
            const float x = equipmentX + static_cast<float>(i / 4) * (equipSlot + equipGap);
            addRect(x, y, equipSlot, equipSlot, 0.065F, 0.075F, 0.080F, 0.94F);
            addBorder(x, y, equipSlot, equipSlot, std::max(1.0F, uiScale), 0.25F, 0.24F, 0.22F, 0.85F);
            addSlotIcon(x, y, equipSlot, playerInventory_.equipmentInventory().slotKind(i));
            inventorySlotRects_.push_back({x, y, equipSlot, equipSlot, SlotHitRect::Region::Equipment, i});
            const auto& stack = playerInventory_.equipmentInventory().slot(i);
            if (!stack.empty()) {
                const auto c = itemColor(stack, i);
                addRect(x + 8.0F * uiScale, y + 8.0F * uiScale, equipSlot - 16.0F * uiScale, equipSlot - 16.0F * uiScale, c[0], c[1], c[2], 0.92F);
            }
        }

        const float accessoryX = equipmentX;
        const float accessoryY = equipmentY + 4.0F * (equipSlot + equipGap) + 18.0F * uiScale;
        for (std::size_t i = 0; i < inventory::PlayerInventory::kAccessorySlots; ++i) {
            const float x = accessoryX + static_cast<float>(i % 2) * (equipSlot + equipGap);
            const float y = accessoryY + static_cast<float>(i / 2) * (equipSlot + equipGap);
            addRect(x, y, equipSlot, equipSlot, 0.055F, 0.048F, 0.070F, 0.94F);
            addBorder(x, y, equipSlot, equipSlot, std::max(1.0F, uiScale), 0.25F, 0.20F, 0.36F, 0.85F);
            addSlotIcon(x, y, equipSlot, inventory::SlotKind::Accessory);
            inventorySlotRects_.push_back({x, y, equipSlot, equipSlot, SlotHitRect::Region::Accessory, i});
            const auto& stack = playerInventory_.accessoryInventory().slot(i);
            if (!stack.empty()) {
                const auto c = itemColor(stack, i);
                addRect(x + 8.0F * uiScale, y + 8.0F * uiScale, equipSlot - 16.0F * uiScale, equipSlot - 16.0F * uiScale, c[0], c[1], c[2], 0.92F);
            }
        }

        const float gridX = panelX + margin * 2.0F + equipW;
        const float gridY = panelY + margin;
        for (std::size_t row = 0; row < inventory::PlayerInventory::kBackpackRows; ++row) {
            for (std::size_t col = 0; col < inventory::PlayerInventory::kBackpackColumns; ++col) {
                const float x = gridX + static_cast<float>(col) * (invSlot + invGap);
                const float y = gridY + static_cast<float>(row) * (invSlot + invGap);
                addRect(x, y, invSlot, invSlot, 0.060F, 0.068F, 0.072F, 0.94F);
                addBorder(x, y, invSlot, invSlot, std::max(1.0F, uiScale), 0.17F, 0.18F, 0.17F, 0.80F);
                const auto slotIndex = row * inventory::PlayerInventory::kBackpackColumns + col;
                inventorySlotRects_.push_back({x, y, invSlot, invSlot, SlotHitRect::Region::Backpack, slotIndex});
                const auto& stack = playerInventory_.mainInventory().slot(slotIndex);
                if (!stack.empty()) {
                    const auto c = itemColor(stack, slotIndex);
                    addRect(x + 6.0F * uiScale, y + 6.0F * uiScale, invSlot - 12.0F * uiScale, invSlot - 12.0F * uiScale, c[0], c[1], c[2], 0.92F);
                    const auto fraction = stackFraction(stack);
                    addRect(x + 4.0F * uiScale, y + invSlot - 6.0F * uiScale, (invSlot - 8.0F * uiScale) * fraction, 2.0F * uiScale, 0.95F, 0.80F, 0.28F, 0.85F);
                }
            }
        }
    }

    if (inventoryOpen_) {
        const auto cursor = window_->cursorPosition();
        const float mx = static_cast<float>(cursor.x);
        const float my = static_cast<float>(cursor.y);
        for (const auto& sr : inventorySlotRects_) {
            if (mx >= sr.x && mx < sr.x + sr.w && my >= sr.y && my < sr.y + sr.h) {
                addRect(sr.x, sr.y, sr.w, sr.h, 1.0F, 1.0F, 1.0F, 0.12F);
                break;
            }
        }
        if (!cursorStack_.empty()) {
            const auto c = itemColor(cursorStack_, 0);
            const float cursorSize = 28.0F * uiScale;
            addRect(mx - cursorSize * 0.5F, my - cursorSize * 0.5F, cursorSize, cursorSize, c[0], c[1], c[2], 0.92F);
            const auto fraction = stackFraction(cursorStack_);
            addRect(mx - cursorSize * 0.5F, my + cursorSize * 0.5F - 3.0F * uiScale, cursorSize * fraction, 2.0F * uiScale, 0.95F, 0.80F, 0.28F, 0.85F);
        }
    }

    renderer_.setUiOverlay(std::move(rects));
}

void Application::evictFarMeshCache()
{
    const auto center = streamingCenter();
    const int h = config_.streaming.renderDistanceChunks + 2;
    const int v = config_.streaming.verticalRenderDistanceChunks + 1;
    const auto keepSurfaceChunk = [this](world::ChunkCoord coord) {
        return surfaceVisibilityRetainSet_.count(coord) != 0;
    };
    const auto removed = meshCache_.removeOutsideRadius(center, h, v, keepSurfaceChunk);
    for (const auto coord : removed) {
        renderer_.removeUploadedMesh(coord);
    }
    if (!removed.empty()) {
        Logger::info("Mesh cache eviction: removed=" + std::to_string(removed.size()));
    }

    // Chunk-data eviction. Previously NOTHING called ChunkManager::evict,
    // so chunks_ grew unbounded — observed reaching 21k+ entries with
    // render distance 16 (expected ~5.5k). At 21k entries every
    // chunks_.forEach iteration in save/sim/streaming is 4x slower than
    // it should be, which is the actual cause of the "slowdown as you
    // explore" pattern.
    //
    // Hysteresis tuning: render_distance + 12 horizontal, +6 vertical.
    // Previously was +6/+3 which proved too tight for fast flight —
    // chunks just behind the player would evict, then the streamer
    // would need to re-generate them on any turn-around. The wider
    // hysteresis keeps the streamer's working set intact AND lets the
    // player whip the camera around without triggering re-generation.
    // We still bound long-distance growth (anything past +12 = ~28
    // chunks beyond render distance is no longer plausibly visible).
    //
    // The dirty().save guard inside evictOutsideRadius ensures we
    // never drop unsaved player edits.
    {
        constexpr std::size_t kMaxChunkEvictionsPerTick = 256;
        const int chunkH = config_.streaming.renderDistanceChunks + 12;
        const int chunkV = config_.streaming.verticalRenderDistanceChunks + 6;
        const auto evictedChunks = chunks_.evictOutsideRadius(
            center, chunkH, chunkV, kMaxChunkEvictionsPerTick, keepSurfaceChunk);
        if (!evictedChunks.empty()) {
            // Mirror the log shape of the mesh eviction so it's easy to
            // see both streams in the same log scan.
            Logger::info("Chunk data eviction: removed="
                + std::to_string(evictedChunks.size())
                + " resident=" + std::to_string(chunks_.residentCount()));
        }
    }
}

void Application::drainOutstandingJobsForShutdown()
{
    jobs_.waitAll();
    (void)saveCoordinator_.drainCompleted(true);
    if (pendingHybridMeshJob_) {
        chunkJobMailbox_.endMesh(world::MeshJobKey{
            pendingHybridMeshJob_->coord,
            pendingHybridMeshJob_->sourceRevision,
            pendingHybridMeshJob_->neighborRevisionHash});
        pendingHybridMeshJob_.reset();
    }
    (void)chunkPipeline_.processRequestsAsync(
        chunks_, saveStore_, terrainGenerator_, jobs_, chunkJobMailbox_, {},
        {.maxLoadsOrGenerationsPerTick = 0, .maxGenerationInstallsPerTick = std::numeric_limits<std::size_t>::max()});

    for (const auto& result : chunkJobMailbox_.drainMesh()) {
        chunkJobMailbox_.endMesh(world::MeshJobKey{result.coord, result.sourceRevision, result.neighborRevisionHash});
    }
    for (const auto& result : chunkJobMailbox_.drainLighting()) {
        chunkJobMailbox_.endLighting(world::LightingJobKey{result.coord, result.sourceRevision, result.neighborLightHash});
    }
    pendingMeshResults_.clear();
    lightingQueue_.clear();
    meshQueue_.clear();
}

void Application::updateAutomationDebug()
{
    bool dumpNow = false;
    if (window_ != nullptr) {
        const bool fDown = window_->keyDown(platform::Key::F);
        if (fDown && !automationDumpLatch_) {
            dumpNow = true;
        }
        automationDumpLatch_ = fDown;
    }

    if (!dumpNow) {
        return;
    }

    const auto snapshot = kineticSolver_.solve(chunks_, kineticCatalog_);
    std::uint32_t totalNodes = 0;
    for (const auto& network : snapshot.networks) {
        totalNodes += network.nodeCount;
    }

    Logger::info(
        "Automation debug: networks=" + std::to_string(snapshot.networks.size())
        + " nodes=" + std::to_string(totalNodes)
        + " overloaded=" + std::to_string(snapshot.overloadedNetworks)
        + (dumpNow ? " (manual dump)" : ""));

    // F6: per-network detail. Show up to 8 networks by default to bound log volume;
    // a manual dump (F key) prints every network.
    const std::size_t maxNetworks = dumpNow ? snapshot.networks.size() : std::min<std::size_t>(8U, snapshot.networks.size());
    for (std::size_t i = 0; i < maxNetworks; ++i) {
        const auto& net = snapshot.networks[i];
        const auto& pos = net.representativeNode;
        Logger::info(
            "  network[" + std::to_string(i) + "]"
            + " id=" + std::to_string(net.id)
            + " nodes=" + std::to_string(net.nodeCount)
            + " src=" + std::to_string(net.sourceCount)
            + " cons=" + std::to_string(net.consumerCount)
            + " rpm=" + std::to_string(net.rpm)
            + " stress=" + std::to_string(net.stressDemand)
            + "/" + std::to_string(net.stressCapacity)
            + (net.overloaded ? " OVERLOAD" : "")
            + " @chunk(" + std::to_string(pos.chunk.x) + "," + std::to_string(pos.chunk.y) + "," + std::to_string(pos.chunk.z) + ")"
            + " local(" + std::to_string(pos.block.x) + "," + std::to_string(pos.block.y) + "," + std::to_string(pos.block.z) + ")");
    }

}

void Application::refreshSurfaceVisibilityRequests(world::ChunkCoord center, bool streamSpaceOnly)
{
    const int renderDistance = std::max(0, config_.streaming.renderDistanceChunks);
    const int verticalDistance = std::max(0, config_.streaming.verticalRenderDistanceChunks);
    const int forwardBucket = streamingForwardBucket(player_.forwardVector());
    if (surfaceVisibilityCacheValid_
        && cachedSurfaceVisibilityCenter_ == center
        && cachedSurfaceVisibilityRenderDistance_ == renderDistance
        && cachedSurfaceVisibilityVerticalDistance_ == verticalDistance
        && cachedSurfaceVisibilityForwardBucket_ == forwardBucket
        && cachedSurfaceVisibilitySpaceOnly_ == streamSpaceOnly) {
        return;
    }

    cachedSurfaceVisibilityCenter_ = center;
    cachedSurfaceVisibilityRenderDistance_ = renderDistance;
    cachedSurfaceVisibilityVerticalDistance_ = verticalDistance;
    cachedSurfaceVisibilityForwardBucket_ = forwardBucket;
    cachedSurfaceVisibilitySpaceOnly_ = streamSpaceOnly;
    surfaceVisibilityCacheValid_ = true;
    surfaceVisibilityRequests_.clear();
    surfaceVisibilityRetainSet_.clear();

    if (streamSpaceOnly || renderDistance <= 0) {
        return;
    }

    // Vertical render distance still bounds hidden underground/interior slabs.
    // This pass keeps only the visible terrain skin alive when ATGS places a
    // valley, seabed, or peak many chunks above/below the player.
    constexpr int kMaxSurfaceVisibilityRadius = 24;
    constexpr int kSurfaceSkirtChunksBelow = 1;
    const int surfaceRadius = std::min(renderDistance, kMaxSurfaceVisibilityRadius);
    const auto forward = player_.forwardVector();
    const float forwardLength = std::sqrt((forward.x * forward.x) + (forward.z * forward.z));
    const float forwardX = forwardLength > 0.0F ? forward.x / forwardLength : 0.0F;
    const float forwardZ = forwardLength > 0.0F ? forward.z / forwardLength : 0.0F;

    const auto addSurfaceRequest = [&](std::int64_t chunkX,
                                       std::int64_t chunkY,
                                       std::int64_t chunkZ,
                                       int dx,
                                       int dz,
                                       float layerBias) {
        const world::ChunkCoord coord{chunkX, chunkY, chunkZ};
        if (!surfaceVisibilityRetainSet_.insert(coord).second) {
            return;
        }
        const float horizontalDistance2 = static_cast<float>((dx * dx) + (dz * dz));
        const float verticalDistanceFromPlayer = static_cast<float>(std::abs(chunkY - center.y));
        const float forwardDot = static_cast<float>(dx) * forwardX + static_cast<float>(dz) * forwardZ;
        const float forwardBias = std::max(0.0F, forwardDot) * 0.25F;
        const float priority = (horizontalDistance2 * 0.20F)
            + (verticalDistanceFromPlayer * 0.04F)
            + layerBias
            - forwardBias
            - 8.0F;
        surfaceVisibilityRequests_.push_back({coord, priority});
    };

    surfaceVisibilityRequests_.reserve(
        static_cast<std::size_t>((surfaceRadius * 2 + 1) * (surfaceRadius * 2 + 1) * 3));

    for (int dz = -surfaceRadius; dz <= surfaceRadius; ++dz) {
        for (int dx = -surfaceRadius; dx <= surfaceRadius; ++dx) {
            const auto chunkX = center.x + dx;
            const auto chunkZ = center.z + dz;
            const float worldX = static_cast<float>(chunkX * world::ChunkSize + world::ChunkSize / 2);
            const float worldZ = static_cast<float>(chunkZ * world::ChunkSize + world::ChunkSize / 2);
            const auto column = terrainGenerator_.sampleColumnAt(worldX, worldZ);
            const auto surfaceChunkY =
                world::floorDiv(static_cast<std::int64_t>(column.surfaceY), world::ChunkSize);

            addSurfaceRequest(chunkX, surfaceChunkY, chunkZ, dx, dz, 0.0F);
            if (column.isOcean) {
                const auto seaChunkY =
                    world::floorDiv(static_cast<std::int64_t>(column.seaLevel), world::ChunkSize);
                addSurfaceRequest(chunkX, seaChunkY, chunkZ, dx, dz, 0.15F);
            }
            for (int skirt = 1; skirt <= kSurfaceSkirtChunksBelow; ++skirt) {
                addSurfaceRequest(
                    chunkX,
                    surfaceChunkY - skirt,
                    chunkZ,
                    dx,
                    dz,
                    0.65F * static_cast<float>(skirt));
            }
        }
    }

    std::sort(surfaceVisibilityRequests_.begin(), surfaceVisibilityRequests_.end(),
        [](const world::ChunkRequest& lhs, const world::ChunkRequest& rhs) {
            if (lhs.priority == rhs.priority) {
                if (lhs.coord.y == rhs.coord.y) {
                    if (lhs.coord.x == rhs.coord.x) {
                        return lhs.coord.z < rhs.coord.z;
                    }
                    return lhs.coord.x < rhs.coord.x;
                }
                return lhs.coord.y < rhs.coord.y;
            }
            return lhs.priority < rhs.priority;
        });
}

const std::vector<world::ChunkRequest>& Application::streamingRequestsForFrame()
{
    const auto center = streamingCenter();
    auto streamingSettings = config_.streaming;
    const auto seaLevelBlockY = static_cast<std::int64_t>(std::floor(terrainGenerator_.settings().seaLevel));
    const auto centerBlockY = center.y * world::ChunkSize;
    const bool streamSpaceOnly = config_.enableSpacePhaseA
        && static_cast<float>(centerBlockY) >= config_.space.nearSpaceStartY;
    if (streamSpaceOnly) {
        streamingSettings.pinnedVerticalChunkY.reset();
    } else {
        streamingSettings.pinnedVerticalChunkY = static_cast<int>(world::floorDiv(seaLevelBlockY, world::ChunkSize));
    }
    refreshSurfaceVisibilityRequests(center, streamSpaceOnly);
    const int forwardBucket = streamingForwardBucket(player_.forwardVector());
    const bool settingsChanged = cachedStreamingSettings_.renderDistanceChunks != streamingSettings.renderDistanceChunks
        || cachedStreamingSettings_.verticalRenderDistanceChunks != streamingSettings.verticalRenderDistanceChunks
        || cachedStreamingSettings_.simulationDistanceChunks != streamingSettings.simulationDistanceChunks
        || cachedStreamingSettings_.physicsDistanceChunks != streamingSettings.physicsDistanceChunks
        || cachedStreamingSettings_.pinnedVerticalChunkY != streamingSettings.pinnedVerticalChunkY
        || cachedStreamingSettings_.pinnedVerticalChunkRadius != streamingSettings.pinnedVerticalChunkRadius;
    const bool centerChanged = !streamingRequestCacheValid_ || !(cachedStreamingCenter_ == center);
    streamingCenterChangedThisFrame_ = centerChanged;
    const bool forwardChanged = cachedStreamingForwardBucket_ != forwardBucket;

    // The full request set is radius-sized and sorted. Rebuilding it every
    // frame was visible in Debug builds, especially while generation was
    // saturated and no new work could be dispatched. Refresh immediately for
    // chunk/settings changes; direction changes are quantized and throttled
    // because they only provide a modest tie-breaker.
    const bool directionRefreshAllowed = frameIndex_ - cachedStreamingRequestFrame_ >= 15;
    const bool centerYChanged = streamingRequestCacheValid_ && cachedStreamingCenter_.y != center.y;
    const bool refreshForDirection = forwardChanged && directionRefreshAllowed;
    const bool canTranslateCachedRequests = streamingRequestCacheValid_
        && centerChanged
        && !centerYChanged
        && !settingsChanged
        && !refreshForDirection;
    // OPTIMIZATION: async replan. The full planRequests() call sorts a
    // 33³ entry vector and spiked to 10+ ms on the main thread in profiles.
    // Now: dispatch the full rebuild to a worker; while it's in flight we
    // keep using the cached (slightly stale) list. The new result lands a
    // frame or two later — invisible at 60 fps.

    // Drain a completed async replan, if any.
    if (pendingReplan_.has_value()
        && pendingReplan_->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        cachedStreamingRequests_ = pendingReplan_->future.get();
        cachedStreamingCenter_ = pendingReplan_->center;
        cachedStreamingSettings_ = pendingReplan_->settings;
        cachedStreamingForwardBucket_ = pendingReplan_->forwardBucket;
        cachedStreamingRequestFrame_ = frameIndex_;
        streamingRequestCursor_ = 0;
        streamingRequestCacheValid_ = true;
        streamingDispatchIdle_ = false;
        pendingReplan_.reset();
    }

    if (canTranslateCachedRequests) {
        const world::ChunkCoord delta{
            center.x - cachedStreamingCenter_.x,
            0,
            center.z - cachedStreamingCenter_.z};
        for (auto& request : cachedStreamingRequests_) {
            request.coord.x += delta.x;
            request.coord.z += delta.z;
        }
        cachedStreamingCenter_ = center;
        streamingRequestCursor_ = 0;
        streamingDispatchIdle_ = false;
    } else if (!streamingRequestCacheValid_) {
        // First-ever call must be synchronous — we have nothing to fall back
        // on. After this, all replans go through the async path.
        cachedStreamingRequests_ = streamer_.planRequests(center, streamingSettings, player_.forwardVector());
        cachedStreamingCenter_ = center;
        cachedStreamingSettings_ = streamingSettings;
        cachedStreamingForwardBucket_ = forwardBucket;
        cachedStreamingRequestFrame_ = frameIndex_;
        streamingRequestCursor_ = 0;
        streamingRequestCacheValid_ = true;
        streamingDispatchIdle_ = false;
    } else if (centerYChanged) {
        // Vertical travel changes the visible chunk band itself. Keeping the
        // old Y request list while an async replan waits behind generation
        // workers can leave resident chunks without visual meshes after mesh
        // eviction, especially after diving underground and returning.
        pendingReplan_.reset();
        cachedStreamingRequests_ = streamer_.planRequests(center, streamingSettings, player_.forwardVector());
        cachedStreamingCenter_ = center;
        cachedStreamingSettings_ = streamingSettings;
        cachedStreamingForwardBucket_ = forwardBucket;
        cachedStreamingRequestFrame_ = frameIndex_;
        streamingRequestCursor_ = 0;
        streamingRequestCacheValid_ = true;
        streamingDispatchIdle_ = false;
    } else if ((centerChanged || settingsChanged || refreshForDirection) && !pendingReplan_.has_value()) {
        // Cache is valid but needs a refresh — dispatch async. While the
        // job runs, the current cached list keeps working (slightly stale
        // priorities, but the right radius). One frame later the result
        // gets swapped in by the drain branch above.
        auto& replan = pendingReplan_.emplace();
        replan.center = center;
        replan.settings = streamingSettings;
        replan.forwardBucket = forwardBucket;
        replan.startFrame = frameIndex_;
        const auto fwd = player_.forwardVector();
        auto* streamer = &streamer_;
        const auto settings = streamingSettings;  // captured by value
        replan.future = jobs_.submit(
            {"chunk.streaming.replan", core::JobPriority::High},
            [streamer, center, settings, fwd]() {
                return streamer->planRequests(center, settings, fwd);
            });
    }

    return cachedStreamingRequests_;
}

const std::vector<world::ChunkRequest>& Application::streamingDispatchRequestsForFrame(
    const std::vector<world::ChunkRequest>& requests)
{
    streamingDispatchRequests_.clear();
    if (requests.empty()) {
        return streamingDispatchRequests_;
    }
    if (config_.chunkPipeline.maxInFlightGeneration > 0
        && chunkJobMailbox_.inFlightGenerationCount() >= config_.chunkPipeline.maxInFlightGeneration
        && chunkJobMailbox_.pendingGenerationResults() == 0) {
        return streamingDispatchRequests_;
    }
    const auto pendingWorkerJobs = jobs_.pendingCount();
    const auto workerCount = std::max<std::size_t>(jobs_.workerCount(), 1U);
    constexpr std::size_t kWorkerBacklogPerThreadBeforeDispatchPause = 8;
    if (pendingWorkerJobs > workerCount * kWorkerBacklogPerThreadBeforeDispatchPause
        && chunkJobMailbox_.inFlightGenerationCount() > 0) {
        return streamingDispatchRequests_;
    }
    constexpr std::size_t kIdleDispatchRescanFrames = 15;
    const bool generationIdle = chunkJobMailbox_.inFlightGenerationCount() == 0
        && chunkJobMailbox_.pendingGenerationResults() == 0;
    if (streamingDispatchIdle_
        && generationIdle
        && !streamingCenterChangedThisFrame_
        && frameIndex_ - lastStreamingDispatchScanFrame_ < kIdleDispatchRescanFrames) {
        return streamingDispatchRequests_;
    }

    constexpr std::size_t kDispatchBudgetCheckInterval = 32;
    constexpr std::size_t kMinimumDispatchScans = 64;
    const auto dispatchScanStart = std::chrono::steady_clock::now();
    const auto dispatchScanBudgetExpired = [&](std::size_t scanned) {
        if (scanned < kMinimumDispatchScans || scanned % kDispatchBudgetCheckInterval != 0) {
            return false;
        }
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - dispatchScanStart).count() >= config_.workBudget.maxStreamDispatchMsPerFrame;
    };

    const std::size_t targetCandidates = std::max<std::size_t>(config_.chunkPipeline.maxLoadsOrGenerationsPerTick * 2U, 8U);
    const std::size_t maxScans = std::min<std::size_t>(
        requests.size(),
        std::max<std::size_t>(targetCandidates * 8U, 96U));
    if (streamingRequestCursor_ >= requests.size()) {
        streamingRequestCursor_ = 0;
    }

    // Snapshot the in-flight set ONCE for the whole filter pass. The previous
    // implementation called `isGenerationInFlight` (which locks the mailbox
    // mutex) up to 512 times per frame, contending with worker pushes.
    // FRAME-CACHED: the snapshot is shared with `dispatchMeshJobs` later in
    // the same tick, halving mailbox lock acquires and deep-copy cost.
    const auto& inFlightSnapshot = getInFlightGenerationSnapshot();

    const auto tryAppendRequest = [&](const world::ChunkRequest& request) {
        const auto alreadySelected = std::find_if(
            streamingDispatchRequests_.begin(),
            streamingDispatchRequests_.end(),
            [coord = request.coord](const world::ChunkRequest& selectedRequest) {
                return selectedRequest.coord == coord;
            });
        if (alreadySelected != streamingDispatchRequests_.end()) {
            return false;
        }
        if (inFlightSnapshot.count(request.coord) != 0) {
            return false;
        }
        if (const auto* existing = chunks_.find(request.coord)) {
            const auto state = existing->state();
            if (state == world::ChunkState::Resident
                || state == world::ChunkState::Meshing
                || state == world::ChunkState::MeshReady
                || state == world::ChunkState::Generating) {
                return false;
            }
        }
        streamingDispatchRequests_.push_back(request);
        return true;
    };

    // ATGS can put nearby visible surfaces many chunks above/below the
    // player's current vertical band. Dispatch a bounded number of those
    // top-surface chunks before the ordinary slab requests. This is not a
    // full vertical bridge; hidden terrain below the visible skin remains
    // governed by verticalRenderDistanceChunks.
    {
        const std::size_t surfaceBudget = std::max<std::size_t>(4U, targetCandidates / 3U);
        std::size_t surfaceAdded = 0;
        for (const auto& request : surfaceVisibilityRequests_) {
            if (surfaceAdded >= surfaceBudget) {
                break;
            }
            if (tryAppendRequest(request)) {
                ++surfaceAdded;
            }
        }
    }

    const float nearRadius = std::max(4.0F, static_cast<float>(config_.streaming.renderDistanceChunks) * 0.45F);
    const float nearPriorityLimit = std::max(25.0F, nearRadius * nearRadius);
    const std::size_t nearTarget = std::min<std::size_t>(
        targetCandidates,
        std::max<std::size_t>(config_.chunkPipeline.maxLoadsOrGenerationsPerTick, targetCandidates / 2U));

    const std::size_t maxNearScans = std::min<std::size_t>(
        requests.size(),
        std::max<std::size_t>(targetCandidates * 4U, 64U));
    std::size_t nearScanned = 0;
    for (const auto& request : requests) {
        if (nearScanned >= maxNearScans
            || streamingDispatchRequests_.size() >= nearTarget
            || request.priority > nearPriorityLimit
            || dispatchScanBudgetExpired(nearScanned)) {
            break;
        }
        ++nearScanned;
        (void)tryAppendRequest(request);
    }

    std::size_t scanned = 0;
    while (scanned < maxScans && streamingDispatchRequests_.size() < targetCandidates) {
        const std::size_t index = (streamingRequestCursor_ + scanned) % requests.size();
        const auto& request = requests[index];
        ++scanned;

        if (request.priority <= nearPriorityLimit && streamingDispatchRequests_.size() >= nearTarget) {
            continue;
        }
        (void)tryAppendRequest(request);
        if (dispatchScanBudgetExpired(nearScanned + scanned)) {
            break;
        }
    }

    streamingRequestCursor_ = (streamingRequestCursor_ + scanned) % requests.size();
    if (streamingDispatchRequests_.empty() && scanned >= requests.size()) {
        streamingRequestCursor_ = 0;
    }
    lastStreamingDispatchScanFrame_ = frameIndex_;
    streamingDispatchIdle_ = streamingDispatchRequests_.empty() && generationIdle && !streamingCenterChangedThisFrame_;
    return streamingDispatchRequests_;
}

const std::unordered_set<world::ChunkCoord, world::ChunkCoordHash>&
Application::getInFlightGenerationSnapshot()
{
    // Lazy frame-level cache. The snapshot is conceptually stale the moment
    // the mailbox mutex releases anyway (workers can push new in-flight
    // entries the very next instruction), so sharing one snapshot across the
    // tick's two consumers (`streamingDispatchRequestsForFrame` and
    // `dispatchMeshJobs`) introduces no new staleness — it just halves the
    // mutex acquires and the deep-copy cost per frame.
    if (cachedInFlightSnapshotFrame_ != frameIndex_) {
        cachedInFlightSnapshot_ = chunkJobMailbox_.snapshotInFlightGeneration();
        cachedInFlightSnapshotFrame_ = frameIndex_;
    }
    return cachedInFlightSnapshot_;
}

int Application::streamingForwardBucket(core::Vec3 forward) const noexcept
{
    const float len2 = forward.x * forward.x + forward.z * forward.z;
    if (len2 < 0.0001F) {
        return 0;
    }
    constexpr float kPi = 3.14159265358979323846F;
    constexpr int kBuckets = 16;
    const float angle = std::atan2(forward.z, forward.x);
    int bucket = static_cast<int>(((angle + kPi) / (2.0F * kPi)) * static_cast<float>(kBuckets));
    if (bucket < 0) {
        bucket = 0;
    }
    if (bucket >= kBuckets) {
        bucket = kBuckets - 1;
    }
    return bucket + 1;
}

std::size_t Application::dispatchTerrainPrepassJobs(const std::vector<world::ChunkRequest>& requests)
{
    constexpr std::size_t kMaxPrepassJobsPerTick = 4;
    constexpr std::size_t kMaxPrepassScansPerTick = 96;

    // Background prepass is only a prediction/cache warmer. On-demand chunk
    // generation can still build missing prepasses on worker threads. Keep
    // this scheduler out of the interactive hot path when real generation,
    // meshing, or previous prepass jobs are active.
    if (jobs_.pendingCount() > 0
        || chunkJobMailbox_.inFlightGenerationCount() > 0
        || chunkJobMailbox_.inFlightMeshCount() > 0
        || chunkJobMailbox_.pendingGenerationResults() > 0
        || chunkJobMailbox_.pendingMeshResults() > 0
        || terrainPrepassCache_->inFlightCount() > 0) {
        return 0;
    }

    std::size_t submitted = 0;
    std::unordered_set<world::TerrainColumnCoord, world::TerrainColumnCoordHash> seen;

    std::size_t scanned = 0;
    for (const auto& request : requests) {
        if (submitted >= kMaxPrepassJobsPerTick) {
            break;
        }
        if (scanned++ >= kMaxPrepassScansPerTick) {
            break;
        }
        if (config_.enableSpacePhaseA) {
            const auto chunkTopY = static_cast<float>(request.coord.y * world::ChunkSize + world::ChunkSize - 1);
            if (chunkTopY >= config_.space.atmosphereTopY) {
                continue;
            }
        }

        const world::TerrainColumnCoord column{request.coord.x, request.coord.z};
        if (!seen.insert(column).second) {
            continue;
        }

        const auto key = terrainGenerator_.prepassKey(column);
        if (!terrainPrepassCache_->tryBeginJob(key)) {
            continue;
        }

        auto* generator = &terrainGenerator_;
        auto cache = terrainPrepassCache_;
        jobs_.submit({"terrain.prepass", core::JobPriority::Low},
            [generator, cache, column, key]() {
                const auto start = std::chrono::steady_clock::now();
                try {
                    auto prepass = generator->buildColumnPrepass(column);
                    cache->completeJob(std::move(prepass), elapsedUs(start, std::chrono::steady_clock::now()));
                } catch (...) {
                    cache->endJobWithoutStore(key);
                    throw;
                }
            });
        ++submitted;
    }

    return submitted;
}

std::uint64_t Application::meshNeighborRevisionHash(world::ChunkCoord coord) const
{
    constexpr std::array<world::ChunkCoord, 6> kFaceDeltas{{
        {-1, 0, 0}, {1, 0, 0},
        {0, -1, 0}, {0, 1, 0},
        {0, 0, -1}, {0, 0, 1},
    }};

    std::uint64_t hash = 1469598103934665603ULL;
    if (const auto* target = chunks_.find(coord)) {
        hash ^= static_cast<std::uint64_t>(target->revision() + 1U);
        if (const auto* light = target->lightData()) {
            hash *= 1099511628211ULL;
            hash ^= static_cast<std::uint64_t>(target->meshRevision() + 1U);
            hash *= 1099511628211ULL;
            hash ^= static_cast<std::uint64_t>(light->skyLight(world::ChunkSize / 2, world::ChunkSize / 2, world::ChunkSize / 2));
            hash ^= static_cast<std::uint64_t>(light->skyLight(0, world::ChunkSize / 2, world::ChunkSize / 2)) << 8U;
            hash ^= static_cast<std::uint64_t>(light->skyLight(world::ChunkSize - 1, world::ChunkSize / 2, world::ChunkSize / 2)) << 16U;
            hash ^= static_cast<std::uint64_t>(light->skyLight(world::ChunkSize / 2, 0, world::ChunkSize / 2)) << 24U;
            hash ^= static_cast<std::uint64_t>(light->skyLight(world::ChunkSize / 2, world::ChunkSize - 1, world::ChunkSize / 2)) << 32U;
        }
        hash *= 1099511628211ULL;
    }
    for (const auto& delta : kFaceDeltas) {
        const world::ChunkCoord neighbour{
            coord.x + delta.x,
            coord.y + delta.y,
            coord.z + delta.z};
        const auto* chunk = chunks_.find(neighbour);
        const auto value = chunk == nullptr ? 0xFFFFFFFFFFFFFFFFULL : static_cast<std::uint64_t>(chunk->revision() + 1U);
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t Application::lightingNeighborHash(world::ChunkCoord coord) const
{
    constexpr std::array<world::ChunkCoord, 6> kFaceDeltas{{
        {-1, 0, 0}, {1, 0, 0},
        {0, -1, 0}, {0, 1, 0},
        {0, 0, -1}, {0, 0, 1},
    }};

    std::uint64_t hash = 1469598103934665603ULL;
    if (const auto* target = chunks_.find(coord)) {
        hash ^= static_cast<std::uint64_t>(target->revision() + 1U);
        hash *= 1099511628211ULL;
        hash ^= target->lightData() == nullptr ? 0ULL : static_cast<std::uint64_t>(
            target->lightData()->skyLight(world::ChunkSize / 2, world::ChunkSize / 2, world::ChunkSize / 2));
        hash *= 1099511628211ULL;
    }
    for (const auto& delta : kFaceDeltas) {
        const world::ChunkCoord neighbour{coord.x + delta.x, coord.y + delta.y, coord.z + delta.z};
        const auto* chunk = chunks_.find(neighbour);
        std::uint64_t value = chunk == nullptr ? 0xFFFFFFFFFFFFFFFFULL : static_cast<std::uint64_t>(chunk->revision() + 1U);
        if (chunk != nullptr && chunk->lightData() != nullptr) {
            value ^= static_cast<std::uint64_t>(chunk->lightData()->skyLight(world::ChunkSize / 2, world::ChunkSize / 2, world::ChunkSize / 2)) << 32U;
            value ^= static_cast<std::uint64_t>(chunk->lightData()->blockLight(world::ChunkSize / 2, world::ChunkSize / 2, world::ChunkSize / 2)) << 40U;
        }
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void Application::enqueueLightingIfNeeded(world::ChunkCoord coord)
{
    if (config_.useGpuShaderLighting) {
        return;
    }
    const auto* chunk = chunks_.find(coord);
    if (chunk == nullptr) {
        return;
    }
    if (chunk->state() != world::ChunkState::Resident && chunk->state() != world::ChunkState::MeshReady) {
        return;
    }
    if (!chunk->dirty().lighting && chunk->lightData() != nullptr) {
        return;
    }
    lightingQueue_.enqueue(coord, chunk->revision(), chunk->meshRevision());
}

void Application::enqueueMeshIfNeeded(world::ChunkCoord coord, float priority)
{
    const auto* chunk = chunks_.find(coord);
    if (chunk == nullptr) {
        return;
    }
    if (chunk->state() != world::ChunkState::Resident && chunk->state() != world::ChunkState::MeshReady) {
        return;
    }
    if (!chunk->dirty().mesh && meshCache_.isCurrent(coord, chunk->revision())) {
        return;
    }
    meshQueue_.enqueue(coord, chunk->revision(), chunk->meshRevision(), priority);
}

void Application::enqueueInstalledChunkWork(const world::ChunkPipelineStats& stats)
{
    for (const auto coord : stats.installedChunks) {
        enqueueMeshIfNeeded(coord, -100.0F);
        enqueueLightingIfNeeded(coord);
    }
    for (const auto coord : stats.neighborDirtyChunks) {
        enqueueMeshIfNeeded(coord, -10.0F);
        enqueueLightingIfNeeded(coord);
    }
}

std::size_t Application::enqueueVisibleMeshWork(const std::vector<world::ChunkRequest>& requests)
{
    if (requests.empty()) {
        return 0;
    }

    const auto maxMeshJobs = config_.workBudget.maxMeshJobsPerTick > 0
        ? config_.workBudget.maxMeshJobsPerTick
        : config_.maxChunkMeshesPerTick;
    const std::size_t maxEnqueues = std::max<std::size_t>(maxMeshJobs, 1U);
    const std::size_t maxScans = std::min<std::size_t>(
        requests.size(),
        std::max<std::size_t>(maxEnqueues * 8U, 128U));

    std::size_t enqueued = 0;
    const auto tryEnqueueMesh = [&](const world::ChunkRequest& request) {
        const auto* chunk = chunks_.find(request.coord);
        if (chunk == nullptr) {
            return false;
        }
        if (chunk->state() != world::ChunkState::Resident && chunk->state() != world::ChunkState::MeshReady) {
            return false;
        }
        if (!chunk->dirty().mesh && meshCache_.isCurrent(request.coord, chunk->revision())) {
            return false;
        }

        return meshQueue_.enqueue(request.coord, chunk->revision(), chunk->meshRevision(), request.priority);
    };

    std::size_t surfaceScanned = 0;
    const std::size_t maxSurfaceScans = std::min<std::size_t>(
        surfaceVisibilityRequests_.size(),
        std::max<std::size_t>(maxEnqueues * 4U, 64U));
    for (const auto& request : surfaceVisibilityRequests_) {
        if (surfaceScanned++ >= maxSurfaceScans || enqueued >= maxEnqueues) {
            break;
        }
        if (tryEnqueueMesh({request.coord, std::min(request.priority, -45.0F)})) {
            ++enqueued;
        }
    }

    std::size_t scanned = 0;
    for (const auto& request : requests) {
        if (scanned++ >= maxScans || enqueued >= maxEnqueues) {
            break;
        }

        if (tryEnqueueMesh({request.coord, std::min(request.priority, -50.0F)})) {
            ++enqueued;
        }
    }
    return enqueued;
}

Application::LightingStats Application::propagateLightingForDirtyChunks()
{
    LightingStats stats{};

        auto completed = chunkJobMailbox_.drainLighting();
        stats.completed = completed.size();
        for (auto& result : completed) {
            chunkJobMailbox_.endLighting(world::LightingJobKey{result.coord, result.sourceRevision, result.neighborLightHash});
            core::recordTimer(stats.propagationTime, result.propagationTimeUs);
            core::recordTimer(stats.queueWaitTime, result.queueWaitUs);

            auto* chunk = chunks_.find(result.coord);
            if (chunk == nullptr) {
                ++stats.staleDiscarded;
                continue;
            }
            if (chunk->revision() != result.sourceRevision || lightingNeighborHash(result.coord) != result.neighborLightHash) {
                ++stats.staleDiscarded;
                enqueueLightingIfNeeded(result.coord);
                continue;
            }

            std::array<std::uint8_t, 6> oldBorderHash{};
            if (const auto* prev = chunk->lightData()) {
                oldBorderHash[0] = prev->skyLight(0, world::ChunkSize / 2, world::ChunkSize / 2);
                oldBorderHash[1] = prev->skyLight(world::ChunkSize - 1, world::ChunkSize / 2, world::ChunkSize / 2);
                oldBorderHash[2] = prev->skyLight(world::ChunkSize / 2, 0, world::ChunkSize / 2);
                oldBorderHash[3] = prev->skyLight(world::ChunkSize / 2, world::ChunkSize - 1, world::ChunkSize / 2);
                oldBorderHash[4] = prev->skyLight(world::ChunkSize / 2, world::ChunkSize / 2, 0);
                oldBorderHash[5] = prev->skyLight(world::ChunkSize / 2, world::ChunkSize / 2, world::ChunkSize - 1);
            }

            std::array<std::uint8_t, 6> newBorderHash{
                result.light.skyLight(0, world::ChunkSize / 2, world::ChunkSize / 2),
                result.light.skyLight(world::ChunkSize - 1, world::ChunkSize / 2, world::ChunkSize / 2),
                result.light.skyLight(world::ChunkSize / 2, 0, world::ChunkSize / 2),
                result.light.skyLight(world::ChunkSize / 2, world::ChunkSize - 1, world::ChunkSize / 2),
                result.light.skyLight(world::ChunkSize / 2, world::ChunkSize / 2, 0),
                result.light.skyLight(world::ChunkSize / 2, world::ChunkSize / 2, world::ChunkSize - 1),
            };

            chunk->setLightData(std::move(result.light));
            chunk->clearLightingDirtyOnly();
            chunk->markMeshDirtyNoRevision();
            enqueueMeshIfNeeded(result.coord);
            ++stats.recomputed;

            constexpr std::array<world::ChunkCoord, 6> kFaceDeltas{{
                {-1, 0, 0}, {1, 0, 0},
                {0, -1, 0}, {0, 1, 0},
                {0, 0, -1}, {0, 0, 1},
            }};
            for (std::size_t i = 0; i < kFaceDeltas.size(); ++i) {
                if (newBorderHash[i] == oldBorderHash[i]) {
                    continue;
                }
                const world::ChunkCoord neighbour{
                    result.coord.x + kFaceDeltas[i].x,
                    result.coord.y + kFaceDeltas[i].y,
                    result.coord.z + kFaceDeltas[i].z};
                if (auto* n = chunks_.find(neighbour)) {
                    n->markLightingDirtyNoRevision();
                    n->markMeshDirtyNoRevision();
                    enqueueLightingIfNeeded(neighbour);
                    enqueueMeshIfNeeded(neighbour);
                }
            }
        }

        const auto frameStart = std::chrono::steady_clock::now();
        const auto maxInFlightLighting = std::max<std::size_t>(maxLightPropagationsPerTick_, 1U) * 2U;
        if (chunkJobMailbox_.inFlightLightingCount() >= maxInFlightLighting) {
            return stats;
        }
        const auto candidates = lightingQueue_.popClosest(streamingCenter(), maxLightPropagationsPerTick_);
        constexpr std::size_t kMinimumLightingSubmissionsPerTick = 2;
        for (const auto& item : candidates) {
            const auto elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
            if (stats.submitted >= kMinimumLightingSubmissionsPerTick
                && elapsedMs >= config_.workBudget.maxLightingMsPerFrame) {
                lightingQueue_.enqueue(item.coord, item.revision, item.meshRevision, item.priority);
                continue;
            }
            if (chunkJobMailbox_.inFlightLightingCount() >= maxInFlightLighting) {
                lightingQueue_.enqueue(item.coord, item.revision, item.meshRevision, item.priority);
                continue;
            }

            auto* chunk = chunks_.find(item.coord);
            if (chunk == nullptr) {
                continue;
            }
            if (chunk->state() != world::ChunkState::Resident && chunk->state() != world::ChunkState::MeshReady) {
                continue;
            }
            if (!chunk->dirty().lighting && chunk->lightData() != nullptr) {
                continue;
            }

            const auto coord = chunk->coord();
            const auto revision = chunk->revision();
            const auto neighborHash = lightingNeighborHash(coord);
            const world::LightingJobKey key{coord, revision, neighborHash};
            if (!chunkJobMailbox_.tryBeginLighting(key)) {
                continue;
            }

            struct LightingSnapshotBundle {
                world::Chunk target;
                std::array<std::unique_ptr<world::Chunk>, 6> neighbours{};
            };
            auto bundle = std::make_shared<LightingSnapshotBundle>();
            bundle->target = *chunk;
            constexpr std::array<world::ChunkCoord, 6> kFaceDeltas{{
                {-1, 0, 0}, {1, 0, 0},
                {0, -1, 0}, {0, 1, 0},
                {0, 0, -1}, {0, 0, 1},
            }};
            for (std::size_t i = 0; i < kFaceDeltas.size(); ++i) {
                const world::ChunkCoord nCoord{coord.x + kFaceDeltas[i].x, coord.y + kFaceDeltas[i].y, coord.z + kFaceDeltas[i].z};
                if (const auto* n = chunks_.find(nCoord)) {
                    bundle->neighbours[i] = std::make_unique<world::Chunk>(*n);
                }
            }

            const auto* lightCatalog = &blockLightCatalog_;
            auto* mailbox = &chunkJobMailbox_;
            const auto enqueueTime = std::chrono::steady_clock::now();
            jobs_.submit({"chunk.light", core::JobPriority::Medium},
                [bundle, coord, revision, neighborHash, lightCatalog, mailbox, enqueueTime]() mutable {
                    const auto start = std::chrono::steady_clock::now();
                    world::ChunkManager snapshotChunks;
                    snapshotChunks.store(bundle->target);
                    for (auto& neighbour : bundle->neighbours) {
                        if (neighbour) {
                            snapshotChunks.store(std::move(*neighbour));
                        }
                    }
                    const auto* target = snapshotChunks.find(coord);
                    world::ChunkLightData light;
                    world::LightPropagator propagator;
                    propagator.propagate(*target, snapshotChunks, *lightCatalog, light);
                    const auto end = std::chrono::steady_clock::now();

                    world::ChunkLightingResult result{};
                    result.coord = coord;
                    result.sourceRevision = revision;
                    result.neighborLightHash = neighborHash;
                    result.propagationTimeUs = elapsedUs(start, end);
                    result.queueWaitUs = elapsedUs(enqueueTime, start);
                    result.light = std::move(light);
                    mailbox->pushLighting(std::move(result));
                });
            ++stats.submitted;
        }

    stats.dirtyQueueScanTimeUs = elapsedUs(frameStart, std::chrono::steady_clock::now());
    return stats;

}

void Application::tickClusterMaintenance()
{
    if (!clusterRenderer_) {
        return; // useClusterLod was off at startup, or init failed
    }

    // Sync the renderer's runtime enable flag with the live config.
    // Lets the user toggle the LOD overlay on/off via the Runtime
    // Settings checkbox without restarting. When OFF we also skip the
    // rest of this function so no new builds are dispatched.
    clusterRenderer_->setEnabled(config_.useClusterLod);
    if (!config_.useClusterLod) {
        return;
    }

    // ---- -1. Evict cluster meshes outside the LOD2 retention radius ----
    // Without this, every cluster the player has *ever* visited stays
    // resident on the GPU. Move around for 60 seconds and `recordDraws`
    // iterates hundreds of cluster meshes per frame, each one touching
    // the origins SSBO. The "FPS drops as we go further out" complaint
    // is exactly this — the cluster set grows monotonically.
    //
    // Policy: evict any cluster whose Chebyshev distance from the
    // streaming center exceeds the build radius + a small hysteresis.
    // The +2 ring prevents thrash when the player walks back and forth
    // across a cluster boundary (rebuild → evict → rebuild is wasteful).
    //
    // Per-frame cap on evictions keeps the tick budget bounded — if the
    // player teleports, we drain over several frames instead of all
    // at once.
    {
        constexpr std::size_t kMaxEvictionsPerTick = 16;
        const auto evictCenterChunk = streamingCenter();
        const auto evictCenterCluster = world::chunkToCluster(evictCenterChunk);
        const std::int64_t chunkRadiusForEvict =
            std::int64_t{config_.streaming.renderDistanceChunks};
        const auto evictLodBands =
            world::computeLodBands(chunkRadiusForEvict, config_.farTerrainQualityMultiplier);
        const std::int64_t evictHorizR =
            std::max<std::int64_t>(3, chunkRadiusForEvict / world::ClusterChunkExtent + 4);
        const std::int64_t evictVertR =
            std::max<std::int64_t>(2,
                std::int64_t{config_.streaming.verticalRenderDistanceChunks}
                    / world::ClusterChunkExtent + 2);

        std::size_t evicted = 0;
        for (auto it = builtClusters_.begin(); it != builtClusters_.end();) {
            const auto dx = it->first.x - evictCenterCluster.x;
            const auto dy = it->first.y - evictCenterCluster.y;
            const auto dz = it->first.z - evictCenterCluster.z;
            const auto adx = dx >= 0 ? dx : -dx;
            const auto ady = dy >= 0 ? dy : -dy;
            const auto adz = dz >= 0 ? dz : -dz;
            const bool outOfRange = (adx > evictHorizR)
                                 || (adz > evictHorizR)
                                 || (ady > evictVertR);
            const bool insideFullChunkBand =
                clusterHorizontalDistanceChunks(it->first, evictCenterChunk) <= evictLodBands.lod0End;
            if ((outOfRange || insideFullChunkBand) && evicted < kMaxEvictionsPerTick) {
                clusterRenderer_->removeClusterMesh(it->first);
                it = builtClusters_.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
    }

    // ---- 0. Refresh the cluster skip-draw set --------------------------
    // Persistence-policy enforcement (see ClusterRenderer.hpp's
    // setSkipDrawClusters comment): skip drawing a cluster this frame
    // when every RESIDENT source chunk inside it has a current mesh.
    //
    // CRITICAL DESIGN POINT: we cannot require "all 64 chunks meshed"
    // because many cluster slots lie outside the player's vertical
    // render distance — those chunks WILL NEVER load. With
    // verticalRenderDistanceChunks=2 and ClusterChunkExtent=4, at most
    // ~half of a cluster's slots are ever populated. If we waited for
    // all 64, no cluster would ever get skipped and the LOD2 mesh would
    // permanently occlude the chunk surface (the visible "swiss-cheese
    // sea covers the world" bug).
    //
    // Correct policy: a cluster's chunk-coverage is "complete" iff every
    // CHUNK THAT EXISTS within its footprint also has a mesh. Where
    // there's no chunk, there's no chunk-mesh to occlude anyway, so the
    // cluster covering that volume is safe to suppress — at worst, the
    // suppressed volume is below the player's loaded slab and never
    // visible.
    //
    // Runs every tick because chunk mesh state changes constantly under
    // streaming + arena pressure. ~64 hash lookups per built cluster.
    {
        std::unordered_set<world::ClusterCoord, world::ClusterCoordHash> skip;
        skip.reserve(builtClusters_.size());
        const auto skipCenterChunk = streamingCenter();
        const auto skipCenterCluster = world::chunkToCluster(skipCenterChunk);
        const auto skipLodBands = world::computeLodBands(
            std::int64_t{config_.streaming.renderDistanceChunks},
            config_.farTerrainQualityMultiplier);
        const std::int64_t skipInnerR = std::max<std::int64_t>(1,
            (skipLodBands.lod0End + 2 * (world::ClusterChunkExtent - 1))
                / world::ClusterChunkExtent);
        for (const auto& [coord, mask] : builtClusters_) {
            if (clusterHorizontalDistanceChunks(coord, skipCenterChunk) <= skipLodBands.lod0End) {
                skip.insert(coord);
                continue;
            }

            const auto dxCluster = coord.x - skipCenterCluster.x;
            const auto dzCluster = coord.z - skipCenterCluster.z;
            const auto adxCluster = dxCluster >= 0 ? dxCluster : -dxCluster;
            const auto adzCluster = dzCluster >= 0 ? dzCluster : -dzCluster;
            if (std::max(adxCluster, adzCluster) < skipInnerR) {
                skip.insert(coord);
                continue;
            }

            const auto origin = world::clusterChunkOrigin(coord);
            bool allCoveredOrAbsent = true;
            bool anyResident = false;
            for (int dz = 0; dz < world::ClusterChunkExtent && allCoveredOrAbsent; ++dz) {
                for (int dy = 0; dy < world::ClusterChunkExtent && allCoveredOrAbsent; ++dy) {
                    for (int dx = 0; dx < world::ClusterChunkExtent && allCoveredOrAbsent; ++dx) {
                        const world::ChunkCoord cc{
                            origin.x + dx, origin.y + dy, origin.z + dz};
                        const auto* chunk = chunks_.find(cc);
                        if (chunk == nullptr) continue; // no chunk = nothing to occlude here
                        anyResident = true;
                        if (meshCache_.find(cc) == nullptr) {
                            allCoveredOrAbsent = false;
                        }
                    }
                }
            }
            // Only skip when there's at least one resident chunk in the
            // cluster AND every resident chunk has a mesh. A cluster with
            // ZERO resident chunks shouldn't draw either (nothing to show
            // and nothing to back), but in practice such clusters won't
            // be in builtClusters_ — they'd have mask=0 in the build
            // logic and wouldn't have been built.
            if (anyResident && allCoveredOrAbsent) {
                skip.insert(coord);
            }
        }
        clusterRenderer_->setSkipDrawClusters(std::move(skip));
    }

    // ---- 0a. Poll for completed greedy-merge worker job ----------------
    // A merged ClusterMesh is sitting in the future once the worker is
    // done with the face-list → greedy-merge pass. Upload it, persist
    // to disk cache, and clear the slot so the next GPU completion can
    // start its own merge.
    if (pendingMergeJob_.has_value()) {
        // wait_for(0) is a non-blocking ready check — cheap atomic load.
        if (pendingMergeJob_->future.wait_for(std::chrono::seconds(0))
            == std::future_status::ready) {
            auto clusterMesh = pendingMergeJob_->future.get();
            const auto coord = pendingMergeJob_->coord;
            const auto mask = pendingMergeJob_->residencyMask;
            pendingMergeJob_.reset();

            if (clusterMesh.vertices.empty() || clusterMesh.indices.empty()) {
                // Empty after greedy merge — record mask so we don't retry.
                builtClusters_[coord] = mask;
            } else if (clusterRenderer_->uploadClusterMesh(coord, clusterMesh)) {
                builtClusters_[coord] = mask;
                // Persist the result for future sessions.
                clusterMeshCache_.storeAsync(jobs_, coord, clusterMesh);
                const auto count = builtClusters_.size();
                if (count > 0 && (count & (count - 1)) == 0) {
                    int resident = 0;
                    for (std::uint64_t m = mask; m != 0; m &= (m - 1)) ++resident;
                    Logger::info(
                        "ClusterRenderer (GPU+worker): " + std::to_string(count)
                        + " LOD2 cluster(s) built (latest: "
                        + std::to_string(coord.x) + ","
                        + std::to_string(coord.y) + ","
                        + std::to_string(coord.z)
                        + ", chunks=" + std::to_string(resident) + "/64"
                        + ", verts=" + std::to_string(clusterMesh.vertices.size())
                        + ", tris=" + std::to_string(clusterMesh.indices.size() / 3) + ")");
                }
            }
            // Upload failure (arena full) → mask not recorded; candidate
            // stays eligible. Next tick will go through the pipeline again.
        }
    }

    // ---- 0b. Poll for completed GPU classification ---------------------
    // GPU classifier just finished; faces are in the readback buffer.
    // Instead of doing the greedy merge here on the main thread (which
    // historically cost 1-3 ms per cluster and showed up as a render_ms
    // spike), dispatch the merge to a worker via the JobSystem. Main
    // thread reclaims the GPU slot immediately and returns to its tick.
    if (clusterGpuSystem_ && pendingClusterJob_.has_value()
        && !pendingMergeJob_.has_value()) {
        if (auto parsed = clusterGpuSystem_->poll()) {
            const auto job = *pendingClusterJob_;
            pendingClusterJob_.reset();

            // Move the face list into the job lambda — the worker owns
            // it from this point. ~50-300 KB; the move is cheap.
            auto faces = std::move(parsed->faces);
            pendingMergeJob_ = PendingMergeJob{
                job.coord, job.residencyMask,
                jobs_.submit(
                    {"cluster.merge", core::JobPriority::Medium},
                    [coord = job.coord,
                     hash = job.sourceRevisionsHash,
                     faces = std::move(faces)]() {
                        // ClusterMesher is stateless — fresh instance
                        // is fine. The merge produces the final
                        // greedy-merged ClusterMesh.
                        render::meshing::ClusterMesher mesher;
                        return mesher.buildMeshFromGpuFaces(coord, hash, faces);
                    })
            };
        }
    }

    // ---- 1. Pick a build target ----------------------------------------
    // Walk the cluster ring around the player. We allow PARTIAL builds
    // (missing chunks are meshed as all-air via ClusterMesher's nullopt
    // handling) — without that, clusters at cluster-distance >= 2 would
    // never become buildable, because they contain chunks at chunk-
    // distance 8-11 which the streamer never loads.
    //
    // Build candidates are selected via two criteria:
    //   (a) Have at least one source chunk resident (otherwise the build
    //       is guaranteed to be empty/all-air — wasted work).
    //   (b) Either never built before, OR built before with a STRICTLY
    //       SMALLER residency mask. That second clause is the rebuild
    //       trigger — as more contained chunks load, we re-mesh.
    //
    // Phase 1D-2 will move builds to worker threads; for now the single-
    // cluster-per-tick throttle keeps the main thread budget tight.
    const auto center = streamingCenter();
    const auto centerCluster = world::chunkToCluster(center);
    const std::int64_t chunkRadius = std::int64_t{config_.streaming.renderDistanceChunks};
    const std::int64_t chunkVerticalRadius =
        std::max<std::int64_t>(1, std::int64_t{config_.streaming.verticalRenderDistanceChunks});
    // R0-LOD plan: derive cluster band edges from the deterministic
    // computeLodBands formula. LOD2 lives in chunk range
    // [lod0End, lod2End]; translating that to cluster rings:
    //   - We BUILD clusters anywhere LOD0/LOD1 chunks also exist
    //     (persistence policy — clusters fade in as chunks evict),
    //     so the inner edge is 0.
    //   - The outer edge is ceil(lod2End / ClusterChunkExtent).
    // Compare to the old `chunkRadius/4 + 2` formula: for R0=16
    // (lod2End=48), the new formula gives outerR=12 vs old=6.
    // That's a meaningfully bigger LOD2 ring — which is correct per
    // the user's spec.
    const auto lodBands =
        world::computeLodBands(chunkRadius, config_.farTerrainQualityMultiplier);
    const std::int64_t innerR = std::max<std::int64_t>(1,
        (lodBands.lod0End + 2 * (world::ClusterChunkExtent - 1))
            / world::ClusterChunkExtent);
    const std::int64_t outerR = std::max<std::int64_t>(2,
        (lodBands.lod2End + world::ClusterChunkExtent - 1) / world::ClusterChunkExtent);
    const std::int64_t outerRY =
        std::max<std::int64_t>(1, chunkVerticalRadius / world::ClusterChunkExtent + 1);

    // Helper: compute current residency mask for a candidate cluster.
    // Bit `clusterLocalChunkIndex(dx,dy,dz)` is set if that chunk is
    // currently Resident or MeshReady. Returns 0 if no chunks are loaded.
    const auto residencyMask = [&](const world::ClusterCoord& cc) -> std::uint64_t {
        std::uint64_t mask = 0;
        const auto origin = world::clusterChunkOrigin(cc);
        for (int dz = 0; dz < world::ClusterChunkExtent; ++dz) {
            for (int dy = 0; dy < world::ClusterChunkExtent; ++dy) {
                for (int dx = 0; dx < world::ClusterChunkExtent; ++dx) {
                    const world::ChunkCoord chunkCoord{
                        origin.x + dx, origin.y + dy, origin.z + dz};
                    const auto* chunk = chunks_.find(chunkCoord);
                    if (chunk == nullptr) continue;
                    const auto st = chunk->state();
                    if (st != world::ChunkState::Resident
                        && st != world::ChunkState::MeshReady) {
                        continue;
                    }
                    const auto slot =
                        render::meshing::clusterLocalChunkIndex(dx, dy, dz);
                    mask |= (std::uint64_t{1} << slot);
                }
            }
        }
        return mask;
    };

    // Find the closest cluster that needs (re-)building, prioritising
    // by Chebyshev distance so the immediate LOD2 surroundings come
    // first. Within the same ring, prefer clusters that are NEW (never
    // built) over rebuilds of partials.
    //
    // Frustum-aware build selection: outside a "close radius" we skip
    // candidates that aren't in the camera frustum. The close radius
    // (2 cluster rings ≈ 8 chunks) means we ALWAYS keep data immediately
    // around the player no matter where they look — needed for instant
    // turn-around. Beyond that ring, we only build what's actually being
    // looked at, which saves GPU classify + greedy-merge work on the 60-80%
    // of clusters behind the camera at any given moment.
    //
    // The frustum comes from the previous frame's recordDraws. A one-tick
    // lag is invisible to the player and beats the alternative of plumbing
    // the camera matrix through Application explicitly.
    const auto& cachedFrustum = clusterRenderer_->lastFrustum();
    constexpr std::int64_t kBuildFrustumCloseRing = 2;
    const auto clusterAabbInFrustum =
        [&cachedFrustum](world::ClusterCoord cc) noexcept {
            const auto origin = world::clusterChunkOrigin(cc);
            const float ox = static_cast<float>(origin.x * world::ChunkSize);
            const float oy = static_cast<float>(origin.y * world::ChunkSize);
            const float oz = static_cast<float>(origin.z * world::ChunkSize);
            const float ext = static_cast<float>(world::ClusterBlockExtent);
            return voxel::core::aabbIntersectsFrustum(cachedFrustum.planes,
                {ox, oy, oz}, {ox + ext, oy + ext, oz + ext});
        };

    std::optional<world::ClusterCoord> target;
    std::uint64_t targetMask = 0;
    std::int64_t targetDistance = outerR + 1;
    bool targetIsNew = false;
    for (std::int64_t dx = -outerR; dx <= outerR; ++dx) {
        for (std::int64_t dy = -outerRY; dy <= outerRY; ++dy) {
            for (std::int64_t dz = -outerR; dz <= outerR; ++dz) {
                const world::ClusterCoord candidate{
                    centerCluster.x + dx, centerCluster.y + dy, centerCluster.z + dz};
                const auto chebyshev = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
                const auto horizontalChebyshev = std::max(std::abs(dx), std::abs(dz));
                if (horizontalChebyshev < innerR) continue;
                if (clusterHorizontalDistanceChunks(candidate, center) <= lodBands.lod0End) continue;
                if (target.has_value() && chebyshev >= targetDistance) continue;

                const auto it = builtClusters_.find(candidate);
                const bool isNew = (it == builtClusters_.end());
                // Fully resident already? Nothing more we can do.
                if (!isNew && it->second == ~std::uint64_t{0}) continue;

                const auto mask = residencyMask(candidate);
                if (mask == 0) continue; // no source data → guaranteed empty build
                if (!isNew && mask == it->second) continue; // unchanged → no work

                // Build-time frustum cull. Only applies outside the close
                // ring AND only when we have a valid frustum from a
                // previous frame (first-frame fallback: build everything).
                if (cachedFrustum.valid
                    && chebyshev > kBuildFrustumCloseRing
                    && !clusterAabbInFrustum(candidate)) {
                    continue;
                }

                // SKIP-BUILD coverage check: don't waste a GPU classify
                // job on a cluster whose resident chunks already have
                // meshes — those chunks will fully occlude the cluster
                // (per persistence policy), so the cluster mesh would
                // be skip-DRAWN the moment it's built. This is the
                // symmetric counterpart to the skip-draw set computed
                // at step 0 of this function.
                //
                // Note we still build clusters whose chunks are PARTIALLY
                // meshed — the cluster fills the gaps until the rest
                // catch up. The check is "every resident chunk meshed"
                // AND "at least one chunk resident" (same as skip-draw).
                {
                    const auto checkOrigin = world::clusterChunkOrigin(candidate);
                    bool everyResidentMeshed = true;
                    bool anyResident = false;
                    for (int ez = 0; ez < world::ClusterChunkExtent && everyResidentMeshed; ++ez) {
                        for (int ey = 0; ey < world::ClusterChunkExtent && everyResidentMeshed; ++ey) {
                            for (int ex = 0; ex < world::ClusterChunkExtent && everyResidentMeshed; ++ex) {
                                const world::ChunkCoord checkCc{
                                    checkOrigin.x + ex,
                                    checkOrigin.y + ey,
                                    checkOrigin.z + ez};
                                if (chunks_.find(checkCc) == nullptr) continue;
                                anyResident = true;
                                if (meshCache_.find(checkCc) == nullptr) {
                                    everyResidentMeshed = false;
                                }
                            }
                        }
                    }
                    if (anyResident && everyResidentMeshed) {
                        continue; // chunks own the visual, skip build
                    }
                }

                // Tiebreak rules within the same Chebyshev ring:
                //   - Strictly closer always wins.
                //   - At the same distance, prefer NEW over rebuild.
                if (chebyshev == targetDistance && target.has_value()) {
                    if (targetIsNew && !isNew) continue;
                }

                target = candidate;
                targetMask = mask;
                targetDistance = chebyshev;
                targetIsNew = isNew;
            }
        }
    }
    if (!target.has_value()) {
        return; // nothing actionable this tick
    }

    // Adaptive build throttle: if the previous frame was already over a
    // soft budget, defer cluster builds this tick. Cluster work adds
    // ~2-5 ms per build (snapshot + memcpy 4.6 MB + GPU submit; greedy
    // merge + upload on a later tick). When the engine is already busy
    // (chunk loading bursts, save flushes, etc.), piling cluster work
    // on top is exactly what causes the user-visible spikes from
    // ~3 ms median to ~24 ms p99.
    //
    // 8 ms threshold (~120 fps) leaves headroom: faster than that, we
    // build aggressively; slower, we let the system catch its breath.
    // Eviction, skip-set updates, and pending-job polling still happen
    // every tick — only the *new build dispatch* is throttled.
    constexpr double kBuildThrottleMs = 8.0;
    if (runtimeStats_.lastFrameMs() > kBuildThrottleMs) {
        return;
    }

    // ---- 2. Snapshot the contained chunks (partial OK) -----------------
    // Slots whose chunks aren't resident are left as std::nullopt;
    // ClusterMesher treats those as all-air, which is what we want for
    // the "fade in from the horizon" effect: as chunks load, the cluster
    // mesh re-builds with more detail.
    render::meshing::ClusterChunkSnapshot snapshot;
    snapshot.coord = *target;
    const auto chunkOrigin = world::clusterChunkOrigin(*target);
    for (int dz = 0; dz < world::ClusterChunkExtent; ++dz) {
        for (int dy = 0; dy < world::ClusterChunkExtent; ++dy) {
            for (int dx = 0; dx < world::ClusterChunkExtent; ++dx) {
                const auto slot = render::meshing::clusterLocalChunkIndex(dx, dy, dz);
                if ((targetMask & (std::uint64_t{1} << slot)) == 0) {
                    continue; // not resident; leave as nullopt
                }
                const world::ChunkCoord cc{
                    chunkOrigin.x + dx, chunkOrigin.y + dy, chunkOrigin.z + dz};
                if (const auto* chunk = chunks_.find(cc); chunk != nullptr) {
                    snapshot.chunks[slot].emplace(chunk->cloneBlocksOnly());
                }
            }
        }
    }

    // ---- 2a. Neighbor face slabs (cross-cluster border culling) --------
    // Phase 1D-2d: snapshot the 16 chunks on each face of the 6
    // neighboring clusters that touch ours. The mesher reads these
    // when filling the 1-cell padded border of its supervoxel grid, so
    // the GPU classifier produces accurate boundary face emission
    // instead of always-emit-conservative behavior (which doubled face
    // geometry at every cluster seam).
    //
    // cloneBlocksOnly is shared_ptr-based, so 96 extra "clones" (6
    // faces × 16 chunks) is just refcount bumps — negligible cost.
    // Neighbors that aren't loaded stay as nullopt → mesher treats
    // those border supervoxels as Unknown, suppressing seam faces.
    {
        constexpr int kE = world::ClusterChunkExtent; // 4
        struct FaceInfo { int axis; int sign; };
        constexpr FaceInfo kFaceTable[6] = {
            {0, +1}, {0, -1}, {1, +1}, {1, -1}, {2, +1}, {2, -1},
        };
        for (int faceIdx = 0; faceIdx < 6; ++faceIdx) {
            const auto& fi = kFaceTable[faceIdx];
            world::ClusterCoord neighborCluster = *target;
            if (fi.axis == 0)      neighborCluster.x += fi.sign;
            else if (fi.axis == 1) neighborCluster.y += fi.sign;
            else                   neighborCluster.z += fi.sign;
            const auto neighborOrigin = world::clusterChunkOrigin(neighborCluster);
            const int uAxis = (fi.axis + 1) % 3;
            const int vAxis = (fi.axis + 2) % 3;
            // The neighbor's chunk on the side facing us: chunk_axis = 0
            // for positive faces (we're approaching from -X/-Y/-Z of
            // the neighbor) and chunk_axis = kE-1 for negative faces.
            const int faceChunk = (fi.sign > 0) ? 0 : (kE - 1);

            for (int v = 0; v < kE; ++v) {
                for (int u = 0; u < kE; ++u) {
                    int chunkOffset[3]{};
                    chunkOffset[fi.axis] = faceChunk;
                    chunkOffset[uAxis]   = u;
                    chunkOffset[vAxis]   = v;
                    const world::ChunkCoord cc{
                        neighborOrigin.x + chunkOffset[0],
                        neighborOrigin.y + chunkOffset[1],
                        neighborOrigin.z + chunkOffset[2]};
                    const auto* chunk = chunks_.find(cc);
                    if (chunk == nullptr) continue;
                    const auto state = chunk->state();
                    if (state != world::ChunkState::Resident
                        && state != world::ChunkState::MeshReady) {
                        continue;
                    }
                    const auto slabIdx = render::meshing::clusterFaceSlabIndex(u, v);
                    snapshot.neighborSlabs[faceIdx][slabIdx]
                        .emplace(chunk->cloneBlocksOnly());
                }
            }
        }
    }

    render::meshing::ClusterMesher mesher;

    // ---- 2b. On-disk cache lookup --------------------------------------
    // Before paying any GPU classify / greedy merge cost, ask the disk
    // cache whether we already built this exact (coord, sourceHash)
    // pair in a previous session. Hit → upload mesh straight to the
    // ClusterRenderer, mark built, return. Miss → fall through to the
    // GPU/CPU build paths below.
    const auto sourceHash =
        render::meshing::hashClusterChunkRevisions(snapshot);
    if (clusterMeshCache_.initialized()) {
        if (auto cached = clusterMeshCache_.tryLoad(*target, sourceHash)) {
            if (cached->vertices.empty() || cached->indices.empty()) {
                // Cached empty cluster (all-air). Mark built and skip.
                builtClusters_[*target] = targetMask;
                return;
            }
            if (clusterRenderer_->uploadClusterMesh(*target, *cached)) {
                builtClusters_[*target] = targetMask;
                const auto count = builtClusters_.size();
                if (count > 0 && (count & (count - 1)) == 0) {
                    Logger::info(
                        "ClusterRenderer (cache): " + std::to_string(count)
                        + " LOD2 cluster(s) built (latest: "
                        + std::to_string(target->x) + ","
                        + std::to_string(target->y) + ","
                        + std::to_string(target->z)
                        + ", verts=" + std::to_string(cached->vertices.size())
                        + ", tris=" + std::to_string(cached->indices.size() / 3) + ")");
                }
                return;
            }
            // Upload failed (arena full) — fall through to rebuild path,
            // which will retry the upload next tick.
        }
    }

    // ---- 3a. GPU path: submit CPU-reduced cells to the classifier ------
    //
    // When the GPU system is initialized and idle, we offload the heavy
    // face-classification work and bail out — the result will be picked
    // up next tick in step 0 (poll → greedy merge → upload). When the
    // GPU is busy or unavailable, fall through to the synchronous CPU
    // path below so cluster builds still make progress.
    // Don't dispatch a new GPU job if any pipeline stage is still in
    // flight: GPU classify (pendingClusterJob_) OR worker merge
    // (pendingMergeJob_). Keeping the cluster pipeline strictly
    // sequential simplifies state and matches our single-slot
    // ClusterGpuMeshing design. (Phase 1D-2f could pipeline multiple
    // builds, but each in-flight cluster also costs GPU memory for its
    // staging cells, so depth >2 hits diminishing returns.)
    if (clusterGpuSystem_ && !pendingClusterJob_.has_value()
        && !pendingMergeJob_.has_value()
        && !clusterGpuSystem_->busy()) {
        auto paddedCells = mesher.buildPaddedCellGrid(snapshot, blockRenderCatalog_);
        if (paddedCells.empty()) {
            // All-air after reduction. Record the mask so we don't retry.
            builtClusters_[*target] = targetMask;
            return;
        }
        if (clusterGpuSystem_->submit(paddedCells)) {
            pendingClusterJob_ = PendingClusterJob{
                *target, targetMask,
                render::meshing::hashClusterChunkRevisions(snapshot)
            };
            return; // wait for next tick's poll
        }
        // Submit failed (queue submit error, etc.) — fall through to CPU.
    }

    // ---- 3b. CPU fallback path: build the whole cluster synchronously --
    const auto clusterMesh = mesher.build(snapshot, blockRenderCatalog_);
    if (clusterMesh.vertices.empty() || clusterMesh.indices.empty()) {
        // Empty cluster (all-air, e.g. high-altitude or below-bedrock).
        // Record the mask so we don't re-attempt at the same residency.
        builtClusters_[*target] = targetMask;
        return;
    }

    // ---- 4. Upload to the GPU ------------------------------------------
    if (!clusterRenderer_->uploadClusterMesh(*target, clusterMesh)) {
        // Arena/staging full this frame; retry next tick (don't record
        // the mask, so the candidate stays eligible).
        return;
    }
    builtClusters_[*target] = targetMask;
    // Persist for next session (same as the GPU path).
    clusterMeshCache_.storeAsync(jobs_, *target, clusterMesh);

    // Progress log: emit at every power-of-two cluster count so the user
    // can see in the log when LOD2 starts appearing on screen.
    const auto count = builtClusters_.size();
    if (count > 0 && (count & (count - 1)) == 0) {
        // Count set bits in target mask for diagnostic — tells the user
        // whether this was a full (64/64) or partial build.
        int resident = 0;
        for (std::uint64_t m = targetMask; m != 0; m &= (m - 1)) ++resident;
        Logger::info(
            "ClusterRenderer: " + std::to_string(count) + " LOD2 cluster(s) built "
            "(latest: " + std::to_string(target->x) + ","
            + std::to_string(target->y) + "," + std::to_string(target->z)
            + ", chunks=" + std::to_string(resident) + "/64"
            + ", verts=" + std::to_string(clusterMesh.vertices.size())
            + ", tris=" + std::to_string(clusterMesh.indices.size() / 3) + ")");
    }
}

void Application::tickRegionMaintenance()
{
    if (!clusterRenderer_) {
        return;
    }
    clusterRenderer_->setEnabled(config_.useClusterLod); // mirror cluster path
    if (!config_.useClusterLod) {
        return;
    }

    const auto center = streamingCenter();
    const auto centerRegion = world::chunkToRegion(center);
    const std::int64_t chunkRadius =
        std::int64_t{config_.streaming.renderDistanceChunks};

    // ---- 0. Refresh skip-region set -----------------------------------
    // A region gets skipped this frame when ALL 64 of its contained
    // LOD2 clusters are in builtClusters_ — same persistence-policy
    // logic as the cluster→chunk skip, one tier higher.
    {
        std::unordered_set<world::RegionCoord, world::RegionCoordHash> skip;
        skip.reserve(builtRegions_.size());
        const auto regionSkipBands =
            world::computeLodBands(chunkRadius, config_.farTerrainQualityMultiplier);
        for (const auto& [coord, hashVal] : builtRegions_) {
            (void)hashVal;
            if (regionHorizontalDistanceChunks(coord, center) <= regionSkipBands.lod2End) {
                skip.insert(coord);
                continue;
            }

            const world::ClusterCoord clusterOrigin{
                coord.x * world::RegionClusterExtent,
                coord.y * world::RegionClusterExtent,
                coord.z * world::RegionClusterExtent};
            bool allClustersBuilt = true;
            for (int dz = 0; dz < world::RegionClusterExtent && allClustersBuilt; ++dz) {
                for (int dy = 0; dy < world::RegionClusterExtent && allClustersBuilt; ++dy) {
                    for (int dx = 0; dx < world::RegionClusterExtent && allClustersBuilt; ++dx) {
                        const world::ClusterCoord cc{
                            clusterOrigin.x + dx,
                            clusterOrigin.y + dy,
                            clusterOrigin.z + dz};
                        if (builtClusters_.count(cc) == 0) {
                            allClustersBuilt = false;
                        }
                    }
                }
            }
            if (allClustersBuilt) {
                skip.insert(coord);
            }
        }
        clusterRenderer_->setSkipDrawRegions(std::move(skip));
    }

    // ---- 0b. Poll pending region merge job ----------------------------
    if (pendingRegionMergeJob_.has_value()) {
        if (pendingRegionMergeJob_->future.wait_for(std::chrono::seconds(0))
            == std::future_status::ready) {
            auto regionMesh = pendingRegionMergeJob_->future.get();
            const auto coord = pendingRegionMergeJob_->coord;
            const auto hash = pendingRegionMergeJob_->sourceRevisionsHash;
            pendingRegionMergeJob_.reset();

            if (regionMesh.vertices.empty() || regionMesh.indices.empty()) {
                builtRegions_[coord] = hash;
                const world::ClusterCoord aliasCoord{
                    coord.x * world::RegionClusterExtent,
                    coord.y * world::RegionClusterExtent,
                    coord.z * world::RegionClusterExtent};
                regionMeshCache_.storeAsync(jobs_, aliasCoord, regionMesh);
            } else if (clusterRenderer_->uploadRegionMesh(coord, regionMesh)) {
                builtRegions_[coord] = hash;
                // Persist the built mesh for next session. Cache key is
                // the alias-cluster coord (region × RegionClusterExtent)
                // since ClusterMeshDiskCache is keyed by ClusterCoord;
                // the LOD3 cache directory keeps these isolated from
                // the LOD2 cache files at the same numeric coord.
                const world::ClusterCoord aliasCoord{
                    coord.x * world::RegionClusterExtent,
                    coord.y * world::RegionClusterExtent,
                    coord.z * world::RegionClusterExtent};
                regionMeshCache_.storeAsync(jobs_, aliasCoord, regionMesh);
                const auto count = builtRegions_.size();
                if (count > 0 && (count & (count - 1)) == 0) {
                    Logger::info(
                        "RegionRenderer: " + std::to_string(count)
                        + " LOD3 region(s) built (latest: "
                        + std::to_string(coord.x) + ","
                        + std::to_string(coord.y) + ","
                        + std::to_string(coord.z)
                        + ", verts=" + std::to_string(regionMesh.vertices.size())
                        + ", tris=" + std::to_string(regionMesh.indices.size() / 3) + ")");
                }
            }
        }
    }

    // ---- 1. Eviction --------------------------------------------------
    // Regions outside the retention radius get dropped. Hysteresis
    // larger than the build radius so walking out and back doesn't
    // thrash the rebuild path. Per-tick cap keeps the eviction loop
    // bounded.
    {
        constexpr std::size_t kMaxEvictionsPerTick = 8;
        // R0-LOD plan: retention is bounded on BOTH sides — outside
        // the outer ring (player walked away), AND inside the inner
        // ring (player walked into LOD3 territory and now needs LOD2
        // there instead). Without the inner-ring eviction, regions
        // become "stuck" rendering on top of LOD0 chunks after the
        // player moves into their footprint. That's the
        // "LOD3 on top of the player" symptom.
        const auto evictionLodBands =
            world::computeLodBands(chunkRadius, config_.farTerrainQualityMultiplier);
        constexpr std::int64_t kRegionExt = world::RegionChunkExtent;

        // Outer eviction: 1 ring beyond build outer ring (hysteresis).
        const std::int64_t buildOuterRing = std::max<std::int64_t>(2,
            (evictionLodBands.lod3End + kRegionExt - 1) / kRegionExt);
        const std::int64_t evictOuterRing = buildOuterRing + 1;

        // Inner eviction: EQUAL to build inner ring (no hysteresis on
        // the inner edge). Background: with 1 ring of hysteresis,
        // regions at chebyshev = buildInnerRing-1 stayed resident,
        // and their worst-case near edge lands inside LOD2 territory
        // (e.g., chebyshev=3 region's near edge can be at chunk 33;
        // lod2End=48 → 15-chunk overlap). That overlap rendered as
        // LOD3 meshes appearing on top of LOD0/LOD2 — the "LOD3 on
        // top of player" symptom.
        //
        // Tradeoff: walking back and forth across the inner boundary
        // causes the region to rebuild ~1-3 ms per crossing. The
        // disk cache handles repeat builds in ~1 ms, so the cost is
        // bounded. Correctness > thrash avoidance.
        // SURFACE ANCHOR (must mirror the build loop below): LOD3
        // surface regions live at fixed region.y, not the player's
        // current altitude. Evicting on |dy| > 1 from centerRegion.y
        // would drop the surface region the moment the player rose
        // above 16 chunks — exactly the "no LOD3 at altitude" bug.
        constexpr std::int64_t kSurfaceRegionYMin = 0;
        constexpr std::int64_t kSurfaceRegionYMax = 1;

        std::size_t evicted = 0;
        for (auto it = builtRegions_.begin();
             it != builtRegions_.end() && evicted < kMaxEvictionsPerTick;) {
            const auto dx = it->first.x - centerRegion.x;
            const auto dz = it->first.z - centerRegion.z;
            const auto adx = dx >= 0 ? dx : -dx;
            const auto adz = dz >= 0 ? dz : -dz;

            // Y axis: region must sit inside the surface anchor band
            // (currently a single region at y=0). Anything outside is
            // a stale build from a previous code path; drop it.
            const bool outOfRange = adx > evictOuterRing
                                 || adz > evictOuterRing
                                 || it->first.y < kSurfaceRegionYMin
                                 || it->first.y > kSurfaceRegionYMax;
            // Inside the inner ring means LOD2/LOD0 owns this volume now;
            // we don't want stale LOD3 meshes rendering on top.
            const bool insideInner =
                regionHorizontalDistanceChunks(it->first, center) <= evictionLodBands.lod2End;

            if (outOfRange || insideInner) {
                clusterRenderer_->removeRegionMesh(it->first);
                it = builtRegions_.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
    }

    // ---- 0c. Poll Stage B (GPU classify) — submit Stage C merge -------
    // The GPU classifier is shared with LOD2 via clusterGpuSystem_.
    // When the fence signals AND our pendingRegionGpuJob_ matches what
    // we submitted, the faces are ready. Drop them into a worker for
    // the greedy merge.
    //
    // Race-safety note: clusterGpuSystem_ has a single output slot. If
    // LOD2 has a pending GPU job AND LOD3 has one, only one of them is
    // actually in flight at the GPU (the other waited at submit-time
    // for the busy() check). The pending state for the *other* tier
    // represents "still queued" — its poll() will see the slot still
    // busy and short-circuit. So we can never read another tier's
    // faces by mistake.
    if (clusterGpuSystem_ && pendingRegionGpuJob_.has_value()
        && !pendingRegionMergeJob_.has_value()) {
        if (auto parsed = clusterGpuSystem_->poll()) {
            const auto job = *pendingRegionGpuJob_;
            pendingRegionGpuJob_.reset();
            auto faces = std::move(parsed->faces);
            // Reuse ClusterMesher::buildMeshFromGpuFaces — it produces
            // vertices in 0..128 units that the shader scales by 4.0
            // for LOD3 via the per-instance scale in origins SSBO .w.
            // The cluster mesher is tier-agnostic at this stage.
            //
            // Cast region coord to alias-cluster coord for the merge's
            // `coord` arg (ClusterMesh stores ClusterCoord). The
            // renderer routes to uploadedRegions_ at upload time based
            // on which uploadXxxMesh API we call.
            const world::ClusterCoord aliasCoord{
                job.coord.x * world::RegionClusterExtent,
                job.coord.y * world::RegionClusterExtent,
                job.coord.z * world::RegionClusterExtent};
            pendingRegionMergeJob_ = PendingRegionMergeJob{
                job.coord,
                job.sourceRevisionsHash,
                jobs_.submit(
                    {"region.merge", core::JobPriority::Medium},
                    [aliasCoord,
                     hash = job.sourceRevisionsHash,
                     faces = std::move(faces)]() {
                        render::meshing::ClusterMesher mesher;
                        return mesher.buildMeshFromGpuFaces(aliasCoord, hash, faces);
                    })
            };
        }
    }

    // ---- 0d. Poll Stage A (worker reduce) — submit Stage B GPU --------
    // If the worker has produced the padded cell grid AND the GPU
    // classifier slot is free (neither LOD2 nor LOD3 currently has
    // work in flight), submit the GPU job.
    if (pendingRegionPaddedJob_.has_value()
        && !pendingRegionGpuJob_.has_value()
        && !pendingRegionMergeJob_.has_value()
        && clusterGpuSystem_
        && !pendingClusterJob_.has_value()
        && !clusterGpuSystem_->busy()) {
        if (pendingRegionPaddedJob_->future.wait_for(std::chrono::seconds(0))
            == std::future_status::ready) {
            auto paddedCells = pendingRegionPaddedJob_->future.get();
            const auto coord = pendingRegionPaddedJob_->coord;
            const auto hash = pendingRegionPaddedJob_->sourceRevisionsHash;
            pendingRegionPaddedJob_.reset();

            if (paddedCells.empty()) {
                // Reduction produced no cells — empty region. Record
                // as built so we don't retry.
                builtRegions_[coord] = hash;
            } else if (clusterGpuSystem_->submit(paddedCells)) {
                pendingRegionGpuJob_ = PendingRegionGpuJob{coord, hash};
            }
            // Submit failure (queue submit error): drop the result on
            // the floor; will be retried on next tick's pick.
        }
    }

    // ---- 2. Adaptive throttle + in-flight gate ------------------------
    // Same 8 ms / one-region-at-a-time discipline as the LOD2 path.
    // Any of the 3 pipeline stages being in flight blocks new work.
    constexpr double kBuildThrottleMs = 8.0;
    if (runtimeStats_.lastFrameMs() > kBuildThrottleMs) {
        return;
    }
    if (pendingRegionPaddedJob_.has_value()
        || pendingRegionGpuJob_.has_value()
        || pendingRegionMergeJob_.has_value()) {
        return; // one region in the pipeline at a time
    }

    // ---- 3. Pick a build target ---------------------------------------
    // R0-LOD plan: derive LOD3 region band from computeLodBands.
    // LOD3 covers chunk range [lod2End, lod3End]; translating to
    // region rings:
    //
    //   inner ring: ceil((lod2End + 15) / 16) — guarantees worst-case
    //               near edge ≥ lod2End (region at ring d has worst-
    //               case near edge d*16 - 15 when player at region
    //               far side; for that to be ≥ lod2End, need
    //               d ≥ ceil((lod2End + 15) / 16)).
    //   outer ring: ceil(lod3End / 16) — region's worst-case far edge
    //               is d*16 + 16; floor-divide gives us the smallest
    //               ring whose near edge sits inside lod3End.
    //
    // For R0=16, farQ=4 (lod2End=48, lod3End=80):
    //   innerRing = ceil((48+15)/16) = 4 (worst-case near = 49 chunks)
    //   outerRing = ceil(80/16) = 5
    // → 2 rings of LOD3 covering ~49-95 chunks worst case.
    //
    // For R0=64, farQ=8 (lod2End=192, lod3End=576):
    //   innerRing = ceil((192+15)/16) = 13
    //   outerRing = ceil(576/16) = 36
    // → 24 rings of LOD3 covering ~193-575 chunks. Beefy horizon.
    const auto lodBands =
        world::computeLodBands(chunkRadius, config_.farTerrainQualityMultiplier);
    // Inner ring: smallest d such that worst-case near edge ≥ lod2End.
    // Region at ring d has worst-case near edge = d*16 - 15 (when
    // player sits at the far side of their own region). Solve:
    //   d*16 - 15 ≥ lod2End
    //   d ≥ (lod2End + 15) / 16
    //   d_min = ceil((lod2End + 15) / 16) = (lod2End + 15 + 15) / 16
    //         = (lod2End + 30) / 16
    //
    // Earlier implementation used (lod2End + 15) / 16 which floors —
    // gave inner ring 3 for lod2End=48 (worst-case near edge 33),
    // overlapping with the LOD2 zone. The correct ceiling pushes
    // inner ring to 4 (near edge 49).
    constexpr std::int64_t kRegionExt = world::RegionChunkExtent;
    const std::int64_t innerRegionRing = std::max<std::int64_t>(2,
        (lodBands.lod2End + (kRegionExt - 1) + (kRegionExt - 1)) / kRegionExt);
    // Outer ring: smallest d such that worst-case far edge ≥ lod3End.
    // Region at ring d has far edge = d*16 + 15 (when player sits at
    // their region's near side). Solve d*16 + 15 ≥ lod3End:
    //   d ≥ (lod3End - 15) / 16, but always at least innerRing.
    // Simpler: just ceil(lod3End / 16) — gives one extra row beyond
    // strict need, which is benign hysteresis.
    const std::int64_t outerRegionRing = std::max<std::int64_t>(innerRegionRing,
        (lodBands.lod3End + kRegionExt - 1) / kRegionExt);
    // SURFACE ANCHOR: clipmap LOD3 meshes are heightmap-derived top
    // surfaces. The terrain surface lives at world Y ≈ [0, ~200],
    // which is wholly inside region.y = 0 (covers world Y 0..511 at
    // 16 chunks/region × 32 blocks/chunk).
    //
    // The earlier code iterated region.y in [centerRegion.y - 1,
    // centerRegion.y + 1]. That worked at ground level but broke
    // when the player flew to altitude: centerRegion.y shifted up,
    // and the surface region (y=0) fell outside the build band. The
    // mesher returned empty for all the
    // built (altitude) regions, so the entire LOD3 horizon collapsed.
    //
    // Fix: anchor the Y iteration to a FIXED surface range,
    // independent of player altitude. Default seed surfaces stay
    // inside region.y=0; keeping the range tight (single region)
    // avoids wasted "all-empty" builds at altitude regions where
    // there is no surface to mesh anyway.
    //
    // ATGS mountains can exceed 511 blocks. Region y=0 covers
    // Y 0..511; region y=1 covers Y 512..1023 for tall peaks and
    // future sky terrain. Empty y=1 regions are recorded/cached as
    // empty so lowlands don't keep rebuilding them.
    constexpr std::int64_t kSurfaceRegionYMin = 0;
    constexpr std::int64_t kSurfaceRegionYMax = 1;

    std::optional<world::RegionCoord> target;
    std::int64_t targetDistance = outerRegionRing + 1;
    for (std::int64_t dx = -outerRegionRing; dx <= outerRegionRing; ++dx) {
        for (std::int64_t ry = kSurfaceRegionYMin; ry <= kSurfaceRegionYMax; ++ry) {
            for (std::int64_t dz = -outerRegionRing; dz <= outerRegionRing; ++dz) {
                // Skip inner rings (covered by LOD2 + chunks).
                // XZ chebyshev only — Y is anchored to surface, not
                // measured against player band.
                const auto chebyshev = std::max(std::abs(dx), std::abs(dz));
                if (chebyshev < innerRegionRing) continue;
                if (target.has_value() && chebyshev >= targetDistance) continue;

                const world::RegionCoord candidate{
                    centerRegion.x + dx, ry, centerRegion.z + dz};
                if (regionHorizontalDistanceChunks(candidate, center) <= lodBands.lod2End) {
                    continue;
                }

                // Already built? Skip. LOD3 clipmaps are keyed by the
                // terrain-version source hash and intentionally ignore
                // player edit deltas until the clipmap has an edit layer.
                if (builtRegions_.count(candidate) != 0) continue;

                target = candidate;
                targetDistance = chebyshev;
            }
        }
    }
    if (!target.has_value()) {
        return;
    }

    // ---- 4. Compute clipmap source hash ------------------------------
    // LOD3 is now clipmap-based (heightmap-derived surface meshes from
    // NoiseTerrainGenerator). No chunk snapshot needed — terrain is
    // sampled directly from noise functions, which work at any world
    // coord regardless of chunk residency. This is the key win over
    // the previous voxel-based LOD3: doesn't require chunks loaded
    // at LOD3 distance.
    //
    // The cache hash includes terrain version + region coord. Terrain
    // version captures seed + settings — if the user changes those,
    // the cache invalidates automatically. (For clipmap there's no
    // per-chunk revision because chunks aren't read.)
    const auto sourceHash = render::meshing::hashClipmapRegion(*target, terrainGenerator_);

    // ---- 4b. Try on-disk cache before paying the build cost -----------
    // Hit → upload the cached mesh, record as built, return. Skips the
    // ~10-20 ms worker build entirely. Cache key uses the alias-cluster
    // coord (region.coord × RegionClusterExtent) so the existing
    // ClusterMeshDiskCache (which keys by ClusterCoord) can store
    // region meshes without modification.
    const world::ClusterCoord aliasCoord{
        target->x * world::RegionClusterExtent,
        target->y * world::RegionClusterExtent,
        target->z * world::RegionClusterExtent};
    if (regionMeshCache_.initialized()) {
        if (auto cached = regionMeshCache_.tryLoad(aliasCoord, sourceHash)) {
            if (cached->vertices.empty() || cached->indices.empty()) {
                // Cached "all-air" region — record as built so we don't
                // re-attempt every tick.
                builtRegions_[*target] = sourceHash;
                return;
            }
            if (clusterRenderer_->uploadRegionMesh(*target, *cached)) {
                builtRegions_[*target] = sourceHash;
                const auto count = builtRegions_.size();
                if (count > 0 && (count & (count - 1)) == 0) {
                    Logger::info(
                        "RegionRenderer (cache): " + std::to_string(count)
                        + " LOD3 region(s) built (latest: "
                        + std::to_string(target->x) + ","
                        + std::to_string(target->y) + ","
                        + std::to_string(target->z)
                        + ", verts=" + std::to_string(cached->vertices.size())
                        + ", tris=" + std::to_string(cached->indices.size() / 3) + ")");
                }
                return;
            }
            // Upload failed (arena full) — fall through to worker
            // dispatch; upload will retry next tick. Not strictly
            // necessary to re-dispatch since the cached mesh is still
            // valid, but the simpler control flow is to just rebuild.
        }
    }

    // ---- 5. Submit clipmap build to a worker --------------------------
    // LOD3 is now clipmap (heightmap-derived surfaces). A single
    // worker job samples NoiseTerrainGenerator at a 65×65 grid over
    // the region's XZ footprint, emits a triangle-strip surface, and
    // returns a ClusterMesh. ~1-3 ms on a worker thread.
    //
    // The 3-stage GPU pipeline that the voxel LOD3 used (Stage A
    // worker reduce → Stage B GPU classify → Stage C worker merge) is
    // gone — clipmap doesn't need the GPU classifier because there's
    // no visibility-classification to do (flat surface, top face only).
    //
    // The polling site in step 0b consumes pendingRegionMergeJob_ the
    // same way for both voxel and clipmap meshes, so no changes needed
    // upstream.
    pendingRegionMergeJob_ = PendingRegionMergeJob{
        *target,
        sourceHash,
        jobs_.submit(
            {"region.clipmap", core::JobPriority::Medium},
            [coord = *target, terrainGen = &terrainGenerator_]() {
                render::meshing::ClipmapRegionMesher mesher;
                return mesher.build(coord, *terrainGen);
            })
    };
}

Application::MeshDispatchStats Application::dispatchMeshJobs()
{
    MeshDispatchStats stats{};
    const auto maxMeshJobs = config_.workBudget.maxMeshJobsPerTick > 0
        ? config_.workBudget.maxMeshJobsPerTick
        : config_.maxChunkMeshesPerTick;
    constexpr std::size_t kMinimumMeshSubmissionsPerTick = 2;
    const bool generationBacklog = chunkJobMailbox_.inFlightGenerationCount() > 0
        || chunkJobMailbox_.pendingGenerationResults() > 0;
    const auto activeMeshBudget = maxMeshJobs;
    const auto maxInFlightMesh = generationBacklog
        ? std::max<std::size_t>(maxMeshJobs, kMinimumMeshSubmissionsPerTick)
        : std::max<std::size_t>(maxMeshJobs, 1U) * 2U;
    const auto center = streamingCenter();
    const auto frameStart = std::chrono::steady_clock::now();
    if (hybridMeshingGpuSystem_ && pendingHybridMeshJob_) {
        if (auto completed = hybridMeshingGpuSystem_->pollClassification()) {
            if (pendingHybridMeshJob_->target) {
                auto mesh = mesher_.buildFromVisibleFaces(*pendingHybridMeshJob_->target, completed->faces);
                const auto end = std::chrono::steady_clock::now();
                mesh.sourceMeshRevisionHash = pendingHybridMeshJob_->neighborRevisionHash;

                world::ChunkMeshResult result{};
                result.coord = pendingHybridMeshJob_->coord;
                result.sourceRevision = pendingHybridMeshJob_->sourceRevision;
                result.sourceMeshRevision = pendingHybridMeshJob_->sourceMeshRevision;
                result.neighborRevisionHash = pendingHybridMeshJob_->neighborRevisionHash;
                result.buildTimeUs = elapsedUs(pendingHybridMeshJob_->submitTime, end);
                result.queueWaitUs = elapsedUs(pendingHybridMeshJob_->enqueueTime, pendingHybridMeshJob_->submitTime);
                result.mesh = std::move(mesh);
                chunkJobMailbox_.pushMesh(std::move(result));
            } else {
                chunkJobMailbox_.endMesh(world::MeshJobKey{
                    pendingHybridMeshJob_->coord,
                    pendingHybridMeshJob_->sourceRevision,
                    pendingHybridMeshJob_->neighborRevisionHash});
            }
            pendingHybridMeshJob_.reset();
        }
    }
    if (chunkJobMailbox_.inFlightMeshCount() >= maxInFlightMesh) {
        return stats;
    }
    // OPTIMIZATION: single-lock snapshot of in-flight generation, so the
    // per-candidate neighbour-busy check below can do hash lookups without
    // contending with worker pushes on the mailbox mutex.
    // FRAME-CACHED: shared with `streamingDispatchRequestsForFrame` earlier
    // in the same tick — see getInFlightGenerationSnapshot().
    const auto& inFlightSnapshot = getInFlightGenerationSnapshot();
    const auto candidates = meshQueue_.popClosest(center, activeMeshBudget);
    for (const auto& item : candidates) {
        const auto elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
        if (stats.submitted >= kMinimumMeshSubmissionsPerTick
            && elapsedMs >= config_.workBudget.maxDirtyScanMsPerFrame) {
            meshQueue_.enqueue(item.coord, item.revision, item.meshRevision, item.priority);
            continue;
        }
        if (chunkJobMailbox_.inFlightMeshCount() >= maxInFlightMesh) {
            meshQueue_.enqueue(item.coord, item.revision, item.meshRevision, item.priority);
            continue;
        }

            auto* chunk = chunks_.find(item.coord);
            if (chunk == nullptr) {
                continue;
            }
            if (chunk->state() != world::ChunkState::Resident && chunk->state() != world::ChunkState::MeshReady) {
                continue;
            }
            if (!chunk->dirty().mesh && meshCache_.isCurrent(chunk->coord(), chunk->revision())) {
                continue;
            }
            const auto coord = chunk->coord();
            const auto distanceScore = chunkDistanceScore(coord, center);
            // OPTIMIZATION (fast-movement): defer the mesh dispatch if any of
            // this chunk's six neighbours is currently being generated. The
            // worker will produce a mesh keyed against a neighbour revision
            // that's about to change — guaranteed stale. By re-enqueueing
            // here, we avoid dispatching work that we already know will be
            // discarded. The chunk stays mesh-dirty; once neighbour generation
            // finishes (typically next 1-2 frames), the dispatch goes ahead
            // with stable neighbour revisions.
            if (distanceScore > 16) {
                constexpr std::array<world::ChunkCoord, 6> kFaceDeltas{{
                    {-1, 0, 0}, {1, 0, 0},
                    {0, -1, 0}, {0, 1, 0},
                    {0, 0, -1}, {0, 0, 1},
                }};
                bool neighbourBusy = false;
                for (const auto& delta : kFaceDeltas) {
                    const world::ChunkCoord nCoord{coord.x + delta.x, coord.y + delta.y, coord.z + delta.z};
                    if (inFlightSnapshot.count(nCoord) != 0) {
                        neighbourBusy = true;
                        break;
                    }
                }
                if (neighbourBusy) {
                    meshQueue_.enqueue(item.coord, item.revision, item.meshRevision, item.priority);
                    continue;
                }
            }
            const auto revision = chunk->revision();
            // Capture meshRevision at dispatch time so the install path can
            // detect post-dispatch dirty-marks (e.g. fluid sim carving more
            // cells while this job is on a worker). Without this, the chain
            // of carves after the first one would never re-mesh.
            const auto dispatchMeshRevision = chunk->meshRevision();
            const auto neighborRevisionHash = meshNeighborRevisionHash(coord);
            const world::MeshJobKey meshKey{coord, revision, neighborRevisionHash};
            if (!chunkJobMailbox_.tryBeginMesh(meshKey)) {
                continue;
            }

            const auto snapshotStart = std::chrono::steady_clock::now();
            // OPTIMIZATION: bundle holds neighbours by value (std::optional)
            // and uses cloneBlocksOnly() to avoid the 32 KB-per-neighbour
            // light-data alloc/memcpy/free that the old `make_unique<Chunk>(*n)`
            // produced. Bundle ownership is unique (only the job lambda holds
            // it) so we use unique_ptr instead of shared_ptr to skip atomic
            // refcount ops.
            struct MeshSnapshotBundle {
                world::Chunk target;
                std::array<std::optional<world::Chunk>, 6> neighbours{};
            };
            auto bundle = std::make_unique<MeshSnapshotBundle>();
            bundle->target = chunk->cloneBlocksOnly();
            if (!config_.useGpuShaderLighting && chunk->lightData() != nullptr) {
                // Only when CPU lighting is enabled do we need light data on
                // the worker. Copy explicitly in that case.
                bundle->target.setLightData(*chunk->lightData());
            }
            constexpr std::array<world::ChunkCoord, 6> kFaceDeltas{{
                {-1, 0, 0}, {1, 0, 0},
                {0, -1, 0}, {0, 1, 0},
                {0, 0, -1}, {0, 0, 1},
            }};
            for (std::size_t i = 0; i < kFaceDeltas.size(); ++i) {
                const world::ChunkCoord nCoord{coord.x + kFaceDeltas[i].x, coord.y + kFaceDeltas[i].y, coord.z + kFaceDeltas[i].z};
                if (const auto* n = chunks_.find(nCoord)) {
                    bundle->neighbours[i].emplace(n->cloneBlocksOnly());
                }
            }
            core::recordTimer(stats.snapshotTime, elapsedUs(snapshotStart, std::chrono::steady_clock::now()));

            const auto* renderCatalog = &blockRenderCatalog_;
            const bool shaderLighting = config_.useGpuShaderLighting;
            render::meshing::MeshingOptions meshOptions{};
            meshOptions.staticWaterSurfaceY = static_cast<int>(std::floor(terrainGenerator_.settings().seaLevel));
            auto* mailbox = &chunkJobMailbox_;
            const auto enqueueTime = std::chrono::steady_clock::now();
            core::JobPriority meshPriority = core::JobPriority::Medium;
            if (generationBacklog) {
                meshPriority = distanceScore <= 4
                    ? core::JobPriority::Critical
                    : (distanceScore <= 16 ? core::JobPriority::High : core::JobPriority::Medium);
            } else if (distanceScore <= 4) {
                meshPriority = core::JobPriority::Critical;
            } else if (distanceScore <= 64) {
                meshPriority = core::JobPriority::High;
            }
            if (hybridMeshingGpuSystem_ && shaderLighting
                && !pendingHybridMeshJob_ && !hybridMeshingGpuSystem_->busy()) {
                render::meshing::ChunkNeighborhood neighborhood{};
                const auto neighbourPtr = [&](std::size_t i) -> const world::Chunk* {
                    return bundle->neighbours[i].has_value() ? &*bundle->neighbours[i] : nullptr;
                };
                neighborhood.negX = neighbourPtr(0);
                neighborhood.posX = neighbourPtr(1);
                neighborhood.negY = neighbourPtr(2);
                neighborhood.posY = neighbourPtr(3);
                neighborhood.negZ = neighbourPtr(4);
                neighborhood.posZ = neighbourPtr(5);

                const auto submitTime = std::chrono::steady_clock::now();
                if (hybridMeshingGpuSystem_->submitClassification(
                        bundle->target, *renderCatalog, nullptr, neighborhood, meshOptions)) {
                    PendingHybridMeshJob pending{};
                    pending.target.emplace(std::move(bundle->target));
                    pending.coord = coord;
                    pending.sourceRevision = revision;
                    pending.sourceMeshRevision = dispatchMeshRevision;
                    pending.neighborRevisionHash = neighborRevisionHash;
                    pending.enqueueTime = enqueueTime;
                    pending.submitTime = submitTime;
                    pendingHybridMeshJob_ = std::move(pending);
                    ++stats.submitted;
                    continue;
                }
            }
            jobs_.submit({"chunk.mesh", meshPriority},
                [bundle = std::move(bundle), coord, revision, dispatchMeshRevision, neighborRevisionHash, mailbox, renderCatalog, shaderLighting, meshOptions, enqueueTime]() mutable {
                    const auto start = std::chrono::steady_clock::now();
                    render::meshing::ChunkNeighborhood neighborhood{};
                    // std::optional<Chunk>::operator->() / get_ptr-style: take
                    // a raw pointer to the contained value when present,
                    // nullptr otherwise. Matches the old unique_ptr semantics.
                    const auto neighbourPtr = [&](std::size_t i) -> const world::Chunk* {
                        return bundle->neighbours[i].has_value() ? &*bundle->neighbours[i] : nullptr;
                    };
                    neighborhood.negX = neighbourPtr(0);
                    neighborhood.posX = neighbourPtr(1);
                    neighborhood.negY = neighbourPtr(2);
                    neighborhood.posY = neighbourPtr(3);
                    neighborhood.negZ = neighbourPtr(4);
                    neighborhood.posZ = neighbourPtr(5);

                    render::meshing::GreedyMesher mesher;
                    auto mesh = mesher.build(bundle->target, *renderCatalog,
                                              shaderLighting ? nullptr : bundle->target.lightData(), neighborhood, meshOptions);
                    const auto end = std::chrono::steady_clock::now();
                    mesh.sourceMeshRevisionHash = neighborRevisionHash;
                    world::ChunkMeshResult result{};
                    result.coord = coord;
                    result.sourceRevision = revision;
                    result.sourceMeshRevision = dispatchMeshRevision;
                    result.neighborRevisionHash = neighborRevisionHash;
                    result.buildTimeUs = elapsedUs(start, end);
                    result.queueWaitUs = elapsedUs(enqueueTime, start);
                    result.mesh = std::move(mesh);
                    mailbox->pushMesh(std::move(result));
                });
            ++stats.submitted;
        }
    stats.dirtyQueueScanTimeUs = elapsedUs(frameStart, std::chrono::steady_clock::now());
    return stats;

}

Application::MeshInstallStats Application::installMeshResults()
{
    auto results = std::move(pendingMeshResults_);
    pendingMeshResults_.clear();
    auto drained = chunkJobMailbox_.drainMesh();
    MeshInstallStats stats{};
    stats.completed = drained.size();
    for (auto& result : drained) {
        chunkJobMailbox_.endMesh(world::MeshJobKey{result.coord, result.sourceRevision, result.neighborRevisionHash});
        results.push_back(std::move(result));
    }
    const auto center = streamingCenter();
    std::sort(results.begin(), results.end(), [center](const auto& lhs, const auto& rhs) {
        const auto l = chunkDistanceScore(lhs.coord, center);
        const auto r = chunkDistanceScore(rhs.coord, center);
        if (l != r) {
            return l < r;
        }
        if (lhs.coord.y != rhs.coord.y) {
            return lhs.coord.y < rhs.coord.y;
        }
        if (lhs.coord.x != rhs.coord.x) {
            return lhs.coord.x < rhs.coord.x;
        }
        return lhs.coord.z < rhs.coord.z;
    });

    const auto maxInstalls = config_.workBudget.maxMeshInstallsPerTick;
    const auto maxUploadBytes = config_.workBudget.maxGpuUploadBytesPerTick;
    const auto frameStart = std::chrono::steady_clock::now();
    const auto uploadSubmitStart = std::chrono::steady_clock::now();
    std::size_t installedThisTick = 0;
    std::uint64_t uploadedBytesThisTick = 0;

    // NOTE on the async-upload attempt: we previously dispatched the vertex
    // and index memcpys to `jobs_.submit(JobPriority::High, ...)` and waited
    // at end of frame. That regressed mesh_install p99 from 10 ms → 20 ms and
    // max from 16 ms → 1305 ms because of priority inversion in the
    // JobSystem — workers don't preempt, so a 20 µs memcpy queued behind a
    // 50-100 ms `chunk.mesh` job stalled until that mesh finished. The
    // architectural split (prepare on main thread, memcpy elsewhere) is still
    // valid, but it needs a dedicated thread pool for short-lived uploads to
    // avoid head-of-line blocking with mesh generation. For now we do the
    // memcpy inline on the main thread — same as before this refactor, but
    // routed through the new `prepareChunkMeshUpload` API so the future
    // dedicated-pool implementation has a clean seam to plug into.
    const auto deferRemaining = [&](std::size_t firstDeferred) {
        if (firstDeferred >= results.size()) {
            return;
        }
        stats.uploadBudgetDeferrals += results.size() - firstDeferred;
        pendingMeshResults_.reserve(pendingMeshResults_.size() + (results.size() - firstDeferred));
        for (std::size_t deferred = firstDeferred; deferred < results.size(); ++deferred) {
            pendingMeshResults_.push_back(std::move(results[deferred]));
        }
    };

    for (std::size_t resultIndex = 0; resultIndex < results.size(); ++resultIndex) {
        auto& result = results[resultIndex];
        core::recordTimer(stats.meshBuildTime, result.buildTimeUs);
        core::recordTimer(stats.queueWaitTime, result.queueWaitUs);

        if (installedThisTick >= maxInstalls) {
            deferRemaining(resultIndex);
            break;
        }
        const auto elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
        if (elapsedMs >= config_.workBudget.maxMeshInstallMsPerFrame) {
            deferRemaining(resultIndex);
            break;
        }

        auto* chunk = chunks_.find(result.coord);
        if (chunk == nullptr) {
            ++stats.staleDiscarded;
            continue; // Chunk evicted while job was in flight.
        }
        if (chunk->revision() != result.sourceRevision
            || chunk->meshRevision() != result.sourceMeshRevision
            || meshNeighborRevisionHash(result.coord) != result.neighborRevisionHash) {
            // Stale: chunk was edited (revision bump) OR re-marked dirty
            // (meshRevision bump from fluid sim / similar silent edits) OR
            // a neighbour's revision changed after the snapshot. Discard
            // and re-enqueue for a fresh build next tick.
            ++stats.staleDiscarded;
            enqueueMeshIfNeeded(result.coord);
            continue;
        }

        const auto bytes = meshBytes(result.mesh);
        if (bytes == 0) {
            meshCache_.store(result.coord, std::move(result.mesh));
            chunk->clearMeshDirtyOnly();
            chunk->setState(world::ChunkState::MeshReady);
            ++installedThisTick;
            continue;
        }
        if (uploadedBytesThisTick > 0 && uploadedBytesThisTick + bytes > maxUploadBytes) {
            deferRemaining(resultIndex);
            break;
        }
        const auto uploadSubmitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - uploadSubmitStart).count();
        if (uploadedBytesThisTick > 0 && uploadSubmitMs >= config_.workBudget.maxUploadSubmitMsPerFrame) {
            deferRemaining(resultIndex);
            break;
        }

        auto prepared = renderer_.prepareChunkMeshUpload(result.coord, result.mesh);
        if (prepared.duplicateSkip) {
            // Renderer already has this revision; treat as a successful no-op
            // install so we still update meshCache_ + chunk state.
            meshDataToFree_.emplace_back(std::move(result.mesh.vertices), std::move(result.mesh.indices));
            meshCache_.store(result.coord, std::move(result.mesh));
            chunk->clearMeshDirtyOnly();
            chunk->setState(world::ChunkState::MeshReady);
            ++installedThisTick;
            continue;
        }
        if (!prepared.valid) {
            deferRemaining(resultIndex);
            break;
        }

        // Inline memcpy on the main thread (see NOTE above about why this is
        // not currently dispatched to workers).
        std::memcpy(prepared.vertexDst, prepared.vertexSrc, prepared.vertexBytes);
        std::memcpy(prepared.indexDst, prepared.indexSrc, prepared.indexBytes);

        uploadedBytesThisTick += bytes;
        stats.uploadedBytes += bytes;
        // GPU has the authoritative copy; drop the CPU-side vertex/index data
        // to halve per-chunk memory usage at scale. Deferred to end-of-tick
        // because in Debug builds the allocator scribbles freed memory and a
        // 100 KB vector free costs ~100 µs.
        meshDataToFree_.emplace_back(std::move(result.mesh.vertices), std::move(result.mesh.indices));
        meshCache_.store(result.coord, std::move(result.mesh));
        chunk->clearMeshDirtyOnly();
        chunk->setState(world::ChunkState::MeshReady);
        ++stats.uploaded;
        ++installedThisTick;
    }
    return stats;
}

void Application::logRuntimeStats(const char* label, const core::RuntimeCounters& counters, double averageFrameMs) const
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << label
        << ": frames=" << counters.frames
        << " avg_ms=" << averageFrameMs
        << " last_ms=" << runtimeStats_.lastFrameMs()
        << " requests=" << counters.chunkRequestsPlanned
        << " gen=" << counters.generationJobsSubmitted << "/" << counters.generationJobsCompleted
        << " mesh=" << counters.meshJobsSubmitted << "/" << counters.meshJobsCompleted
        << " stale=" << counters.meshJobsDiscardedStale
        << " light=" << counters.lightingJobsSubmitted << "/" << counters.lightingJobsCompleted
        << " light_stale=" << counters.lightingJobsDiscardedStale
        << " neighbor_remesh=" << counters.remeshesCausedByNeighborInstall
        << " edits=" << counters.blockEditsAccepted << "/" << counters.blockEditsRejected
        << " dirty_mesh=" << counters.dirtyMeshChunksQueued << "/" << counters.dirtyMeshChunksCoalesced
        << " dirty_light=" << counters.dirtyLightingChunksQueued << "/" << counters.dirtyLightingChunksCoalesced
        << " dirty_q(l/m)=" << counters.dirtyLightingQueueLength << "/" << counters.dirtyMeshQueueLength
        << " backlog(jobs/gen/mesh/light)=" << counters.jobSystemPending << "/" << counters.pendingGenerationResults
        << "/" << counters.pendingMeshResults << "/" << counters.pendingLightingResults
        << " save_q=" << counters.saveQueueLength
        << " saves=" << counters.savesFlushed
        << " upload_defer=" << counters.uploadBudgetDeferrals
        << " budget_sat(l/m/s)=" << counters.lightingBudgetSaturated << "/" << counters.meshInstallBudgetSaturated << "/" << counters.saveBudgetSaturated
        << " slow_frames=" << counters.slowFrames
        << " gpu_uploads=" << counters.gpuUploads
        << " upload_batches=" << counters.uploadBatchCount
        << " upload_batch_kb=" << (static_cast<double>(counters.uploadBatchBytes) / 1024.0)
        << " upload_q=" << counters.uploadQueueLength
        << " drawable=" << counters.chunksMadeDrawable
        << " draw_visible=" << counters.chunksDrawn
        << " draw_culled=" << counters.chunksCulled
        << " gpu_cull(d/sec/vis/cpu/mis)=" << counters.gpuCullDispatches
        << "/" << counters.gpuCullSections << "/" << counters.gpuCullVisible
        << "/" << counters.gpuCullCpuVisible << "/" << counters.gpuCullMismatches
        << " gpu_draw_cmds=" << counters.gpuCullDrawCommands
        << " scene_sync(entries/full)=" << counters.sceneEntriesSynced << "/" << counters.sceneFullSyncs
        << " staging_kb=" << (static_cast<double>(counters.stagingUploadBytes) / 1024.0)
        << " prepass_jobs=" << counters.terrainPrepassJobsSubmitted << "/" << counters.terrainPrepassJobsCompleted
        << " prepass_cache=" << counters.terrainPrepassCacheHits << "/" << counters.terrainPrepassCacheMisses
        << "/" << counters.terrainPrepassCacheEntries
        << " prepass_ms(avg/max)=" << timerAverageMs(counters.terrainPrepass) << "/" << timerMaxMs(counters.terrainPrepass)
        << " gen_ms(avg/max)=" << timerAverageMs(counters.terrainGeneration) << "/" << timerMaxMs(counters.terrainGeneration)
        << " gen_prepass_ms(avg/max)=" << timerAverageMs(counters.terrainGenerationFromPrepass) << "/" << timerMaxMs(counters.terrainGenerationFromPrepass)
        << " gen_direct_ms(avg/max)=" << timerAverageMs(counters.terrainGenerationDirect) << "/" << timerMaxMs(counters.terrainGenerationDirect)
        << " light_ms(avg/max)=" << timerAverageMs(counters.lightingPropagation) << "/" << timerMaxMs(counters.lightingPropagation)
        << " mesh_build_ms(avg/max)=" << timerAverageMs(counters.meshBuild) << "/" << timerMaxMs(counters.meshBuild)
        << " snapshot_ms(avg/max)=" << timerAverageMs(counters.meshSnapshot) << "/" << timerMaxMs(counters.meshSnapshot)
        << " gpu_ms(avg/max)=" << timerAverageMs(counters.gpuUpload) << "/" << timerMaxMs(counters.gpuUpload)
        << " fence_ms(avg/max)=" << timerAverageMs(counters.rendererFenceWait) << "/" << timerMaxMs(counters.rendererFenceWait)
        << " load_ms(avg/max)=" << timerAverageMs(counters.load) << "/" << timerMaxMs(counters.load)
        << " save_ms(avg/max)=" << timerAverageMs(counters.save) << "/" << timerMaxMs(counters.save)
        << " queue_ms(avg/max)=" << timerAverageMs(counters.queueWait) << "/" << timerMaxMs(counters.queueWait)
        << " stage_ms(stream/player/mesh_i/mesh_d/light/save/sim/render)="
        << timerAverageMs(counters.stageStreaming) << "/" << timerAverageMs(counters.stagePlayer)
        << "/" << timerAverageMs(counters.stageMeshInstall) << "/" << timerAverageMs(counters.stageMeshDispatch)
        << "/" << timerAverageMs(counters.stageLighting) << "/" << timerAverageMs(counters.stageSave)
        << "/" << timerAverageMs(counters.stageSimulation) << "/" << timerAverageMs(counters.stageRender)
        << " stream_sub_ms(plan/dispatch/prepass/pipeline/enqueue)="
        << timerAverageMs(counters.stageStreamPlan) << "/" << timerAverageMs(counters.stageStreamDispatch)
        << "/" << timerAverageMs(counters.stageStreamPrepass)
        << "/" << timerAverageMs(counters.stageStreamPipeline) << "/" << timerAverageMs(counters.stageStreamEnqueue)
        << " stage_max_ms(stream/player/mesh_i/mesh_d/light/save/sim/render)="
        << timerMaxMs(counters.stageStreaming) << "/" << timerMaxMs(counters.stagePlayer)
        << "/" << timerMaxMs(counters.stageMeshInstall) << "/" << timerMaxMs(counters.stageMeshDispatch)
        << "/" << timerMaxMs(counters.stageLighting) << "/" << timerMaxMs(counters.stageSave)
        << "/" << timerMaxMs(counters.stageSimulation) << "/" << timerMaxMs(counters.stageRender)
        << " stream_sub_max_ms(plan/dispatch/prepass/pipeline/enqueue)="
        << timerMaxMs(counters.stageStreamPlan) << "/" << timerMaxMs(counters.stageStreamDispatch)
        << "/" << timerMaxMs(counters.stageStreamPrepass)
        << "/" << timerMaxMs(counters.stageStreamPipeline) << "/" << timerMaxMs(counters.stageStreamEnqueue)
        << " resident=" << counters.residentChunks
        << " mesh_cache=" << counters.meshCacheEntries
        << " in_flight_gen=" << counters.inFlightGeneration
        << " in_flight_mesh=" << counters.inFlightMesh
        << " in_flight_light=" << counters.inFlightLighting
        << " workers=" << counters.workerCount;
    if (counters.duplicateGpuUploadSkips > 0) {
        out << " duplicate_upload_skips=" << counters.duplicateGpuUploadSkips;
    }
    Logger::info(out.str());
}

void Application::logSlowFrame(double frameMs, const core::RuntimeCounters& counters) const
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << "Slow frame"
        << ": frame=" << frameIndex_
        << " ms=" << frameMs
        << " stages(stream/player/mesh_i/mesh_d/light/save/sim/render)="
        << timerMaxMs(counters.stageStreaming) << "/" << timerMaxMs(counters.stagePlayer)
        << "/" << timerMaxMs(counters.stageMeshInstall) << "/" << timerMaxMs(counters.stageMeshDispatch)
        << "/" << timerMaxMs(counters.stageLighting) << "/" << timerMaxMs(counters.stageSave)
        << "/" << timerMaxMs(counters.stageSimulation) << "/" << timerMaxMs(counters.stageRender)
        << " stream_sub(plan/dispatch/prepass/pipeline/enqueue)="
        << timerMaxMs(counters.stageStreamPlan) << "/" << timerMaxMs(counters.stageStreamDispatch)
        << "/" << timerMaxMs(counters.stageStreamPrepass)
        << "/" << timerMaxMs(counters.stageStreamPipeline) << "/" << timerMaxMs(counters.stageStreamEnqueue)
        << " worker_timers(gen/light/mesh/snapshot/gpu/fence/queue)="
        << timerMaxMs(counters.terrainGeneration) << "/" << timerMaxMs(counters.lightingPropagation)
        << "/" << timerMaxMs(counters.meshBuild) << "/" << timerMaxMs(counters.meshSnapshot)
        << "/" << timerMaxMs(counters.gpuUpload) << "/" << timerMaxMs(counters.rendererFenceWait)
        << "/" << timerMaxMs(counters.queueWait)
        << " work(req/gen/mesh/light)=" << counters.chunkRequestsPlanned
        << "/" << counters.generationJobsSubmitted << "/" << counters.meshJobsSubmitted
        << "/" << counters.lightingJobsSubmitted
        << " backlog(jobs/gen/mesh/light)=" << counters.jobSystemPending
        << "/" << counters.pendingGenerationResults << "/" << counters.pendingMeshResults
        << "/" << counters.pendingLightingResults
        << " dirty_q(l/m)=" << counters.dirtyLightingQueueLength << "/" << counters.dirtyMeshQueueLength
        << " uploads=" << counters.gpuUploads
        << " upload_q=" << counters.uploadQueueLength
        << " upload_kb=" << (static_cast<double>(counters.stagingUploadBytes) / 1024.0)
        << " resident=" << counters.residentChunks
        << " mesh_cache=" << counters.meshCacheEntries
        << " in_flight(g/m/l)=" << counters.inFlightGeneration << "/" << counters.inFlightMesh
        << "/" << counters.inFlightLighting
        << " workers=" << counters.workerCount;
    Logger::warn(out.str());
}

void Application::shutdown()
{
    if (!initialized_) {
        return;
    }

    // L5: persist player + inventory + bump world descriptor's lastPlayedAtMs
    // *before* draining the chunk save coordinator, so even if the coordinator
    // hits an error we at least preserve the player's position.
    if (!config_.titleScreenMode) {
        const auto& worldRoot = config_.paths.saveRoot();
        save::PlayerStateSnapshot snapshot{};
        snapshot.position = player_.dPosition();
        snapshot.yawRadians = player_.yawRadians();
        snapshot.pitchRadians = player_.pitchRadians();
        snapshot.noclip = player_.noclip();
        if (!playerStateSaveService_.save(worldRoot, snapshot)) {
            Logger::warn("Failed to write player state under " + worldRoot.string());
        }
        if (!playerInventorySaveService_.save(worldRoot, playerInventory_, items_)) {
            Logger::warn("Failed to write player inventory under " + worldRoot.string());
        }
        // Update world.json with the current last-played time. We update the
        // descriptor in place so existing fields (seed, name, createdAtMs)
        // survive untouched.
        save::WorldRegistry registry(worldRoot.parent_path());
        auto descriptor = registry.readDescriptor(worldRoot);
        if (descriptor.has_value()) {
            descriptor->lastPlayedAtMs = save::WorldRegistry::nowUnixMs();
            (void)registry.writeDescriptor(worldRoot, *descriptor);
        }
    }

    drainOutstandingJobsForShutdown();
    auto saveStats = saveCoordinator_.flushPending(
        true,
        chunks_,
        worldSaveService_,
        jobs_,
        save::SaveCoordinatorSettings{
            config_.paths.saveRoot(),
            config_.workBudget.maxSavesPerFlush,
            config_.workBudget.saveFlushIntervalFrames},
        frameIndex_);
    jobs_.waitAll();
    saveStats.savesFlushed += saveCoordinator_.drainCompleted(true);
    saveStats.saveQueueLength = worldSaveService_.dirtyChunkCount(chunks_) + saveCoordinator_.pendingJobCount();
    if (saveStats.savesFlushed > 0) {
        runtimeStats_.recordFrame(0.0, saveStats);
    }
    auto shutdownCounters = runtimeStats_.totals();
    shutdownCounters.jobSystemPending = jobs_.pendingCount();
    shutdownCounters.workerCount = jobs_.workerCount();
    shutdownCounters.pendingGenerationResults = chunkJobMailbox_.pendingGenerationResults();
    shutdownCounters.pendingMeshResults = chunkJobMailbox_.pendingMeshResults() + pendingMeshResults_.size();
    shutdownCounters.pendingLightingResults = chunkJobMailbox_.pendingLightingResults();
    shutdownCounters.residentChunks = chunks_.residentCount();
    shutdownCounters.meshCacheEntries = meshCache_.size();
    shutdownCounters.inFlightGeneration = chunkJobMailbox_.inFlightGenerationCount();
    shutdownCounters.inFlightMesh = chunkJobMailbox_.inFlightMeshCount();
    shutdownCounters.inFlightLighting = chunkJobMailbox_.inFlightLightingCount();
    shutdownCounters.dirtyLightingQueueLength = lightingQueue_.size();
    shutdownCounters.dirtyMeshQueueLength = meshQueue_.size();
    logRuntimeStats("Shutdown runtime stats", shutdownCounters, runtimeStats_.averageFrameMs());
    Logger::info(
        "Shutdown summary:"
        " resident_chunks=" + std::to_string(chunks_.residentCount())
        + " mesh_cache=" + std::to_string(meshCache_.size())
        + " in_flight_gen=" + std::to_string(chunkJobMailbox_.inFlightGenerationCount())
        + " in_flight_mesh=" + std::to_string(chunkJobMailbox_.inFlightMeshCount())
        + " in_flight_light=" + std::to_string(chunkJobMailbox_.inFlightLightingCount())
        + " dirty_light_q=" + std::to_string(lightingQueue_.size())
        + " dirty_mesh_q=" + std::to_string(meshQueue_.size())
        + " workers=" + std::to_string(jobs_.workerCount())
        + " uploaded_mesh_mb=" + std::to_string(static_cast<double>(renderer_.totalUploadedMeshBytes()) / (1024.0 * 1024.0))
        + " save_format_version=" + std::to_string(save::RegionFileStore::kSaveFormatVersion)
        + " zstd=" + (save::RegionFileStore::zstdEnabled() ? "on" : "off"));

    Logger::info("Shutting down engine modules");
    // Drain any in-flight GPU LOD job before tearing down the system,
    // otherwise the fence wait inside shutdown() will block on a queue
    // that's also about to be destroyed.
    // Also drain the worker-thread merge if one is pending — calling
    // .get() blocks until the worker finishes; cheaper than letting
    // the JobSystem outlive the std::future (which would crash on
    // worker completion when the parent state is gone).
    if (pendingMergeJob_.has_value() && pendingMergeJob_->future.valid()) {
        pendingMergeJob_->future.wait();
    }
    pendingMergeJob_.reset();
    // LOD3 region merge — same lifetime discipline as the cluster merge.
    if (pendingRegionMergeJob_.has_value() && pendingRegionMergeJob_->future.valid()) {
        pendingRegionMergeJob_->future.wait();
    }
    pendingRegionMergeJob_.reset();
    // LOD3 region Stage A (worker reduce) — block on its future too so
    // the worker isn't holding a snapshot pointer when we tear down.
    if (pendingRegionPaddedJob_.has_value() && pendingRegionPaddedJob_->future.valid()) {
        pendingRegionPaddedJob_->future.wait();
    }
    pendingRegionPaddedJob_.reset();
    // pendingRegionGpuJob_ carries no future — its work lives in the
    // shared clusterGpuSystem_, which we reset below. Just clear it.
    pendingRegionGpuJob_.reset();
    clusterGpuSystem_.reset();
    pendingClusterJob_.reset();
    clusterRenderer_.reset();
    hybridMeshingGpuSystem_.reset();
    fluidGpuSystem_.reset();
    renderer_.shutdown();
    window_.reset();
    meshCache_.clear();
    jobs_.stop();
    initialized_ = false;
    runtimeStats_.reset();
    frameIndex_ = 0;
    leftMouseWasDown_ = false;
    rightMouseWasDown_ = false;
    automationDumpLatch_ = false;
    freecam_ = false;
    freecamToggleLatch_ = false;
    noclipToggleLatch_ = false;
    escapeToggleLatch_ = false;
    playerSpawnResolved_ = false;
    hasPlayerCursor_ = false;
    streamingRequestCacheValid_ = false;
    cachedStreamingRequestFrame_ = 0;
    streamingRequestCursor_ = 0;
    lastStreamingDispatchScanFrame_ = 0;
    streamingCenterChangedThisFrame_ = false;
    streamingDispatchIdle_ = false;
    lastSpawnResolveAttemptFrame_ = 0;
    selectedPlaceBlock_ = coreBlocks_.stone;
    cachedStreamingRequests_.clear();
    streamingDispatchRequests_.clear();
    pendingMeshResults_.clear();
    pendingHybridMeshJob_.reset();
}

} // namespace voxel
