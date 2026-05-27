#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <voxel/core/RuntimeStats.hpp>

namespace voxel::render {

// Rolling-window debug overlay. Each frame the application calls
// `record(frameMs, counters)` with the same per-frame counters it would log;
// the overlay keeps the last `kHistory` samples for live graphing, and the
// ImGui draw call (`draw`) renders the window. The window can be toggled
// with a key in the application (default F3) — the overlay itself is just
// the data + draw logic.
//
// All ImGui calls live inside the .cpp so callers don't have to depend on
// the imgui headers.
class DebugOverlay {
public:
    // Rolling history window. 240 samples ~ 4 seconds at 60 FPS — enough to
    // see a frame-time spike against the recent baseline.
    static constexpr std::size_t kHistory = 7200;

    // Append the latest frame's measurements to the rolling buffers.
    // Cheap: a handful of array writes; no allocations.
    void record(double frameMs, const core::RuntimeCounters& counters,
                const std::string& extraInfo = {});

    // Build the ImGui window for this frame. Must be called between
    // `ImGui::NewFrame()` and `ImGui::Render()`. `visible` controls whether
    // the window is drawn — pass it through from the application's toggle.
    void draw(bool visible);

    // Produce the same text format used by Application::logRuntimeStats for
    // a single sample. Used by the "Copy to Clipboard" button. Public so the
    // app can print it on demand too.
    [[nodiscard]] std::string formatSnapshotText() const;

    // True when the overlay would like to consume mouse input (cursor over
    // an ImGui window). The application can use this to suppress block
    // interaction while interacting with the debug UI.
    [[nodiscard]] bool wantsMouseCapture() const noexcept { return mouseCapture_; }

private:
    // Push one sample into a ring buffer.
    template <typename T>
    void push(std::array<T, kHistory>& buf, T value) noexcept
    {
        buf[writeIndex_] = value;
        // writeIndex_ is bumped once per `record()` at the end.
    }

    void advance() noexcept
    {
        writeIndex_ = (writeIndex_ + 1u) % kHistory;
        if (sampleCount_ < kHistory) {
            ++sampleCount_;
        }
    }

    // Helpers for the draw step (defined in .cpp to keep imgui out of header).
    void drawFrameSection();
    void drawStagesSection();
    void drawMeshSection();
    void drawMemorySection();
    void drawCaptureSection();
    void drawSpikeSection();
    void drawMiscSection();
    void resetCaptureStats();
    void startCsvCapture();
    void stopCsvCapture();
    void writeCsvHeader();
    void writeCsvSample(double frameMs, const core::RuntimeCounters& counters);

    struct SpikeRecord {
        std::uint64_t sample{};
        float frameMs{};
        float streamMs{};
        float streamPlanMs{};
        float streamDispatchMs{};
        float streamPrepassMs{};
        float streamPipelineMs{};
        float streamEnqueueMs{};
        float meshInstallMs{};
        float meshDispatchMs{};
        float simulationMs{};
        float renderMs{};
        std::uint64_t residentChunks{};
        std::uint64_t dirtyMeshQueue{};
        std::uint64_t inFlightGeneration{};
        std::uint64_t inFlightMesh{};
    };

    std::array<float, kHistory> frameMsHistory_{};
    std::array<float, kHistory> stageStreamingMs_{};
    std::array<float, kHistory> stageStreamPlanMs_{};
    std::array<float, kHistory> stageStreamDispatchMs_{};
    std::array<float, kHistory> stageStreamPrepassMs_{};
    std::array<float, kHistory> stageStreamPipelineMs_{};
    std::array<float, kHistory> stageStreamEnqueueMs_{};
    std::array<float, kHistory> stagePlayerMs_{};
    std::array<float, kHistory> stageMeshInstallMs_{};
    std::array<float, kHistory> stageMeshDispatchMs_{};
    std::array<float, kHistory> stageLightingMs_{};
    std::array<float, kHistory> stageSaveMs_{};
    std::array<float, kHistory> stageSimulationMs_{};
    std::array<float, kHistory> stageRenderMs_{};
    std::array<float, kHistory> meshBuildAvgMs_{};
    std::array<float, kHistory> meshBuildMaxMs_{};
    std::array<float, kHistory> genAvgMs_{};
    std::array<std::uint32_t, kHistory> dirtyMeshQueue_{};
    std::array<std::uint32_t, kHistory> staleMeshes_{};
    std::array<std::uint32_t, kHistory> inFlightGeneration_{};
    std::array<std::uint32_t, kHistory> inFlightMesh_{};
    std::array<std::uint32_t, kHistory> residentChunks_{};
    std::array<std::uint32_t, kHistory> drawables_{};

    // Latest sample as a copy — used by `formatSnapshotText()`.
    core::RuntimeCounters latestCounters_{};
    double latestFrameMs_{0.0};
    std::string latestExtra_{};

    std::size_t writeIndex_{0};
    std::size_t sampleCount_{0};
    std::uint64_t totalSamples_{0};
    double frameMsTotal_{0.0};
    float worstFrameMs_{0.0F};
    SpikeRecord worstFrame_{};
    float spikeThresholdMs_{33.0F};
    bool paused_{false};
    bool csvCaptureEnabled_{false};
    std::string csvPath_{"logs/perf_capture.csv"};
    std::ofstream csv_;
    std::vector<SpikeRecord> spikes_;

    bool mouseCapture_{false};
};

} // namespace voxel::render
