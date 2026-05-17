#include <voxel/core/Application.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <voxel/core/Logger.hpp>
#include <voxel/data/RegistryLoader.hpp>
#include <voxel/render/MaterialTable.hpp>
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

} // namespace

Application::Application(ApplicationConfig config)
    : config_(std::move(config)),
      chunks_(),
      streamer_(chunks_),
      saveStore_(config_.paths.saveRoot()),
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

        if (config_.maxFrames > 0 && frame >= config_.maxFrames) {
            break;
        }
    }

    shutdown();
    return 0;
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
    seedCreativeInventory();
    blockRenderCatalog_ = blocks_.buildRenderCatalog();
    blockLightCatalog_ = blocks_.buildLightCatalog();
    kineticCatalog_ = blocks_.buildKineticCatalog();
    player_.setWalkSpeed(config_.playerWalkSpeed);
    player_.setFlySpeed(config_.playerFlySpeed);
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
    runtimeStats_.reset();
    initialized_ = true;
}

void Application::tick()
{
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

    // 1. Install any chunks generated by workers in previous ticks (drains mailbox).
    // 2. Plan + dispatch generation jobs for new requests.
    const auto& requests = streamingRequestsForFrame();
    const auto& dispatchRequests = streamingDispatchRequestsForFrame(requests);
    frameCounters.chunkRequestsPlanned = requests.size();
    frameCounters.terrainPrepassJobsSubmitted = dispatchTerrainPrepassJobs(requests);
    auto chunkPipelineSettings = config_.chunkPipeline;
    chunkPipelineSettings.installPriorityCenter = streamingCenter();
    const auto stats = chunkPipeline_.processRequestsAsync(
        chunks_, saveStore_, terrainGenerator_, jobs_, chunkJobMailbox_, dispatchRequests, chunkPipelineSettings);
    frameCounters.generationJobsSubmitted = stats.dispatched;
    frameCounters.generationJobsCompleted = stats.loaded;
    frameCounters.remeshesCausedByNeighborInstall = stats.neighborRemeshes;
    frameCounters.terrainGeneration = stats.generationTime;
    frameCounters.terrainGenerationFromPrepass = stats.generationFromPrepassTime;
    frameCounters.terrainGenerationDirect = stats.generationDirectTime;
    core::mergeTimer(frameCounters.queueWait, stats.queueWaitTime);
    frameCounters.load = stats.loadTime;
    enqueueInstalledChunkWork(stats);
    markStage(frameCounters.stageStreaming);

    if (window_ != nullptr) {
        const bool escapeDown = window_->keyDown(platform::Key::Escape);
        if (escapeDown && !escapeToggleLatch_) {
            if (inventoryOpen_) {
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
                renderer_.setDebugCameraPose(player_.eyePosition(), player_.yawRadians(), player_.pitchRadians());
            }
            Logger::info(std::string{"Camera mode: "} + (freecam_ ? "freecam" : "player"));
        }
        freecamToggleLatch_ = freecamDown;

        const bool inventoryDown = window_->keyDown(platform::Key::I);
        if (inventoryDown && !inventoryToggleLatch_) {
            inventoryOpen_ = !inventoryOpen_;
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

        const bool shouldAttemptSpawnResolve = !playerSpawnResolved_
            && (frameIndex_ < 5 || frameIndex_ - lastSpawnResolveAttemptFrame_ >= 10);
        if (shouldAttemptSpawnResolve) {
            lastSpawnResolveAttemptFrame_ = frameIndex_;
            playerSpawnResolved_ = tryResolvePlayerSpawn();
        }

        if (inventoryOpen_) {
            renderer_.setDebugCameraPose(player_.eyePosition(), player_.yawRadians(), player_.pitchRadians());
        } else if (freecam_) {
            renderer_.updateDebugCamera(*window_, 1.0F / 60.0F);
        } else if (!playerSpawnResolved_) {
            renderer_.setDebugCameraPose(player_.eyePosition(), player_.yawRadians(), player_.pitchRadians());
        } else {
            player_.tick(chunks_, gatherPlayerInput(), 1.0F / 60.0F);
            renderer_.setDebugCameraPose(player_.eyePosition(), player_.yawRadians(), player_.pitchRadians());
        }
        updateSelectedBlock();
        const auto interactionStats = (window_->cursorCaptured() && !inventoryOpen_) ? handleWorldInteraction() : core::RuntimeCounters{};
        frameCounters.blockEditsAccepted += interactionStats.blockEditsAccepted;
        frameCounters.blockEditsRejected += interactionStats.blockEditsRejected;
        frameCounters.dirtyMeshChunksQueued += interactionStats.dirtyMeshChunksQueued;
        frameCounters.dirtyMeshChunksCoalesced += interactionStats.dirtyMeshChunksCoalesced;
        frameCounters.dirtyLightingChunksQueued += interactionStats.dirtyLightingChunksQueued;
        frameCounters.dirtyLightingChunksCoalesced += interactionStats.dirtyLightingChunksCoalesced;
    }
    markStage(frameCounters.stagePlayer);

    // 3. Install mesh results from prior ticks, then dispatch new mesh jobs
    //    before lighting so nearby terrain becomes visible with fallback
    //    lighting instead of waiting behind expensive light propagation.
    const auto meshInstallStats = installMeshResults();
    markStage(frameCounters.stageMeshInstall);
    const auto meshDispatchStats = dispatchMeshJobs();
    markStage(frameCounters.stageMeshDispatch);
    const auto lightingStats = propagateLightingForDirtyChunks();
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
    frameCounters.dirtyQueueScanTimeUs = lightingStats.dirtyQueueScanTimeUs + meshDispatchStats.dirtyQueueScanTimeUs;
    frameCounters.meshBuild = meshInstallStats.meshBuildTime;
    core::mergeTimer(frameCounters.queueWait, meshInstallStats.queueWaitTime);
    frameCounters.uploadBudgetDeferrals = meshInstallStats.uploadBudgetDeferrals;
    if (lightingStats.recomputed >= maxLightPropagationsPerTick_) {
        frameCounters.lightingBudgetSaturated = 1;
    }
    if (meshInstallStats.uploadBudgetDeferrals > 0) {
        frameCounters.meshInstallBudgetSaturated = 1;
    }

    const bool saveInterval = frameIndex_ > 0 && config_.workBudget.saveFlushIntervalFrames > 0
        && (frameIndex_ % config_.workBudget.saveFlushIntervalFrames) == 0;
    const auto saveStats = flushPendingSaves(saveInterval);
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
        const auto beStats = blockEntityScheduler_.tick(beContext, chunks_, blocks_, config_.workBudget.maxLightingMsPerFrame);
        (void)beStats;
    }
    updateAutomationDebug();
    updateVisualOverlay();
    markStage(frameCounters.stageSimulation);

    renderer_.beginFrame();
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
    const auto pos = player_.position();
    return {
        world::floorDiv(static_cast<std::int64_t>(std::floor(pos.x)), world::ChunkSize),
        world::floorDiv(static_cast<std::int64_t>(std::floor(pos.y)), world::ChunkSize),
        world::floorDiv(static_cast<std::int64_t>(std::floor(pos.z)), world::ChunkSize)
    };
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

core::RuntimeCounters Application::flushPendingSaves(bool force)
{
    core::RuntimeCounters stats{};
    stats.savesFlushed += drainCompletedSaveJobs(false);

    const auto dirty = worldSaveService_.dirtyChunkCount(chunks_);
    stats.saveQueueLength = dirty + asyncSaveJobs_.size();
    const bool intervalDue = frameIndex_ > 0 && config_.workBudget.saveFlushIntervalFrames > 0
        && (frameIndex_ % config_.workBudget.saveFlushIntervalFrames) == 0;
    if (dirty == 0 || (!force && !intervalDue)) {
        return stats;
    }

    const auto saveStart = std::chrono::steady_clock::now();
    const auto maxSnapshots = config_.workBudget.maxSavesPerFlush;
    std::vector<world::Chunk> snapshots;
    snapshots.reserve(std::min(dirty, maxSnapshots));
    chunks_.forEach([&snapshots, maxSnapshots](world::Chunk& chunk) {
        if (snapshots.size() >= maxSnapshots || !chunk.dirty().save) {
            return;
        }
        snapshots.push_back(chunk);
        chunk.clearSaveDirty();
    });
    core::recordTimer(stats.save, elapsedUs(saveStart, std::chrono::steady_clock::now()));

    const auto scheduled = snapshots.size();
    if (scheduled > 0) {
        const auto saveRoot = config_.paths.saveRoot();
        asyncSaveJobs_.push_back({jobs_.submit({"chunk.save", core::JobPriority::Low},
            [saveRoot, snapshots = std::move(snapshots)]() mutable {
                save::RegionFileStore store(saveRoot);
                std::size_t saved = 0;
                for (const auto& snapshot : snapshots) {
                    store.saveChunk(snapshot);
                    ++saved;
                }
                return saved;
            })});
    }

    stats.saveQueueLength = worldSaveService_.dirtyChunkCount(chunks_) + asyncSaveJobs_.size();
    if (stats.saveQueueLength > 0) {
        stats.saveBudgetSaturated = 1;
    }
    if (scheduled > 0 || stats.savesFlushed > 0) {
        Logger::info(
            "Save flush: scheduled_chunks=" + std::to_string(scheduled)
            + " completed_chunks=" + std::to_string(stats.savesFlushed)
            + " remaining=" + std::to_string(stats.saveQueueLength)
            + (force ? " forced" : ""));
    }
    return stats;
}

std::size_t Application::drainCompletedSaveJobs(bool wait)
{
    std::size_t completed = 0;
    auto it = asyncSaveJobs_.begin();
    while (it != asyncSaveJobs_.end()) {
        if (wait) {
            it->future.wait();
        } else if (it->future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            ++it;
            continue;
        }
        completed += it->future.get();
        it = asyncSaveJobs_.erase(it);
    }
    return completed;
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
          << (window_->cursorCaptured() ? " mouse-locked" : " mouse-free")
          << " | [" << (hotbar_.selectedSlot() == 9 ? 0 : hotbar_.selectedSlot() + 1) << "] " << slot.name
          << " | " << std::fixed << std::setprecision(1) << runtimeStats_.lastFrameMs() << " ms"
          << " | chunks " << chunks_.residentCount()
          << " mesh " << meshCache_.size();
    window_->setTitle(title.str());
}

void Application::updateVisualOverlay()
{
    if (window_ == nullptr) {
        renderer_.setUiOverlay({});
        return;
    }

    std::vector<render::VulkanRenderer::UiRect> rects;
    rects.reserve(inventoryOpen_ ? 180 : 64);

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

    const auto extent = window_->framebufferExtent();
    const float windowWidth = static_cast<float>(std::max<std::uint32_t>(extent.width, 1U));
    const float windowHeight = static_cast<float>(std::max<std::uint32_t>(extent.height, 1U));

    // Crosshair.
    addRect(windowWidth * 0.5F - 8.0F, windowHeight * 0.5F - 1.0F, 16.0F, 2.0F, 0.94F, 0.90F, 0.70F, 0.80F);
    addRect(windowWidth * 0.5F - 1.0F, windowHeight * 0.5F - 8.0F, 2.0F, 16.0F, 0.94F, 0.90F, 0.70F, 0.80F);

    constexpr float slotSize = 42.0F;
    constexpr float gap = 5.0F;
    constexpr float hotbarSlots = 12.0F;
    const float hotbarWidth = hotbarSlots * slotSize + (hotbarSlots - 1.0F) * gap;
    const float hotbarX = (windowWidth - hotbarWidth) * 0.5F;
    const float hotbarY = windowHeight - 58.0F;

    addRect(hotbarX - 10.0F, hotbarY - 10.0F, hotbarWidth + 20.0F, slotSize + 20.0F, 0.02F, 0.025F, 0.028F, 0.72F);
    for (std::size_t i = 0; i < 12; ++i) {
        const float x = hotbarX + static_cast<float>(i) * (slotSize + gap);
        const bool selected = i == hotbar_.selectedSlot();
        addRect(x, hotbarY, slotSize, slotSize, 0.08F, 0.095F, 0.10F, 0.90F);
        addBorder(x, hotbarY, slotSize, slotSize, selected ? 3.0F : 1.0F,
            selected ? 1.0F : 0.22F, selected ? 0.82F : 0.24F, selected ? 0.20F : 0.25F, selected ? 1.0F : 0.70F);
        const auto& hotbarStack = playerInventory_.hotbarInventory().slot(i);
        if (!hotbarStack.empty()) {
            const auto c = itemColor(hotbarStack, i);
            addRect(x + 9.0F, hotbarY + 9.0F, slotSize - 18.0F, slotSize - 18.0F, c[0], c[1], c[2], 0.95F);
            addRect(x + 8.0F, hotbarY + slotSize - 8.0F, (slotSize - 16.0F) * stackFraction(hotbarStack), 3.0F,
                0.95F, 0.80F, 0.28F, 0.85F);
        } else if (i < player::CreativeHotbar::SlotCount && hotbar_.validSlot(i)) {
            const auto c = blockColor(i);
            addRect(x + 11.0F, hotbarY + 11.0F, slotSize - 22.0F, slotSize - 22.0F, c[0], c[1], c[2], 0.45F);
        }
        if (i < 10) {
            addMiniDigit(x + 4.0F, hotbarY + 4.0F, i == 9 ? 0U : i + 1U, 0.88F, 0.84F, 0.66F, 0.72F);
        } else {
            addRect(x + 5.0F, hotbarY + 11.0F, 10.0F, 2.0F, 0.88F, 0.84F, 0.66F, 0.72F);
            if (i == 11) {
                addRect(x + 9.0F, hotbarY + 7.0F, 2.0F, 10.0F, 0.88F, 0.84F, 0.66F, 0.72F);
            }
        }
    }

    if (inventoryOpen_) {
        const float panelX = 188.0F;
        const float panelY = 72.0F;
        const float panelW = 904.0F;
        const float panelH = 548.0F;
        addRect(panelX, panelY, panelW, panelH, 0.025F, 0.030F, 0.034F, 0.92F);
        addBorder(panelX, panelY, panelW, panelH, 2.0F, 0.45F, 0.36F, 0.20F, 0.95F);

        const float equipmentX = panelX + 30.0F;
        const float equipmentY = panelY + 34.0F;
        for (std::size_t i = 0; i < inventory::PlayerInventory::kEquipmentSlots; ++i) {
            const float y = equipmentY + static_cast<float>(i % 4) * 50.0F;
            const float x = equipmentX + static_cast<float>(i / 4) * 54.0F;
            addRect(x, y, 42.0F, 42.0F, 0.065F, 0.075F, 0.080F, 0.94F);
            addBorder(x, y, 42.0F, 42.0F, 1.0F, 0.25F, 0.24F, 0.22F, 0.85F);
            const auto& stack = playerInventory_.equipmentInventory().slot(i);
            if (!stack.empty()) {
                const auto c = itemColor(stack, i);
                addRect(x + 9.0F, y + 9.0F, 24.0F, 24.0F, c[0], c[1], c[2], 0.92F);
            }
        }

        const float accessoryX = equipmentX;
        const float accessoryY = equipmentY + 238.0F;
        for (std::size_t i = 0; i < inventory::PlayerInventory::kAccessorySlots; ++i) {
            const float x = accessoryX + static_cast<float>(i % 2) * 54.0F;
            const float y = accessoryY + static_cast<float>(i / 2) * 50.0F;
            addRect(x, y, 42.0F, 42.0F, 0.055F, 0.048F, 0.070F, 0.94F);
            addBorder(x, y, 42.0F, 42.0F, 1.0F, 0.25F, 0.20F, 0.36F, 0.85F);
            const auto& stack = playerInventory_.accessoryInventory().slot(i);
            if (!stack.empty()) {
                const auto c = itemColor(stack, i);
                addRect(x + 9.0F, y + 9.0F, 24.0F, 24.0F, c[0], c[1], c[2], 0.92F);
            }
        }

        const float gridX = panelX + 184.0F;
        const float gridY = panelY + 34.0F;
        constexpr float invSlot = 34.0F;
        constexpr float invGap = 4.0F;
        for (std::size_t row = 0; row < inventory::PlayerInventory::kBackpackRows; ++row) {
            for (std::size_t col = 0; col < inventory::PlayerInventory::kBackpackColumns; ++col) {
                const float x = gridX + static_cast<float>(col) * (invSlot + invGap);
                const float y = gridY + static_cast<float>(row) * (invSlot + invGap);
                addRect(x, y, invSlot, invSlot, 0.060F, 0.068F, 0.072F, 0.94F);
                addBorder(x, y, invSlot, invSlot, 1.0F, 0.17F, 0.18F, 0.17F, 0.80F);
                const auto slotIndex = row * inventory::PlayerInventory::kBackpackColumns + col;
                const auto& stack = playerInventory_.mainInventory().slot(slotIndex);
                if (!stack.empty()) {
                    const auto c = itemColor(stack, slotIndex);
                    addRect(x + 7.0F, y + 7.0F, invSlot - 14.0F, invSlot - 14.0F, c[0], c[1], c[2], 0.92F);
                    const auto fraction = stackFraction(stack);
                    addRect(x + 5.0F, y + invSlot - 7.0F, (invSlot - 10.0F) * fraction, 3.0F, 0.95F, 0.80F, 0.28F, 0.85F);
                }
            }
        }
    }

    renderer_.setUiOverlay(std::move(rects));
}

void Application::evictFarMeshCache()
{
    const auto center = streamingCenter();
    const int h = config_.streaming.renderDistanceChunks + 2;
    const int v = config_.streaming.verticalRenderDistanceChunks + 1;
    const auto removed = meshCache_.removeOutsideRadius(center, h, v);
    for (const auto coord : removed) {
        renderer_.removeUploadedMesh(coord);
    }
    if (!removed.empty()) {
        Logger::info("Mesh cache eviction: removed=" + std::to_string(removed.size()));
    }
}

void Application::drainOutstandingJobsForShutdown()
{
    jobs_.waitAll();
    (void)drainCompletedSaveJobs(true);
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

const std::vector<world::ChunkRequest>& Application::streamingRequestsForFrame()
{
    const auto center = streamingCenter();
    const int forwardBucket = streamingForwardBucket(player_.forwardVector());
    const bool settingsChanged = cachedStreamingSettings_.renderDistanceChunks != config_.streaming.renderDistanceChunks
        || cachedStreamingSettings_.verticalRenderDistanceChunks != config_.streaming.verticalRenderDistanceChunks
        || cachedStreamingSettings_.simulationDistanceChunks != config_.streaming.simulationDistanceChunks
        || cachedStreamingSettings_.physicsDistanceChunks != config_.streaming.physicsDistanceChunks;
    const bool centerChanged = !streamingRequestCacheValid_ || !(cachedStreamingCenter_ == center);
    const bool forwardChanged = cachedStreamingForwardBucket_ != forwardBucket;

    // The full request set is radius-sized and sorted. Rebuilding it every
    // frame was visible in Debug builds, especially while generation was
    // saturated and no new work could be dispatched. Refresh immediately for
    // chunk/settings changes; direction changes are quantized and throttled
    // because they only provide a modest tie-breaker.
    const bool directionRefreshAllowed = frameIndex_ - cachedStreamingRequestFrame_ >= 15;
    if (!streamingRequestCacheValid_ || centerChanged || settingsChanged || (forwardChanged && directionRefreshAllowed)) {
        cachedStreamingRequests_ = streamer_.planRequests(center, config_.streaming, player_.forwardVector());
        cachedStreamingCenter_ = center;
        cachedStreamingSettings_ = config_.streaming;
        cachedStreamingForwardBucket_ = forwardBucket;
        cachedStreamingRequestFrame_ = frameIndex_;
        streamingRequestCursor_ = 0;
        streamingRequestCacheValid_ = true;
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

    const std::size_t targetCandidates = std::max<std::size_t>(config_.chunkPipeline.maxLoadsOrGenerationsPerTick * 2U, 8U);
    const std::size_t maxScans = std::min<std::size_t>(
        requests.size(),
        std::max<std::size_t>(targetCandidates * 16U, 128U));
    if (streamingRequestCursor_ >= requests.size()) {
        streamingRequestCursor_ = 0;
    }

    std::size_t scanned = 0;
    while (scanned < maxScans && streamingDispatchRequests_.size() < targetCandidates) {
        const std::size_t index = (streamingRequestCursor_ + scanned) % requests.size();
        const auto& request = requests[index];
        ++scanned;

        if (chunkJobMailbox_.isGenerationInFlight(request.coord)) {
            continue;
        }
        if (const auto* existing = chunks_.find(request.coord)) {
            const auto state = existing->state();
            if (state == world::ChunkState::Resident
                || state == world::ChunkState::Meshing
                || state == world::ChunkState::MeshReady
                || state == world::ChunkState::Generating) {
                continue;
            }
        }
        streamingDispatchRequests_.push_back(request);
    }

    streamingRequestCursor_ = (streamingRequestCursor_ + scanned) % requests.size();
    if (streamingDispatchRequests_.empty() && streamingRequestCursor_ != 0) {
        streamingRequestCursor_ = 0;
    }
    return streamingDispatchRequests_;
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
    constexpr std::size_t kMaxPrepassJobsPerTick = 16;
    if (chunkJobMailbox_.inFlightGenerationCount() >= config_.chunkPipeline.maxLoadsOrGenerationsPerTick
        || chunkJobMailbox_.inFlightMeshCount() >= config_.workBudget.maxMeshJobsPerTick
        || chunkJobMailbox_.pendingGenerationResults() > 0
        || chunkJobMailbox_.pendingMeshResults() > 0) {
        return 0;
    }

    std::size_t submitted = 0;
    std::unordered_set<world::TerrainColumnCoord, world::TerrainColumnCoordHash> seen;

    for (const auto& request : requests) {
        if (submitted >= kMaxPrepassJobsPerTick) {
            break;
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

void Application::enqueueMeshIfNeeded(world::ChunkCoord coord)
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
    meshQueue_.enqueue(coord, chunk->revision(), chunk->meshRevision());
}

void Application::enqueueInstalledChunkWork(const world::ChunkPipelineStats& stats)
{
    for (const auto coord : stats.installedChunks) {
        enqueueLightingIfNeeded(coord);
        enqueueMeshIfNeeded(coord);
    }
    for (const auto coord : stats.neighborDirtyChunks) {
        enqueueLightingIfNeeded(coord);
        enqueueMeshIfNeeded(coord);
    }
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

Application::MeshDispatchStats Application::dispatchMeshJobs()
{
    MeshDispatchStats stats{};
        const auto maxMeshJobs = config_.workBudget.maxMeshJobsPerTick > 0
            ? config_.workBudget.maxMeshJobsPerTick
            : config_.maxChunkMeshesPerTick;
        const auto frameStart = std::chrono::steady_clock::now();
        const auto maxInFlightMesh = std::max<std::size_t>(maxMeshJobs, 1U) * 2U;
        if (chunkJobMailbox_.inFlightMeshCount() >= maxInFlightMesh) {
            return stats;
        }
        const auto candidates = meshQueue_.popClosest(streamingCenter(), maxMeshJobs);
        constexpr std::size_t kMinimumMeshSubmissionsPerTick = 2;
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
            const auto revision = chunk->revision();
            const auto neighborRevisionHash = meshNeighborRevisionHash(coord);
            const world::MeshJobKey meshKey{coord, revision, neighborRevisionHash};
            if (!chunkJobMailbox_.tryBeginMesh(meshKey)) {
                continue;
            }

            const auto snapshotStart = std::chrono::steady_clock::now();
            struct MeshSnapshotBundle {
                world::Chunk target;
                std::array<std::unique_ptr<world::Chunk>, 6> neighbours{};
            };
            auto bundle = std::make_shared<MeshSnapshotBundle>();
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
                    bundle->neighbours[i]->clearLightData();
                }
            }
            core::recordTimer(stats.snapshotTime, elapsedUs(snapshotStart, std::chrono::steady_clock::now()));

            const auto* renderCatalog = &blockRenderCatalog_;
            auto* mailbox = &chunkJobMailbox_;
            const auto enqueueTime = std::chrono::steady_clock::now();
            jobs_.submit({"chunk.mesh", core::JobPriority::Critical},
                [bundle, coord, revision, neighborRevisionHash, mailbox, renderCatalog, enqueueTime]() mutable {
                    const auto start = std::chrono::steady_clock::now();
                    render::meshing::ChunkNeighborhood neighborhood{};
                    neighborhood.negX = bundle->neighbours[0].get();
                    neighborhood.posX = bundle->neighbours[1].get();
                    neighborhood.negY = bundle->neighbours[2].get();
                    neighborhood.posY = bundle->neighbours[3].get();
                    neighborhood.negZ = bundle->neighbours[4].get();
                    neighborhood.posZ = bundle->neighbours[5].get();

                    render::meshing::GreedyMesher mesher;
                    auto mesh = mesher.build(bundle->target, *renderCatalog,
                                              bundle->target.lightData(), neighborhood);
                    const auto end = std::chrono::steady_clock::now();
                    mesh.sourceMeshRevisionHash = neighborRevisionHash;
                    world::ChunkMeshResult result{};
                    result.coord = coord;
                    result.sourceRevision = revision;
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
    for (auto& result : results) {
        core::recordTimer(stats.meshBuildTime, result.buildTimeUs);
        core::recordTimer(stats.queueWaitTime, result.queueWaitUs);

        if (installedThisTick >= maxInstalls) {
            pendingMeshResults_.push_back(std::move(result));
            ++stats.uploadBudgetDeferrals;
            continue;
        }
        const auto elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
        if (elapsedMs >= config_.workBudget.maxMeshInstallMsPerFrame) {
            pendingMeshResults_.push_back(std::move(result));
            ++stats.uploadBudgetDeferrals;
            continue;
        }

        auto* chunk = chunks_.find(result.coord);
        if (chunk == nullptr) {
            ++stats.staleDiscarded;
            continue; // Chunk evicted while job was in flight.
        }
        if (chunk->revision() != result.sourceRevision || meshNeighborRevisionHash(result.coord) != result.neighborRevisionHash) {
            ++stats.staleDiscarded;
            enqueueMeshIfNeeded(result.coord);
            continue; // Stale: chunk was edited after snapshot. Will redispatch next tick.
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
            pendingMeshResults_.push_back(std::move(result));
            ++stats.uploadBudgetDeferrals;
            continue;
        }
        const auto uploadSubmitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - uploadSubmitStart).count();
        if (uploadedBytesThisTick > 0 && uploadSubmitMs >= config_.workBudget.maxUploadSubmitMsPerFrame) {
            pendingMeshResults_.push_back(std::move(result));
            ++stats.uploadBudgetDeferrals;
            continue;
        }

        if (!renderer_.uploadChunkMesh(result.coord, result.mesh)) {
            pendingMeshResults_.push_back(std::move(result));
            ++stats.uploadBudgetDeferrals;
            continue;
        }
        uploadedBytesThisTick += bytes;
        stats.uploadedBytes += bytes;
        // GPU has the authoritative copy; drop the CPU-side vertex/index data
        // to halve per-chunk memory usage at scale.
        { std::vector<render::meshing::VoxelVertex>().swap(result.mesh.vertices); }
        { std::vector<std::uint32_t>().swap(result.mesh.indices); }
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
        << " stage_max_ms(stream/player/mesh_i/mesh_d/light/save/sim/render)="
        << timerMaxMs(counters.stageStreaming) << "/" << timerMaxMs(counters.stagePlayer)
        << "/" << timerMaxMs(counters.stageMeshInstall) << "/" << timerMaxMs(counters.stageMeshDispatch)
        << "/" << timerMaxMs(counters.stageLighting) << "/" << timerMaxMs(counters.stageSave)
        << "/" << timerMaxMs(counters.stageSimulation) << "/" << timerMaxMs(counters.stageRender)
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

    drainOutstandingJobsForShutdown();
    const auto saveStats = flushPendingSaves(true);
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
    lastSpawnResolveAttemptFrame_ = 0;
    selectedPlaceBlock_ = world::makeBlockState(BlockTypeId{2});
    cachedStreamingRequests_.clear();
    streamingDispatchRequests_.clear();
    pendingMeshResults_.clear();
}

} // namespace voxel
