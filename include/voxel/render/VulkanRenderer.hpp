#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <voxel/render/BufferArena.hpp>
#include <voxel/render/DebugCamera.hpp>
#include <voxel/render/IRenderer.hpp>
#include <voxel/render/MaterialTable.hpp>
#include <voxel/render/RenderGraph.hpp>
#include <voxel/render/meshing/GreedyMesher.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::render {

class VulkanRenderer final : public IRenderer {
public:
    void initialize(const RendererConfig& config) override;
    [[nodiscard]] bool beginFrame() override;
    void endFrame() override;
    void shutdown() override;

    [[nodiscard]] RenderGraph& graph() noexcept;
    void updateDebugCamera(const platform::IWindow& window, float deltaSeconds) noexcept;
    void setDebugCameraPose(core::Vec3 position, float yawRadians, float pitchRadians) noexcept;
    void setDebugCameraPose(core::DVec3 position, float yawRadians, float pitchRadians) noexcept;
    [[nodiscard]] core::Vec3 debugCameraPosition() const noexcept;
    [[nodiscard]] core::DVec3 debugCameraDPosition() const noexcept;
    [[nodiscard]] core::Vec3 debugCameraForward() const noexcept;
    [[nodiscard]] platform::WindowExtent drawableExtent() const noexcept;
    [[nodiscard]] bool uploadChunkMesh(world::ChunkCoord coord, const meshing::ChunkMesh& mesh);

    // Async-upload split: `prepareChunkMeshUpload` does everything except the
    // two vertex/index memcpys (arena allocate, staging reserve, scene-entry
    // insert, pendingUploads_ push). It returns destination pointers into the
    // mapped staging buffer; the caller dispatches the actual memcpy to worker
    // threads and MUST ensure all memcpys complete before this renderer's
    // `endFrame()` (which submits the vkCmdCopyBuffer commands). The mesh
    // source data must remain valid until the memcpy completes.
    struct PreparedMeshUpload {
        bool valid{false};            // false → not uploaded (duplicate or failure)
        bool duplicateSkip{false};    // true → coord already uploaded at same revision
        std::byte* vertexDst{nullptr};
        std::byte* indexDst{nullptr};
        const void* vertexSrc{nullptr};
        const void* indexSrc{nullptr};
        std::size_t vertexBytes{0};
        std::size_t indexBytes{0};
    };
    [[nodiscard]] PreparedMeshUpload prepareChunkMeshUpload(world::ChunkCoord coord,
                                                            const meshing::ChunkMesh& mesh);

    void removeUploadedMesh(world::ChunkCoord coord);
    void setDebugBlockOutline(std::optional<world::PlanetCoord> block);
    // W0: tell the renderer the camera is currently inside water. Drives the
    // underwater fog effect in the fragment shader. Strength 0..1 — pass 1.0
    // when the eye is in a water block, 0.0 otherwise. (Smoothing or partial
    // submersion can pass intermediate values later.)
    void setCameraUnderwater(float strength) noexcept { cameraUnderwaterStrength_ = strength; }
    void setClearColor(float r, float g, float b, float a = 1.0F) noexcept;
    void setCameraFarPlane(float farPlane) noexcept;
    struct AtmosphereSettings {
        float fogNear{96.0F};
        float fogFar{4200.0F};
        float fogStrength{0.62F};
        float farLightLift{0.58F};
    };
    void setAtmosphereSettings(AtmosphereSettings settings) noexcept;
    struct SkySettings {
        float horizonLift{0.20F};
        float saturation{1.18F};
        float cloudStrength{0.82F};
        float brightness{1.08F};
    };
    void setSkySettings(SkySettings settings) noexcept;

    // Dear ImGui integration.
    //
    //   initializeImGui(window) — call once after `initialize()`, passing the
    //                             GLFW window. Sets up the ImGui Vulkan/GLFW
    //                             backends and a small descriptor pool.
    //   beginImGuiFrame()      — call at the start of the application tick.
    //                             Wraps ImGui_ImplVulkan/Glfw NewFrame +
    //                             ImGui::NewFrame().
    //   endImGuiFrame()        — call right before the renderer's endFrame()
    //                             so the draw data is ready when the
    //                             commands are recorded. Wraps ImGui::Render().
    //
    // The actual draw call (ImGui_ImplVulkan_RenderDrawData) lives inside
    // recordFrameCommands() so the ImGui draws run in the same render pass
    // as the world geometry (right before swapchain present).
    void initializeImGui(platform::IWindow& window);
    void beginImGuiFrame();
    void endImGuiFrame();
    [[nodiscard]] bool isImGuiInitialized() const noexcept { return imguiInitialized_; }
    struct UiRect {
        float x{};
        float y{};
        float width{};
        float height{};
        float r{};
        float g{};
        float b{};
        float a{1.0F};
    };
    void setUiOverlay(std::vector<UiRect> rects);
    void clearUploadedMeshes();
    void uploadMaterialTable(const std::vector<MaterialGpuData>& materials);

    struct FrameStats {
        std::uint64_t gpuUploads{};
        std::uint64_t duplicateUploadSkips{};
        std::uint64_t stagingUploadBytes{};
        std::uint64_t gpuUploadTimeUs{};
        std::uint64_t gpuUploadMaxUs{};
        std::uint64_t uploadBatchCount{};
        std::uint64_t uploadBatchBytes{};
        std::uint64_t uploadQueueLength{};
        std::uint64_t chunksMadeDrawable{};
        std::uint64_t rendererFenceWaitUs{};
        std::uint64_t rendererFenceWaitMaxUs{};
        std::uint64_t chunksDrawn{};
        std::uint64_t chunksCulled{};
        std::uint64_t gpuCullDispatches{};
        std::uint64_t gpuCullSections{};
        std::uint64_t gpuCullVisible{};
        std::uint64_t gpuCullCpuVisible{};
        std::uint64_t gpuCullMismatches{};
        std::uint64_t gpuCullDrawCommands{};
        std::uint64_t sceneEntriesSynced{};
        std::uint64_t sceneFullSyncs{};
    };

    [[nodiscard]] FrameStats drainFrameStats() noexcept;
    [[nodiscard]] std::uint64_t totalUploadedMeshBytes() const noexcept;

    // ---- Compute / extension API ---------------------------------------
    // Minimal surface for systems like `FluidGpuSystem` that want to host
    // their own Vulkan resources alongside the renderer. Hides VMA from the
    // caller — the returned `ComputeBuffer` holds opaque handles that the
    // caller passes back to `destroyComputeBuffer()` on shutdown.
    struct ComputeBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkDeviceSize size{0};
        void* mapped{nullptr}; // nullptr if device-local; non-null if host-visible coherent
    };

    // Allocate a STORAGE_BUFFER for compute use. `hostVisible=true` returns a
    // buffer that's persistently mapped (mapped pointer in result); otherwise
    // device-local (mapped is nullptr; use staging + cmdCopyBuffer to fill).
    // Adds VK_BUFFER_USAGE_TRANSFER_DST_BIT and TRANSFER_SRC_BIT so the buffer
    // can be filled / copied from without re-creation.
    [[nodiscard]] ComputeBuffer createComputeBuffer(VkDeviceSize bytes, bool hostVisible);
    void destroyComputeBuffer(ComputeBuffer& buf) noexcept;

    // Read-only accessors. Returned handles are valid until the renderer is
    // shut down; callers must not destroy them.
    [[nodiscard]] VkDevice device() const noexcept { return device_; }
    [[nodiscard]] VkDescriptorPool descriptorPool() const noexcept { return descriptorPool_; }
    [[nodiscard]] VkQueue graphicsQueue() const noexcept { return graphicsQueue_; }
    [[nodiscard]] std::uint32_t graphicsQueueFamily() const noexcept;
    // VMA allocator the renderer owns. Exposed so subsystems like
    // ClusterRenderer can plug their own BufferArenas into the same
    // allocator instance — avoids running two parallel VMA pool states.
    [[nodiscard]] VmaAllocator allocator() const noexcept { return allocator_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    // Stage `bytes` from CPU memory into the renderer's per-frame staging
    // ring, then record a vkCmdCopyBuffer(staging → dst@dstOffset) for the
    // GPU to consume at end-of-frame. Used by ClusterRenderer (and any
    // future subsystem) to push into device-local arenas through the same
    // staging path that chunk uploads use.
    //
    // Returns false if the staging ring is full this frame; caller should
    // defer the upload. The staging ring is sized at 16 MB per in-flight
    // frame, comfortably above the largest cluster mesh (~50-100 KB).
    [[nodiscard]] bool stagingUpload(VkBuffer dst, VkDeviceSize dstOffset,
                                     const void* data, VkDeviceSize bytes);

    // Shared material table descriptor set (set 1 in the chunk pipeline
    // layout). Exposed so ClusterRenderer can bind the same materials in
    // the cluster pipeline — clusters render with the same material atlas
    // as chunks for visual continuity across LOD boundaries.
    [[nodiscard]] VkDescriptorSet materialDescriptorSet() const noexcept { return materialDescriptorSet_; }

    // External-draw hook context. Passed to any registered hook after the
    // chunk draws complete and before the debug-line / UI passes — the
    // same render pass, same depth buffer, same push-constant payload.
    // Used by ClusterRenderer to inject LOD2 cluster draws into the
    // frame without VulkanRenderer needing a direct dependency on
    // ClusterRenderer's type.
    struct ExternalDrawContext {
        VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
        // Same layout as the chunk path's vkCmdPushConstants payload.
        // Receivers can repackage into their own push-constants struct.
        core::Mat4 viewProjection{};
        float lightParams[4]{};
        float cameraParams[4]{};
        float cameraWorldParams[4]{};
        float atmosphereParams[4]{};
    };
    using ExternalDrawHook = std::function<void(const ExternalDrawContext&)>;
    using SwapchainRecreatedHook = std::function<void()>;
    // Set an "after chunk draws" callback. Pass an empty function to
    // remove. The hook is invoked once per frame inside the same render
    // pass that drew chunks, so receivers can issue further indexed
    // draws against the swapchain framebuffer without managing their
    // own render passes.
    void setExternalDrawHook(ExternalDrawHook hook);
    // Called after VulkanRenderer has finished rebuilding its swapchain,
    // render pass, framebuffers, and internal graphics pipelines. External
    // modules whose pipelines were created against renderPass() must rebuild
    // their swapchain-dependent pipeline objects here before the next frame.
    void setSwapchainRecreatedHook(SwapchainRecreatedHook hook);

    // Render pass + extent. ClusterRenderer needs the render pass at
    // pipeline-creation time and the swapchain extent for the dynamic
    // viewport / scissor.
    [[nodiscard]] VkRenderPass renderPass() const noexcept { return renderPass_; }
    [[nodiscard]] VkExtent2D swapchainExtent() const noexcept { return swapchainExtent_; }

private:
    struct QueueFamilies {
        std::optional<std::uint32_t> graphics;
        std::optional<std::uint32_t> present;

        [[nodiscard]] bool complete() const noexcept { return graphics.has_value() && present.has_value(); }
    };

    struct SwapchainSupport {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    struct GpuBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkDeviceSize size{};
    };

    struct UploadedChunkMesh {
        world::ChunkCoord coord{};
        Revision revision{};
        std::uint64_t meshRevisionHash{};
        BufferArena::Slice vertexSlice{};
        BufferArena::Slice indexSlice{};
        std::uint32_t vertexCount{};
        std::uint32_t indexCount{};
        std::uint32_t opaqueIndexCount{};
        std::uint32_t opaqueFirstIndex{};
        std::uint32_t cutoutIndexCount{};
        std::uint32_t cutoutFirstIndex{};
        std::uint32_t transparentIndexCount{};
        std::uint32_t transparentFirstIndex{};
    };

    struct DebugLineMesh {
        std::optional<world::PlanetCoord> block{};
        GpuBuffer vertices{};
        GpuBuffer indices{};
        std::uint32_t indexCount{};
    };

    struct RetiredBuffer {
        GpuBuffer buffer{};
        std::uint64_t retireFrame{};
    };

    struct RetiredSlice {
        BufferArena::Slice slice{};
        std::uint64_t retireFrame{};
    };

    struct PendingUpload {
        VkDeviceSize stagingOffset{};
        VkBuffer dst{VK_NULL_HANDLE};
        VkDeviceSize dstOffset{};
        VkDeviceSize bytes{};
    };

    struct UiVertex {
        float position[2]{};
        float color[4]{};
    };

    struct PushConstants {
        core::Mat4 viewProjection{};
        float lightParams[4]{};   // sunDir.xyz, time (currently unused .w)
        // W0: underwater state for the fragment shader.
        // .x = underwater strength (0=above water, 1=fully submerged)
        // .y..w = water tint RGB applied as fog when underwater
        float cameraParams[4]{};
        // World-space camera position lets the fragment shader do cheap
        // aerial perspective and far-terrain light lift without extra passes.
        float cameraWorldParams[4]{}; // .xyz = camera world position
        float atmosphereParams[4]{};  // .x near, .y far, .z strength, .w far-light lift
    };

    struct SkyPushConstants {
        float cameraForward[4]{};
        float cameraRight[4]{};
        float cameraUpAspect[4]{};
        float sunDirTime[4]{};
        float skyParams[4]{}; // horizon lift, saturation, cloud strength, brightness
    };

    struct ChunkSceneEntry {
        float origin[4]{};
        float boundsMin[4]{};
        float extent[4]{};
        std::uint32_t indexCount{};
        std::uint32_t firstIndex{};
        std::int32_t  vertexOffset{};
        std::uint32_t cutoutIndexCount{};
        std::uint32_t cutoutFirstIndex{};
        std::int32_t  cutoutVertexOffset{};
        std::uint32_t transparentIndexCount{};
        std::uint32_t transparentFirstIndex{};
        std::int32_t  transparentVertexOffset{};
        std::uint32_t pad0{};
        std::uint32_t pad1{};
        std::uint32_t pad2{};
    };

    struct CullPushConstants {
        core::Mat4 viewProjection{};
        std::uint32_t chunkCount{};
        std::uint32_t maxCommands{};
        std::uint32_t pad1{};
        std::uint32_t pad2{};
    };

    struct FrameContext {
        VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
        VkFence inFlight{VK_NULL_HANDLE};
        VkSemaphore imageAvailable{VK_NULL_HANDLE};
        VkSemaphore renderFinished{VK_NULL_HANDLE};

        GpuBuffer indirectCommand{};
        void* indirectMapped{nullptr};
        GpuBuffer indirectCount{};
        void* indirectCountMapped{nullptr};
        GpuBuffer chunkOrigin{};
        void* originMapped{nullptr};
        GpuBuffer scene{};
        void* sceneMapped{nullptr};
        std::uint32_t sceneCount{0};
        std::uint64_t sceneGeneration{0};
        std::vector<std::uint64_t> sceneEntryGenerations;
        bool submittedGpuCull{false};
        bool compareGpuCull{false};
        std::uint32_t submittedSceneCount{0};
        std::uint32_t submittedCpuVisibleCount{0};
        std::uint32_t submittedGpuDrawCommands{0};

        VkDescriptorSet chunkDescriptorSet{VK_NULL_HANDLE};
        VkDescriptorSet cullDescriptorSet{VK_NULL_HANDLE};

        GpuBuffer uiVertex{};
        void* uiMapped{nullptr};
        std::uint32_t uiVertexCount{};
    };

    void createInstance(const RendererConfig& config);
    void createSurface(const RendererConfig& config);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();
    void createSwapchain(const RendererConfig& config);
    void createImageViews();
    void createRenderPass();
    void createGraphicsPipeline();
    void createSkyPipeline();
    void createDebugLinePipeline();
    void createUiPipeline();
    void createDepthResources();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void createIndirectResources();
    void destroyIndirectResources();
    void createUiResources();
    void destroyUiResources();
    void createMaterialResources();
    void destroyMaterialResources();
    void createCullResources();
    void destroyCullResources();
    void collectCompletedGpuCullStats(FrameContext& frame);
    void rebuildSceneBufferIfDirty(FrameContext& frame);
    void upsertSceneEntryForCoord(world::ChunkCoord coord, const ChunkSceneEntry& entry);
    void removeSceneEntriesForChunk(world::ChunkCoord chunkCoord);
    void flushRetiredSlices();
    void recordPendingUploads(VkCommandBuffer commandBuffer);
    void rebuildUiVertexBuffer(FrameContext& frame);
    void recordFrameCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex);
    void recreateSwapchain();
    void cleanupSwapchain();
    void destroyBuffer(GpuBuffer& buffer);
    void retireBuffer(GpuBuffer& buffer);
    void flushRetiredBuffers();
    void destroyAllRetiredBuffers();
    void destroyImage(VkImage& image, VmaAllocation& allocation, VkImageView& view);
    [[nodiscard]] GpuBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) const;
    void createImage(
        std::uint32_t width,
        std::uint32_t height,
        VkFormat format,
        VkImageUsageFlags usage,
        VkMemoryPropertyFlags properties,
        VkImage& image,
        VmaAllocation& allocation) const;
    [[nodiscard]] VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) const;
    void uploadBuffer(GpuBuffer& buffer, const void* data, VkDeviceSize size) const;
    void uploadViaStaging(GpuBuffer& dst, const void* data, VkDeviceSize bytes);
    [[nodiscard]] bool uploadIntoBuffer(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize bytes);
    [[nodiscard]] VkShaderModule createShaderModule(const std::vector<char>& code) const;
    [[nodiscard]] VkFormat findDepthFormat() const;

    [[nodiscard]] bool deviceSuitable(VkPhysicalDevice device) const;
    [[nodiscard]] QueueFamilies findQueueFamilies(VkPhysicalDevice device) const;
    [[nodiscard]] SwapchainSupport querySwapchainSupport(VkPhysicalDevice device) const;
    [[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    [[nodiscard]] VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
    [[nodiscard]] VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, const RendererConfig& config) const;

    RenderGraph graph_;
    RendererConfig swapchainConfig_{};
    bool initialized_{false};
    VkInstance instance_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkQueue graphicsQueue_{VK_NULL_HANDLE};
    VkQueue presentQueue_{VK_NULL_HANDLE};
    QueueFamilies queueFamilies_{};
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkFormat swapchainFormat_{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchainExtent_{};
    VkFormat depthFormat_{VK_FORMAT_UNDEFINED};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    VkImage depthImage_{VK_NULL_HANDLE};
    VmaAllocation depthAllocation_{VK_NULL_HANDLE};
    VkImageView depthImageView_{VK_NULL_HANDLE};
    VkRenderPass renderPass_{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline graphicsPipeline_{VK_NULL_HANDLE};
    VkPipeline transparentPipeline_{VK_NULL_HANDLE};
    VkPipelineLayout skyPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline skyPipeline_{VK_NULL_HANDLE};
    VkPipeline debugLinePipeline_{VK_NULL_HANDLE};
    VkPipelineLayout uiPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline uiPipeline_{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> framebuffers_;
    VkCommandPool commandPool_{VK_NULL_HANDLE};

    // Dear ImGui resources. ImGui needs its own descriptor pool (it expects
    // to allocate a small number of CIS sets at init for fonts/textures);
    // we also keep init state so shutdown can run idempotently even if
    // initialize() fails partway through.
    VkDescriptorPool imguiDescriptorPool_{VK_NULL_HANDLE};
    bool imguiInitialized_{false};
    DebugCamera debugCamera_;
    BufferArena vertexArena_{};
    BufferArena indexArena_{};

    static constexpr std::uint32_t kMaxDrawCommands = 16384;
    static constexpr std::uint32_t kMaxIndirectCommands = kMaxDrawCommands * 3;
    static constexpr std::uint32_t kFramesInFlight = 2;
    static constexpr VkDeviceSize kStagingSlotSize = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::uint32_t kMaxUiRects = 2048;

    VkDescriptorSetLayout chunkDescriptorSetLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout cullDescriptorSetLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout materialDescriptorSetLayout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    VkDescriptorSet materialDescriptorSet_{VK_NULL_HANDLE};
    GpuBuffer materialTableBuffer_{};
    VkPipelineLayout cullPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline cullPipeline_{VK_NULL_HANDLE};
    bool useGpuCull_{false};
    bool compareGpuCull_{false};
    bool supportsDrawIndirectCount_{false};

    std::array<FrameContext, kFramesInFlight> frames_{};
    std::uint32_t currentFrame_{0};
    std::uint32_t acquiredImageIndex_{0};
    bool frameActive_{false};
    std::unordered_map<world::ChunkCoord, UploadedChunkMesh, world::ChunkCoordHash> uploadedMeshes_;
    DebugLineMesh debugLineMesh_{};
    std::vector<RetiredBuffer> retiredBuffers_;
    std::vector<RetiredSlice> retiredVertexSlices_;
    std::vector<RetiredSlice> retiredIndexSlices_;
    std::vector<PendingUpload> pendingUploads_;
    std::vector<UiRect> uiOverlayRects_;
    std::uint64_t pendingDrawableChunks_{};
    FrameStats frameStats_{};
    std::uint64_t totalUploadedMeshBytes_{};
    std::uint64_t frameIndex_{};
    // W0: underwater fog amount, set by Application each frame.
    float cameraUnderwaterStrength_{0.0F};
    AtmosphereSettings atmosphereSettings_{};
    SkySettings skySettings_{};
    float clearColor_[4]{0.50F, 0.68F, 0.92F, 1.0F};

    // External-draw hook (Phase 1C-4b). Application sets this in initialize
    // to invoke ClusterRenderer::recordDraws after chunk draws complete.
    // Empty function = no-op (skipped in recordFrameCommands). Lives in
    // the renderer because the call site is mid-render-pass, but the
    // hook itself is provided by the caller — keeps VulkanRenderer
    // independent of ClusterRenderer's type.
    ExternalDrawHook externalDrawHook_;
    SwapchainRecreatedHook swapchainRecreatedHook_;

    VkBuffer stagingArenaBuffer_{VK_NULL_HANDLE};
    VmaAllocation stagingArenaAllocation_{VK_NULL_HANDLE};
    std::byte* stagingArenaMapped_{nullptr};
    VkDeviceSize stagingSlotOffset_{0};

    std::vector<ChunkSceneEntry> sceneEntries_;
    std::vector<world::ChunkCoord> sceneCoords_;
    std::vector<std::uint64_t> sceneEntryGenerations_;
    std::unordered_map<world::ChunkCoord, std::size_t, world::ChunkCoordHash> sceneEntryIndex_;
    // (Previous `chunkToSections_` multimap removed: with 1 section per chunk
    // the chunk coord IS the scene-entry key, so `sceneEntryIndex_` alone
    // covers the lookup. Removed to eliminate the heaviest hash-map mutation
    // from the install-loop hot path.)
    std::uint64_t nextSceneEntryGeneration_{1};
    std::uint64_t sceneGeneration_{1};
};

} // namespace voxel::render
