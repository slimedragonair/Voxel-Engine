#include <voxel/core/Application.hpp>
#include <voxel/core/Logger.hpp>
#include <voxel/save/WorldRegistry.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::size_t parseFrameLimit(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} != "--frames") {
            continue;
        }

        std::size_t frames = 0;
        const std::string_view value{argv[i + 1]};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), frames);
        if (result.ec == std::errc{}) {
            return frames;
        }
    }
    return 0;
}

int parseIntFlag(int argc, char** argv, std::string_view name, int fallback)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} != name) {
            continue;
        }
        int value = 0;
        const std::string_view raw{argv[i + 1]};
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (result.ec == std::errc{}) {
            return value;
        }
    }
    return fallback;
}

double parseDoubleFlag(int argc, char** argv, std::string_view name, double fallback)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} != name) {
            continue;
        }
        double value = 0.0;
        const std::string_view raw{argv[i + 1]};
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (result.ec == std::errc{}) {
            return value;
        }
    }
    return fallback;
}

bool hasFlag(int argc, char** argv, std::string_view name)
{
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == name) {
            return true;
        }
    }
    return false;
}

std::string parseStringFlag(int argc, char** argv, std::string_view name, std::string fallback)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} != name) {
            continue;
        }
        return std::string{argv[i + 1]};
    }
    return fallback;
}

std::uint64_t parseUint64Flag(int argc, char** argv, std::string_view name, std::uint64_t fallback)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} != name) {
            continue;
        }
        std::uint64_t value = 0;
        const std::string_view raw{argv[i + 1]};
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (result.ec == std::errc{}) {
            return value;
        }
    }
    return fallback;
}

// N3: shows the graphical title screen by spinning up an Application in
// title-screen mode, then returns whichever world the player chose (or
// std::nullopt if they hit Quit).
std::optional<voxel::save::WorldEntry> runTitleScreen(const voxel::ApplicationConfig& templateConfig)
{
    voxel::ApplicationConfig titleConfig = templateConfig;
    titleConfig.titleScreenMode = true;
    titleConfig.maxFrames = 0;          // title screen never auto-quits.
    titleConfig.debugStartPosition.reset();
    titleConfig.debugStartNoclip = false;

    voxel::Application titleApp(titleConfig);
    voxel::Logger::info("Showing title screen...");
    const int titleCode = titleApp.run();
    if (titleCode == voxel::Application::kReturnToTitle) {
        if (const auto& chosen = titleApp.nextWorldRequest(); chosen.has_value()) {
            return *chosen;
        }
    }
    return std::nullopt;
}

bool solidSpaceFeature(voxel::world::SpaceFeatureType type) noexcept
{
    switch (type) {
    case voxel::world::SpaceFeatureType::AsteroidCluster:
    case voxel::world::SpaceFeatureType::IceField:
    case voxel::world::SpaceFeatureType::MetalRichAsteroids:
    case voxel::world::SpaceFeatureType::CrystalAsteroids:
    case voxel::world::SpaceFeatureType::Comet:
    case voxel::world::SpaceFeatureType::RingDebris:
        return true;
    default:
        return false;
    }
}

std::optional<voxel::core::Vec3> spaceTestStart(const voxel::world::SpaceSettings& settings)
{
    voxel::world::SpaceEnvironment space(settings);
    const int firstSpaceSectorY = static_cast<int>(
        std::floor(settings.atmosphereTopY / static_cast<float>(settings.sectorSizeBlocks)));
    std::optional<voxel::core::Vec3> bestStart;
    int bestScore = -1;
    const auto featureScore = [](voxel::world::SpaceFeatureType type) noexcept {
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
    for (int sy = firstSpaceSectorY; sy < firstSpaceSectorY + 8; ++sy) {
        for (int sz = -8; sz <= 8; ++sz) {
            for (int sx = -8; sx <= 8; ++sx) {
                for (const auto& feature : space.featuresForSector({sx, sy, sz})) {
                    if (!solidSpaceFeature(feature.type)) {
                        continue;
                    }
                    const auto center = space.featureWorldCenter(feature);
                    if (center.y < settings.atmosphereTopY) {
                        continue;
                    }
                    const int score = featureScore(feature.type);
                    if (score > bestScore) {
                        bestScore = score;
                        bestStart = voxel::core::Vec3{center.x + 96.0F, center.y + 48.0F, center.z + 96.0F};
                    }
                }
            }
        }
    }
    return bestStart;
}

std::size_t automaticFastWorkerCount()
{
    const auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    if (hw <= 2U) {
        return 1U;
    }
    return std::max<std::size_t>(1U, (hw * 70U) / 100U);
}

std::size_t automaticSteadyWorkerCount()
{
    const auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    if (hw <= 2U) {
        return 1U;
    }
    return std::max<std::size_t>(1U, (hw * 65U) / 100U);
}

void applySteadyStreamingProfile(voxel::ApplicationConfig& config, std::size_t workerBudget)
{
    workerBudget = std::max<std::size_t>(workerBudget, 6U);
    // BUDGET TIGHTENING: previous profile capped at 32 mesh jobs / 48 mesh
    // installs / 32 generations per tick, which let individual frames spike
    // to ~35-41 ms during cold-start streaming bursts. Halving the per-frame
    // caps trades slower world-fill (~2x more frames to reach steady state,
    // ~5s instead of ~2.5s for a render-distance=8 cold start) for smoother
    // frame times. The time budgets (maxMeshInstallMsPerFrame, etc.) are
    // also tightened proportionally so the per-iteration time gate bails out
    // earlier when work piles up.
    config.chunkPipeline.maxLoadsOrGenerationsPerTick = std::clamp<std::size_t>(workerBudget * 2U, 16U, 48U);
    config.chunkPipeline.maxGenerationInstallsPerTick = std::clamp<std::size_t>(workerBudget * 2U, 16U, 64U);
    config.chunkPipeline.minGenerationInstallsPerTick = std::clamp<std::size_t>(workerBudget / 2U, 4U, 8U);
    config.chunkPipeline.maxGenerationInstallMsPerTick = 2.0;
    config.chunkPipeline.maxInFlightGeneration = std::clamp<std::size_t>(workerBudget * 10U, 96U, 320U);
    config.maxChunkMeshesPerTick = std::clamp<std::size_t>(workerBudget * 4U, 32U, 128U);
    config.workBudget.maxMeshJobsPerTick = config.maxChunkMeshesPerTick;
    config.workBudget.maxMeshInstallsPerTick = std::clamp<std::size_t>(workerBudget * 3U, 32U, 128U);
    config.workBudget.maxGpuUploadBytesPerTick = 64U * 1024U * 1024U;
    config.workBudget.maxMeshInstallMsPerFrame = 2.5;
    config.workBudget.maxUploadSubmitMsPerFrame = 6.0;
    config.workBudget.maxDirtyScanMsPerFrame = 1.5;
    config.workBudget.maxStreamDispatchMsPerFrame = 1.25;
}

void applyFastStreamingProfile(voxel::ApplicationConfig& config, std::size_t workerBudget)
{
    workerBudget = std::max<std::size_t>(workerBudget, 8U);
    config.chunkPipeline.maxLoadsOrGenerationsPerTick = std::clamp<std::size_t>(workerBudget * 2U, 32U, 64U);
    config.chunkPipeline.maxGenerationInstallsPerTick = std::clamp<std::size_t>(workerBudget * 2U, 32U, 96U);
    config.chunkPipeline.minGenerationInstallsPerTick = std::clamp<std::size_t>(workerBudget, 8U, 16U);
    config.chunkPipeline.maxGenerationInstallMsPerTick = 6.0;
    config.chunkPipeline.maxInFlightGeneration = std::clamp<std::size_t>(workerBudget * 8U, 128U, 256U);
    config.maxChunkMeshesPerTick = std::clamp<std::size_t>(workerBudget * 5U, 80U, 192U);
    config.workBudget.maxMeshJobsPerTick = config.maxChunkMeshesPerTick;
    config.workBudget.maxMeshInstallsPerTick = std::clamp<std::size_t>(workerBudget * 4U, 64U, 192U);
    config.workBudget.maxGpuUploadBytesPerTick = 64U * 1024U * 1024U;
    config.workBudget.maxMeshInstallMsPerFrame = 8.0;
    config.workBudget.maxUploadSubmitMsPerFrame = 10.0;
    config.workBudget.maxDirtyScanMsPerFrame = 4.0;
    config.workBudget.maxStreamDispatchMsPerFrame = 4.0;
}

} // namespace

int main(int argc, char** argv)
{
    voxel::ApplicationConfig config;
    config.name = "AetherForge: Infinite Creation Sandbox";
    config.maxFrames = parseFrameLimit(argc, argv);

    // F4: streamer radius is X/Z horizontal × Y vertical. Defaults to a
    // modest 8×2 (17×5×17 = 1445 chunks); pass `--render-distance N` for
    // stress runs (16 ≈ 33×5×33 = 5445, 32 ≈ 65×5×65 = 21125).
    config.streaming.renderDistanceChunks = parseIntFlag(argc, argv, "--render-distance", 8);
    config.streaming.verticalRenderDistanceChunks = parseIntFlag(argc, argv, "--vertical-distance", 2);
    config.streaming.simulationDistanceChunks = 2;
    config.streaming.physicsDistanceChunks = 2;
    const int workers = parseIntFlag(argc, argv, "--workers", 0);
    config.workerCount = workers > 0
        ? static_cast<std::size_t>(workers)
        : automaticSteadyWorkerCount();
    applySteadyStreamingProfile(config, config.workerCount);
    config.workBudget.maxLightingPerTick = 6;
    config.workBudget.maxLightingMsPerFrame = 1.5;
    if (hasFlag(argc, argv, "--fast-streaming")) {
        if (workers <= 0) {
            config.workerCount = automaticFastWorkerCount();
        }
        applyFastStreamingProfile(config, config.workerCount > 0 ? config.workerCount : static_cast<std::size_t>(workers));
    }
    config.slowFrameLogThresholdMs = parseDoubleFlag(argc, argv, "--slow-frame-ms", config.slowFrameLogThresholdMs);
    config.spaceCameraFarPlane = static_cast<float>(std::clamp(
        parseDoubleFlag(argc, argv, "--space-far-plane", static_cast<double>(config.spaceCameraFarPlane)),
        1024.0,
        524288.0));
    config.enableGpuCulling = !hasFlag(argc, argv, "--cpu-cull");
    if (hasFlag(argc, argv, "--gpu-cull")) {
        config.enableGpuCulling = true;
    }
    config.compareGpuCulling = hasFlag(argc, argv, "--gpu-cull-compare");
    if (config.compareGpuCulling) {
        config.enableGpuCulling = true;
    }
    config.useGpuShaderLighting = !hasFlag(argc, argv, "--cpu-lighting");
    // GPU hybrid meshing: GPU classifies visible faces, CPU greedy-merges
    // them. Default ON; opt out with --cpu-meshing for comparisons.
    config.useGpuHybridMeshing = !hasFlag(argc, argv, "--cpu-meshing");
    config.useGpuTerrainGeneration = hasFlag(argc, argv, "--gpu-terrain");
    // Phase 1D-2: LOD2 cluster rendering. Default ON now that the GPU
    // classifier pipeline lands the heavy work off the main thread.
    // Opt out at startup with --no-cluster-lod, or toggle live via the
    // Runtime Settings overlay (F3-ish menu) checkbox.
    config.useClusterLod = !hasFlag(argc, argv, "--no-cluster-lod");
    if (hasFlag(argc, argv, "--space-test-start")) {
        if (auto start = spaceTestStart(config.space)) {
            config.debugStartPosition = *start;
            config.debugStartNoclip = true;
            config.streaming.verticalRenderDistanceChunks = std::max(config.streaming.verticalRenderDistanceChunks, 1);
        } else {
            voxel::Logger::warn("No deterministic space feature found for --space-test-start");
        }
    }

    // N3: resolve which world to enter. `--world <name>` skips the title
    // screen (creating the world if it doesn't exist); `--frames N`
    // (headless) falls back to dev_world; otherwise the graphical title
    // screen runs and the player picks. On in-game "Quit to Title" /
    // "Switch" (returned via Application::kReturnToTitle), we loop back
    // and show the title screen again.
    voxel::save::WorldRegistry worldRegistry(config.paths.savesDirectory());
    const std::string worldFlag = parseStringFlag(argc, argv, "--world", "");
    const std::uint64_t seedFlag = parseUint64Flag(argc, argv, "--seed", 0);
    const bool headlessSkip = config.maxFrames > 0 || hasFlag(argc, argv, "--skip-menu");

    std::optional<voxel::save::WorldEntry> pendingSwitchTarget;
    while (true) {
        std::optional<voxel::save::WorldEntry> chosen;
        if (pendingSwitchTarget.has_value()) {
            chosen = std::move(pendingSwitchTarget);
            pendingSwitchTarget.reset();
        } else if (!worldFlag.empty()) {
            if (auto existing = worldRegistry.findByName(worldFlag)) {
                chosen = std::move(existing);
            } else {
                chosen = worldRegistry.createWorld(worldFlag, seedFlag);
            }
        } else if (headlessSkip) {
            const auto devRoot = worldRegistry.savesDirectory() / "dev_world";
            if (auto existing = worldRegistry.findByDirectory(devRoot)) {
                chosen = std::move(existing);
            } else {
                chosen = worldRegistry.createWorld("Dev World", seedFlag);
            }
        } else {
            chosen = runTitleScreen(config);
        }
        if (!chosen.has_value()) {
            voxel::Logger::info("Player quit from the title screen.");
            return 0;
        }

        config.paths.setActiveWorldRoot(chosen->root);
        config.worldSeed = chosen->descriptor.seed;
        config.worldDisplayName = chosen->descriptor.name;
        config.titleScreenMode = false;

        voxel::Application app(config);
        voxel::Logger::info("Starting world \"" + chosen->descriptor.name
            + "\" (seed=" + std::to_string(chosen->descriptor.seed)
            + ", root=" + chosen->root.string() + ")");
        const int code = app.run();
        if (code == voxel::Application::kReturnToTitle) {
            if (const auto& next = app.nextWorldRequest(); next.has_value()) {
                pendingSwitchTarget = *next;
            }
            // Drop the headless frame budget so the next iteration actually
            // shows the title screen (rather than recursing into headless).
            config.maxFrames = 0;
            continue;
        }
        return code;
    }
}
