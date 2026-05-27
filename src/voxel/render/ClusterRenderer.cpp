#include <voxel/render/ClusterRenderer.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <vulkan/vulkan.h>

#include <voxel/core/Logger.hpp>
#include <voxel/render/BufferArena.hpp>
#include <voxel/render/VulkanRenderer.hpp>

#ifndef VOXEL_SHADER_DIR
#define VOXEL_SHADER_DIR "shaders"
#endif

// Phase 1C-2: cluster GPU resources (vertex/index arenas + scene-entry SSBO).
// Upload + render path stubbed at the bookkeeping level; Phase 1C-3 wires
// the actual staging copies and 1C-4 adds the graphics pipeline.

namespace voxel::render {

namespace {

// Cluster meshes at half resolution are roughly 16-32× the size of a typical
// chunk mesh (similar surface area, but the cluster spans 64 chunks worth).
// Real measurements pending Phase 1D in-game capture, so these budgets are
// conservative starting points and can be tuned later.
constexpr VkDeviceSize kClusterVertexArenaBytes = 64ULL * 1024ULL * 1024ULL;   // 64 MB
constexpr VkDeviceSize kClusterIndexArenaBytes  = 32ULL * 1024ULL * 1024ULL;   // 32 MB

// Scene entry layout matches `VulkanRenderer::ChunkSceneEntry` byte-for-byte
// so the existing voxel_cull.comp shader can be reused unchanged — it culls
// AABBs against the frustum and doesn't care whether the entry represents a
// chunk or a cluster. We declare a parallel struct here to keep cluster
// rendering decoupled from the renderer's internal types.
struct ClusterSceneEntry {
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
// Layout breakdown:
//   3 × vec4   = 48 bytes  (origin, boundsMin, extent)
//   3 × (u32+u32+i32) = 36 bytes  (per-surface draw params)
//   3 × u32    = 12 bytes  (padding to keep cleanly 16-byte-aligned arrays)
// Total = 96 bytes.
static_assert(sizeof(ClusterSceneEntry) == 96,
              "ClusterSceneEntry must match ChunkSceneEntry layout exactly so the "
              "GPU cull shader is shareable across LODs");

constexpr std::uint32_t kMaxClusterDrawCommands = 4096; // generous LOD2 ring cap
constexpr VkDeviceSize kSceneEntryBufferBytes =
    static_cast<VkDeviceSize>(kMaxClusterDrawCommands) * sizeof(ClusterSceneEntry);
// One vec4 per resident cluster (origin xyz + 4-byte pad). Same size cap as
// kMaxClusterDrawCommands so a sceneEntryIndex slot always maps to a valid
// origins[] entry.
constexpr VkDeviceSize kOriginsBufferBytes =
    static_cast<VkDeviceSize>(kMaxClusterDrawCommands) * 4U * sizeof(float);

// Indirect draw command buffer. Each cluster can emit up to 3 commands
// (opaque, cutout, transparent), so size = max clusters × 3.
//
// VkDrawIndexedIndirectCommand is 20 bytes: {indexCount, instanceCount,
// firstIndex, vertexOffset, firstInstance}. We host-visible map this so
// recordDraws can build the array directly into GPU-readable memory —
// no separate staging step needed.
constexpr VkDeviceSize kIndirectCommandBytes = 20U;
constexpr std::uint32_t kMaxIndirectCommands = kMaxClusterDrawCommands * 3U;
constexpr VkDeviceSize kIndirectBufferBytes =
    static_cast<VkDeviceSize>(kMaxIndirectCommands) * kIndirectCommandBytes;
static_assert(sizeof(VkDrawIndexedIndirectCommand) == kIndirectCommandBytes,
              "VkDrawIndexedIndirectCommand layout must be 20 bytes");

[[nodiscard]] std::vector<char> readSpv(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    const auto size = static_cast<std::size_t>(file.tellg());
    std::vector<char> buf(size);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

} // namespace

struct ClusterRenderer::GpuResources {
    BufferArena vertexArena;
    BufferArena indexArena;
    VulkanRenderer::ComputeBuffer sceneEntryBuffer{};      // host-visible scratch for CPU writes
    VulkanRenderer::ComputeBuffer clusterOriginsBuffer{};  // SSBO consumed by cluster.vert
    VulkanRenderer::ComputeBuffer indirectCommandBuffer{}; // host-visible, holds VkDrawIndexedIndirectCommand[]

    // Cluster pipeline + supporting descriptor objects.
    VkDescriptorSetLayout clusterDescriptorSetLayout{VK_NULL_HANDLE};
    // Material set layout is shape-compatible with VulkanRenderer's
    // materialDescriptorSetLayout_ — we create our own here so the cluster
    // pipeline layout doesn't depend on the renderer's internal type.
    VkDescriptorSetLayout materialDescriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet       clusterDescriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout      pipelineLayout{VK_NULL_HANDLE};
    VkPipeline            pipeline{VK_NULL_HANDLE};
    VkPipeline            pipelineTransparent{VK_NULL_HANDLE};
};

// Push-constants layout MUST match VulkanRenderer::PushConstants byte-for-
// byte so chunks and clusters share the same vkCmdPushConstants payload.
// Kept private to this .cpp (the public ClusterRenderer header doesn't
// need it, and the renderer's struct is private). If VulkanRenderer's
// PushConstants ever grows, the static_assert in setupPipelineLayout()
// (below) will fire on size mismatch.
struct ClusterPushConstants {
    float viewProjection[16];   // mat4
    float lightParams[4];       // sunDir.xyz, time
    float cameraParams[4];      // underwater state for voxel.frag
    float cameraWorldParams[4]; // camera xyz for aerial perspective
    float atmosphereParams[4];  // fog distances + far-light lift
};
static_assert(sizeof(ClusterPushConstants) == 128,
              "ClusterPushConstants must remain in sync with VulkanRenderer::PushConstants");

ClusterRenderer::ClusterRenderer(VulkanRenderer& renderer)
    : renderer_(renderer),
      gpu_(std::make_unique<GpuResources>())
{
}

ClusterRenderer::~ClusterRenderer()
{
    shutdown();
}

bool ClusterRenderer::initialize()
{
    if (initialized_) {
        return true;
    }
    if (!renderer_.initialized()) {
        Logger::error("ClusterRenderer::initialize: VulkanRenderer not initialized yet");
        return false;
    }

    // ---- 1. Vertex + index BufferArenas for cluster meshes -------------
    // Separate from chunk arenas: cluster meshes change at a different
    // cadence (whole-cluster invalidation when ANY contained chunk edits),
    // so mixing would fragment the chunk arena. Each arena owns its own
    // VkBuffer and VMA allocation.
    gpu_->vertexArena.initialize(renderer_.device(), renderer_.allocator(),
                                  kClusterVertexArenaBytes,
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    gpu_->indexArena.initialize(renderer_.device(), renderer_.allocator(),
                                 kClusterIndexArenaBytes,
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                               | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                               | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    // ---- 2. Scene-entry storage buffer ---------------------------------
    // Host-visible coherent so the main thread can write entries directly
    // (mirrors how ChunkSceneEntry is staged in VulkanRenderer's existing
    // path — Phase 1C-3 will sync this to a device-local copy when GPU
    // cull is wired up).
    gpu_->sceneEntryBuffer = renderer_.createComputeBuffer(
        kSceneEntryBufferBytes, /*hostVisible=*/true);
    if (gpu_->sceneEntryBuffer.buffer == VK_NULL_HANDLE) {
        Logger::error("ClusterRenderer::initialize: scene entry buffer allocation failed");
        shutdown();
        return false;
    }

    // Pre-reserve the bookkeeping map at a reasonable size — most LOD2
    // rings have a few hundred clusters resident, peak ~1000.
    uploadedClusters_.reserve(512);

    // ---- 3. Cluster origins SSBO (consumed by cluster.vert) ------------
    // One vec4 per resident cluster slot. Host-visible coherent so the
    // CPU can write origins directly without an upload step (matches the
    // chunk path's CPU-write approach).
    gpu_->clusterOriginsBuffer = renderer_.createComputeBuffer(
        kOriginsBufferBytes, /*hostVisible=*/true);
    if (gpu_->clusterOriginsBuffer.buffer == VK_NULL_HANDLE) {
        Logger::error("ClusterRenderer::initialize: cluster origins buffer allocation failed");
        shutdown();
        return false;
    }

    // ---- 3b. Indirect-draw command buffer (host-visible) ----------------
    // Holds up to kMaxIndirectCommands VkDrawIndexedIndirectCommand
    // entries. recordDraws writes them at frame start; the indirect
    // draw call reads them from the GPU. Host-coherent mapping keeps
    // writes immediately visible (no manual flush needed).
    gpu_->indirectCommandBuffer = renderer_.createComputeBuffer(
        kIndirectBufferBytes, /*hostVisible=*/true);
    if (gpu_->indirectCommandBuffer.buffer == VK_NULL_HANDLE) {
        Logger::error("ClusterRenderer::initialize: indirect command buffer allocation failed");
        shutdown();
        return false;
    }

    // ---- 4. Descriptor set layouts -------------------------------------
    // Cluster set (set 0): one storage buffer at binding 0 — cluster.vert
    // reads `clusterInstances.origins[gl_InstanceIndex]`.
    VkDescriptorSetLayoutBinding clusterBinding{};
    clusterBinding.binding = 0;
    clusterBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    clusterBinding.descriptorCount = 1;
    clusterBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo clusterLayoutInfo{};
    clusterLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    clusterLayoutInfo.bindingCount = 1;
    clusterLayoutInfo.pBindings = &clusterBinding;
    if (vkCreateDescriptorSetLayout(renderer_.device(), &clusterLayoutInfo, nullptr,
                                    &gpu_->clusterDescriptorSetLayout) != VK_SUCCESS) {
        Logger::error("ClusterRenderer::initialize: cluster descriptor layout failed");
        shutdown();
        return false;
    }

    // Material set (set 1): shape-compatible with VulkanRenderer's
    // materialDescriptorSetLayout_. We create our own layout object so the
    // cluster pipeline layout doesn't reference the renderer's internal
    // VkDescriptorSetLayout — but the SHAPE matches, so binding the
    // renderer's material descriptor set at draw time is valid.
    VkDescriptorSetLayoutBinding materialBinding{};
    materialBinding.binding = 0;
    materialBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    materialBinding.descriptorCount = 1;
    materialBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo materialLayoutInfo{};
    materialLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    materialLayoutInfo.bindingCount = 1;
    materialLayoutInfo.pBindings = &materialBinding;
    if (vkCreateDescriptorSetLayout(renderer_.device(), &materialLayoutInfo, nullptr,
                                    &gpu_->materialDescriptorSetLayout) != VK_SUCCESS) {
        Logger::error("ClusterRenderer::initialize: material descriptor layout failed");
        shutdown();
        return false;
    }

    // ---- 5. Allocate cluster descriptor set ----------------------------
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = renderer_.descriptorPool();
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &gpu_->clusterDescriptorSetLayout;
    if (vkAllocateDescriptorSets(renderer_.device(), &allocInfo,
                                  &gpu_->clusterDescriptorSet) != VK_SUCCESS) {
        Logger::error("ClusterRenderer::initialize: descriptor set allocation failed "
                      "(descriptor pool may be exhausted)");
        shutdown();
        return false;
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = gpu_->clusterOriginsBuffer.buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = gpu_->clusterDescriptorSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(renderer_.device(), 1, &write, 0, nullptr);

    // ---- 6. Pipeline layout (cluster set + material set + push consts) -
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(ClusterPushConstants);
    const std::array<VkDescriptorSetLayout, 2> setLayouts{
        gpu_->clusterDescriptorSetLayout,
        gpu_->materialDescriptorSetLayout,
    };
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    if (vkCreatePipelineLayout(renderer_.device(), &layoutInfo, nullptr,
                               &gpu_->pipelineLayout) != VK_SUCCESS) {
        Logger::error("ClusterRenderer::initialize: pipeline layout failed");
        shutdown();
        return false;
    }

    // ---- 7. Load shaders ------------------------------------------------
    const std::filesystem::path vertSpvPath =
        std::filesystem::path(VOXEL_SHADER_DIR) / "cluster.vert.spv";
    const std::filesystem::path fragSpvPath =
        std::filesystem::path(VOXEL_SHADER_DIR) / "voxel.frag.spv";
    const auto vertSpv = readSpv(vertSpvPath);
    const auto fragSpv = readSpv(fragSpvPath);
    if (vertSpv.empty() || fragSpv.empty()) {
        Logger::error("ClusterRenderer::initialize: failed to read shader SPV "
                      "(cluster.vert.spv or voxel.frag.spv missing)");
        shutdown();
        return false;
    }

    const auto makeShaderModule = [&](const std::vector<char>& code) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size();
        info.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(renderer_.device(), &info, nullptr, &m);
        return m;
    };
    VkShaderModule vertModule = makeShaderModule(vertSpv);
    VkShaderModule fragModule = makeShaderModule(fragSpv);
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        Logger::error("ClusterRenderer::initialize: shader module creation failed");
        if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(renderer_.device(), vertModule, nullptr);
        if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(renderer_.device(), fragModule, nullptr);
        shutdown();
        return false;
    }

    // ---- 8. Build the graphics pipeline --------------------------------
    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";
    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";
    const VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    // Vertex input — ClusterVertex format. Convert the locked desc table
    // from ClusterMesh.hpp into Vulkan structs.
    VkVertexInputBindingDescription binding{};
    binding.binding = meshing::ClusterVertexInputDesc::kBinding;
    binding.stride = meshing::ClusterVertexInputDesc::kStride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        const auto& attr = meshing::ClusterVertexInputDesc::kAttributes[i];
        attributes[i].location = attr.location;
        attributes[i].binding = meshing::ClusterVertexInputDesc::kBinding;
        attributes[i].format = static_cast<VkFormat>(attr.format);
        attributes[i].offset = attr.offset;
    }
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
    // MUST match VulkanRenderer's chunk pipeline. The engine uses
    // reverse-Z: depth buffer is cleared to 0.0 (far plane), gl_Position
    // outputs near=1/far=0, depth compare is GREATER_OR_EQUAL so closer
    // (higher-z) fragments win. We previously had VK_COMPARE_OP_LESS
    // here, which against a reverse-Z buffer meant the cluster passed
    // only when it was FARTHER than the chunk — exactly the bug
    // observed (far clusters drawing on top of near chunks).
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
      | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    pipelineInfo.layout = gpu_->pipelineLayout;
    pipelineInfo.renderPass = renderer_.renderPass();
    pipelineInfo.subpass = 0;

    const auto opaqueResult = vkCreateGraphicsPipelines(
        renderer_.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu_->pipeline);

    // Transparent variant: same pipeline but depth-write disabled so
    // transparent draws don't punch holes in subsequent depth tests.
    depthStencil.depthWriteEnable = VK_FALSE;
    const auto transparentResult = vkCreateGraphicsPipelines(
        renderer_.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu_->pipelineTransparent);

    vkDestroyShaderModule(renderer_.device(), vertModule, nullptr);
    vkDestroyShaderModule(renderer_.device(), fragModule, nullptr);

    if (opaqueResult != VK_SUCCESS || transparentResult != VK_SUCCESS) {
        Logger::error("ClusterRenderer::initialize: pipeline creation failed");
        shutdown();
        return false;
    }

    initialized_ = true;
    Logger::info("ClusterRenderer::initialize: LOD2 cluster pipeline ready "
                 "(vertex arena 64 MB, index arena 32 MB, scene entries "
                 + std::to_string(kSceneEntryBufferBytes / 1024) +
                 " KB, origins " + std::to_string(kOriginsBufferBytes / 1024) +
                 " KB, opaque + transparent pipelines compiled, draw hook "
                 "wired). Cluster meshes build lazily — 1 per tick until "
                 "the LOD2 ring fills in.");
    return true;
}

void ClusterRenderer::shutdown() noexcept
{
    if (!gpu_) {
        return;
    }
    if (renderer_.device() != VK_NULL_HANDLE) {
        if (gpu_->pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(renderer_.device(), gpu_->pipeline, nullptr);
            gpu_->pipeline = VK_NULL_HANDLE;
        }
        if (gpu_->pipelineTransparent != VK_NULL_HANDLE) {
            vkDestroyPipeline(renderer_.device(), gpu_->pipelineTransparent, nullptr);
            gpu_->pipelineTransparent = VK_NULL_HANDLE;
        }
        if (gpu_->pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(renderer_.device(), gpu_->pipelineLayout, nullptr);
            gpu_->pipelineLayout = VK_NULL_HANDLE;
        }
        if (gpu_->materialDescriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(renderer_.device(), gpu_->materialDescriptorSetLayout, nullptr);
            gpu_->materialDescriptorSetLayout = VK_NULL_HANDLE;
        }
        if (gpu_->clusterDescriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(renderer_.device(), gpu_->clusterDescriptorSetLayout, nullptr);
            gpu_->clusterDescriptorSetLayout = VK_NULL_HANDLE;
        }
        // Descriptor sets are freed implicitly when the pool is destroyed.
        gpu_->clusterDescriptorSet = VK_NULL_HANDLE;
    }
    renderer_.destroyComputeBuffer(gpu_->clusterOriginsBuffer);
    renderer_.destroyComputeBuffer(gpu_->indirectCommandBuffer);
    renderer_.destroyComputeBuffer(gpu_->sceneEntryBuffer);
    gpu_->indexArena.shutdown();
    gpu_->vertexArena.shutdown();
    uploadedClusters_.clear();
    initialized_ = false;
}

bool ClusterRenderer::uploadClusterMesh(world::ClusterCoord coord,
                                         const meshing::ClusterMesh& mesh,
                                         LodTier tier)
{
    if (!initialized_) {
        return false;
    }
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return true; // nothing to upload — treat as success / no-op
    }

    // Dedup against an already-resident cluster at the same revision.
    const auto existing = uploadedClusters_.find(coord);
    if (existing != uploadedClusters_.end()
        && existing->second.sourceRevisionsHash == mesh.sourceRevisionsHash) {
        return true; // already current
    }

    // 1. Allocate arena slices for the new mesh BEFORE touching the
    //    existing entry. If allocation fails we want to leave the old
    //    cluster mesh intact (so the player still sees something).
    const VkDeviceSize vertexBytes =
        static_cast<VkDeviceSize>(mesh.vertices.size()) * sizeof(meshing::ClusterVertex);
    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(mesh.indices.size()) * sizeof(std::uint32_t);
    constexpr VkDeviceSize kVertexAlignment = sizeof(meshing::ClusterVertex); // 12 bytes
    constexpr VkDeviceSize kIndexAlignment  = sizeof(std::uint32_t);          // 4 bytes

    BufferArena::Slice vertexSlice{};
    BufferArena::Slice indexSlice{};
    if (!gpu_->vertexArena.allocate(vertexBytes, kVertexAlignment, vertexSlice)) {
        Logger::warn("ClusterRenderer::uploadClusterMesh: vertex arena full; "
                     "deferring upload for coord ("
                     + std::to_string(coord.x) + "," + std::to_string(coord.y)
                     + "," + std::to_string(coord.z) + ")");
        return false;
    }
    if (!gpu_->indexArena.allocate(indexBytes, kIndexAlignment, indexSlice)) {
        gpu_->vertexArena.release(vertexSlice);
        Logger::warn("ClusterRenderer::uploadClusterMesh: index arena full");
        return false;
    }

    // 2. Stage both buffers via the renderer's staging ring. If either
    //    upload fails (staging full this frame) we release the slices we
    //    just took and let the caller retry next frame.
    if (!renderer_.stagingUpload(gpu_->vertexArena.buffer(), vertexSlice.offset,
                                  mesh.vertices.data(), vertexBytes)
        || !renderer_.stagingUpload(gpu_->indexArena.buffer(), indexSlice.offset,
                                     mesh.indices.data(), indexBytes)) {
        gpu_->vertexArena.release(vertexSlice);
        gpu_->indexArena.release(indexSlice);
        return false;
    }

    // 3. Retire the old slices if we're replacing an existing cluster.
    //    For Phase 1C-3 we release immediately; the staging upload above
    //    is recorded for *this* frame's submission, so the GPU is still
    //    reading the OLD slices for the previous frame's draws — Phase
    //    1C-4 will introduce a proper retire queue tied to the renderer's
    //    fence so we don't recycle a slice the GPU is still reading.
    //    Until 1C-4 ships, the immediate release is safe ONLY because
    //    we're not yet rendering cluster meshes (the draw path is
    //    stubbed). Documented here so the assumption is explicit.
    if (existing != uploadedClusters_.end()) {
        gpu_->vertexArena.release(
            {existing->second.vertexOffset, existing->second.vertexBytes});
        gpu_->indexArena.release(
            {existing->second.indexOffset, existing->second.indexBytes});
    }

    // 4. Build a scene entry. Origin is the cluster's world block origin.
    //    Bounds extent is the full cluster volume + 1 block of conservative
    //    padding (matches the chunk scene entry's +1/+2 padding so frustum
    //    culling matches the chunk path's looseness).
    const auto chunkOrigin = world::clusterChunkOrigin(coord);
    const float originX = static_cast<float>(chunkOrigin.x * world::ChunkSize);
    const float originY = static_cast<float>(chunkOrigin.y * world::ChunkSize);
    const float originZ = static_cast<float>(chunkOrigin.z * world::ChunkSize);

    // Per-tier world extent for the frustum-cull AABB. LOD2 covers a
    // cluster's 128-block volume; LOD3 covers a region's 512-block
    // volume. We add +2 of slack on each axis to match the existing
    // chunk path's frustum-test looseness.
    const float worldExtent =
        (tier == LodTier::Region)
            ? static_cast<float>(world::RegionBlockExtent)
            : static_cast<float>(world::ClusterBlockExtent);

    ClusterSceneEntry entry{};
    entry.origin[0] = originX;
    entry.origin[1] = originY;
    entry.origin[2] = originZ;
    entry.origin[3] = 0.0F;
    entry.boundsMin[0] = originX - 1.0F;
    entry.boundsMin[1] = originY - 1.0F;
    entry.boundsMin[2] = originZ - 1.0F;
    entry.boundsMin[3] = 0.0F;
    entry.extent[0] = worldExtent + 2.0F;
    entry.extent[1] = worldExtent + 2.0F;
    entry.extent[2] = worldExtent + 2.0F;
    entry.extent[3] = 0.0F;

    // Split draw ranges by surface so the renderer can issue separate
    // opaque/cutout/transparent draws. Phase 1B mesher emits all three
    // surface types into `mesh.drawRanges`; the cluster scene entry
    // mirrors the chunk path's first-in-surface convention.
    entry.indexCount = 0;
    entry.cutoutIndexCount = 0;
    entry.transparentIndexCount = 0;
    const auto indexBase = static_cast<std::uint32_t>(indexSlice.offset / sizeof(std::uint32_t));
    const auto vertexOff = static_cast<std::int32_t>(vertexSlice.offset / sizeof(meshing::ClusterVertex));
    for (const auto& range : mesh.drawRanges) {
        if (range.surface == meshing::MeshSurface::Opaque) {
            if (entry.indexCount == 0) {
                entry.firstIndex = indexBase + range.indexOffset;
                entry.vertexOffset = vertexOff;
            }
            entry.indexCount += range.indexCount;
        } else if (range.surface == meshing::MeshSurface::Cutout) {
            if (entry.cutoutIndexCount == 0) {
                entry.cutoutFirstIndex = indexBase + range.indexOffset;
                entry.cutoutVertexOffset = vertexOff;
            }
            entry.cutoutIndexCount += range.indexCount;
        } else {
            if (entry.transparentIndexCount == 0) {
                entry.transparentFirstIndex = indexBase + range.indexOffset;
                entry.transparentVertexOffset = vertexOff;
            }
            entry.transparentIndexCount += range.indexCount;
        }
    }

    // 5. Write the scene entry into the host-visible buffer. We pick the
    //    smallest unused slot (or reuse the existing slot when replacing).
    //    Phase 1C-4 will add the GPU-side cluster scene entry sync (this
    //    buffer is host-visible so the write is immediately visible to
    //    the device once the next frame's command buffer is submitted).
    std::uint32_t sceneIdx = existing != uploadedClusters_.end()
        ? existing->second.sceneEntryIndex
        : static_cast<std::uint32_t>(uploadedClusters_.size());
    if (gpu_->sceneEntryBuffer.mapped != nullptr) {
        auto* sceneBase = static_cast<ClusterSceneEntry*>(gpu_->sceneEntryBuffer.mapped);
        sceneBase[sceneIdx] = entry;
    }

    // 6. Update bookkeeping.
    UploadedCluster record{};
    record.vertexOffset = vertexSlice.offset;
    record.vertexBytes = vertexBytes;
    record.indexOffset = indexSlice.offset;
    record.indexBytes = indexBytes;
    record.sceneEntryIndex = sceneIdx;
    record.sourceRevisionsHash = mesh.sourceRevisionsHash;
    record.tier = tier;
    uploadedClusters_[coord] = record;
    return true;
}

void ClusterRenderer::removeClusterMesh(world::ClusterCoord coord)
{
    if (!initialized_) {
        return;
    }
    const auto it = uploadedClusters_.find(coord);
    if (it == uploadedClusters_.end()) {
        return;
    }
    // Phase 1C-4 TODO: defer the release until the GPU finishes its
    // in-flight draws referencing this slice (via the renderer's
    // retire-buffer queue). Immediate release is safe today only
    // because cluster rendering isn't wired into recordFrameCommands
    // yet — there are no in-flight draws against these slices.
    gpu_->vertexArena.release({it->second.vertexOffset, it->second.vertexBytes});
    gpu_->indexArena.release({it->second.indexOffset, it->second.indexBytes});

    // Zero out the scene entry slot so a stale draw command can't reach
    // it before the GPU sees the eviction. Indices > 0 mean an active
    // draw range; clearing indexCount to 0 makes the cull shader skip it.
    if (gpu_->sceneEntryBuffer.mapped != nullptr
        && it->second.sceneEntryIndex != 0xFFFFFFFFu) {
        auto* sceneBase = static_cast<ClusterSceneEntry*>(gpu_->sceneEntryBuffer.mapped);
        sceneBase[it->second.sceneEntryIndex] = ClusterSceneEntry{};
    }
    uploadedClusters_.erase(it);
}

void ClusterRenderer::clearAllClusters()
{
    if (!initialized_) {
        return;
    }
    // Release every slice. After this, the arenas should be empty
    // (modulo the immediate-release caveat noted in uploadClusterMesh).
    for (const auto& [coord, rec] : uploadedClusters_) {
        gpu_->vertexArena.release({rec.vertexOffset, rec.vertexBytes});
        gpu_->indexArena.release({rec.indexOffset, rec.indexBytes});
    }
    for (const auto& [coord, rec] : uploadedRegions_) {
        gpu_->vertexArena.release({rec.vertexOffset, rec.vertexBytes});
        gpu_->indexArena.release({rec.indexOffset, rec.indexBytes});
    }
    if (gpu_->sceneEntryBuffer.mapped != nullptr) {
        std::memset(gpu_->sceneEntryBuffer.mapped, 0,
                    static_cast<std::size_t>(gpu_->sceneEntryBuffer.size));
    }
    uploadedClusters_.clear();
    uploadedRegions_.clear();
}

std::size_t ClusterRenderer::residentClusterCount() const noexcept
{
    return uploadedClusters_.size();
}

void ClusterRenderer::setSkipDrawClusters(
    std::unordered_set<world::ClusterCoord, world::ClusterCoordHash> skip)
{
    skipDrawClusters_ = std::move(skip);
}

// ============================================================================
// LOD3 Region API. Same underlying machinery as the cluster path, just
// keyed by RegionCoord and stored in a separate map. uploadRegionMesh
// delegates to the cluster upload routine via a coord-cast: each
// RegionCoord (rx, ry, rz) is internally treated as a ClusterCoord
// {rx * RegionClusterExtent, ry * RegionClusterExtent, rz * RegionClusterExtent}
// for the purposes of computing the world origin in `uploadClusterMesh`,
// but the result is filed in `uploadedRegions_` keyed by the original
// RegionCoord. This way both paths use the same allocation + staging
// + scene-entry code.
// ============================================================================

bool ClusterRenderer::uploadRegionMesh(world::RegionCoord coord,
                                        const meshing::ClusterMesh& mesh)
{
    // The world origin of region (rx, ry, rz) lives at chunk
    // (rx * RegionChunkExtent, ...) — same as cluster (rx * RegionClusterExtent,
    // ...) when computed via clusterChunkOrigin. So we cast the
    // RegionCoord to the equivalent ClusterCoord and reuse the cluster
    // upload path with tier=Region. The result lands in uploadedRegions_
    // by virtue of the manual move at the end of this function.
    const world::ClusterCoord aliasCoord{
        coord.x * world::RegionClusterExtent,
        coord.y * world::RegionClusterExtent,
        coord.z * world::RegionClusterExtent,
    };

    // Manual dedup against the region map (mirrors uploadClusterMesh).
    const auto existingRegion = uploadedRegions_.find(coord);
    if (existingRegion != uploadedRegions_.end()
        && existingRegion->second.sourceRevisionsHash == mesh.sourceRevisionsHash) {
        return true; // already current
    }

    // Reuse the cluster upload routine. It allocates arenas, stages,
    // writes the scene entry, and inserts into uploadedClusters_[aliasCoord]
    // with tier=Region. We then MOVE that entry into uploadedRegions_
    // and erase the cluster map's temporary entry so the two coord
    // spaces don't permanently overlap.
    //
    // NOTE: if a real LOD2 cluster lives at `aliasCoord` (rare but
    // possible — e.g., when a region's origin happens to coincide with
    // a built cluster), the cluster path's dedup would short-circuit on
    // a hash match, or we'd overwrite the cluster entry. Application is
    // responsible for not building LOD2 clusters at region-origin
    // positions when LOD3 is active there (the tickRegionMaintenance
    // distance band starts past where clusters are built).
    if (!uploadClusterMesh(aliasCoord, mesh, LodTier::Region)) {
        return false;
    }
    auto handoff = uploadedClusters_.find(aliasCoord);
    if (handoff != uploadedClusters_.end()) {
        uploadedRegions_[coord] = handoff->second;
        uploadedClusters_.erase(handoff);
    }
    return true;
}

void ClusterRenderer::removeRegionMesh(world::RegionCoord coord)
{
    if (!initialized_) {
        return;
    }
    const auto it = uploadedRegions_.find(coord);
    if (it == uploadedRegions_.end()) {
        return;
    }
    gpu_->vertexArena.release({it->second.vertexOffset, it->second.vertexBytes});
    gpu_->indexArena.release({it->second.indexOffset, it->second.indexBytes});
    if (gpu_->sceneEntryBuffer.mapped != nullptr
        && it->second.sceneEntryIndex != 0xFFFFFFFFu) {
        auto* sceneBase = static_cast<ClusterSceneEntry*>(gpu_->sceneEntryBuffer.mapped);
        sceneBase[it->second.sceneEntryIndex] = ClusterSceneEntry{};
    }
    uploadedRegions_.erase(it);
}

std::size_t ClusterRenderer::residentRegionCount() const noexcept
{
    return uploadedRegions_.size();
}

void ClusterRenderer::setSkipDrawRegions(
    std::unordered_set<world::RegionCoord, world::RegionCoordHash> skip)
{
    skipDrawRegions_ = std::move(skip);
}

void ClusterRenderer::recordDraws(VkCommandBuffer commandBuffer,
                                   const float viewProjection[16],
                                   const float lightParams[4],
                                   const float cameraParams[4],
                                   const float cameraWorldParams[4],
                                   const float atmosphereParams[4])
{
    if (!initialized_ || !enabled_
        || (uploadedClusters_.empty() && uploadedRegions_.empty())
        || gpu_->pipeline == VK_NULL_HANDLE) {
        return;
    }

    // Extract frustum planes once per frame. We do it from the raw VP
    // float[16] by reinterpreting as voxel::core::Mat4 — same memory
    // layout (column-major 4x4 floats) by construction.
    {
        voxel::core::Mat4 vp{};
        std::memcpy(vp.m.data(), viewProjection, sizeof(vp.m));
        lastFrustum_.planes = voxel::core::extractFrustumPlanes(vp);
        lastFrustum_.valid = true;
    }
    const auto& frustum = lastFrustum_.planes;

    // ---- 1. Snapshot resident clusters into a draw list ----------------
    // We sort by scene-entry index for cache-friendly iteration and so
    // the origins SSBO writes happen in linear order. Each draw uses
    // `firstInstance = sceneIdx`, which becomes `gl_InstanceIndex` in
    // cluster.vert — that's how the vertex shader picks the right origin.
    struct DrawEntry {
        std::uint32_t sceneIdx;
        float originX, originY, originZ;
        float lodScale;   // 1.0 for LOD2, 4.0 for LOD3, etc.
        std::uint32_t opaqueIndexCount;
        std::uint32_t opaqueFirstIndex;
        std::int32_t  opaqueVertexOffset;
        std::uint32_t cutoutIndexCount;
        std::uint32_t cutoutFirstIndex;
        std::int32_t  cutoutVertexOffset;
        std::uint32_t transparentIndexCount;
        std::uint32_t transparentFirstIndex;
        std::int32_t  transparentVertexOffset;
    };

    // Per-LOD-tier scale factor that cluster.vert reads from origins[i].w.
    // Vertex positions are emitted in 0..128 "supervoxel × 2" units. LOD2
    // (cluster) treats them as block units directly (scale = 1). LOD3
    // (region) covers 4× the linear extent in world units, so the shader
    // scales positions up by 4. Higher tiers extend the same pattern.
    auto scaleForTier = [](LodTier t) noexcept -> float {
        switch (t) {
            case LodTier::Cluster: return 1.0F;
            case LodTier::Region:  return 4.0F;
        }
        return 1.0F;
    };
    std::vector<DrawEntry> draws;
    draws.reserve(uploadedClusters_.size());

    // The scene-entry buffer holds the per-cluster bounds + draw params
    // we wrote in uploadClusterMesh. Read them back to populate the
    // DrawEntry list — same data, but in the order we want to iterate.
    auto* sceneBase = static_cast<ClusterSceneEntry*>(gpu_->sceneEntryBuffer.mapped);
    std::size_t frustumCulled = 0;

    // Helper: given an entry + tier, push a DrawEntry into `draws`. We
    // share the body between the LOD2 cluster loop and the LOD3 region
    // loop below so any future cull/sort tweaks land in one place.
    auto buildDrawEntry = [&](const UploadedCluster& rec) {
        const auto& entry = sceneBase[rec.sceneEntryIndex];
        if (entry.indexCount == 0 && entry.cutoutIndexCount == 0
            && entry.transparentIndexCount == 0) {
            return; // empty, skip
        }
        const voxel::core::Vec3 minCorner{
            entry.boundsMin[0], entry.boundsMin[1], entry.boundsMin[2]};
        const voxel::core::Vec3 maxCorner{
            entry.boundsMin[0] + entry.extent[0],
            entry.boundsMin[1] + entry.extent[1],
            entry.boundsMin[2] + entry.extent[2]};
        if (!voxel::core::aabbIntersectsFrustum(frustum, minCorner, maxCorner)) {
            ++frustumCulled;
            return;
        }
        DrawEntry d{};
        d.sceneIdx = rec.sceneEntryIndex;
        d.originX = entry.origin[0];
        d.originY = entry.origin[1];
        d.originZ = entry.origin[2];
        d.lodScale = scaleForTier(rec.tier);
        d.opaqueIndexCount = entry.indexCount;
        d.opaqueFirstIndex = entry.firstIndex;
        d.opaqueVertexOffset = entry.vertexOffset;
        d.cutoutIndexCount = entry.cutoutIndexCount;
        d.cutoutFirstIndex = entry.cutoutFirstIndex;
        d.cutoutVertexOffset = entry.cutoutVertexOffset;
        d.transparentIndexCount = entry.transparentIndexCount;
        d.transparentFirstIndex = entry.transparentFirstIndex;
        d.transparentVertexOffset = entry.transparentVertexOffset;
        draws.push_back(d);
    };

    // ---- LOD2 cluster pass --------------------------------------------
    for (const auto& [coord, rec] : uploadedClusters_) {
        if (rec.sceneEntryIndex == 0xFFFFFFFFu) continue;
        if (skipDrawClusters_.count(coord) != 0) continue; // covered by LOD0
        buildDrawEntry(rec);
    }

    // ---- LOD3 region pass ---------------------------------------------
    // Regions get skipped when their region-coord is in skipDrawRegions_
    // (populated by Application when LOD2 clusters fully tile a region's
    // footprint — same persistence-policy idea, one tier higher).
    for (const auto& [coord, rec] : uploadedRegions_) {
        if (rec.sceneEntryIndex == 0xFFFFFFFFu) continue;
        if (skipDrawRegions_.count(coord) != 0) continue;
        buildDrawEntry(rec);
    }
    if (draws.empty()) {
        return;
    }
    std::sort(draws.begin(), draws.end(),
              [](const DrawEntry& a, const DrawEntry& b) { return a.sceneIdx < b.sceneIdx; });

    // One-shot diagnostic: log the FIRST time we actually execute draws.
    // Tells the user definitively that the cluster draw path is firing
    // (vs uploads happening but draws not).
    if (!firstDrawLogged_) {
        firstDrawLogged_ = true;
        std::uint64_t totalIndices = 0;
        for (const auto& d : draws) {
            totalIndices += d.opaqueIndexCount + d.cutoutIndexCount + d.transparentIndexCount;
        }
        voxel::Logger::info(
            "ClusterRenderer: first draw firing — " + std::to_string(draws.size())
            + " visible cluster(s), " + std::to_string(frustumCulled)
            + " frustum-culled, " + std::to_string(totalIndices / 3U)
            + " triangles total. If you still see no LOD, check chunk render distance vs LOD2 ring "
            "(default chunks 0-8, LOD2 8-96).");
    } else {
        (void)frustumCulled; // diagnostic only; visible in first-draw log
    }

    // ---- 2. Write origins SSBO -----------------------------------------
    // One vec4 per draw at offset (sceneIdx * 16 bytes). Sparse writes
    // are fine — cluster.vert only reads the slots referenced by
    // firstInstance values used in our vkCmdDrawIndexed calls.
    auto* origins = static_cast<float*>(gpu_->clusterOriginsBuffer.mapped);
    for (const auto& d : draws) {
        const std::size_t base = static_cast<std::size_t>(d.sceneIdx) * 4U;
        origins[base + 0] = d.originX;
        origins[base + 1] = d.originY;
        origins[base + 2] = d.originZ;
        // .w carries the per-instance LOD scale read by cluster.vert.
        // LOD2 = 1.0 (pass-through), LOD3 = 4.0 (4× linear extent).
        origins[base + 3] = d.lodScale;
    }

    // ---- 3. Bind pipeline + descriptor sets + vertex/index buffers -----
    // Cluster pipeline first (opaque + cutout share this one). Transparent
    // uses gpu_->pipelineTransparent which differs only in depthWriteEnable.
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu_->pipeline);

    // Bind sets: 0 = cluster origins, 1 = renderer's material set.
    // The cluster pipeline's layout uses shape-compatible descriptor set
    // layouts, so binding the renderer's actual material descriptor set
    // here is valid.
    const std::array<VkDescriptorSet, 2> sets{
        gpu_->clusterDescriptorSet,
        renderer_.materialDescriptorSet(),
    };
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gpu_->pipelineLayout, 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(),
                            0, nullptr);

    // Push constants — same payload as the chunk pass.
    ClusterPushConstants pc{};
    std::memcpy(pc.viewProjection, viewProjection, sizeof(pc.viewProjection));
    std::memcpy(pc.lightParams, lightParams, sizeof(pc.lightParams));
    std::memcpy(pc.cameraParams, cameraParams, sizeof(pc.cameraParams));
    std::memcpy(pc.cameraWorldParams, cameraWorldParams, sizeof(pc.cameraWorldParams));
    std::memcpy(pc.atmosphereParams, atmosphereParams, sizeof(pc.atmosphereParams));
    vkCmdPushConstants(commandBuffer, gpu_->pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);

    // Bind vertex + index buffers (the whole cluster arenas — per-draw
    // offsets pick the slice).
    const VkBuffer vertexBuffers[] = {gpu_->vertexArena.buffer()};
    const VkDeviceSize vertexOffsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, vertexOffsets);
    vkCmdBindIndexBuffer(commandBuffer, gpu_->indexArena.buffer(), 0, VK_INDEX_TYPE_UINT32);

    // ---- 4. Build indirect-draw command arrays --------------------------
    //
    // INDIRECT-DRAW CONSOLIDATION (Phase 1D-2c): replaces what used to be
    // a per-cluster `vkCmdDrawIndexed` loop (one call per surface per
    // cluster, up to ~500 draw calls per frame). Now we transcribe each
    // draw into a `VkDrawIndexedIndirectCommand`, write the array into
    // the host-visible `indirectCommandBuffer`, and issue exactly TWO
    // `vkCmdDrawIndexedIndirect` calls: one for opaque+cutout (shared
    // pipeline) and one for transparent.
    //
    // Memory layout in the indirect buffer:
    //   [0 .. opaqueCount)            — opaque + cutout commands
    //   [opaqueCount .. totalCount)   — transparent commands
    //
    // Both runs are stored contiguously so a single host write fills
    // the buffer in one memcpy if we collect them carefully. We use
    // two staging vectors and copy each block separately to keep code
    // simple; the cost is one extra memcpy of <100 KB.
    auto* indirectBase = static_cast<VkDrawIndexedIndirectCommand*>(
        gpu_->indirectCommandBuffer.mapped);
    std::uint32_t opaqueCount = 0;
    std::uint32_t transparentCount = 0;

    // Opaque + cutout pass: pack at the START of the buffer.
    for (const auto& d : draws) {
        if (d.opaqueIndexCount > 0) {
            auto& cmd = indirectBase[opaqueCount++];
            cmd.indexCount    = d.opaqueIndexCount;
            cmd.instanceCount = 1;
            cmd.firstIndex    = d.opaqueFirstIndex;
            cmd.vertexOffset  = d.opaqueVertexOffset;
            cmd.firstInstance = d.sceneIdx;  // → gl_InstanceIndex in cluster.vert
        }
        if (d.cutoutIndexCount > 0) {
            auto& cmd = indirectBase[opaqueCount++];
            cmd.indexCount    = d.cutoutIndexCount;
            cmd.instanceCount = 1;
            cmd.firstIndex    = d.cutoutFirstIndex;
            cmd.vertexOffset  = d.cutoutVertexOffset;
            cmd.firstInstance = d.sceneIdx;
        }
    }
    // Transparent pass: pack AFTER the opaque/cutout block.
    const std::uint32_t transparentBaseSlot = opaqueCount;
    for (const auto& d : draws) {
        if (d.transparentIndexCount > 0) {
            auto& cmd = indirectBase[transparentBaseSlot + transparentCount++];
            cmd.indexCount    = d.transparentIndexCount;
            cmd.instanceCount = 1;
            cmd.firstIndex    = d.transparentFirstIndex;
            cmd.vertexOffset  = d.transparentVertexOffset;
            cmd.firstInstance = d.sceneIdx;
        }
    }

    // ---- 5. Issue indirect draws ---------------------------------------
    // Two calls total: one for opaque+cutout (depth-write pipeline,
    // already bound), one for transparent (depth-readonly pipeline).
    if (opaqueCount > 0) {
        vkCmdDrawIndexedIndirect(commandBuffer,
            gpu_->indirectCommandBuffer.buffer,
            /*offset=*/0,
            opaqueCount,
            sizeof(VkDrawIndexedIndirectCommand));
    }
    if (transparentCount > 0) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          gpu_->pipelineTransparent);
        vkCmdDrawIndexedIndirect(commandBuffer,
            gpu_->indirectCommandBuffer.buffer,
            /*offset=*/static_cast<VkDeviceSize>(transparentBaseSlot)
                       * sizeof(VkDrawIndexedIndirectCommand),
            transparentCount,
            sizeof(VkDrawIndexedIndirectCommand));
    }
}

} // namespace voxel::render
