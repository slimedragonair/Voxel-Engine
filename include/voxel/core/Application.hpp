#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <voxel/automation/KineticNetwork.hpp>
#include <voxel/automation/NetworkGraph.hpp>
#include <voxel/core/JobSystem.hpp>
#include <voxel/core/Paths.hpp>
#include <voxel/core/RuntimeStats.hpp>
#include <voxel/data/BlockRegistry.hpp>
#include <voxel/data/CoreContentIds.hpp>
#include <voxel/data/ItemRegistry.hpp>
#include <voxel/data/RecipeRegistry.hpp>
#include <voxel/ecs/Components.hpp>
#include <voxel/ecs/SimulationSystem.hpp>
#include <voxel/inventory/Inventory.hpp>
#include <voxel/magic/MagicSystem.hpp>
#include <voxel/network/NetworkSystem.hpp>
#include <voxel/physics/PhysicsSystem.hpp>
#include <voxel/player/CreativeHotbar.hpp>
#include <voxel/player/PlayerController.hpp>
#include <voxel/player/PlayerSpawnResolver.hpp>
#include <voxel/platform/GlfwWindow.hpp>
#include <voxel/render/DebugOverlay.hpp>
#include <voxel/render/ClusterRenderer.hpp>
#include <voxel/render/VulkanRenderer.hpp>
#include <voxel/render/meshing/ChunkMeshCache.hpp>
#include <voxel/render/meshing/GreedyMesher.hpp>
#include <voxel/render/meshing/ClusterGpuMeshing.hpp>
#include <voxel/render/meshing/ClusterMeshDiskCache.hpp>
#include <voxel/render/meshing/HybridMeshingGpuSystem.hpp>
#include <voxel/save/PlayerInventorySaveService.hpp>
#include <voxel/save/PlayerStateSaveService.hpp>
#include <voxel/save/RegionFileStore.hpp>
#include <voxel/save/SaveCoordinator.hpp>
#include <voxel/save/WorldRegistry.hpp>
#include <voxel/save/WorldSaveService.hpp>
#include <voxel/world/BlockEditor.hpp>
#include <voxel/world/BlockCollisionCatalog.hpp>
#include <voxel/world/BlockEditQueue.hpp>
#include <voxel/world/BlockEntityBehavior.hpp>
#include <voxel/world/CoreBehaviors.hpp>
#include <voxel/world/ChunkJobMailbox.hpp>
#include <voxel/world/ChunkDirtyQueue.hpp>
#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/ChunkPipeline.hpp>
#include <voxel/world/ChunkStreamer.hpp>
#include <voxel/world/FlatTerrainGenerator.hpp>
#include <voxel/world/FluidGpuSystem.hpp>
#include <voxel/world/FluidSystem.hpp>
#include <voxel/world/Lod.hpp>
#include <voxel/world/LightPropagator.hpp>
#include <voxel/world/NoiseTerrainGenerator.hpp>
#include <voxel/world/Raycast.hpp>
#include <voxel/world/SpaceEnvironment.hpp>
#include <voxel/world/TerrainDefinitionRegistry.hpp>

namespace voxel {

struct SlotHitRect {
    float x, y, w, h;
    enum class Region : std::uint8_t { Hotbar, Backpack, Equipment, Accessory };
    Region region;
    std::size_t index;
};

struct ApplicationConfig {
    std::string name{"AetherForge: Infinite Creation"};
    int width{1280};
    int height{720};
    std::size_t maxFrames{0};
    core::Paths paths{};
    // L3: explicit world seed. 0 means "let the terrain generator use its
    // built-in default". The sandbox menu fills this in from the chosen
    // WorldDescriptor so every save reloads with the seed it was created on.
    std::uint64_t worldSeed{0};
    // L3: optional human-readable name (matches WorldDescriptor::name).
    // Logged + shown in the window title so the player can see which world
    // they loaded into.
    std::string worldDisplayName{};
    // N1: when true, Application boots into a title-screen-only mode — no
    // world is generated, streamed, lit, or simulated. `tick()` short-
    // circuits into the title-screen ImGui path. Used by the sandbox
    // launcher on first run so the player picks a world graphically before
    // any world resources are committed.
    bool titleScreenMode{false};
    world::StreamingSettings streaming{};
    world::ChunkPipelineSettings chunkPipeline{};
    std::size_t maxChunkMeshesPerTick{32};
    std::size_t workerCount{0};
    float mouseSensitivity{0.0022F};
    float playerWalkSpeed{4.3F};
    float playerFlySpeed{10.0F};
    std::optional<core::Vec3> debugStartPosition{};
    bool debugStartNoclip{false};
    bool enableGpuCulling{true};
    bool compareGpuCulling{false};
    bool useGpuShaderLighting{true};
    // EXPERIMENTAL (Phase 0/1 scaffolding only). When true, the engine will
    // construct a `FluidGpuSystem` alongside the CPU `FluidSystem`. The GPU
    // system is currently a no-op stub — leave this `false` until the
    // compute pipeline is implemented per `docs/gpu_fluid_sim_design.md`.
    bool useGpuFluidSim{false};
    // GPU pre-classifies visible chunk faces, CPU performs the final greedy
    // merge. Can be disabled from the sandbox with --cpu-meshing.
    bool useGpuHybridMeshing{true};
    // EXPERIMENTAL: GPU compute terrain generation. Default false until the
    // generated block output matches the CPU terrain pipeline for target seeds.
    bool useGpuTerrainGeneration{false};
    // LOD2 cluster rendering (Phase 1C foundation; Phase 1C-3/4 wires the
    // pipeline + indirect draw, Phase 1D wires the streamer). When true,
    // ClusterRenderer is constructed alongside the regular chunk pipeline
    // and supports concurrent chunk + cluster rendering for the
    // persistence-during-transition invariant described in
    // `docs/lod_persistence_policy.md`.
    bool useClusterLod{false};
    world::LodSettings lod{};
    // R0-LOD plan: separate knob controlling how far LOD3 (region)
    // builds extend beyond LOD2. Higher = more far terrain visible
    // and more disk/GPU memory. Defaults to 4 (a balance between
    // visible horizon and build cost); high-end PCs can crank to
    // 8-16+, low-end can drop to 1-2. Used by computeLodBands() in
    // Lod.hpp to derive the LOD3 outer band edge.
    std::int64_t farTerrainQualityMultiplier{4};
    bool enableSpacePhaseA{true};
    world::SpaceSettings space{};
    float normalCameraFarPlane{1024.0F};
    float spaceCameraFarPlane{81920.0F};
    render::VulkanRenderer::AtmosphereSettings atmosphere{};
    render::VulkanRenderer::SkySettings sky{};
    struct ChunkWorkBudget {
        std::size_t maxLightingPerTick{6};
        std::size_t maxMeshJobsPerTick{32};
        // Profile feedback: throttling 48 → 24 did NOT reduce mesh_install_ms
        // p99 (still ~10 ms), but it grew dirty_mesh_q p99 from 179 → 432.
        // The per-install cost is fixed by allocator + memcpy in Debug; the
        // tick budget bounds wall-clock time but not per-iteration cost.
        // Reverting to 32 (mid-point) keeps drain rate sane.
        std::size_t maxMeshInstallsPerTick{32};
        // Per-tick upload budget for chunk + cluster mesh staging. 1 GB
        // is generous — typical worst-case cold-start spend is ~50-100 MB
        // per tick. The cap exists to bound the staging-arena pressure
        // when many large meshes finish at once, not to throttle normal
        // operation. Raised from 64 MB so the staging ring doesn't have
        // to spread chunk+cluster uploads across multiple ticks.
        std::size_t maxGpuUploadBytesPerTick{1024U * 1024U * 1024U};
        std::size_t maxSavesPerFlush{8};
        std::size_t saveFlushIntervalFrames{120};
        double maxLightingMsPerFrame{1.5};
        // Kept tight (3 ms) so the per-iteration time gate still bails out
        // before a wave runs away. Combined with the 32-item cap, this caps
        // worst-case install-stage time at ~5 ms.
        double maxMeshInstallMsPerFrame{3.0};
        double maxUploadSubmitMsPerFrame{3.0};
        double maxDirtyScanMsPerFrame{2.0};
        double maxStreamDispatchMsPerFrame{1.25};
    } workBudget{};
    double slowFrameLogThresholdMs{50.0};
};

class Application {
public:
    explicit Application(ApplicationConfig config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // M2: run() return codes. `kReturnToTitle` lets the launcher loop back
    // to the main-menu picker without exiting the process — used by the
    // World Manager's "Save & Quit to Title" / "Switch to <world>" buttons.
    static constexpr int kExitNormal = 0;
    static constexpr int kReturnToTitle = 2;

    int run();

    // After run() returns kReturnToTitle, this may carry a direct switch
    // target so the launcher can bypass the menu and re-enter immediately.
    [[nodiscard]] const std::optional<save::WorldEntry>& nextWorldRequest() const noexcept
    {
        return nextWorldRequest_;
    }

private:
    struct MeshInstallStats {
        std::size_t completed{};
        std::size_t uploaded{};
        std::size_t staleDiscarded{};
        std::size_t uploadBudgetDeferrals{};
        std::uint64_t uploadedBytes{};
        core::RuntimeCounters::Timer meshBuildTime{};
        core::RuntimeCounters::Timer queueWaitTime{};
    };

    struct MeshDispatchStats {
        std::size_t submitted{};
        std::uint64_t dirtyQueueScanTimeUs{};
        core::RuntimeCounters::Timer snapshotTime{};
    };

    struct LightingStats {
        std::size_t recomputed{};
        std::size_t submitted{};
        std::size_t completed{};
        std::size_t staleDiscarded{};
        std::uint64_t dirtyQueueScanTimeUs{};
        core::RuntimeCounters::Timer propagationTime{};
        core::RuntimeCounters::Timer queueWaitTime{};
    };

    struct PendingHybridMeshJob {
        std::optional<world::Chunk> target;
        world::ChunkCoord coord{};
        Revision sourceRevision{};
        Revision sourceMeshRevision{};
        std::uint64_t neighborRevisionHash{};
        std::chrono::steady_clock::time_point enqueueTime{};
        std::chrono::steady_clock::time_point submitTime{};
    };

    void initialize();
    void tick();
    [[nodiscard]] world::ChunkCoord streamingCenter() const noexcept;
    // R0-LOD plan: returns true when `coord` is inside the LOD0
    // ACTIVE-SIM band — i.e., within `simulationDistanceChunks` of
    // the streaming center on each horizontal axis. Sim systems
    // (block entities, mobs, fluids) gate full-rate work on this.
    // Chunks outside this band but inside the LOD0 render range are
    // LOD1: they still draw and collide, but their sim is throttled
    // or skipped. The simulationDistance setting can be lower than
    // renderDistance, which is exactly what creates the LOD1 ring.
    [[nodiscard]] bool isChunkInActiveSim(world::ChunkCoord coord) const noexcept;
    // Phase 1D-1: per-tick LOD2 maintenance. Walks the cluster ring around
    // the player, picks one unbuild cluster whose 64 chunks are all
    // Resident, builds it on the main thread, and uploads to
    // clusterRenderer_. Throttled to one build per tick to keep the main
    // thread responsive — Phase 1D-2 moves the build off-thread. No-op
    // when useClusterLod is false.
    void tickClusterMaintenance();
    // Phase 1D-3: LOD3 region build/eviction. Runs after the cluster
    // maintenance so region builds see fresh cluster coverage for the
    // skip-region-when-fully-clustered policy. Same shape as
    // tickClusterMaintenance just at the region scale.
    void tickRegionMaintenance();
    [[nodiscard]] player::PlayerInput gatherPlayerInput();
    void updateSelectedBlock();
    void updateSpellCasting();
    void drawRuntimeSettingsOverlay();
    // M2: the graphical World Manager. Drawn during the ImGui frame between
    // beginImGuiFrame() and endImGuiFrame() in tick(); only renders when
    // `worldManagerOpen_` is true (F9 toggles it).
    void drawWorldManagerOverlay();
    void resizeWorkerPool(std::size_t workerCount);
    void seedCreativeInventory();
    [[nodiscard]] core::RuntimeCounters handleWorldInteraction();
    void invalidateLodForEditedChunk(world::ChunkCoord coord);
    void handleInventoryInteraction();
    bool tryResolvePlayerSpawn();
    void updateSpaceEnvironment(core::DVec3 cameraPos);
    void updateWindowTitle();
    void updateVisualOverlay();
    void evictFarMeshCache();
    void drainOutstandingJobsForShutdown();
    void updateAutomationDebug();
    [[nodiscard]] const std::vector<world::ChunkRequest>& streamingRequestsForFrame();
    void refreshSurfaceVisibilityRequests(world::ChunkCoord center, bool streamSpaceOnly);
    [[nodiscard]] const std::vector<world::ChunkRequest>& streamingDispatchRequestsForFrame(
        const std::vector<world::ChunkRequest>& requests);
    [[nodiscard]] int streamingForwardBucket(core::Vec3 forward) const noexcept;
    // Returns the frame-cached in-flight generation snapshot, refreshing it
    // from the mailbox if this is the first call this frame. See the
    // member-field comments above `cachedInFlightSnapshot_` for rationale.
    [[nodiscard]] const std::unordered_set<world::ChunkCoord, world::ChunkCoordHash>&
        getInFlightGenerationSnapshot();
    [[nodiscard]] std::size_t dispatchTerrainPrepassJobs(const std::vector<world::ChunkRequest>& requests);
    [[nodiscard]] std::uint64_t meshNeighborRevisionHash(world::ChunkCoord coord) const;
    [[nodiscard]] std::uint64_t lightingNeighborHash(world::ChunkCoord coord) const;
    void enqueueLightingIfNeeded(world::ChunkCoord coord);
    void enqueueMeshIfNeeded(world::ChunkCoord coord, float priority = 0.0F);
    void enqueueInstalledChunkWork(const world::ChunkPipelineStats& stats);
    [[nodiscard]] std::size_t enqueueVisibleMeshWork(const std::vector<world::ChunkRequest>& requests);
    [[nodiscard]] LightingStats propagateLightingForDirtyChunks();
    [[nodiscard]] MeshDispatchStats dispatchMeshJobs();
    [[nodiscard]] MeshInstallStats installMeshResults();
    void logRuntimeStats(const char* label, const core::RuntimeCounters& counters, double averageFrameMs) const;
    void logSlowFrame(double frameMs, const core::RuntimeCounters& counters) const;
    void shutdown();

    ApplicationConfig config_;
    core::JobSystem jobs_;
    data::BlockRegistry blocks_;
    data::CoreBlockIds coreBlocks_;
    data::ItemRegistry items_;
    data::RecipeRegistry recipes_;
    render::meshing::BlockRenderCatalog blockRenderCatalog_;
    world::BlockCollisionCatalog blockCollisionCatalog_;
    world::BlockLightCatalog blockLightCatalog_;
    automation::KineticBlockCatalog kineticCatalog_;
    automation::KineticNetworkSolver kineticSolver_;
    world::ChunkManager chunks_;
    world::ChunkStreamer streamer_;
    world::ChunkPipeline chunkPipeline_;
    world::ChunkJobMailbox chunkJobMailbox_;
    // W2: active-cell fluid simulation. Idle until a player edit wakes the
    // queue or the spawn point seeds a few cells.
    world::FluidSystem fluidSystem_;
    // EXPERIMENTAL: GPU compute fluid sim. Constructed only when
    // `config_.useGpuFluidSim` is true. Phase 1 just validates Vulkan
    // resource allocation at startup — tick() is still a CPU no-op.
    std::unique_ptr<world::FluidGpuSystem> fluidGpuSystem_;
    std::shared_ptr<world::TerrainColumnPrepassCache> terrainPrepassCache_{std::make_shared<world::TerrainColumnPrepassCache>()};
    world::NoiseTerrainGenerator terrainGenerator_;
    world::SpaceEnvironment spaceEnvironment_;
    world::SpaceEnvironmentState currentSpaceState_{};
    world::LightPropagator lightPropagator_;
    std::size_t maxLightPropagationsPerTick_{16};
    world::ChunkDirtyQueue lightingQueue_;
    world::ChunkDirtyQueue meshQueue_;
    render::meshing::GreedyMesher mesher_;
    std::unique_ptr<render::meshing::HybridMeshingGpuSystem> hybridMeshingGpuSystem_;
    // LOD2 cluster rendering subsystem. Constructed only when
    // `config_.useClusterLod` is true. Owns its own vertex/index arenas
    // and scene-entry buffer (independent of the chunk arenas in
    // VulkanRenderer). See `docs/lod_persistence_policy.md` for the
    // chunk-mesh-stays-until-cluster-ready invariant this enables.
    std::unique_ptr<render::ClusterRenderer> clusterRenderer_;
    // Phase 1D-1: cluster build bookkeeping. Each entry maps a cluster
    // we've uploaded to clusterRenderer_ -> the 64-bit "which of the 64
    // contained chunks were resident at build time" mask. We rebuild
    // when the residency mask grows (more source chunks arrived), and
    // skip the candidate entirely once the mask is all-ones (fully
    // resident — no further data can improve it). Phase 1D-3 will add
    // invalidation hooks so block edits inside a cluster mark it dirty
    // for re-build.
    std::unordered_map<world::ClusterCoord, std::uint64_t,
                       world::ClusterCoordHash> builtClusters_;

    // Phase 1D-2: GPU LOD meshing. Owns the cluster_mesh_classify.comp
    // pipeline + buffers. Replaces the synchronous CPU ClusterMesher
    // path inside tickClusterMaintenance with a submit/poll pipeline
    // that moves the heavy face-classification work off the main
    // thread. Constructed only when `config_.useClusterLod` is true.
    std::unique_ptr<render::meshing::ClusterGpuMeshing> clusterGpuSystem_;

    // Phase 1D-2b: persistent cache for built cluster meshes. Survives
    // across sessions — re-visiting an area loads the LOD mesh from
    // disk (~1 ms) instead of re-running GPU classify + greedy merge
    // (~5-15 ms total). Eliminates the "world fills in over 10 seconds
    // at spawn" pattern observed in earlier captures.
    render::meshing::ClusterMeshDiskCache clusterMeshCache_;
    // Phase 1D-3b: LOD3 region disk cache. Same class as the LOD2
    // cache, separate baseDir ({saveRoot}/lod3_cache). Region coords
    // get cast to ClusterCoord for storage (region.{x,y,z} *
    // RegionClusterExtent) — the two caches live in different dirs so
    // the cast-collision (LOD2 cluster (4,0,0) vs LOD3 region (1,0,0)
    // both becoming key (4,0,0)) is harmless. Each region mesh blob is
    // ~50-150 KB depending on terrain complexity; an explored world
    // with hundreds of unique regions visited eats ~50-200 MB of disk.
    render::meshing::ClusterMeshDiskCache regionMeshCache_;
    // Tracks the cluster currently in-flight on the GPU. When set, we
    // poll clusterGpuSystem_ each tick; on completion, do the CPU
    // greedy merge + upload, then submit the next candidate.
    struct PendingClusterJob {
        world::ClusterCoord coord{};
        std::uint64_t residencyMask{};
        std::uint64_t sourceRevisionsHash{};
    };
    std::optional<PendingClusterJob> pendingClusterJob_;

    // Phase 1D-2e: greedy merge runs on a worker thread, not the main
    // thread. When the GPU classifier completes, the merge job (1-3 ms
    // CPU) is dispatched to the JobSystem. Main thread polls this
    // future each tick; when ready, picks up the mesh, uploads, caches.
    //
    // This is the last big sync block in the cluster pipeline — moving
    // it off-thread eliminates the per-frame spike when a cluster's
    // GPU classification lands.
    struct PendingMergeJob {
        world::ClusterCoord coord{};
        std::uint64_t residencyMask{};
        std::future<render::meshing::ClusterMesh> future;
    };
    std::optional<PendingMergeJob> pendingMergeJob_;

    // Phase 1D-3: LOD3 region bookkeeping. Parallel structure to the
    // LOD2 cluster path above but at 4× the linear extent (a region =
    // 16×16×16 chunks vs cluster's 4×4×4). The merge job runs on a
    // worker thread same as the LOD2 path — the meshing logic is
    // synchronous CPU (RegionMesher::build) since the region's
    // 1-sample-per-supervoxel reduction is much smaller than a full
    // chunk-by-chunk pass and doesn't need the GPU classifier.
    std::unordered_map<world::RegionCoord, std::uint64_t,
                       world::RegionCoordHash> builtRegions_;
    struct PendingRegionMergeJob {
        world::RegionCoord coord{};
        std::uint64_t sourceRevisionsHash{};
        std::future<render::meshing::ClusterMesh> future;
    };
    std::optional<PendingRegionMergeJob> pendingRegionMergeJob_;

    // Phase 1D-3c: GPU LOD3 classification — 3-stage pipeline state.
    // Stage A (worker reduce): pendingRegionPaddedJob_
    // Stage B (GPU classify):  pendingRegionGpuJob_
    // Stage C (worker merge):  pendingRegionMergeJob_ above
    // At most one stage of any one region is in flight at a time;
    // tickRegionMaintenance advances the pipeline each frame.
    struct PendingRegionPaddedJob {
        world::RegionCoord coord{};
        std::uint64_t sourceRevisionsHash{};
        std::future<std::vector<render::meshing::ClusterGpuMeshing::GpuCellInfo>> future;
    };
    std::optional<PendingRegionPaddedJob> pendingRegionPaddedJob_;

    // GPU classify in flight for a region. The actual GPU work runs on
    // the shared clusterGpuSystem_ (single slot, coordinated with LOD2
    // via the busy() check). We only need to remember which region we
    // submitted so we can route the faces to the right merge job.
    struct PendingRegionGpuJob {
        world::RegionCoord coord{};
        std::uint64_t sourceRevisionsHash{};
    };
    std::optional<PendingRegionGpuJob> pendingRegionGpuJob_;

    render::meshing::ChunkMeshCache meshCache_;
    core::RuntimeStats runtimeStats_;
    render::VulkanRenderer renderer_;
    // In-game debug overlay (toggled with F3). Owns rolling per-stage history
    // + the "copy to clipboard" formatter.
    render::DebugOverlay debugOverlay_;
    bool runtimeSettingsMouseCapture_{false};
    bool debugOverlayVisible_{false};
    bool debugOverlayToggleLatch_{false};
    save::RegionFileStore saveStore_;
    save::WorldSaveService worldSaveService_;
    save::SaveCoordinator saveCoordinator_;
    // L2/L5: per-world player IO. Both services are stateless; they hold the
    // world root via the path argument on each save/load call.
    save::PlayerInventorySaveService playerInventorySaveService_{};
    save::PlayerStateSaveService playerStateSaveService_{};
    // M2: the in-game World Manager overlay queries this for rename/delete and
    // for the "Switch to another world" affordance. It points at the global
    // saves directory rather than the active world root.
    save::WorldRegistry worldRegistry_;
    // F9 toggles the World Manager overlay. `returnToTitleRequested_` makes
    // run() exit with kReturnToTitle so sandbox main can loop back to the
    // launcher menu. `nextWorldRequest_` carries the direct switch target so
    // we can bypass the menu and load straight into a different world.
    bool worldManagerOpen_{false};
    bool worldManagerToggleLatch_{false};
    bool returnToTitleRequested_{false};
    std::optional<save::WorldEntry> nextWorldRequest_{};
    // ImGui state for the World Manager: per-world inline rename buffer and
    // the index of the world the player asked to delete (so the confirm
    // popup knows which one to act on).
    std::vector<std::string> worldRenameBuffers_;
    int pendingDeleteIndex_{-1};
    bool worldManagerCaptureMouse_{false};
    world::VoxelRaycaster raycaster_;
    world::BlockEditor blockEditor_;
    world::BlockEditQueue blockEditQueue_;
    player::PlayerController player_;
    player::PlayerSpawnResolver spawnResolver_;
    player::CreativeHotbar hotbar_;
    std::unique_ptr<platform::IWindow> window_;
    automation::NetworkGraph automationGraph_;
    physics::PhysicsSystem physics_;
    magic::MagicSystem magic_;
    network::NetworkSystem network_;
    ecs::GameRegistry gameRegistry_;
    ecs::SimulationSystem simulation_;
    inventory::PlayerInventory playerInventory_;
    inventory::InventoryManager machineInventories_;
    world::BlockEntityTypeRegistry blockEntityTypes_;
    world::BlockEntityTickScheduler blockEntityScheduler_;
    world::TerrainDefinitionRegistry terrainDefinitions_;
    // OPTIMIZATION: vertex/index vectors freed at end-of-tick instead of
    // inside the mesh-install loop. The Debug allocator scribbles freed
    // memory; deferring a wave of vector frees out of the install budget
    // dropped mesh_install_ms p99 from ~10 ms to <3 ms.
    std::vector<std::pair<std::vector<render::meshing::VoxelVertex>, std::vector<std::uint32_t>>> meshDataToFree_;
    bool initialized_{false};
    std::size_t frameIndex_{0};
    bool leftMouseWasDown_{false};
    bool rightMouseWasDown_{false};
    bool automationDumpLatch_{false};
    bool freecam_{false};
    bool freecamToggleLatch_{false};
    bool noclipToggleLatch_{false};
    bool inventoryOpen_{false};
    bool inventoryToggleLatch_{false};
    bool escapeToggleLatch_{false};
    bool spellCastingMode_{false};
    std::size_t prevSpellSlot_{0};
    inventory::ItemStack cursorStack_{};
    std::vector<SlotHitRect> inventorySlotRects_;
    bool invLeftMouseWasDown_{false};
    bool invRightMouseWasDown_{false};
    bool playerSpawnResolved_{false};
    bool hasPlayerCursor_{false};
    std::size_t lastSlowFrameLogFrame_{0};
    bool streamingRequestCacheValid_{false};
    world::ChunkCoord cachedStreamingCenter_{};
    world::StreamingSettings cachedStreamingSettings_{};
    int cachedStreamingForwardBucket_{0};
    std::size_t cachedStreamingRequestFrame_{0};
    std::size_t streamingRequestCursor_{0};
    std::size_t lastStreamingDispatchScanFrame_{0};
    bool streamingCenterChangedThisFrame_{false};
    bool streamingDispatchIdle_{false};
    std::size_t lastSpawnResolveAttemptFrame_{0};
    double lastPlayerCursorX_{0.0};
    double lastPlayerCursorY_{0.0};
    BlockStateId selectedPlaceBlock_{};
    std::vector<world::ChunkRequest> cachedStreamingRequests_;
    std::vector<world::ChunkRequest> surfaceVisibilityRequests_;
    std::unordered_set<world::ChunkCoord, world::ChunkCoordHash> surfaceVisibilityRetainSet_;
    world::ChunkCoord cachedSurfaceVisibilityCenter_{};
    int cachedSurfaceVisibilityRenderDistance_{-1};
    int cachedSurfaceVisibilityVerticalDistance_{-1};
    int cachedSurfaceVisibilityForwardBucket_{-1};
    bool cachedSurfaceVisibilitySpaceOnly_{false};
    bool surfaceVisibilityCacheValid_{false};
    std::vector<world::ChunkRequest> streamingDispatchRequests_;
    std::vector<world::ChunkMeshResult> pendingMeshResults_;
    std::optional<PendingHybridMeshJob> pendingHybridMeshJob_;

    // OPTIMIZATION: frame-cached in-flight generation snapshot. Previously
    // both `streamingDispatchRequestsForFrame` and `dispatchMeshJobs` each
    // called `mailbox.snapshotInFlightGeneration()` per frame — that's two
    // lock-acquire + deep-copy passes (~400µs total in Debug) where one
    // would do. The snapshot is conceptually stale the moment the mutex
    // releases anyway, so sharing it across the two callers within one
    // tick is semantically equivalent. `inFlightSnapshotFrame_` tags the
    // cached value; consumers refresh once per frame on first access.
    std::unordered_set<world::ChunkCoord, world::ChunkCoordHash> cachedInFlightSnapshot_;
    // Sentinel: SIZE_MAX means "not yet populated"; any real frameIndex_
    // value triggers exactly one refresh per frame on first access.
    std::size_t cachedInFlightSnapshotFrame_{static_cast<std::size_t>(-1)};

    // OPTIMIZATION: async replan of the streaming request list. The full
    // planRequests() call sorts ~30K entries and can spike to 10+ ms on the
    // main thread. We dispatch it to a worker; the result is consumed on
    // the next frame whose call to streamingRequestsForFrame() sees the
    // mailbox populated. While the replan is in flight, the cached (slightly
    // stale) request list is used — invisible at 60 fps.
    struct PendingReplan {
        std::future<std::vector<world::ChunkRequest>> future;
        world::ChunkCoord center{};
        world::StreamingSettings settings{};
        int forwardBucket{0};
        std::size_t startFrame{0};
    };
    std::optional<PendingReplan> pendingReplan_;
};

} // namespace voxel
