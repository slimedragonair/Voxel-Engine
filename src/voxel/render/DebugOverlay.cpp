#include <voxel/render/DebugOverlay.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

#include <imgui.h>

namespace voxel::render {

namespace {

float toMs(std::uint64_t us) noexcept
{
    return static_cast<float>(us) / 1000.0f;
}

float avgMs(const core::RuntimeCounters::Timer& t) noexcept
{
    return t.count == 0 ? 0.0f
                        : (static_cast<float>(t.totalUs) / static_cast<float>(t.count)) / 1000.0f;
}

float maxMs(const core::RuntimeCounters::Timer& t) noexcept
{
    return toMs(t.maxUs);
}

// Draw an ImGui plot whose data is a ring buffer. The plot wraps around at
// `writeIndex`, so we pass the offset to ImGui::PlotLines so the latest
// sample appears at the right edge.
void plotRingFloat(const char* label, const std::array<float, DebugOverlay::kHistory>& buf,
                   std::size_t writeIndex, std::size_t sampleCount, float scaleMin, float scaleMax,
                   const char* overlay)
{
    if (sampleCount == 0) return;
    const int offset = static_cast<int>(writeIndex % DebugOverlay::kHistory);
    ImGui::PlotLines(label,
                     buf.data(),
                     static_cast<int>(DebugOverlay::kHistory),
                     offset,
                     overlay,
                     scaleMin,
                     scaleMax,
                     ImVec2(0, 60));
}

} // namespace

void DebugOverlay::record(double frameMs, const core::RuntimeCounters& counters,
                          const std::string& extraInfo)
{
    latestCounters_ = counters;
    latestFrameMs_ = frameMs;
    latestExtra_ = extraInfo;

    if (csvCaptureEnabled_) {
        writeCsvSample(frameMs, counters);
    }
    if (paused_) {
        return;
    }

    frameMsHistory_[writeIndex_] = static_cast<float>(frameMs);

    stageStreamingMs_[writeIndex_] = maxMs(counters.stageStreaming);
    stageStreamPlanMs_[writeIndex_] = maxMs(counters.stageStreamPlan);
    stageStreamDispatchMs_[writeIndex_] = maxMs(counters.stageStreamDispatch);
    stageStreamPrepassMs_[writeIndex_] = maxMs(counters.stageStreamPrepass);
    stageStreamPipelineMs_[writeIndex_] = maxMs(counters.stageStreamPipeline);
    stageStreamEnqueueMs_[writeIndex_] = maxMs(counters.stageStreamEnqueue);
    stagePlayerMs_[writeIndex_] = maxMs(counters.stagePlayer);
    stageMeshInstallMs_[writeIndex_] = maxMs(counters.stageMeshInstall);
    stageMeshDispatchMs_[writeIndex_] = maxMs(counters.stageMeshDispatch);
    stageLightingMs_[writeIndex_] = maxMs(counters.stageLighting);
    stageSaveMs_[writeIndex_] = maxMs(counters.stageSave);
    stageSimulationMs_[writeIndex_] = maxMs(counters.stageSimulation);
    stageRenderMs_[writeIndex_] = maxMs(counters.stageRender);

    meshBuildAvgMs_[writeIndex_] = avgMs(counters.meshBuild);
    meshBuildMaxMs_[writeIndex_] = maxMs(counters.meshBuild);
    genAvgMs_[writeIndex_] = avgMs(counters.terrainGeneration);

    dirtyMeshQueue_[writeIndex_] = static_cast<std::uint32_t>(counters.dirtyMeshQueueLength);
    staleMeshes_[writeIndex_] = static_cast<std::uint32_t>(counters.meshJobsDiscardedStale);
    inFlightGeneration_[writeIndex_] = static_cast<std::uint32_t>(counters.inFlightGeneration);
    inFlightMesh_[writeIndex_] = static_cast<std::uint32_t>(counters.inFlightMesh);
    residentChunks_[writeIndex_] = static_cast<std::uint32_t>(counters.residentChunks);
    drawables_[writeIndex_] = static_cast<std::uint32_t>(counters.chunksDrawn);

    ++totalSamples_;
    frameMsTotal_ += frameMs;

    const SpikeRecord sample{
        totalSamples_,
        static_cast<float>(frameMs),
        maxMs(counters.stageStreaming),
        maxMs(counters.stageStreamPlan),
        maxMs(counters.stageStreamDispatch),
        maxMs(counters.stageStreamPrepass),
        maxMs(counters.stageStreamPipeline),
        maxMs(counters.stageStreamEnqueue),
        maxMs(counters.stageMeshInstall),
        maxMs(counters.stageMeshDispatch),
        maxMs(counters.stageSimulation),
        maxMs(counters.stageRender),
        counters.residentChunks,
        counters.dirtyMeshQueueLength,
        counters.inFlightGeneration,
        counters.inFlightMesh};
    if (sample.frameMs > worstFrameMs_) {
        worstFrameMs_ = sample.frameMs;
        worstFrame_ = sample;
    }
    if (sample.frameMs >= spikeThresholdMs_) {
        if (spikes_.size() >= 256U) {
            spikes_.erase(spikes_.begin());
        }
        spikes_.push_back(sample);
    }

    advance();
}

void DebugOverlay::draw(bool visible)
{
    mouseCapture_ = false;
    if (!visible) {
        return;
    }

    // The overlay window. NoNav keeps the controls' tab/cursor behavior from
    // stealing keyboard focus from the player.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoNavInputs;
    ImGui::SetNextWindowSize(ImVec2(520, 720), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Debug — Pipeline & Frame Stats", nullptr, flags)) {
        ImGui::End();
        return;
    }

    // Capture mouse for ImGui interaction so the caller can suppress world clicks.
    mouseCapture_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    // Top row: live frame time + copy button.
    drawFrameSection();
    ImGui::Separator();
    drawCaptureSection();
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Pipeline stages (ms, per-tick max)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        drawStagesSection();
    }
    if (ImGui::CollapsingHeader("Recorded spikes")) {
        drawSpikeSection();
    }
    if (ImGui::CollapsingHeader("Meshing & generation", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawMeshSection();
    }
    if (ImGui::CollapsingHeader("Memory & queues")) {
        drawMemorySection();
    }
    if (ImGui::CollapsingHeader("Misc")) {
        drawMiscSection();
    }

    ImGui::End();
}

void DebugOverlay::drawFrameSection()
{
    const float current = static_cast<float>(latestFrameMs_);

    // Auto-scale the graph upper bound to highlight spikes without saturating.
    float maxObserved = 0.0f;
    for (std::size_t i = 0; i < sampleCount_; ++i) {
        const auto idx = (writeIndex_ + kHistory - 1 - i) % kHistory;
        maxObserved = std::max(maxObserved, frameMsHistory_[idx]);
    }
    const float scaleMax = std::max(33.3f, std::ceil(maxObserved * 1.1f));

    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%.2f ms (%.0f fps)",
                  current, current > 0.0f ? 1000.0f / current : 0.0f);
    plotRingFloat("Frame", frameMsHistory_, writeIndex_, sampleCount_, 0.0f, scaleMax, overlay);

    // 16.7 / 33.3 ms reference lines (60/30 fps targets) as text below.
    ImGui::Text("targets: %.1f ms = 60fps   %.1f ms = 30fps   %.1f ms = current spike max",
                16.67f, 33.33f, maxObserved);

    if (ImGui::Button("Copy diagnostics to clipboard")) {
        const auto text = formatSnapshotText();
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(paused_ ? "Resume graphs" : "Pause graphs")) {
        paused_ = !paused_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset capture stats")) {
        resetCaptureStats();
    }
    ImGui::TextDisabled("samples=%llu avg=%.2fms worst=%.2fms at #%llu",
                        static_cast<unsigned long long>(totalSamples_),
                        totalSamples_ == 0 ? 0.0 : frameMsTotal_ / static_cast<double>(totalSamples_),
                        worstFrameMs_,
                        static_cast<unsigned long long>(worstFrame_.sample));
}

void DebugOverlay::drawStagesSection()
{
    // Per-stage area chart. Each stage gets its own row so the visual
    // breakdown of which stage spiked is obvious without staring at numbers.
    plotRingFloat("stream",   stageStreamingMs_,    writeIndex_, sampleCount_, 0.0f, 30.0f, nullptr);
    if (ImGui::TreeNode("stream breakdown")) {
        plotRingFloat("stream_plan",     stageStreamPlanMs_,     writeIndex_, sampleCount_, 0.0f, 20.0f, nullptr);
        plotRingFloat("stream_dispatch", stageStreamDispatchMs_, writeIndex_, sampleCount_, 0.0f, 20.0f, nullptr);
        plotRingFloat("stream_prepass",  stageStreamPrepassMs_,  writeIndex_, sampleCount_, 0.0f, 20.0f, nullptr);
        plotRingFloat("stream_pipeline", stageStreamPipelineMs_, writeIndex_, sampleCount_, 0.0f, 30.0f, nullptr);
        plotRingFloat("stream_enqueue",  stageStreamEnqueueMs_,  writeIndex_, sampleCount_, 0.0f, 10.0f, nullptr);
        ImGui::TreePop();
    }
    plotRingFloat("player",   stagePlayerMs_,       writeIndex_, sampleCount_, 0.0f, 10.0f, nullptr);
    plotRingFloat("mesh_i",   stageMeshInstallMs_,  writeIndex_, sampleCount_, 0.0f, 10.0f, nullptr);
    plotRingFloat("mesh_d",   stageMeshDispatchMs_, writeIndex_, sampleCount_, 0.0f, 10.0f, nullptr);
    plotRingFloat("light",    stageLightingMs_,     writeIndex_, sampleCount_, 0.0f, 10.0f, nullptr);
    plotRingFloat("save",     stageSaveMs_,         writeIndex_, sampleCount_, 0.0f, 10.0f, nullptr);
    plotRingFloat("sim",      stageSimulationMs_,   writeIndex_, sampleCount_, 0.0f, 10.0f, nullptr);
    plotRingFloat("render",   stageRenderMs_,       writeIndex_, sampleCount_, 0.0f, 20.0f, nullptr);

    if (ImGui::BeginTable("stages_table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Stage");
        ImGui::TableSetupColumn("Avg (ms)");
        ImGui::TableSetupColumn("Max (ms)");
        ImGui::TableHeadersRow();

        const auto row = [](const char* name, const core::RuntimeCounters::Timer& t) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", avgMs(t));
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", maxMs(t));
        };
        row("stream",        latestCounters_.stageStreaming);
        row("  plan",        latestCounters_.stageStreamPlan);
        row("  dispatch",    latestCounters_.stageStreamDispatch);
        row("  pipeline",    latestCounters_.stageStreamPipeline);
        row("  enqueue",     latestCounters_.stageStreamEnqueue);
        row("player",        latestCounters_.stagePlayer);
        row("mesh_install",  latestCounters_.stageMeshInstall);
        row("mesh_dispatch", latestCounters_.stageMeshDispatch);
        row("lighting",      latestCounters_.stageLighting);
        row("save",          latestCounters_.stageSave);
        row("simulation",    latestCounters_.stageSimulation);
        row("render",        latestCounters_.stageRender);
        ImGui::EndTable();
    }
}

void DebugOverlay::drawMeshSection()
{
    plotRingFloat("mesh_build avg (ms)", meshBuildAvgMs_, writeIndex_, sampleCount_, 0.0f, 30.0f, nullptr);
    plotRingFloat("mesh_build max (ms)", meshBuildMaxMs_, writeIndex_, sampleCount_, 0.0f, 100.0f, nullptr);
    plotRingFloat("gen avg (ms)",        genAvgMs_,        writeIndex_, sampleCount_, 0.0f, 200.0f, nullptr);

    ImGui::Text("dirty mesh queue: %llu  |  stale this interval: %llu",
                static_cast<unsigned long long>(latestCounters_.dirtyMeshQueueLength),
                static_cast<unsigned long long>(latestCounters_.meshJobsDiscardedStale));
    ImGui::Text("in flight  gen: %llu  mesh: %llu  light: %llu",
                static_cast<unsigned long long>(latestCounters_.inFlightGeneration),
                static_cast<unsigned long long>(latestCounters_.inFlightMesh),
                static_cast<unsigned long long>(latestCounters_.inFlightLighting));
    ImGui::Text("submitted gen=%llu/%llu  mesh=%llu/%llu  light=%llu/%llu",
                static_cast<unsigned long long>(latestCounters_.generationJobsSubmitted),
                static_cast<unsigned long long>(latestCounters_.generationJobsCompleted),
                static_cast<unsigned long long>(latestCounters_.meshJobsSubmitted),
                static_cast<unsigned long long>(latestCounters_.meshJobsCompleted),
                static_cast<unsigned long long>(latestCounters_.lightingJobsSubmitted),
                static_cast<unsigned long long>(latestCounters_.lightingJobsCompleted));
    ImGui::Text("neighbor remeshes (this interval): %llu",
                static_cast<unsigned long long>(latestCounters_.remeshesCausedByNeighborInstall));
    ImGui::Text("upload defers: %llu  budget saturated (l/m/s): %llu/%llu/%llu",
                static_cast<unsigned long long>(latestCounters_.uploadBudgetDeferrals),
                static_cast<unsigned long long>(latestCounters_.lightingBudgetSaturated),
                static_cast<unsigned long long>(latestCounters_.meshInstallBudgetSaturated),
                static_cast<unsigned long long>(latestCounters_.saveBudgetSaturated));
}

void DebugOverlay::drawMemorySection()
{
    ImGui::Text("resident chunks: %llu",
                static_cast<unsigned long long>(latestCounters_.residentChunks));
    ImGui::Text("mesh cache: %llu  drawable: %llu",
                static_cast<unsigned long long>(latestCounters_.meshCacheEntries),
                static_cast<unsigned long long>(latestCounters_.chunksMadeDrawable));
    ImGui::Text("staging upload bytes (this interval): %.2f KB",
                static_cast<float>(latestCounters_.stagingUploadBytes) / 1024.0f);
    ImGui::Text("upload batches: %llu  bytes: %.2f KB  q: %llu",
                static_cast<unsigned long long>(latestCounters_.uploadBatchCount),
                static_cast<float>(latestCounters_.uploadBatchBytes) / 1024.0f,
                static_cast<unsigned long long>(latestCounters_.uploadQueueLength));
    ImGui::Text("workers: %llu  job system pending: %llu",
                static_cast<unsigned long long>(latestCounters_.workerCount),
                static_cast<unsigned long long>(latestCounters_.jobSystemPending));
}

void DebugOverlay::drawCaptureSection()
{
    ImGui::Text("long capture: history=%llu/%llu  spikes=%llu  csv=%s",
                static_cast<unsigned long long>(sampleCount_),
                static_cast<unsigned long long>(kHistory),
                static_cast<unsigned long long>(spikes_.size()),
                csvCaptureEnabled_ ? "recording" : "off");
    ImGui::SetNextItemWidth(90.0F);
    ImGui::InputFloat("spike threshold ms", &spikeThresholdMs_, 1.0F, 5.0F, "%.1f");
    if (spikeThresholdMs_ < 1.0F) {
        spikeThresholdMs_ = 1.0F;
    }
    if (csvCaptureEnabled_) {
        if (ImGui::Button("Stop CSV capture")) {
            stopCsvCapture();
        }
    } else if (ImGui::Button("Start CSV capture")) {
        startCsvCapture();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", csvPath_.c_str());
}

void DebugOverlay::drawSpikeSection()
{
    if (spikes_.empty()) {
        ImGui::TextDisabled("No spikes captured above %.1f ms.", spikeThresholdMs_);
        return;
    }
    if (ImGui::Button("Copy spike table")) {
        std::ostringstream out;
        out << "sample,frame_ms,stream,plan,dispatch,prepass,pipeline,enqueue,mesh_i,mesh_d,sim,render,resident,dirty_mesh,inflight_gen,inflight_mesh\n";
        for (const auto& s : spikes_) {
            out << s.sample << "," << s.frameMs << "," << s.streamMs << ","
                << s.streamPlanMs << "," << s.streamDispatchMs << "," << s.streamPrepassMs << ","
                << s.streamPipelineMs << "," << s.streamEnqueueMs << "," << s.meshInstallMs << "," << s.meshDispatchMs << ","
                << s.simulationMs << "," << s.renderMs << "," << s.residentChunks << ","
                << s.dirtyMeshQueue << "," << s.inFlightGeneration << "," << s.inFlightMesh << "\n";
        }
        const auto text = out.str();
        ImGui::SetClipboardText(text.c_str());
    }
    if (ImGui::BeginTable("spike_table", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 220))) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("frame");
        ImGui::TableSetupColumn("stream");
        ImGui::TableSetupColumn("plan");
        ImGui::TableSetupColumn("dispatch");
        ImGui::TableSetupColumn("prepass");
        ImGui::TableSetupColumn("pipeline");
        ImGui::TableSetupColumn("enqueue");
        ImGui::TableSetupColumn("mesh_i/d");
        ImGui::TableSetupColumn("resident");
        ImGui::TableHeadersRow();
        const std::size_t begin = spikes_.size() > 128U ? spikes_.size() - 128U : 0U;
        for (std::size_t i = begin; i < spikes_.size(); ++i) {
            const auto& s = spikes_[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%llu", static_cast<unsigned long long>(s.sample));
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", s.frameMs);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", s.streamMs);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f", s.streamPlanMs);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.2f", s.streamDispatchMs);
            ImGui::TableSetColumnIndex(5); ImGui::Text("%.2f", s.streamPrepassMs);
            ImGui::TableSetColumnIndex(6); ImGui::Text("%.2f", s.streamPipelineMs);
            ImGui::TableSetColumnIndex(7); ImGui::Text("%.2f", s.streamEnqueueMs);
            ImGui::TableSetColumnIndex(8); ImGui::Text("%.2f/%.2f", s.meshInstallMs, s.meshDispatchMs);
            ImGui::TableSetColumnIndex(9); ImGui::Text("%llu", static_cast<unsigned long long>(s.residentChunks));
        }
        ImGui::EndTable();
    }
}

void DebugOverlay::drawMiscSection()
{
    ImGui::Text("slow frames (this interval): %llu",
                static_cast<unsigned long long>(latestCounters_.slowFrames));
    ImGui::Text("draws (vis/cull/cmds): %llu / %llu / %llu",
                static_cast<unsigned long long>(latestCounters_.chunksDrawn),
                static_cast<unsigned long long>(latestCounters_.chunksCulled),
                static_cast<unsigned long long>(latestCounters_.gpuCullDrawCommands));
    if (!latestExtra_.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(latestExtra_.c_str());
    }
}

void DebugOverlay::resetCaptureStats()
{
    frameMsHistory_.fill(0.0F);
    stageStreamingMs_.fill(0.0F);
    stageStreamPlanMs_.fill(0.0F);
    stageStreamDispatchMs_.fill(0.0F);
    stageStreamPipelineMs_.fill(0.0F);
    stageStreamEnqueueMs_.fill(0.0F);
    stagePlayerMs_.fill(0.0F);
    stageMeshInstallMs_.fill(0.0F);
    stageMeshDispatchMs_.fill(0.0F);
    stageLightingMs_.fill(0.0F);
    stageSaveMs_.fill(0.0F);
    stageSimulationMs_.fill(0.0F);
    stageRenderMs_.fill(0.0F);
    meshBuildAvgMs_.fill(0.0F);
    meshBuildMaxMs_.fill(0.0F);
    genAvgMs_.fill(0.0F);
    dirtyMeshQueue_.fill(0U);
    staleMeshes_.fill(0U);
    inFlightGeneration_.fill(0U);
    inFlightMesh_.fill(0U);
    residentChunks_.fill(0U);
    drawables_.fill(0U);
    writeIndex_ = 0;
    sampleCount_ = 0;
    totalSamples_ = 0;
    frameMsTotal_ = 0.0;
    worstFrameMs_ = 0.0F;
    worstFrame_ = {};
    spikes_.clear();
}

void DebugOverlay::startCsvCapture()
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(csvPath_).parent_path(), ec);
    csv_.open(csvPath_, std::ios::out | std::ios::trunc);
    if (!csv_) {
        csvCaptureEnabled_ = false;
        return;
    }
    csvCaptureEnabled_ = true;
    writeCsvHeader();
}

void DebugOverlay::stopCsvCapture()
{
    csvCaptureEnabled_ = false;
    if (csv_.is_open()) {
        csv_.flush();
        csv_.close();
    }
}

void DebugOverlay::writeCsvHeader()
{
    if (!csv_) {
        return;
    }
    csv_ << "sample,frame_ms,stream_ms,stream_plan_ms,stream_dispatch_ms,stream_prepass_ms,stream_pipeline_ms,stream_enqueue_ms,"
            "player_ms,mesh_install_ms,mesh_dispatch_ms,light_ms,save_ms,sim_ms,render_ms,"
            "gen_submitted,gen_completed,mesh_submitted,mesh_completed,mesh_stale,neighbor_remesh,"
            "resident,mesh_cache,dirty_mesh_q,inflight_gen,inflight_mesh,jobs_pending,gpu_uploads,upload_batches,upload_kb,draw_cmds\n";
}

void DebugOverlay::writeCsvSample(double frameMs, const core::RuntimeCounters& c)
{
    if (!csv_) {
        csvCaptureEnabled_ = false;
        return;
    }
    csv_ << totalSamples_ << ','
         << std::fixed << std::setprecision(3) << frameMs << ','
         << maxMs(c.stageStreaming) << ','
         << maxMs(c.stageStreamPlan) << ','
         << maxMs(c.stageStreamDispatch) << ','
         << maxMs(c.stageStreamPrepass) << ','
         << maxMs(c.stageStreamPipeline) << ','
         << maxMs(c.stageStreamEnqueue) << ','
         << maxMs(c.stagePlayer) << ','
         << maxMs(c.stageMeshInstall) << ','
         << maxMs(c.stageMeshDispatch) << ','
         << maxMs(c.stageLighting) << ','
         << maxMs(c.stageSave) << ','
         << maxMs(c.stageSimulation) << ','
         << maxMs(c.stageRender) << ','
         << c.generationJobsSubmitted << ','
         << c.generationJobsCompleted << ','
         << c.meshJobsSubmitted << ','
         << c.meshJobsCompleted << ','
         << c.meshJobsDiscardedStale << ','
         << c.remeshesCausedByNeighborInstall << ','
         << c.residentChunks << ','
         << c.meshCacheEntries << ','
         << c.dirtyMeshQueueLength << ','
         << c.inFlightGeneration << ','
         << c.inFlightMesh << ','
         << c.jobSystemPending << ','
         << c.gpuUploads << ','
         << c.uploadBatchCount << ','
         << (static_cast<double>(c.uploadBatchBytes) / 1024.0) << ','
         << c.gpuCullDrawCommands << '\n';
}

std::string DebugOverlay::formatSnapshotText() const
{
    // Same field layout used by `Application::logRuntimeStats` so the copied
    // text drops cleanly into bug reports / discord.
    std::ostringstream out;
    const auto& c = latestCounters_;
    out.precision(2);
    out << std::fixed;
    out << "frame_ms=" << latestFrameMs_
        << " samples=" << totalSamples_ << "\n";
    out << "stage_ms(stream/player/mesh_i/mesh_d/light/save/sim/render)="
        << maxMs(c.stageStreaming) << "/" << maxMs(c.stagePlayer) << "/"
        << maxMs(c.stageMeshInstall) << "/" << maxMs(c.stageMeshDispatch) << "/"
        << maxMs(c.stageLighting) << "/" << maxMs(c.stageSave) << "/"
        << maxMs(c.stageSimulation) << "/" << maxMs(c.stageRender) << "\n";
    out << "stream_breakdown_ms(plan/dispatch/prepass/pipeline/enqueue)="
        << maxMs(c.stageStreamPlan) << "/" << maxMs(c.stageStreamDispatch) << "/"
        << maxMs(c.stageStreamPrepass) << "/"
        << maxMs(c.stageStreamPipeline) << "/" << maxMs(c.stageStreamEnqueue) << "\n";
    out << "mesh_build_ms(avg/max)=" << avgMs(c.meshBuild) << "/" << maxMs(c.meshBuild) << "\n";
    out << "gen_ms(avg/max)=" << avgMs(c.terrainGeneration) << "/" << maxMs(c.terrainGeneration) << "\n";
    out << "queue_ms(avg/max)=" << avgMs(c.queueWait) << "/" << maxMs(c.queueWait) << "\n";
    out << "dirty_mesh_q=" << c.dirtyMeshQueueLength
        << " stale=" << c.meshJobsDiscardedStale
        << " neighbor_remesh=" << c.remeshesCausedByNeighborInstall << "\n";
    out << "in_flight(g/m/l)=" << c.inFlightGeneration << "/"
        << c.inFlightMesh << "/" << c.inFlightLighting << "\n";
    out << "resident=" << c.residentChunks
        << " mesh_cache=" << c.meshCacheEntries
        << " drawables=" << c.chunksMadeDrawable << "\n";
    out << "gpu_uploads=" << c.gpuUploads
        << " upload_batches=" << c.uploadBatchCount
        << " upload_kb=" << (static_cast<double>(c.uploadBatchBytes) / 1024.0) << "\n";
    out << "slow_frames=" << c.slowFrames
        << " workers=" << c.workerCount
        << " worst_frame_ms=" << worstFrameMs_
        << " spike_count=" << spikes_.size() << "\n";
    if (!latestExtra_.empty()) {
        out << latestExtra_ << "\n";
    }
    return out.str();
}

} // namespace voxel::render
