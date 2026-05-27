#include <voxel/render/VulkanRenderer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

#include <voxel/core/Logger.hpp>
#include <voxel/platform/GlfwWindow.hpp>
#include <voxel/world/CoordinateUtils.hpp>

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_glfw.h>

namespace voxel::render {

namespace {

constexpr std::array<const char*, 1> DeviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

void check(VkResult result, const char* message)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(message);
    }
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* extensionName)
{
    std::uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr), "Failed to enumerate device extensions");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()), "Failed to read device extensions");

    return std::any_of(extensions.begin(), extensions.end(), [extensionName](const VkExtensionProperties& extension) {
        return std::string{extension.extensionName} == extensionName;
    });
}

VmaAllocationCreateInfo allocationInfoFor(VkMemoryPropertyFlags properties) noexcept
{
    VmaAllocationCreateInfo allocInfo{};
    const bool hostVisible = (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    allocInfo.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocInfo.requiredFlags = properties;
    if (hostVisible) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }
    return allocInfo;
}

std::vector<char> readBinaryFile(const std::string& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open shader file: " + path);
    }

    const auto size = file.tellg();
    std::vector<char> buffer(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

} // namespace

void VulkanRenderer::initialize(const RendererConfig& config)
{
    static_assert(sizeof(ChunkSceneEntry) % 16 == 0,
        "ChunkSceneEntry is stored in a std430 array and must have 16-byte stride alignment.");
    static_assert(sizeof(CullPushConstants) == sizeof(core::Mat4) + 16,
        "CullPushConstants must match the shader push constant block.");
    static_assert(sizeof(PushConstants) == 128,
        "World draw push constants should stay at the broadly supported 128-byte minimum.");

    swapchainConfig_ = config;
    createInstance(config);
    createSurface(config);
    pickPhysicalDevice();
    createLogicalDevice();
    createAllocator();
    createSwapchain(config);
    createImageViews();
    createRenderPass();
    // J2b: descriptor set layout + SSBO + indirect command buffer must exist
    // before the graphics pipeline so the pipeline layout can reference them.
    createIndirectResources();
    createMaterialResources();
    // J3: cull compute pipeline depends on the indirect / origin buffers
    // created above.
    createCullResources();
    createGraphicsPipeline();
    createSkyPipeline();
    createDebugLinePipeline();
    createUiPipeline();
    createDepthResources();
    createFramebuffers();
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
    createUiResources();
    useGpuCull_ = config.enableGpuCulling;
    compareGpuCull_ = config.compareGpuCulling;

    // J1: allocate the shared chunk-mesh arenas. Sized for stress-test
    // workloads. Phase 7 (overhang terrain) produces ~20-30% more faces per
    // chunk in mountain/badlands biomes, so we doubled the index arena to
    // avoid the "Index arena full; chunk mesh discarded" cascade that
    // occurs when streaming 2000+ chunks at once.
    //
    // Sizing rationale: each visible quad uses 4 verts × 16B = 64B + 6 idx × 4B
    // = 24B. For 5000 chunks averaging ~6K faces, that's ~1.9 GB vert +
    // ~0.7 GB idx in the worst case. We rely on far-mesh eviction to keep
    // the working set well below that.
    constexpr VkDeviceSize kVertexArenaBytes = 384ULL * 1024ULL * 1024ULL; // 384 MB
    constexpr VkDeviceSize kIndexArenaBytes  = 192ULL * 1024ULL * 1024ULL; // 192 MB
    vertexArena_.initialize(device_, allocator_, kVertexArenaBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    indexArena_.initialize(device_, allocator_, kIndexArenaBytes,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    // L: shared staging arena — one ring-buffer slot per in-flight frame.
    const auto stagingArena = createBuffer(
        kStagingSlotSize * kFramesInFlight,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    stagingArenaBuffer_ = stagingArena.buffer;
    stagingArenaAllocation_ = stagingArena.allocation;
    check(vmaMapMemory(allocator_, stagingArenaAllocation_,
        reinterpret_cast<void**>(&stagingArenaMapped_)),
        "Failed to map staging arena buffer");

    initialized_ = true;
    graph_.addPass({"chunk_mesh_upload"});
    graph_.addPass({"gpu_culling"});
    graph_.addPass({"procedural_sky"});
    graph_.addPass({"voxel_opaque"});
    graph_.addPass({"transparent_and_particles"});
    graph_.addPass({"ui"});
    graph_.addPass({"debug"});
    Logger::info(std::string{"Initialized Vulkan renderer boundary; gpu_cull="}
        + (useGpuCull_ ? "on" : "off")
        + " compare=" + (compareGpuCull_ ? "on" : "off"));
}

void VulkanRenderer::beginFrame()
{
    if (!initialized_) {
        return;
    }
    if (swapchainConfig_.window != nullptr) {
        const auto framebuffer = swapchainConfig_.window->framebufferExtent();
        if (framebuffer.width == 0 || framebuffer.height == 0) {
            return;
        }
        if (framebuffer.width != swapchainExtent_.width || framebuffer.height != swapchainExtent_.height) {
            recreateSwapchain();
        }
    }

    // K: pick the current frame slot and wait *only* on its own fence. The
    // other frame may still have work in flight on the GPU; we don't touch it.
    FrameContext& frame = frames_[currentFrame_];

    const auto fenceWaitStart = std::chrono::steady_clock::now();
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, std::numeric_limits<std::uint64_t>::max()),
        "Failed to wait for frame fence");
    const auto fenceWaitUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - fenceWaitStart).count());
    frameStats_.rendererFenceWaitUs += fenceWaitUs;
    if (fenceWaitUs > frameStats_.rendererFenceWaitMaxUs) {
        frameStats_.rendererFenceWaitMaxUs = fenceWaitUs;
    }
    collectCompletedGpuCullStats(frame);
    flushRetiredBuffers();
    flushRetiredSlices();

    // L: reset the staging arena bump pointer to this frame's slot. Safe
    // because the frame fence above guarantees the GPU is done reading it.
    stagingSlotOffset_ = 0;

    const auto acquireResult = vkAcquireNextImageKHR(device_, swapchain_, std::numeric_limits<std::uint64_t>::max(),
        frame.imageAvailable, VK_NULL_HANDLE, &acquiredImageIndex_);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    check(vkResetFences(device_, 1, &frame.inFlight), "Failed to reset frame fence");
    check(vkResetCommandBuffer(frame.commandBuffer, 0), "Failed to reset command buffer");
}

void VulkanRenderer::recreateSwapchain()
{
    if (!initialized_ || device_ == VK_NULL_HANDLE) {
        return;
    }
    if (swapchainConfig_.window != nullptr) {
        const auto framebuffer = swapchainConfig_.window->framebufferExtent();
        if (framebuffer.width == 0 || framebuffer.height == 0) {
            return;
        }
    }

    vkDeviceWaitIdle(device_);
    cleanupSwapchain();
    createSwapchain(swapchainConfig_);
    createImageViews();
    createRenderPass();
    createGraphicsPipeline();
    createSkyPipeline();
    createDebugLinePipeline();
    createUiPipeline();
    createDepthResources();
    createFramebuffers();
    for (auto& frame : frames_) {
        frame.sceneGeneration = 0;
    }
}

void VulkanRenderer::endFrame()
{
    if (!initialized_) {
        return;
    }

    FrameContext& frame = frames_[currentFrame_];
    recordFrameCommands(frame.commandBuffer, acquiredImageIndex_);

    const VkSemaphore waitSemaphores[] = {frame.imageAvailable};
    const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const VkSemaphore signalSemaphores[] = {frame.renderFinished};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    check(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frame.inFlight), "Failed to submit graphics queue");

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &acquiredImageIndex_;

    const auto presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
        ++frameIndex_;
        currentFrame_ = (currentFrame_ + 1U) % kFramesInFlight;
        return;
    }
    if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }
    ++frameIndex_;
    currentFrame_ = (currentFrame_ + 1U) % kFramesInFlight;
}

void VulkanRenderer::initializeImGui(platform::IWindow& window)
{
    if (!initialized_ || imguiInitialized_) {
        return;
    }
    // The GLFW backend needs the native window handle. The renderer normally
    // works against the abstract IWindow, but ImGui is GLFW-specific so we
    // need a concrete type here. dynamic_cast keeps the dependency narrow.
    auto* glfwWindow = dynamic_cast<platform::GlfwWindow*>(&window);
    if (glfwWindow == nullptr) {
        Logger::warn("ImGui initialization skipped: window is not a GlfwWindow.");
        return;
    }

    // Descriptor pool — ImGui v1.91 only allocates a single combined-image
    // sampler descriptor for the font texture. We keep a small pool to leave
    // room for future textures (icons, custom widgets).
    {
        const VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 8;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(std::size(poolSizes));
        poolInfo.pPoolSizes = poolSizes;
        check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &imguiDescriptorPool_),
              "Failed to create ImGui descriptor pool");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // don't write imgui.ini next to the exe
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(glfwWindow->nativeHandle(), true);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = instance_;
    initInfo.PhysicalDevice = physicalDevice_;
    initInfo.Device = device_;
    initInfo.QueueFamily = queueFamilies_.graphics.value();
    initInfo.Queue = graphicsQueue_;
    initInfo.DescriptorPool = imguiDescriptorPool_;
    initInfo.RenderPass = renderPass_;
    initInfo.MinImageCount = static_cast<std::uint32_t>(swapchainImages_.size());
    initInfo.ImageCount = static_cast<std::uint32_t>(swapchainImages_.size());
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.Subpass = 0;
    initInfo.Allocator = nullptr;
    initInfo.CheckVkResultFn = nullptr;
    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        Logger::warn("ImGui_ImplVulkan_Init returned false; aborting ImGui setup.");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return;
    }

    imguiInitialized_ = true;
    Logger::info("ImGui initialized.");
}

void VulkanRenderer::beginImGuiFrame()
{
    if (!imguiInitialized_) {
        return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void VulkanRenderer::endImGuiFrame()
{
    if (!imguiInitialized_) {
        return;
    }
    // Just builds the draw data; the actual draw call happens inside
    // recordFrameCommands() so it shares the world render pass.
    ImGui::Render();
}

void VulkanRenderer::shutdown()
{
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    // Tear down ImGui first — its draw resources reference the device.
    if (imguiInitialized_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }
    if (imguiDescriptorPool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, imguiDescriptorPool_, nullptr);
        imguiDescriptorPool_ = VK_NULL_HANDLE;
    }

    clearUploadedMeshes();
    destroyAllRetiredBuffers();
    // J1: release shared mesh arenas. Safe here because vkDeviceWaitIdle
    // above already ensured the GPU is no longer reading them.
    vertexArena_.shutdown();
    indexArena_.shutdown();

    // L: release the shared staging arena.
    if (stagingArenaMapped_ != nullptr) {
        vmaUnmapMemory(allocator_, stagingArenaAllocation_);
        stagingArenaMapped_ = nullptr;
    }
    GpuBuffer stagingBuf{stagingArenaBuffer_, stagingArenaAllocation_, kStagingSlotSize * kFramesInFlight};
    destroyBuffer(stagingBuf);
    stagingArenaBuffer_ = VK_NULL_HANDLE;
    stagingArenaAllocation_ = VK_NULL_HANDLE;
    // J3 must be torn down before J2b because the cull descriptors reference
    // the indirect / origin buffers.
    destroyUiResources();
    destroyCullResources();
    destroyMaterialResources();
    destroyIndirectResources();

    // K: tear down per-frame sync objects.
    for (auto& frame : frames_) {
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
            frame.imageAvailable = VK_NULL_HANDLE;
        }
        if (frame.renderFinished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frame.renderFinished, nullptr);
            frame.renderFinished = VK_NULL_HANDLE;
        }
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(device_, frame.inFlight, nullptr);
            frame.inFlight = VK_NULL_HANDLE;
        }
        frame.commandBuffer = VK_NULL_HANDLE; // freed implicitly with the pool below
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    cleanupSwapchain();
    if (device_ != VK_NULL_HANDLE) {
        if (allocator_ != VK_NULL_HANDLE) {
            vmaDestroyAllocator(allocator_);
            allocator_ = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
    graph_.clear();
}

RenderGraph& VulkanRenderer::graph() noexcept
{
    return graph_;
}

void VulkanRenderer::updateDebugCamera(const platform::IWindow& window, float deltaSeconds) noexcept
{
    debugCamera_.updateFromInput(window, deltaSeconds);
}

void VulkanRenderer::setDebugCameraPose(core::Vec3 position, float yawRadians, float pitchRadians) noexcept
{
    debugCamera_.setPose(position, yawRadians, pitchRadians);
}

void VulkanRenderer::setDebugCameraPose(core::DVec3 position, float yawRadians, float pitchRadians) noexcept
{
    debugCamera_.setPose(position, yawRadians, pitchRadians);
}

void VulkanRenderer::setCameraFarPlane(float farPlane) noexcept
{
    debugCamera_.setFarPlane(farPlane);
}

core::Vec3 VulkanRenderer::debugCameraPosition() const noexcept
{
    return debugCamera_.position();
}

core::DVec3 VulkanRenderer::debugCameraDPosition() const noexcept
{
    return debugCamera_.dPosition();
}

core::Vec3 VulkanRenderer::debugCameraForward() const noexcept
{
    return debugCamera_.forwardVector();
}

platform::WindowExtent VulkanRenderer::drawableExtent() const noexcept
{
    return {swapchainExtent_.width, swapchainExtent_.height};
}

VulkanRenderer::PreparedMeshUpload
VulkanRenderer::prepareChunkMeshUpload(world::ChunkCoord coord, const meshing::ChunkMesh& mesh)
{
    PreparedMeshUpload out{};
    if (!initialized_ || mesh.vertices.empty() || mesh.indices.empty()) {
        return out;
    }
    const auto uploadStart = std::chrono::steady_clock::now();

    const auto existing = uploadedMeshes_.find(coord);

    if (existing != uploadedMeshes_.end()
        && existing->second.revision == mesh.sourceRevision
        && existing->second.meshRevisionHash == mesh.sourceMeshRevisionHash) {
        ++frameStats_.duplicateUploadSkips;
        out.duplicateSkip = true;
        return out;
    }

    const VkDeviceSize vertexBytes = sizeof(mesh.vertices[0]) * mesh.vertices.size();
    const VkDeviceSize indexBytes = sizeof(mesh.indices[0]) * mesh.indices.size();
    const auto uploadedBytes = static_cast<std::uint64_t>(vertexBytes + indexBytes);
    if (stagingSlotOffset_ + vertexBytes + indexBytes > kStagingSlotSize) {
        Logger::warn("Staging arena full for this frame; chunk mesh upload skipped");
        return out;
    }
    if (existing != uploadedMeshes_.end()) {
        const auto retireFrame = frameIndex_ + kFramesInFlight + 1U;
        retiredVertexSlices_.push_back({existing->second.vertexSlice, retireFrame});
        retiredIndexSlices_.push_back({existing->second.indexSlice, retireFrame});
        removeSceneEntriesForChunk(existing->first);
        uploadedMeshes_.erase(existing);
    }

    BufferArena::Slice vertexSlice{};
    BufferArena::Slice indexSlice{};
    constexpr VkDeviceSize kVertexAlignment = sizeof(meshing::VoxelVertex); // 16 bytes
    constexpr VkDeviceSize kIndexAlignment  = sizeof(std::uint32_t);        // 4 bytes
    if (!vertexArena_.allocate(vertexBytes, kVertexAlignment, vertexSlice)) {
        Logger::warn("Vertex arena full; chunk mesh discarded");
        return out;
    }
    if (!indexArena_.allocate(indexBytes, kIndexAlignment, indexSlice)) {
        Logger::warn("Index arena full; chunk mesh discarded");
        vertexArena_.release(vertexSlice);
        return out;
    }

    // Reserve staging slots — bump-allocate without memcpying. The caller
    // (or a worker thread the caller dispatches) will write into these
    // destinations. We still push to pendingUploads_ here because the
    // GPU-side vkCmdCopyBuffer only reads at submit time, by which point
    // the memcpy will have completed.
    const VkDeviceSize slotBase = currentFrame_ * kStagingSlotSize;
    const VkDeviceSize stagingVertexOffset = stagingSlotOffset_;
    const VkDeviceSize stagingIndexOffset = stagingVertexOffset + vertexBytes;
    stagingSlotOffset_ = stagingIndexOffset + indexBytes;
    frameStats_.stagingUploadBytes += static_cast<std::uint64_t>(vertexBytes + indexBytes);
    pendingUploads_.push_back(PendingUpload{slotBase + stagingVertexOffset,
                                             vertexArena_.buffer(),
                                             vertexSlice.offset,
                                             vertexBytes});
    pendingUploads_.push_back(PendingUpload{slotBase + stagingIndexOffset,
                                             indexArena_.buffer(),
                                             indexSlice.offset,
                                             indexBytes});

    out.valid = true;
    out.vertexDst = stagingArenaMapped_ + slotBase + stagingVertexOffset;
    out.indexDst = stagingArenaMapped_ + slotBase + stagingIndexOffset;
    out.vertexSrc = mesh.vertices.data();
    out.indexSrc = mesh.indices.data();
    out.vertexBytes = static_cast<std::size_t>(vertexBytes);
    out.indexBytes = static_cast<std::size_t>(indexBytes);

    const auto indexBase = static_cast<std::uint32_t>(indexSlice.offset / sizeof(std::uint32_t));
    const auto vertexOff = static_cast<std::int32_t>(vertexSlice.offset / sizeof(meshing::VoxelVertex));

    UploadedChunkMesh uploaded;
    uploaded.coord = coord;
    uploaded.revision = mesh.sourceRevision;
    uploaded.meshRevisionHash = mesh.sourceMeshRevisionHash;
    uploaded.vertexSlice = vertexSlice;
    uploaded.indexSlice = indexSlice;
    uploaded.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    uploaded.indexCount = static_cast<std::uint32_t>(mesh.indices.size());

    uploaded.opaqueIndexCount = 0;
    uploaded.opaqueFirstIndex = 0;
    uploaded.cutoutIndexCount = 0;
    uploaded.cutoutFirstIndex = 0;
    uploaded.transparentIndexCount = 0;
    uploaded.transparentFirstIndex = 0;

    // Single-section path: one ChunkSceneEntry per chunk. The chunk coord IS
    // the scene-entry key (no y-doubling), which eliminates the previous
    // `chunkToSections_` multimap entirely.
    {
        const auto& sec = mesh.sections[0];
        ChunkSceneEntry entry{};
        entry.origin[0] = static_cast<float>(coord.x * world::ChunkSize);
        entry.origin[1] = static_cast<float>(coord.y * world::ChunkSize);
        entry.origin[2] = static_cast<float>(coord.z * world::ChunkSize);
        entry.origin[3] = 0.0F;
        entry.boundsMin[0] = entry.origin[0] - 1.0F;
        entry.boundsMin[1] = entry.origin[1] - 1.0F;
        entry.boundsMin[2] = entry.origin[2] - 1.0F;
        entry.boundsMin[3] = 0.0F;
        entry.extent[0] = static_cast<float>(world::ChunkSize) + 2.0F;
        entry.extent[1] = static_cast<float>(world::ChunkSize) + 2.0F;
        entry.extent[2] = static_cast<float>(world::ChunkSize) + 2.0F;
        entry.extent[3] = 0.0F;
        entry.indexCount = sec.opaqueIndexCount;
        entry.firstIndex = indexBase + sec.opaqueFirstIndex;
        entry.vertexOffset = vertexOff;
        entry.cutoutIndexCount = sec.cutoutIndexCount;
        entry.cutoutFirstIndex = indexBase + sec.cutoutFirstIndex;
        entry.cutoutVertexOffset = vertexOff;
        entry.transparentIndexCount = sec.transparentIndexCount;
        entry.transparentFirstIndex = indexBase + sec.transparentFirstIndex;
        entry.transparentVertexOffset = vertexOff;
        entry.pad0 = 0;
        entry.pad1 = 0;
        entry.pad2 = 0;

        uploaded.opaqueIndexCount = sec.opaqueIndexCount;
        uploaded.cutoutIndexCount = sec.cutoutIndexCount;
        uploaded.transparentIndexCount = sec.transparentIndexCount;
        uploaded.opaqueFirstIndex = indexBase + sec.opaqueFirstIndex;
        uploaded.cutoutFirstIndex = indexBase + sec.cutoutFirstIndex;
        uploaded.transparentFirstIndex = indexBase + sec.transparentFirstIndex;

        upsertSceneEntryForCoord(coord, entry);
    }
    uploadedMeshes_.emplace(coord, uploaded);
    ++pendingDrawableChunks_;
    const auto uploadUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - uploadStart).count());
    ++frameStats_.gpuUploads;
    frameStats_.gpuUploadTimeUs += uploadUs;
    if (uploadUs > frameStats_.gpuUploadMaxUs) {
        frameStats_.gpuUploadMaxUs = uploadUs;
    }
    // Staging bytes were accounted for above when slots were reserved.
    totalUploadedMeshBytes_ += uploadedBytes;
    return out;
}

bool VulkanRenderer::uploadChunkMesh(world::ChunkCoord coord, const meshing::ChunkMesh& mesh)
{
    // Backwards-compatible synchronous path. Prepare on main thread, then do
    // the memcpys inline. Callers that want to parallelise memcpy across
    // worker threads should use `prepareChunkMeshUpload` directly.
    auto prepared = prepareChunkMeshUpload(coord, mesh);
    if (prepared.duplicateSkip) {
        return true;
    }
    if (!prepared.valid) {
        return false;
    }
    std::memcpy(prepared.vertexDst, prepared.vertexSrc, prepared.vertexBytes);
    std::memcpy(prepared.indexDst, prepared.indexSrc, prepared.indexBytes);
    return true;
}

void VulkanRenderer::removeUploadedMesh(world::ChunkCoord coord)
{
    const auto existing = uploadedMeshes_.find(coord);
    if (existing == uploadedMeshes_.end()) {
        return;
    }
    removeSceneEntriesForChunk(existing->first);
    const auto retireFrame = frameIndex_ + kFramesInFlight + 1U;
    retiredVertexSlices_.push_back({existing->second.vertexSlice, retireFrame});
    retiredIndexSlices_.push_back({existing->second.indexSlice, retireFrame});
    uploadedMeshes_.erase(existing);
}

void VulkanRenderer::setDebugBlockOutline(std::optional<world::PlanetCoord> block)
{
    if (!initialized_) {
        return;
    }
    if (debugLineMesh_.block == block) {
        return;
    }

    retireBuffer(debugLineMesh_.vertices);
    retireBuffer(debugLineMesh_.indices);
    debugLineMesh_ = {};
    debugLineMesh_.block = block;
    if (!block.has_value()) {
        return;
    }

    constexpr std::uint32_t kMaterial = 99U;
    const std::array<meshing::VoxelVertex, 8> vertices{{
        {0U | (0U << 6U) | (0U << 12U), 0, 255, kMaterial},
        {1U | (0U << 6U) | (0U << 12U), 0, 255, kMaterial},
        {1U | (1U << 6U) | (0U << 12U), 0, 255, kMaterial},
        {0U | (1U << 6U) | (0U << 12U), 0, 255, kMaterial},
        {0U | (0U << 6U) | (1U << 12U), 0, 255, kMaterial},
        {1U | (0U << 6U) | (1U << 12U), 0, 255, kMaterial},
        {1U | (1U << 6U) | (1U << 12U), 0, 255, kMaterial},
        {0U | (1U << 6U) | (1U << 12U), 0, 255, kMaterial},
    }};
    const std::array<std::uint32_t, 24> indices{{
        0, 1, 1, 2, 2, 3, 3, 0,
        4, 5, 5, 6, 6, 7, 7, 4,
        0, 4, 1, 5, 2, 6, 3, 7,
    }};

    const VkDeviceSize vertexBytes = sizeof(vertices[0]) * vertices.size();
    const VkDeviceSize indexBytes = sizeof(indices[0]) * indices.size();
    debugLineMesh_.vertices = createBuffer(
        vertexBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    debugLineMesh_.indices = createBuffer(
        indexBytes,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(debugLineMesh_.vertices, vertices.data(), vertexBytes);
    uploadViaStaging(debugLineMesh_.indices, indices.data(), indexBytes);
    debugLineMesh_.indexCount = static_cast<std::uint32_t>(indices.size());
}

void VulkanRenderer::setUiOverlay(std::vector<UiRect> rects)
{
    if (rects.size() > kMaxUiRects) {
        rects.resize(kMaxUiRects);
    }
    uiOverlayRects_ = std::move(rects);
}

void VulkanRenderer::setClearColor(float r, float g, float b, float a) noexcept
{
    clearColor_[0] = std::clamp(r, 0.0F, 1.0F);
    clearColor_[1] = std::clamp(g, 0.0F, 1.0F);
    clearColor_[2] = std::clamp(b, 0.0F, 1.0F);
    clearColor_[3] = std::clamp(a, 0.0F, 1.0F);
}

void VulkanRenderer::setAtmosphereSettings(AtmosphereSettings settings) noexcept
{
    settings.fogNear = std::clamp(settings.fogNear, 0.0F, 8192.0F);
    settings.fogFar = std::clamp(settings.fogFar, settings.fogNear + 1.0F, 65536.0F);
    settings.fogStrength = std::clamp(settings.fogStrength, 0.0F, 1.5F);
    settings.farLightLift = std::clamp(settings.farLightLift, 0.0F, 1.0F);
    atmosphereSettings_ = settings;
}

void VulkanRenderer::setSkySettings(SkySettings settings) noexcept
{
    settings.horizonLift = std::clamp(settings.horizonLift, -0.40F, 0.80F);
    settings.saturation = std::clamp(settings.saturation, 0.0F, 2.0F);
    settings.cloudStrength = std::clamp(settings.cloudStrength, 0.0F, 1.25F);
    settings.brightness = std::clamp(settings.brightness, 0.25F, 2.0F);
    skySettings_ = settings;
}

void VulkanRenderer::clearUploadedMeshes()
{
    pendingUploads_.clear();
    pendingDrawableChunks_ = 0;

    for (auto& [coord, mesh] : uploadedMeshes_) {
        vertexArena_.release(mesh.vertexSlice);
        indexArena_.release(mesh.indexSlice);
    }
    uploadedMeshes_.clear();
    sceneEntries_.clear();
    sceneCoords_.clear();
    sceneEntryGenerations_.clear();
    sceneEntryIndex_.clear();
    nextSceneEntryGeneration_ = 1;
    ++sceneGeneration_;
    retiredVertexSlices_.clear();
    retiredIndexSlices_.clear();
    destroyBuffer(debugLineMesh_.vertices);
    destroyBuffer(debugLineMesh_.indices);
    debugLineMesh_ = {};
}

void VulkanRenderer::uploadMaterialTable(const std::vector<MaterialGpuData>& materials)
{
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(materials.size()) * sizeof(MaterialGpuData);
    uploadViaStaging(materialTableBuffer_, materials.data(), bytes);
}

VulkanRenderer::FrameStats VulkanRenderer::drainFrameStats() noexcept
{
    const auto stats = frameStats_;
    frameStats_ = {};
    return stats;
}

std::uint64_t VulkanRenderer::totalUploadedMeshBytes() const noexcept
{
    return totalUploadedMeshBytes_;
}

void VulkanRenderer::createInstance(const RendererConfig& config)
{
    if (config.window == nullptr) {
        throw std::runtime_error("VulkanRenderer requires a platform window");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = config.appName.data();
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "AetherForge: Infinite Creation";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    const auto extensions = config.window->requiredVulkanExtensions();
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    check(vkCreateInstance(&createInfo, nullptr, &instance_), "Failed to create Vulkan instance");
}

void VulkanRenderer::createSurface(const RendererConfig& config)
{
    surface_ = config.window->createVulkanSurface(instance_);
}

void VulkanRenderer::pickPhysicalDevice()
{
    std::uint32_t deviceCount = 0;
    check(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "Failed to enumerate physical devices");
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan physical devices found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    check(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "Failed to read physical devices");

    for (const auto device : devices) {
        if (deviceSuitable(device)) {
            physicalDevice_ = device;
            queueFamilies_ = findQueueFamilies(device);
            supportsDrawIndirectCount_ = true;
            return;
        }
    }

    throw std::runtime_error("No suitable Vulkan physical device found");
}

void VulkanRenderer::createLogicalDevice()
{
    const std::set<std::uint32_t> uniqueFamilies{queueFamilies_.graphics.value(), queueFamilies_.present.value()};
    const float priority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    queueInfos.reserve(uniqueFamilies.size());

    for (const auto family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        queueInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceFeatures features{};
    // J2b: required for vkCmdDrawIndexedIndirect with drawCount > 1.
    features.multiDrawIndirect = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.drawIndirectCount = VK_TRUE;
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features12;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = queueInfos.data();
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(DeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = DeviceExtensions.data();

    check(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "Failed to create Vulkan device");
    vkGetDeviceQueue(device_, queueFamilies_.graphics.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, queueFamilies_.present.value(), 0, &presentQueue_);
}

void VulkanRenderer::createAllocator()
{
    VmaAllocatorCreateInfo createInfo{};
    createInfo.flags = VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
    createInfo.physicalDevice = physicalDevice_;
    createInfo.device = device_;
    createInfo.instance = instance_;
    createInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    check(vmaCreateAllocator(&createInfo, &allocator_), "Failed to create Vulkan memory allocator");
}

void VulkanRenderer::createSwapchain(const RendererConfig& config)
{
    const auto support = querySwapchainSupport(physicalDevice_);
    const auto surfaceFormat = chooseSurfaceFormat(support.formats);
    const auto presentMode = choosePresentMode(support.presentModes);
    const auto extent = chooseSwapExtent(support.capabilities, config);

    std::uint32_t imageCount = support.capabilities.minImageCount + 1U;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const std::uint32_t familyIndices[] = {queueFamilies_.graphics.value(), queueFamilies_.present.value()};
    if (queueFamilies_.graphics != queueFamilies_.present) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = familyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    check(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "Failed to create swapchain");

    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr), "Failed to count swapchain images");
    swapchainImages_.resize(imageCount);
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data()), "Failed to get swapchain images");

    swapchainFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;
    debugCamera_.setAspect(static_cast<float>(extent.width) / static_cast<float>(extent.height == 0 ? 1 : extent.height));
}

void VulkanRenderer::createImageViews()
{
    swapchainImageViews_.resize(swapchainImages_.size());
    for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapchainImages_[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapchainFormat_;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        check(vkCreateImageView(device_, &createInfo, nullptr, &swapchainImageViews_[i]), "Failed to create swapchain image view");
    }
}

void VulkanRenderer::createRenderPass()
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    depthFormat_ = findDepthFormat();
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat_;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorReference{};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthReference{};
    depthReference.attachment = 1;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = &depthReference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    const std::array<VkAttachmentDescription, 2> attachments{colorAttachment, depthAttachment};

    VkRenderPassCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    createInfo.pAttachments = attachments.data();
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dependency;

    check(vkCreateRenderPass(device_, &createInfo, nullptr, &renderPass_), "Failed to create render pass");
}

void VulkanRenderer::createGraphicsPipeline()
{
    // J2b: chunk pipeline uses the SSBO-based shader. The debug-line pipeline
    // still uses the original voxel.vert (push-constant origin).
    const auto vertCode = readBinaryFile(std::string{VOXEL_SHADER_DIR} + "/voxel_chunk.vert.spv");
    const auto fragCode = readBinaryFile(std::string{VOXEL_SHADER_DIR} + "/voxel.frag.spv");
    const auto vertShader = createShaderModule(vertCode);
    const auto fragShader = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertShader;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragShader;
    fragStage.pName = "main";

    const VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(meshing::VoxelVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributes{};
    attributes[0] = {0, 0, VK_FORMAT_R32_UINT, offsetof(meshing::VoxelVertex, packedPos)};
    attributes[1] = {1, 0, VK_FORMAT_R32_UINT, offsetof(meshing::VoxelVertex, packedUv)};
    attributes[2] = {2, 0, VK_FORMAT_R32_UINT, offsetof(meshing::VoxelVertex, packedLight)};
    attributes[3] = {3, 0, VK_FORMAT_R32_UINT, offsetof(meshing::VoxelVertex, packedMaterial)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstants{};
    pushConstants.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstants.offset = 0;
    pushConstants.size = sizeof(PushConstants);

    const std::array<VkDescriptorSetLayout, 2> setLayouts{chunkDescriptorSetLayout_, materialDescriptorSetLayout_};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstants;
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_), "Failed to create pipeline layout");

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline_), "Failed to create graphics pipeline");

    depthStencil.depthWriteEnable = VK_FALSE;
    check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &transparentPipeline_),
          "Failed to create transparent graphics pipeline");

    vkDestroyShaderModule(device_, fragShader, nullptr);
    vkDestroyShaderModule(device_, vertShader, nullptr);
}

void VulkanRenderer::createDebugLinePipeline()
{
    const auto vertCode = readBinaryFile(std::string{VOXEL_SHADER_DIR} + "/voxel.vert.spv");
    const auto fragCode = readBinaryFile(std::string{VOXEL_SHADER_DIR} + "/debug_line.frag.spv");
    const auto vertShader = createShaderModule(vertCode);
    const auto fragShader = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertShader;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragShader;
    fragStage.pName = "main";

    const VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(meshing::VoxelVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributes{};
    attributes[0] = {0, 0, VK_FORMAT_R32_UINT, offsetof(meshing::VoxelVertex, packedPos)};
    attributes[1] = {1, 0, VK_FORMAT_R32_UINT, offsetof(meshing::VoxelVertex, packedUv)};
    attributes[2] = {2, 0, VK_FORMAT_R32_UINT, offsetof(meshing::VoxelVertex, packedLight)};
    attributes[3] = {3, 0, VK_FORMAT_R32_UINT, offsetof(meshing::VoxelVertex, packedMaterial)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &debugLinePipeline_),
          "Failed to create debug line pipeline");

    vkDestroyShaderModule(device_, fragShader, nullptr);
    vkDestroyShaderModule(device_, vertShader, nullptr);
}

void VulkanRenderer::createSkyPipeline()
{
    const auto vertCode = readBinaryFile(std::string{VOXEL_SHADER_DIR} + "/sky.vert.spv");
    const auto fragCode = readBinaryFile(std::string{VOXEL_SHADER_DIR} + "/sky.frag.spv");
    const auto vertShader = createShaderModule(vertCode);
    const auto fragShader = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertShader;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragShader;
    fragStage.pName = "main";

    const VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstants{};
    pushConstants.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstants.offset = 0;
    pushConstants.size = sizeof(SkyPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstants;
    check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &skyPipelineLayout_),
        "Failed to create sky pipeline layout");

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = skyPipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skyPipeline_),
        "Failed to create sky pipeline");

    vkDestroyShaderModule(device_, fragShader, nullptr);
    vkDestroyShaderModule(device_, vertShader, nullptr);
}

void VulkanRenderer::createUiPipeline()
{
    const auto vertCode = readBinaryFile(std::string{VOXEL_SHADER_DIR} + "/ui.vert.spv");
    const auto fragCode = readBinaryFile(std::string{VOXEL_SHADER_DIR} + "/ui.frag.spv");
    const auto vertShader = createShaderModule(vertCode);
    const auto fragShader = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertShader;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragShader;
    fragStage.pName = "main";

    const VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(UiVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, position)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UiVertex, color)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &uiPipelineLayout_), "Failed to create UI pipeline layout");

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = uiPipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &uiPipeline_),
        "Failed to create UI pipeline");

    vkDestroyShaderModule(device_, fragShader, nullptr);
    vkDestroyShaderModule(device_, vertShader, nullptr);
}

void VulkanRenderer::createDepthResources()
{
    createImage(
        swapchainExtent_.width,
        swapchainExtent_.height,
        depthFormat_,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        depthImage_,
        depthAllocation_);
    depthImageView_ = createImageView(depthImage_, depthFormat_, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void VulkanRenderer::createFramebuffers()
{
    framebuffers_.resize(swapchainImageViews_.size());
    for (std::size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        const VkImageView attachments[] = {swapchainImageViews_[i], depthImageView_};

        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = renderPass_;
        createInfo.attachmentCount = 2;
        createInfo.pAttachments = attachments;
        createInfo.width = swapchainExtent_.width;
        createInfo.height = swapchainExtent_.height;
        createInfo.layers = 1;

        check(vkCreateFramebuffer(device_, &createInfo, nullptr, &framebuffers_[i]), "Failed to create framebuffer");
    }
}

void VulkanRenderer::createCommandPool()
{
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = queueFamilies_.graphics.value();

    check(vkCreateCommandPool(device_, &createInfo, nullptr, &commandPool_), "Failed to create command pool");
}

void VulkanRenderer::createCommandBuffers()
{
    // K: one command buffer per *frame in flight*, not per swapchain image.
    // Frame N+1's CPU may want to record into its command buffer while frame
    // N's GPU is still executing — giving each frame its own buffer prevents
    // that race regardless of which swapchain image got acquired.
    std::array<VkCommandBuffer, kFramesInFlight> buffers{};
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<std::uint32_t>(buffers.size());

    check(vkAllocateCommandBuffers(device_, &allocInfo, buffers.data()),
        "Failed to allocate per-frame command buffers");

    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        frames_[i].commandBuffer = buffers[i];
    }
}

void VulkanRenderer::createSyncObjects()
{
    // K: per-frame sync. Each frame owns its own pair of semaphores and a
    // fence; the CPU can queue up to kFramesInFlight frames before having to
    // wait on the oldest fence at the start of beginFrame.
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto& frame : frames_) {
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable),
            "Failed to create per-frame image semaphore");
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.renderFinished),
            "Failed to create per-frame render semaphore");
        check(vkCreateFence(device_, &fenceInfo, nullptr, &frame.inFlight),
            "Failed to create per-frame fence");
    }
}

void VulkanRenderer::createIndirectResources()
{
    // K: descriptor set layouts. Layouts are immutable and shared by every
    // frame; only the allocated descriptor sets (and the buffers they bind)
    // need to be per-frame.

    // Chunk (vertex shader): 1 storage buffer = chunk origins.
    VkDescriptorSetLayoutBinding chunkBinding{};
    chunkBinding.binding = 0;
    chunkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunkBinding.descriptorCount = 1;
    chunkBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo chunkLayout{};
    chunkLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    chunkLayout.bindingCount = 1;
    chunkLayout.pBindings = &chunkBinding;
    check(vkCreateDescriptorSetLayout(device_, &chunkLayout, nullptr, &chunkDescriptorSetLayout_),
        "Failed to create chunk descriptor set layout");

    // Cull (compute): scene, compact indirect commands, compact origins, and draw counts.
    std::array<VkDescriptorSetLayoutBinding, 4> cullBindings{};
    for (std::uint32_t i = 0; i < cullBindings.size(); ++i) {
        cullBindings[i].binding = i;
        cullBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        cullBindings[i].descriptorCount = 1;
        cullBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo cullLayout{};
    cullLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    cullLayout.bindingCount = static_cast<std::uint32_t>(cullBindings.size());
    cullLayout.pBindings = cullBindings.data();
    check(vkCreateDescriptorSetLayout(device_, &cullLayout, nullptr, &cullDescriptorSetLayout_),
        "Failed to create cull descriptor set layout");

    // Shared descriptor pool. Sizes:
    //   - kFramesInFlight × (1 chunk set + 1 cull set) + 1 material set
    //   - reserve +4 sets and +8 storage descriptors for compute extensions
    //     (FluidGpuSystem uses 1 set + 3 storage descriptors; the rest is
    //     headroom for future compute systems without re-creating the pool).
    constexpr std::uint32_t kComputeExtensionSets = 4U;
    constexpr std::uint32_t kComputeExtensionStorageDescriptors = 8U;
    const std::uint32_t maxSets = kFramesInFlight * 2U + 1U + kComputeExtensionSets;
    const std::uint32_t storageDescriptors = kFramesInFlight * (1U + 4U) + 1U
                                           + kComputeExtensionStorageDescriptors;
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = storageDescriptors;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = maxSets;
    check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_),
        "Failed to create descriptor pool");

    const VkDeviceSize commandBytes = static_cast<VkDeviceSize>(kMaxIndirectCommands) * sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize originBytes = static_cast<VkDeviceSize>(kMaxIndirectCommands) * sizeof(float) * 4;
    constexpr VkDeviceSize countBytes = sizeof(std::uint32_t) * 4U;

    for (auto& frame : frames_) {
        // J3 indirect buffer (also storage so compute can write).
        frame.indirectCommand = createBuffer(
            commandBytes,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vmaMapMemory(allocator_, frame.indirectCommand.allocation, &frame.indirectMapped),
            "Failed to map per-frame indirect command buffer");

        frame.indirectCount = createBuffer(
            countBytes,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vmaMapMemory(allocator_, frame.indirectCount.allocation, &frame.indirectCountMapped),
            "Failed to map per-frame indirect count buffer");

        frame.chunkOrigin = createBuffer(
            originBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vmaMapMemory(allocator_, frame.chunkOrigin.allocation, &frame.originMapped),
            "Failed to map per-frame chunk origin buffer");

        // Per-frame chunk descriptor set (points at this frame's origin buffer).
        VkDescriptorSetAllocateInfo setAllocInfo{};
        setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocInfo.descriptorPool = descriptorPool_;
        setAllocInfo.descriptorSetCount = 1;
        setAllocInfo.pSetLayouts = &chunkDescriptorSetLayout_;
        check(vkAllocateDescriptorSets(device_, &setAllocInfo, &frame.chunkDescriptorSet),
            "Failed to allocate per-frame chunk descriptor set");

        VkDescriptorBufferInfo originInfo{};
        originInfo.buffer = frame.chunkOrigin.buffer;
        originInfo.offset = 0;
        originInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet originWrite{};
        originWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        originWrite.dstSet = frame.chunkDescriptorSet;
        originWrite.dstBinding = 0;
        originWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        originWrite.descriptorCount = 1;
        originWrite.pBufferInfo = &originInfo;
        vkUpdateDescriptorSets(device_, 1, &originWrite, 0, nullptr);
    }
}

void VulkanRenderer::destroyIndirectResources()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    for (auto& frame : frames_) {
        if (frame.indirectMapped != nullptr) {
            vmaUnmapMemory(allocator_, frame.indirectCommand.allocation);
            frame.indirectMapped = nullptr;
        }
        if (frame.originMapped != nullptr) {
            vmaUnmapMemory(allocator_, frame.chunkOrigin.allocation);
            frame.originMapped = nullptr;
        }
        if (frame.indirectCountMapped != nullptr) {
            vmaUnmapMemory(allocator_, frame.indirectCount.allocation);
            frame.indirectCountMapped = nullptr;
        }
        destroyBuffer(frame.indirectCommand);
        destroyBuffer(frame.indirectCount);
        destroyBuffer(frame.chunkOrigin);
        // Descriptor sets are owned by the pool; releasing the pool releases them.
        frame.chunkDescriptorSet = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        for (auto& frame : frames_) {
            frame.cullDescriptorSet = VK_NULL_HANDLE;
        }
        materialDescriptorSet_ = VK_NULL_HANDLE;
    }
    if (chunkDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, chunkDescriptorSetLayout_, nullptr);
        chunkDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (cullDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, cullDescriptorSetLayout_, nullptr);
        cullDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::createUiResources()
{
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(kMaxUiRects) * 6U * sizeof(UiVertex);
    for (auto& frame : frames_) {
        frame.uiVertex = createBuffer(
            vertexBytes,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vmaMapMemory(allocator_, frame.uiVertex.allocation, &frame.uiMapped),
            "Failed to map per-frame UI vertex buffer");
    }
}

void VulkanRenderer::destroyUiResources()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    for (auto& frame : frames_) {
        if (frame.uiMapped != nullptr) {
            vmaUnmapMemory(allocator_, frame.uiVertex.allocation);
            frame.uiMapped = nullptr;
        }
        frame.uiVertexCount = 0;
        destroyBuffer(frame.uiVertex);
    }
}

void VulkanRenderer::createMaterialResources()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &materialDescriptorSetLayout_),
        "Failed to create material descriptor set layout");

    constexpr VkDeviceSize kMaterialBufferBytes = 256 * 64;
    materialTableBuffer_ = createBuffer(
        kMaterialBufferBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &materialDescriptorSetLayout_;
    check(vkAllocateDescriptorSets(device_, &allocInfo, &materialDescriptorSet_),
        "Failed to allocate material descriptor set");

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = materialTableBuffer_.buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = kMaterialBufferBytes;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = materialDescriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanRenderer::destroyMaterialResources()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    destroyBuffer(materialTableBuffer_);
    if (materialDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, materialDescriptorSetLayout_, nullptr);
        materialDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::createCullResources()
{
    // K: pipeline + per-frame scene buffer + per-frame cull descriptor sets.
    // The descriptor set layout was already created in createIndirectResources
    // because the shared descriptor pool needs to know about it ahead of time.

    const VkDeviceSize sceneBytes = static_cast<VkDeviceSize>(kMaxDrawCommands) * sizeof(ChunkSceneEntry); // same struct, now larger
    for (auto& frame : frames_) {
        frame.scene = createBuffer(
            sceneBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vmaMapMemory(allocator_, frame.scene.allocation, &frame.sceneMapped),
            "Failed to map per-frame scene buffer");

        // Per-frame cull descriptor set. Points at this frame's scene,
        // indirect, and origin buffers — all already created.
        VkDescriptorSetAllocateInfo setAllocInfo{};
        setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocInfo.descriptorPool = descriptorPool_;
        setAllocInfo.descriptorSetCount = 1;
        setAllocInfo.pSetLayouts = &cullDescriptorSetLayout_;
        check(vkAllocateDescriptorSets(device_, &setAllocInfo, &frame.cullDescriptorSet),
            "Failed to allocate per-frame cull descriptor set");

        std::array<VkDescriptorBufferInfo, 4> bufferInfos{};
        bufferInfos[0].buffer = frame.scene.buffer;
        bufferInfos[0].offset = 0;
        bufferInfos[0].range = VK_WHOLE_SIZE;
        bufferInfos[1].buffer = frame.indirectCommand.buffer;
        bufferInfos[1].offset = 0;
        bufferInfos[1].range = VK_WHOLE_SIZE;
        bufferInfos[2].buffer = frame.chunkOrigin.buffer;
        bufferInfos[2].offset = 0;
        bufferInfos[2].range = VK_WHOLE_SIZE;
        bufferInfos[3].buffer = frame.indirectCount.buffer;
        bufferInfos[3].offset = 0;
        bufferInfos[3].range = VK_WHOLE_SIZE;

        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = frame.cullDescriptorSet;
            writes[i].dstBinding = i;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo = &bufferInfos[i];
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // Compute pipeline (shared).
    const auto compCode = readBinaryFile(std::string{VOXEL_SHADER_DIR} + "/voxel_cull.comp.spv");
    const auto compShader = createShaderModule(compCode);

    VkPipelineShaderStageCreateInfo compStage{};
    compStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compStage.module = compShader;
    compStage.pName = "main";

    VkPushConstantRange cullPush{};
    cullPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    cullPush.offset = 0;
    cullPush.size = sizeof(CullPushConstants);

    VkPipelineLayoutCreateInfo cullLayoutInfo{};
    cullLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    cullLayoutInfo.setLayoutCount = 1;
    cullLayoutInfo.pSetLayouts = &cullDescriptorSetLayout_;
    cullLayoutInfo.pushConstantRangeCount = 1;
    cullLayoutInfo.pPushConstantRanges = &cullPush;
    check(vkCreatePipelineLayout(device_, &cullLayoutInfo, nullptr, &cullPipelineLayout_),
        "Failed to create cull pipeline layout");

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = compStage;
    pipelineInfo.layout = cullPipelineLayout_;
    check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &cullPipeline_),
        "Failed to create cull compute pipeline");

    vkDestroyShaderModule(device_, compShader, nullptr);
}

void VulkanRenderer::destroyCullResources()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (cullPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, cullPipeline_, nullptr);
        cullPipeline_ = VK_NULL_HANDLE;
    }
    if (cullPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, cullPipelineLayout_, nullptr);
        cullPipelineLayout_ = VK_NULL_HANDLE;
    }
    for (auto& frame : frames_) {
        if (frame.sceneMapped != nullptr) {
            vmaUnmapMemory(allocator_, frame.scene.allocation);
            frame.sceneMapped = nullptr;
        }
        destroyBuffer(frame.scene);
        // cullDescriptorSet is owned by the shared descriptor pool, which is
        // destroyed in destroyIndirectResources().
    }
}

void VulkanRenderer::collectCompletedGpuCullStats(FrameContext& frame)
{
    if (!frame.submittedGpuCull || frame.indirectCountMapped == nullptr) {
        frame.submittedGpuCull = false;
        return;
    }

    const auto* counts = static_cast<const std::uint32_t*>(frame.indirectCountMapped);
    const std::uint32_t visible = counts[3];
    const std::uint32_t drawCommands = counts[0] + counts[1] + counts[2];

    ++frameStats_.gpuCullDispatches;
    frameStats_.gpuCullSections += frame.submittedSceneCount;
    frameStats_.gpuCullVisible += visible;
    frameStats_.gpuCullDrawCommands += drawCommands;
    if (frame.compareGpuCull) {
        frameStats_.gpuCullCpuVisible += frame.submittedCpuVisibleCount;
        if (visible != frame.submittedCpuVisibleCount) {
            ++frameStats_.gpuCullMismatches;
        }
    }

    frame.submittedGpuCull = false;
    frame.compareGpuCull = false;
    frame.submittedSceneCount = 0;
    frame.submittedCpuVisibleCount = 0;
    frame.submittedGpuDrawCommands = 0;
}

void VulkanRenderer::rebuildSceneBufferIfDirty(FrameContext& frame)
{
    if (frame.sceneGeneration >= sceneGeneration_ || frame.sceneMapped == nullptr) {
        return;
    }
    const std::uint32_t count = std::min(static_cast<std::uint32_t>(sceneEntries_.size()), kMaxDrawCommands);
    if (frame.sceneEntryGenerations.size() > count) {
        frame.sceneEntryGenerations.resize(count);
    } else if (frame.sceneEntryGenerations.size() < count) {
        frame.sceneEntryGenerations.resize(count, 0);
    }

    auto* mappedEntries = static_cast<ChunkSceneEntry*>(frame.sceneMapped);
    std::uint64_t syncedEntries = 0;
    bool fullSync = false;
    if (frame.sceneCount > count || frame.sceneEntryGenerations.size() != sceneEntryGenerations_.size()) {
        fullSync = true;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (fullSync || frame.sceneEntryGenerations[i] != sceneEntryGenerations_[i]) {
            mappedEntries[i] = sceneEntries_[i];
            frame.sceneEntryGenerations[i] = sceneEntryGenerations_[i];
            ++syncedEntries;
        }
    }
    frame.sceneCount = count;
    frame.sceneGeneration = sceneGeneration_;
    frameStats_.sceneEntriesSynced += syncedEntries;
    if (fullSync) {
        ++frameStats_.sceneFullSyncs;
    }
}

void VulkanRenderer::upsertSceneEntryForCoord(world::ChunkCoord coord, const ChunkSceneEntry& entry)
{
    const auto indexIt = sceneEntryIndex_.find(coord);
    if (indexIt != sceneEntryIndex_.end() && indexIt->second < sceneEntries_.size()) {
        const auto index = indexIt->second;
        sceneEntries_[index] = entry;
        sceneEntryGenerations_[index] = nextSceneEntryGeneration_++;
        ++sceneGeneration_;
        return;
    }
    if (sceneEntries_.size() < kMaxDrawCommands) {
        sceneEntryIndex_[coord] = sceneEntries_.size();
        sceneEntries_.push_back(entry);
        sceneCoords_.push_back(coord);
        sceneEntryGenerations_.push_back(nextSceneEntryGeneration_++);
        ++sceneGeneration_;
    }
}

void VulkanRenderer::removeSceneEntriesForChunk(world::ChunkCoord chunkCoord)
{
    // Single-section world: the scene-entry key IS the chunk coord. One
    // `find` + swap-pop replaces the previous multimap-range loop.
    const auto indexIt = sceneEntryIndex_.find(chunkCoord);
    if (indexIt == sceneEntryIndex_.end()) {
        return;
    }
    const std::size_t idx = indexIt->second;
    if (idx >= sceneEntries_.size()) {
        sceneEntryIndex_.erase(indexIt);
        return;
    }
    if (idx != sceneEntries_.size() - 1) {
        sceneEntries_[idx] = sceneEntries_.back();
        sceneCoords_[idx] = sceneCoords_.back();
        sceneEntryGenerations_[idx] = nextSceneEntryGeneration_++;
        sceneEntryIndex_[sceneCoords_[idx]] = idx;
    }
    sceneEntries_.pop_back();
    sceneCoords_.pop_back();
    sceneEntryGenerations_.pop_back();
    sceneEntryIndex_.erase(indexIt);
    ++sceneGeneration_;
}

void VulkanRenderer::recordPendingUploads(VkCommandBuffer commandBuffer)
{
    if (pendingUploads_.empty()) {
        frameStats_.uploadQueueLength = 0;
        return;
    }

    const auto queued = static_cast<std::uint64_t>(pendingUploads_.size());
    std::uint64_t batchBytes = 0;
    for (const auto& upload : pendingUploads_) {
        VkBufferCopy region{};
        region.srcOffset = upload.stagingOffset;
        region.dstOffset = upload.dstOffset;
        region.size = upload.bytes;
        vkCmdCopyBuffer(commandBuffer, stagingArenaBuffer_, upload.dst, 1, &region);
        batchBytes += static_cast<std::uint64_t>(upload.bytes);
    }

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
        | VK_ACCESS_INDEX_READ_BIT
        | VK_ACCESS_SHADER_READ_BIT
        | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
            | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
            | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0,
        1, &barrier,
        0, nullptr,
        0, nullptr);

    pendingUploads_.clear();

    ++frameStats_.uploadBatchCount;
    frameStats_.uploadBatchBytes += batchBytes;
    frameStats_.uploadQueueLength = queued;
    frameStats_.chunksMadeDrawable += pendingDrawableChunks_;
    pendingDrawableChunks_ = 0;
}

void VulkanRenderer::rebuildUiVertexBuffer(FrameContext& frame)
{
    frame.uiVertexCount = 0;
    if (frame.uiMapped == nullptr || uiOverlayRects_.empty() || swapchainExtent_.width == 0 || swapchainExtent_.height == 0) {
        return;
    }

    auto* vertices = static_cast<UiVertex*>(frame.uiMapped);
    const auto toNdcX = [this](float x) {
        return (x / static_cast<float>(swapchainExtent_.width)) * 2.0F - 1.0F;
    };
    const auto toNdcY = [this](float y) {
        return (y / static_cast<float>(swapchainExtent_.height)) * 2.0F - 1.0F;
    };

    const std::size_t rectCount = std::min<std::size_t>(uiOverlayRects_.size(), kMaxUiRects);
    for (std::size_t i = 0; i < rectCount; ++i) {
        const auto& rect = uiOverlayRects_[i];
        const float x0 = toNdcX(rect.x);
        const float y0 = toNdcY(rect.y);
        const float x1 = toNdcX(rect.x + rect.width);
        const float y1 = toNdcY(rect.y + rect.height);
        const float color[4] = {rect.r, rect.g, rect.b, rect.a};

        const std::array<UiVertex, 6> quad{{
            {{x0, y0}, {color[0], color[1], color[2], color[3]}},
            {{x1, y0}, {color[0], color[1], color[2], color[3]}},
            {{x1, y1}, {color[0], color[1], color[2], color[3]}},
            {{x0, y0}, {color[0], color[1], color[2], color[3]}},
            {{x1, y1}, {color[0], color[1], color[2], color[3]}},
            {{x0, y1}, {color[0], color[1], color[2], color[3]}},
        }};
        std::memcpy(vertices + frame.uiVertexCount, quad.data(), sizeof(UiVertex) * quad.size());
        frame.uiVertexCount += static_cast<std::uint32_t>(quad.size());
    }
}

void VulkanRenderer::recordFrameCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    check(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin command buffer");

    recordPendingUploads(commandBuffer);

    VkClearValue clear{};
    clear.color = {{clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]}};
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0] = clear;
    clearValues[1].depthStencil = {0.0F, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = framebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent_;
    renderPassInfo.clearValueCount = static_cast<std::uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    VkViewport viewport{};
    viewport.x = 0.0F;
    viewport.y = 0.0F;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent_;

    bool renderPassOpen = false;
    const auto beginMainRenderPass = [&]() {
        if (renderPassOpen) {
            return;
        }
        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        renderPassOpen = true;
    };

    beginMainRenderPass();
    if (skyPipeline_ != VK_NULL_HANDLE) {
        constexpr core::Vec3 worldUp{0.0F, 1.0F, 0.0F};
        const auto cameraForward = debugCamera_.forwardVector();
        auto skyForward = core::Vec3{cameraForward.x, 0.0F, cameraForward.z};
        if (core::length(skyForward) <= 0.001F) {
            skyForward = {0.0F, 0.0F, 1.0F};
        }
        skyForward = core::normalize(skyForward);
        auto right = core::normalize(core::cross(skyForward, worldUp));
        if (core::length(right) <= 0.001F) {
            right = {1.0F, 0.0F, 0.0F};
        }

        SkyPushConstants skyPush{};
        skyPush.cameraForward[0] = skyForward.x;
        skyPush.cameraForward[1] = skyForward.y;
        skyPush.cameraForward[2] = skyForward.z;
        skyPush.cameraRight[0] = right.x;
        skyPush.cameraRight[1] = right.y;
        skyPush.cameraRight[2] = right.z;
        skyPush.cameraUpAspect[0] = worldUp.x;
        skyPush.cameraUpAspect[1] = worldUp.y;
        skyPush.cameraUpAspect[2] = worldUp.z;
        skyPush.cameraUpAspect[3] = viewport.width / std::max(1.0F, viewport.height);
        skyPush.sunDirTime[0] = 0.3F;
        skyPush.sunDirTime[1] = 0.8F;
        skyPush.sunDirTime[2] = 0.5F;
        skyPush.sunDirTime[3] = static_cast<float>(frameIndex_) * (1.0F / 60.0F);
        skyPush.skyParams[0] = skySettings_.horizonLift;
        skyPush.skyParams[1] = skySettings_.saturation;
        skyPush.skyParams[2] = skySettings_.cloudStrength;
        skyPush.skyParams[3] = skySettings_.brightness;

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
        vkCmdPushConstants(commandBuffer, skyPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SkyPushConstants), &skyPush);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    if (graphicsPipeline_ != VK_NULL_HANDLE && !uploadedMeshes_.empty()
        && vertexArena_.buffer() != VK_NULL_HANDLE && indexArena_.buffer() != VK_NULL_HANDLE) {
        const auto viewProjection = debugCamera_.viewProjection();

        // K: every per-frame buffer + descriptor we touch below belongs to the
        // current frame slot. The other frame may still be reading its own
        // copies on the GPU — that's the whole point of the pipelining.
        FrameContext& frame = frames_[currentFrame_];

        // J3: refresh THIS frame's scene buffer if dirty. Each frame's dirty
        // flag was broadcast by uploadChunkMesh/removeUploadedMesh, so all
        // frames eventually catch up after a chunk-set change.
        rebuildSceneBufferIfDirty(frame);

        const bool computeAvailable = useGpuCull_
            && supportsDrawIndirectCount_
            && cullPipeline_ != VK_NULL_HANDLE
            && frame.sceneCount > 0;

        std::uint32_t drawCount = 0;
        std::uint32_t cpuVisibleForCompare = 0;
        std::uint32_t stableTransparentCount = 0;

        if (computeAvailable) {
            drawCount = frame.sceneCount;
            frame.submittedGpuCull = true;
            frame.compareGpuCull = compareGpuCull_;
            frame.submittedSceneCount = frame.sceneCount;
            frame.submittedCpuVisibleCount = 0;
            frame.submittedGpuDrawCommands = 0;

            if (compareGpuCull_) {
                const auto frustum = core::extractFrustumPlanes(viewProjection);
                for (std::uint32_t i = 0; i < frame.sceneCount; ++i) {
                    const auto& entry = sceneEntries_[i];
                    const core::Vec3 minCorner{entry.boundsMin[0], entry.boundsMin[1], entry.boundsMin[2]};
                    const core::Vec3 maxCorner{
                        minCorner.x + entry.extent[0],
                        minCorner.y + entry.extent[1],
                        minCorner.z + entry.extent[2]};
                    const bool hasDrawableSurface = entry.indexCount > 0
                        || entry.cutoutIndexCount > 0
                        || entry.transparentIndexCount > 0;
                    if (hasDrawableSurface && core::aabbIntersectsFrustum(frustum, minCorner, maxCorner)) {
                        ++cpuVisibleForCompare;
                    }
                }
                frame.submittedCpuVisibleCount = cpuVisibleForCompare;
            }

            // Transparent alpha blending is order-dependent. GPU compaction
            // uses atomics, so opaque/cutout stay on that path while water and
            // glass use a deterministic CPU-built back-to-front command list.
            {
                struct TransparentCandidate {
                    std::uint32_t sceneIndex{};
                    float distanceSq{};
                };

                const auto frustum = core::extractFrustumPlanes(viewProjection);
                const auto cameraPos = debugCamera_.position();
                std::vector<TransparentCandidate> transparent;
                transparent.reserve(frame.sceneCount);
                for (std::uint32_t i = 0; i < frame.sceneCount; ++i) {
                    const auto& entry = sceneEntries_[i];
                    if (entry.transparentIndexCount == 0) {
                        continue;
                    }
                    const core::Vec3 minCorner{entry.boundsMin[0], entry.boundsMin[1], entry.boundsMin[2]};
                    const core::Vec3 maxCorner{
                        minCorner.x + entry.extent[0],
                        minCorner.y + entry.extent[1],
                        minCorner.z + entry.extent[2]};
                    if (!core::aabbIntersectsFrustum(frustum, minCorner, maxCorner)) {
                        continue;
                    }

                    const core::Vec3 center{
                        minCorner.x + entry.extent[0] * 0.5F,
                        minCorner.y + entry.extent[1] * 0.5F,
                        minCorner.z + entry.extent[2] * 0.5F};
                    const auto dx = center.x - cameraPos.x;
                    const auto dy = center.y - cameraPos.y;
                    const auto dz = center.z - cameraPos.z;
                    transparent.push_back({i, dx * dx + dy * dy + dz * dz});
                }

                std::stable_sort(transparent.begin(), transparent.end(),
                    [](const TransparentCandidate& lhs, const TransparentCandidate& rhs) {
                        if (lhs.distanceSq != rhs.distanceSq) {
                            return lhs.distanceSq > rhs.distanceSq;
                        }
                        return lhs.sceneIndex < rhs.sceneIndex;
                    });

                auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(frame.indirectMapped);
                auto* origins = static_cast<float*>(frame.originMapped);
                for (const auto& candidate : transparent) {
                    if (stableTransparentCount >= kMaxDrawCommands) {
                        break;
                    }
                    const auto& entry = sceneEntries_[candidate.sceneIndex];
                    const auto slot = 2U * kMaxDrawCommands + stableTransparentCount;
                    auto& cmd = commands[slot];
                    cmd.indexCount = entry.transparentIndexCount;
                    cmd.instanceCount = 1U;
                    cmd.firstIndex = entry.transparentFirstIndex;
                    cmd.vertexOffset = entry.transparentVertexOffset;
                    cmd.firstInstance = slot;

                    const std::size_t originBase = static_cast<std::size_t>(slot) * 4U;
                    origins[originBase + 0] = entry.origin[0];
                    origins[originBase + 1] = entry.origin[1];
                    origins[originBase + 2] = entry.origin[2];
                    origins[originBase + 3] = 0.0F;
                    ++stableTransparentCount;
                }
            }

            const VkDeviceSize gpuCompactedCommandBytes =
                static_cast<VkDeviceSize>(2U * kMaxDrawCommands) * sizeof(VkDrawIndexedIndirectCommand);
            vkCmdFillBuffer(commandBuffer, frame.indirectCommand.buffer, 0, gpuCompactedCommandBytes, 0);
            vkCmdFillBuffer(commandBuffer, frame.indirectCount.buffer, 0, VK_WHOLE_SIZE, 0);

            std::array<VkBufferMemoryBarrier, 2> clearBarriers{};
            clearBarriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            clearBarriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            clearBarriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            clearBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            clearBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            clearBarriers[0].buffer = frame.indirectCommand.buffer;
            clearBarriers[0].offset = 0;
            clearBarriers[0].size = gpuCompactedCommandBytes;
            clearBarriers[1] = clearBarriers[0];
            clearBarriers[1].buffer = frame.indirectCount.buffer;
            clearBarriers[1].size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(commandBuffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr,
                static_cast<std::uint32_t>(clearBarriers.size()), clearBarriers.data(),
                0, nullptr);

            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipelineLayout_,
                0, 1, &frame.cullDescriptorSet, 0, nullptr);

            CullPushConstants cullPush{};
            cullPush.viewProjection = viewProjection;
            cullPush.chunkCount = frame.sceneCount;
            cullPush.maxCommands = kMaxDrawCommands;
            vkCmdPushConstants(commandBuffer, cullPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(CullPushConstants), &cullPush);

            // 64 threads per workgroup — matches local_size_x in voxel_cull.comp.
            const std::uint32_t groupCount = (frame.sceneCount + 63U) / 64U;
            vkCmdDispatch(commandBuffer, groupCount, 1, 1);

            // Barrier: compute → indirect-read + vertex-shader-read for THIS
            // frame's indirect + origin buffers (the next frame's copies are
            // untouched and still safe).
            std::array<VkBufferMemoryBarrier, 3> bufferBarriers{};
            bufferBarriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bufferBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bufferBarriers[0].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            bufferBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferBarriers[0].buffer = frame.indirectCommand.buffer;
            bufferBarriers[0].offset = 0;
            bufferBarriers[0].size = VK_WHOLE_SIZE;
            bufferBarriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bufferBarriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bufferBarriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            bufferBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferBarriers[1].buffer = frame.chunkOrigin.buffer;
            bufferBarriers[1].offset = 0;
            bufferBarriers[1].size = VK_WHOLE_SIZE;
            bufferBarriers[2].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bufferBarriers[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bufferBarriers[2].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            bufferBarriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferBarriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferBarriers[2].buffer = frame.indirectCount.buffer;
            bufferBarriers[2].offset = 0;
            bufferBarriers[2].size = VK_WHOLE_SIZE;

            vkCmdPipelineBarrier(commandBuffer,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                0, 0, nullptr,
                static_cast<std::uint32_t>(bufferBarriers.size()), bufferBarriers.data(),
                0, nullptr);
        }

        beginMainRenderPass();
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);

        // J1: bind the shared arena buffers exactly once per frame.
        const VkBuffer vertexBuffers[] = {vertexArena_.buffer()};
        const VkDeviceSize vertexOffsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, vertexOffsets);
        vkCmdBindIndexBuffer(commandBuffer, indexArena_.buffer(), 0, VK_INDEX_TYPE_UINT32);

        // Bind descriptor sets: set 0 = chunk origins, set 1 = material table.
        const std::array<VkDescriptorSet, 2> descriptorSets{frame.chunkDescriptorSet, materialDescriptorSet_};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
            0, static_cast<std::uint32_t>(descriptorSets.size()), descriptorSets.data(), 0, nullptr);

        // Push view-projection + lighting params (sun direction + time)
        // + camera params (underwater fog strength + tint).
        PushConstants push{};
        push.viewProjection = viewProjection;
        push.lightParams[0] = 0.3F;
        push.lightParams[1] = 0.8F;
        push.lightParams[2] = 0.5F;
        push.lightParams[3] = static_cast<float>(frameIndex_) * (1.0F / 60.0F);
        // W0: underwater fog. Tint is a deep blue-green; the fragment shader
        // mixes it in based on view depth × strength.
        push.cameraParams[0] = cameraUnderwaterStrength_;
        push.cameraParams[1] = 0.12F;  // tint R
        push.cameraParams[2] = 0.30F;  // tint G
        push.cameraParams[3] = 0.42F;  // tint B
        const auto cameraPos = debugCamera_.position();
        push.cameraWorldParams[0] = cameraPos.x;
        push.cameraWorldParams[1] = cameraPos.y;
        push.cameraWorldParams[2] = cameraPos.z;
        push.cameraWorldParams[3] = 0.0F;
        push.atmosphereParams[0] = atmosphereSettings_.fogNear;
        push.atmosphereParams[1] = atmosphereSettings_.fogFar;
        push.atmosphereParams[2] = atmosphereSettings_.fogStrength;
        push.atmosphereParams[3] = atmosphereSettings_.farLightLift;
        vkCmdPushConstants(commandBuffer, pipelineLayout_,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(PushConstants), &push);

        // J4: track per-surface draw counts for the indirect calls below.
        // Opaque commands occupy positions [0, meshCount); transparent
        // commands occupy positions [meshCount, 2*meshCount) — consistent
        // with the GPU cull shader's output layout.
        std::uint32_t cmdOpaqueCount = 0;
        std::uint32_t cmdCutoutCount = 0;
        std::uint32_t cmdTransparentCount = 0;

        if (!computeAvailable) {
            const auto frustum = core::extractFrustumPlanes(viewProjection);
            auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(frame.indirectMapped);
            auto* origins = static_cast<float*>(frame.originMapped);
            std::uint64_t opaqueCulled = 0;
            const std::uint32_t sectionCount = std::min(static_cast<std::uint32_t>(sceneEntries_.size()), kMaxDrawCommands);
            for (std::uint32_t i = 0; i < sectionCount; ++i) {
                const auto& entry = sceneEntries_[i];
                const core::Vec3 minCorner{entry.boundsMin[0], entry.boundsMin[1], entry.boundsMin[2]};
                const core::Vec3 maxCorner{
                    minCorner.x + entry.extent[0],
                    minCorner.y + entry.extent[1],
                    minCorner.z + entry.extent[2]};
                const bool visible = core::aabbIntersectsFrustum(frustum, minCorner, maxCorner);

                auto& opaqueCmd = commands[i];
                opaqueCmd.indexCount = visible ? entry.indexCount : 0u;
                opaqueCmd.instanceCount = visible ? 1u : 0u;
                opaqueCmd.firstIndex = entry.firstIndex;
                opaqueCmd.vertexOffset = entry.vertexOffset;
                opaqueCmd.firstInstance = i;
                const std::size_t opaqueBase = static_cast<std::size_t>(i) * 4;
                origins[opaqueBase + 0] = entry.origin[0];
                origins[opaqueBase + 1] = entry.origin[1];
                origins[opaqueBase + 2] = entry.origin[2];
                origins[opaqueBase + 3] = 0.0F;
                if (!visible) {
                    ++opaqueCulled;
                }

                auto& cutoutCmd = commands[sectionCount + i];
                cutoutCmd.indexCount = (visible && entry.cutoutIndexCount > 0u) ? entry.cutoutIndexCount : 0u;
                cutoutCmd.instanceCount = (visible && entry.cutoutIndexCount > 0u) ? 1u : 0u;
                cutoutCmd.firstIndex = entry.cutoutFirstIndex;
                cutoutCmd.vertexOffset = entry.cutoutVertexOffset;
                cutoutCmd.firstInstance = sectionCount + i;
                const std::size_t cutoutBase = static_cast<std::size_t>(sectionCount + i) * 4;
                origins[cutoutBase + 0] = entry.origin[0];
                origins[cutoutBase + 1] = entry.origin[1];
                origins[cutoutBase + 2] = entry.origin[2];
                origins[cutoutBase + 3] = 0.0F;

                auto& transCmd = commands[2 * sectionCount + i];
                transCmd.indexCount = (visible && entry.transparentIndexCount > 0u) ? entry.transparentIndexCount : 0u;
                transCmd.instanceCount = (visible && entry.transparentIndexCount > 0u) ? 1u : 0u;
                transCmd.firstIndex = entry.transparentFirstIndex;
                transCmd.vertexOffset = entry.transparentVertexOffset;
                transCmd.firstInstance = 2 * sectionCount + i;
                const std::size_t transBase = static_cast<std::size_t>(2 * sectionCount + i) * 4;
                origins[transBase + 0] = entry.origin[0];
                origins[transBase + 1] = entry.origin[1];
                origins[transBase + 2] = entry.origin[2];
                origins[transBase + 3] = 0.0F;
            }
            frameStats_.chunksDrawn += sectionCount;
            frameStats_.chunksCulled += opaqueCulled;
            cmdOpaqueCount = sectionCount;
            cmdCutoutCount = sectionCount;
            cmdTransparentCount = sectionCount;
        }

        const auto cmdStride = static_cast<std::uint32_t>(sizeof(VkDrawIndexedIndirectCommand));
        if (computeAvailable) {
            vkCmdDrawIndexedIndirectCount(commandBuffer,
                frame.indirectCommand.buffer,
                0,
                frame.indirectCount.buffer,
                0,
                kMaxDrawCommands,
                cmdStride);
            vkCmdDrawIndexedIndirectCount(commandBuffer,
                frame.indirectCommand.buffer,
                static_cast<VkDeviceSize>(kMaxDrawCommands) * cmdStride,
                frame.indirectCount.buffer,
                sizeof(std::uint32_t),
                kMaxDrawCommands,
                cmdStride);
            if (stableTransparentCount > 0) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, transparentPipeline_);
                vkCmdDrawIndexedIndirect(commandBuffer,
                    frame.indirectCommand.buffer,
                    static_cast<VkDeviceSize>(2U * kMaxDrawCommands) * cmdStride,
                    stableTransparentCount,
                    cmdStride);
            }
            frameStats_.chunksDrawn += frame.sceneCount;
        } else if (cmdOpaqueCount > 0) {
            vkCmdDrawIndexedIndirect(commandBuffer, frame.indirectCommand.buffer, 0,
                cmdOpaqueCount, cmdStride);
            if (cmdCutoutCount > 0) {
                vkCmdDrawIndexedIndirect(commandBuffer, frame.indirectCommand.buffer,
                    static_cast<VkDeviceSize>(cmdOpaqueCount) * cmdStride,
                    cmdCutoutCount, cmdStride);
            }
            if (cmdTransparentCount > 0) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, transparentPipeline_);
                vkCmdDrawIndexedIndirect(commandBuffer, frame.indirectCommand.buffer,
                    static_cast<VkDeviceSize>(cmdOpaqueCount + cmdCutoutCount) * cmdStride,
                    cmdTransparentCount, cmdStride);
            }
        }

        // Phase 1C-4b: external draw hook. Currently used by ClusterRenderer
        // to inject LOD2 cluster draws after the chunk pass. The hook runs
        // inside the same render pass, sharing the depth buffer with the
        // chunk pass — so chunk geometry naturally occludes any LOD2
        // cluster mesh that would otherwise duplicate the same world
        // space (the persistence-policy continuity guarantee).
        if (externalDrawHook_) {
            ExternalDrawContext ctx{};
            ctx.commandBuffer = commandBuffer;
            ctx.viewProjection = push.viewProjection;
            for (int i = 0; i < 4; ++i) {
                ctx.lightParams[i] = push.lightParams[i];
                ctx.cameraParams[i] = push.cameraParams[i];
                ctx.cameraWorldParams[i] = push.cameraWorldParams[i];
                ctx.atmosphereParams[i] = push.atmosphereParams[i];
            }
            externalDrawHook_(ctx);
        }

        if (debugLinePipeline_ != VK_NULL_HANDLE
            && debugLineMesh_.block.has_value()
            && debugLineMesh_.vertices.buffer != VK_NULL_HANDLE
            && debugLineMesh_.indices.buffer != VK_NULL_HANDLE) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, debugLinePipeline_);
            const VkBuffer debugVertexBuffers[] = {debugLineMesh_.vertices.buffer};
            const VkDeviceSize debugOffsets[] = {0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, debugVertexBuffers, debugOffsets);
            vkCmdBindIndexBuffer(commandBuffer, debugLineMesh_.indices.buffer, 0, VK_INDEX_TYPE_UINT32);

            const auto world = world::toWorldBlock(debugLineMesh_.block->chunk, debugLineMesh_.block->block);
            PushConstants debugPush{};
            debugPush.viewProjection = viewProjection;
            debugPush.lightParams[0] = static_cast<float>(world.x);
            debugPush.lightParams[1] = static_cast<float>(world.y);
            debugPush.lightParams[2] = static_cast<float>(world.z);
            debugPush.lightParams[3] = 0.0F;
            vkCmdPushConstants(commandBuffer, pipelineLayout_,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(PushConstants), &debugPush);
            vkCmdDrawIndexed(commandBuffer, debugLineMesh_.indexCount, 1, 0, 0, 0);
        }
    }

    FrameContext& uiFrame = frames_[currentFrame_];
    rebuildUiVertexBuffer(uiFrame);
    if (uiPipeline_ != VK_NULL_HANDLE && uiFrame.uiVertex.buffer != VK_NULL_HANDLE && uiFrame.uiVertexCount > 0) {
        beginMainRenderPass();
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline_);
        const VkBuffer vertexBuffers[] = {uiFrame.uiVertex.buffer};
        const VkDeviceSize vertexOffsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, vertexOffsets);
        vkCmdDraw(commandBuffer, uiFrame.uiVertexCount, 1, 0, 0);
    }

    beginMainRenderPass();

    // ImGui pass: draw the debug overlay on top of the world + hotbar.
    // ImGui::Render() must have been called from the application before we
    // get here (Application calls renderer.endImGuiFrame() before endFrame()).
    if (imguiInitialized_) {
        if (auto* drawData = ImGui::GetDrawData()) {
            ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
        }
    }

    vkCmdEndRenderPass(commandBuffer);

    check(vkEndCommandBuffer(commandBuffer), "Failed to end command buffer");
}

void VulkanRenderer::cleanupSwapchain()
{
    if (uiPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, uiPipeline_, nullptr);
        uiPipeline_ = VK_NULL_HANDLE;
    }
    if (uiPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, uiPipelineLayout_, nullptr);
        uiPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (debugLinePipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, debugLinePipeline_, nullptr);
        debugLinePipeline_ = VK_NULL_HANDLE;
    }
    if (skyPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, skyPipeline_, nullptr);
        skyPipeline_ = VK_NULL_HANDLE;
    }
    if (skyPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, skyPipelineLayout_, nullptr);
        skyPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (transparentPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, transparentPipeline_, nullptr);
        transparentPipeline_ = VK_NULL_HANDLE;
    }
    if (graphicsPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
        graphicsPipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }

    for (auto framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    framebuffers_.clear();

    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }

    for (auto imageView : swapchainImageViews_) {
        vkDestroyImageView(device_, imageView, nullptr);
    }
    swapchainImageViews_.clear();
    destroyImage(depthImage_, depthAllocation_, depthImageView_);

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::destroyImage(VkImage& image, VmaAllocation& allocation, VkImageView& view)
{
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image, allocation);
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::destroyBuffer(GpuBuffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
        buffer.buffer = VK_NULL_HANDLE;
        buffer.allocation = VK_NULL_HANDLE;
    }
    buffer.size = 0;
}

void VulkanRenderer::retireBuffer(GpuBuffer& buffer)
{
    if (buffer.buffer == VK_NULL_HANDLE && buffer.allocation == VK_NULL_HANDLE) {
        return;
    }
    retiredBuffers_.push_back(RetiredBuffer{buffer, frameIndex_ + kFramesInFlight + 1U});
    buffer = {};
}

void VulkanRenderer::flushRetiredBuffers()
{
    auto it = retiredBuffers_.begin();
    while (it != retiredBuffers_.end()) {
        if (it->retireFrame > frameIndex_) {
            ++it;
            continue;
        }
        destroyBuffer(it->buffer);
        it = retiredBuffers_.erase(it);
    }
}

void VulkanRenderer::flushRetiredSlices()
{
    auto vIt = retiredVertexSlices_.begin();
    while (vIt != retiredVertexSlices_.end()) {
        if (vIt->retireFrame > frameIndex_) {
            ++vIt;
            continue;
        }
        vertexArena_.release(vIt->slice);
        vIt = retiredVertexSlices_.erase(vIt);
    }
    auto iIt = retiredIndexSlices_.begin();
    while (iIt != retiredIndexSlices_.end()) {
        if (iIt->retireFrame > frameIndex_) {
            ++iIt;
            continue;
        }
        indexArena_.release(iIt->slice);
        iIt = retiredIndexSlices_.erase(iIt);
    }
}

void VulkanRenderer::destroyAllRetiredBuffers()
{
    for (auto& retired : retiredBuffers_) {
        destroyBuffer(retired.buffer);
    }
    retiredBuffers_.clear();
}

void VulkanRenderer::createImage(
    std::uint32_t width,
    std::uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkImage& image,
    VmaAllocation& allocation) const
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    auto allocInfo = allocationInfoFor(properties);
    check(vmaCreateImage(allocator_, &imageInfo, &allocInfo, &image, &allocation, nullptr),
        "Failed to allocate image");
}

VkImageView VulkanRenderer::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) const
{
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = image;
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = format;
    createInfo.subresourceRange.aspectMask = aspectFlags;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    check(vkCreateImageView(device_, &createInfo, nullptr, &view), "Failed to create image view");
    return view;
}

VulkanRenderer::GpuBuffer VulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) const
{
    GpuBuffer result;
    result.size = size;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    auto allocInfo = allocationInfoFor(properties);
    check(vmaCreateBuffer(allocator_, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, nullptr),
        "Failed to allocate buffer");

    return result;
}

// ---- Compute / extension API ------------------------------------------
// Thin wrappers over VMA used by `FluidGpuSystem` and any future compute-
// integrated systems. `hostVisible` selects HOST_VISIBLE_COHERENT (persistently
// mapped) vs DEVICE_LOCAL (staged via cmdCopyBuffer).
VulkanRenderer::ComputeBuffer VulkanRenderer::createComputeBuffer(VkDeviceSize bytes, bool hostVisible)
{
    ComputeBuffer result;
    result.size = bytes;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    // Storage-buffer usage + transfer src/dst so we can clear/copy without
    // re-creating the buffer (cmdFillBuffer needs TRANSFER_DST; readback
    // copies need TRANSFER_SRC on the source GPU buffer). Adding
    // INDIRECT_BUFFER_BIT is free (zero perf impact for buffers that
    // aren't used as indirect) and lets the cluster path use compute
    // buffers as vkCmdDrawIndexedIndirect sources without needing a
    // separate buffer-creation path.
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                     | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                     | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                     | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    if (hostVisible) {
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                        | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    } else {
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }

    VmaAllocationInfo info{};
    check(vmaCreateBuffer(allocator_, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &info),
        "Failed to allocate compute buffer");
    result.mapped = info.pMappedData; // non-null only for host-visible
    return result;
}

std::uint32_t VulkanRenderer::graphicsQueueFamily() const noexcept
{
    return queueFamilies_.graphics.value_or(0U);
}

void VulkanRenderer::destroyComputeBuffer(ComputeBuffer& buf) noexcept
{
    if (buf.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buf.buffer, buf.allocation);
    }
    buf.buffer = VK_NULL_HANDLE;
    buf.allocation = VK_NULL_HANDLE;
    buf.size = 0;
    buf.mapped = nullptr;
}

void VulkanRenderer::uploadBuffer(GpuBuffer& buffer, const void* data, VkDeviceSize size) const
{
    void* mapped = nullptr;
    check(vmaMapMemory(allocator_, buffer.allocation, &mapped), "Failed to map buffer memory");
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vmaUnmapMemory(allocator_, buffer.allocation);
}

bool VulkanRenderer::stagingUpload(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize bytes)
{
    // Public wrapper that delegates to the (still-private) staging ring
    // path. Kept as a separate symbol from `uploadIntoBuffer` so callers
    // outside the renderer don't accidentally bypass any future
    // book-keeping we might add (overflow stats, telemetry, etc).
    return uploadIntoBuffer(dst, dstOffset, data, bytes);
}

void VulkanRenderer::setExternalDrawHook(ExternalDrawHook hook)
{
    externalDrawHook_ = std::move(hook);
}

bool VulkanRenderer::uploadIntoBuffer(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize bytes)
{
    if (bytes == 0) {
        return true;
    }
    frameStats_.stagingUploadBytes += static_cast<std::uint64_t>(bytes);

    // L: bump-allocate from the current frame's staging arena slot.
    const VkDeviceSize slotBase = currentFrame_ * kStagingSlotSize;
    const VkDeviceSize offset = stagingSlotOffset_;
    if (offset + bytes > kStagingSlotSize) {
        Logger::warn("Staging arena overflow; dropping upload");
        return false;
    }
    std::memcpy(stagingArenaMapped_ + slotBase + offset, data, static_cast<std::size_t>(bytes));
    stagingSlotOffset_ = offset + bytes;

    pendingUploads_.push_back(PendingUpload{slotBase + offset, dst, dstOffset, bytes});
    return true;
}

void VulkanRenderer::uploadViaStaging(GpuBuffer& dst, const void* data, VkDeviceSize bytes)
{
    frameStats_.stagingUploadBytes += static_cast<std::uint64_t>(bytes);
    auto staging = createBuffer(
        bytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    uploadBuffer(staging, data, bytes);

    // One-shot command buffer for the GPU-side copy.
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device_, &allocInfo, &cmd), "Failed to allocate staging command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &beginInfo), "Failed to begin staging command buffer");

    VkBufferCopy region{};
    region.size = bytes;
    vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);

    check(vkEndCommandBuffer(cmd), "Failed to end staging command buffer");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence transferFence = VK_NULL_HANDLE;
    check(vkCreateFence(device_, &fenceInfo, nullptr, &transferFence), "Failed to create staging fence");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    check(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, transferFence), "Failed to submit staging copy");

    // Wait only on this transfer, not the whole device.
    vkWaitForFences(device_, 1, &transferFence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, transferFence, nullptr);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);

    destroyBuffer(staging);
}

VkShaderModule VulkanRenderer::createShaderModule(const std::vector<char>& code) const
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());

    VkShaderModule shader = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device_, &createInfo, nullptr, &shader), "Failed to create shader module");
    return shader;
}

VkFormat VulkanRenderer::findDepthFormat() const
{
    const std::array<VkFormat, 3> candidates{
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT
    };

    for (const auto format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            return format;
        }
    }

    throw std::runtime_error("Failed to find supported depth format");
}

bool VulkanRenderer::deviceSuitable(VkPhysicalDevice device) const
{
    const auto families = findQueueFamilies(device);
    const bool extensionsSupported = hasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(device, &features2);
    const bool indirectFeatures = features2.features.multiDrawIndirect == VK_TRUE
        && features12.drawIndirectCount == VK_TRUE;
    bool swapchainReady = false;
    if (extensionsSupported) {
        const auto support = querySwapchainSupport(device);
        swapchainReady = !support.formats.empty() && !support.presentModes.empty();
    }

    return families.complete() && extensionsSupported && swapchainReady && indirectFeatures;
}

VulkanRenderer::QueueFamilies VulkanRenderer::findQueueFamilies(VkPhysicalDevice device) const
{
    QueueFamilies families;

    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> properties(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, properties.data());

    for (std::uint32_t i = 0; i < properties.size(); ++i) {
        if ((properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            families.graphics = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        check(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport), "Failed to query present support");
        if (presentSupport == VK_TRUE) {
            families.present = i;
        }

        if (families.complete()) {
            break;
        }
    }

    return families;
}

VulkanRenderer::SwapchainSupport VulkanRenderer::querySwapchainSupport(VkPhysicalDevice device) const
{
    SwapchainSupport support;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &support.capabilities), "Failed to query surface capabilities");

    std::uint32_t formatCount = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr), "Failed to count surface formats");
    support.formats.resize(formatCount);
    if (formatCount > 0) {
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, support.formats.data()), "Failed to read surface formats");
    }

    std::uint32_t presentModeCount = 0;
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr), "Failed to count present modes");
    support.presentModes.resize(presentModeCount);
    if (presentModeCount > 0) {
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, support.presentModes.data()), "Failed to read present modes");
    }

    return support;
}

VkSurfaceFormatKHR VulkanRenderer::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const
{
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR VulkanRenderer::choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const
{
    for (const auto presentMode : presentModes) {
        if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return presentMode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRenderer::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, const RendererConfig& config) const
{
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    auto width = static_cast<std::uint32_t>(config.width);
    auto height = static_cast<std::uint32_t>(config.height);
    if (config.window != nullptr) {
        const auto framebuffer = config.window->framebufferExtent();
        width = framebuffer.width;
        height = framebuffer.height;
    }

    return {
        std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)
    };
}

} // namespace voxel::render
