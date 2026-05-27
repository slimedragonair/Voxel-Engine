#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>

#include <voxel/core/RuntimeStats.hpp>

namespace voxel::core {
class JobSystem;
} // namespace voxel::core

namespace voxel::world {
class ChunkManager;
} // namespace voxel::world

namespace voxel::save {

class WorldSaveService;

struct SaveCoordinatorSettings {
    std::filesystem::path saveRoot{};
    std::size_t maxSavesPerFlush{8};
    std::size_t saveFlushIntervalFrames{120};
};

class SaveCoordinator {
public:
    [[nodiscard]] core::RuntimeCounters flushPending(
        bool force,
        world::ChunkManager& chunks,
        const WorldSaveService& worldSaveService,
        core::JobSystem& jobs,
        const SaveCoordinatorSettings& settings,
        std::uint64_t frameIndex);

    [[nodiscard]] std::size_t drainCompleted(bool wait);
    [[nodiscard]] std::size_t pendingJobCount() const noexcept { return asyncJobs_.size(); }

private:
    struct AsyncSaveJob {
        std::future<std::size_t> future;
    };

    std::deque<AsyncSaveJob> asyncJobs_;
};

} // namespace voxel::save
