cmake_minimum_required(VERSION 3.24)

project(VoxelEngineFinale
    VERSION 0.1.0
    DESCRIPTION "Vulkan-first 32^3 chunk voxel engine skeleton"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VOXEL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(VOXEL_ENABLE_ZSTD "Compress chunk save files with Zstd. Requires network access to fetch facebook/zstd." OFF)

find_package(Vulkan REQUIRED)

include(FetchContent)

if(VOXEL_ENABLE_ZSTD)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(zstd)
endif()
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.3.0
)
FetchContent_MakeAvailable(vma)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.13.2
)
FetchContent_MakeAvailable(entt)

# Dear ImGui — used for the in-game debug overlay (per-pipeline timings,
# frame-time graph, "copy diagnostics to clipboard" button). We pull the
# docking-branch tag since the debug overlay benefits from dockable windows
# and the inline plot widgets it adds.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
)
FetchContent_MakeAvailable(imgui)
# ImGui has no CMakeLists; build it as a static library with the core +
# Vulkan/GLFW backends we need.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan glfw)
# Silence the noisy MSVC level-4 warnings in third-party code so we don't
# fail under VOXEL_WARNINGS_AS_ERRORS.
if(MSVC)
    target_compile_options(imgui PRIVATE /W3 /wd4267 /wd4244)
endif()

set(VOXEL_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${VOXEL_SHADER_OUTPUT_DIR})
set(VOXEL_SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/voxel.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/voxel_chunk.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/cluster.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/voxel.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/sky.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/sky.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/debug_line.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/voxel_cull.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/fluid_sim.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/mesh_face_classify.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/cluster_mesh_classify.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/terrain_generate.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/ui.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/ui.frag
)
set(VOXEL_SHADER_OUTPUTS)
foreach(shader ${VOXEL_SHADER_SOURCES})
    get_filename_component(shader_name ${shader} NAME)
    set(output ${VOXEL_SHADER_OUTPUT_DIR}/${shader_name}.spv)
    add_custom_command(
        OUTPUT ${output}
        COMMAND Vulkan::glslc ${shader} -o ${output}
        DEPENDS ${shader}
        VERBATIM
    )
    list(APPEND VOXEL_SHADER_OUTPUTS ${output})
endforeach()
add_custom_target(voxel_shaders DEPENDS ${VOXEL_SHADER_OUTPUTS})

add_library(voxel_engine STATIC
    src/voxel/automation/KineticNetwork.cpp
    src/voxel/automation/NetworkGraph.cpp
    src/voxel/core/Application.cpp
    src/voxel/core/Math.cpp
    src/voxel/core/Paths.cpp
    src/voxel/core/JobSystem.cpp
    src/voxel/core/Logger.cpp
    src/voxel/data/BlockRegistry.cpp
    src/voxel/data/CoreContentIds.cpp
    src/voxel/data/ItemRegistry.cpp
    src/voxel/data/RecipeRegistry.cpp
    src/voxel/data/RegistryLoader.cpp
    src/voxel/ecs/SimulationSystem.cpp
    src/voxel/inventory/Inventory.cpp
    src/voxel/inventory/InventorySerialization.cpp
    src/voxel/magic/MagicSystem.cpp
    src/voxel/network/NetworkSystem.cpp
    src/voxel/physics/PhysicsSystem.cpp
    src/voxel/player/CreativeHotbar.cpp
    src/voxel/player/PlayerController.cpp
    src/voxel/player/PlayerSpawnResolver.cpp
    src/voxel/platform/GlfwWindow.cpp
    src/voxel/render/BufferArena.cpp
    src/voxel/render/ClusterRenderer.cpp
    src/voxel/render/MaterialTable.cpp
    src/voxel/render/RenderGraph.cpp
    src/voxel/render/DebugCamera.cpp
    src/voxel/render/DebugOverlay.cpp
    src/voxel/render/VmaAllocator.cpp
    src/voxel/render/VulkanRenderer.cpp
    src/voxel/render/meshing/BlockRenderCatalog.cpp
    src/voxel/render/meshing/ChunkMeshCache.cpp
    src/voxel/render/meshing/ClusterGpuMeshing.cpp
    src/voxel/render/meshing/ClusterMesher.cpp
    src/voxel/render/meshing/ClusterMeshDiskCache.cpp
    src/voxel/render/meshing/RegionMesher.cpp
    src/voxel/render/meshing/ClipmapRegionMesher.cpp
    src/voxel/render/meshing/GreedyMesher.cpp
    src/voxel/render/meshing/HybridMeshingGpuSystem.cpp
    src/voxel/save/PlayerInventorySaveService.cpp
    src/voxel/save/PlayerStateSaveService.cpp
    src/voxel/save/RegionFileStore.cpp
    src/voxel/save/SaveCoordinator.cpp
    src/voxel/save/WorldRegistry.cpp
    src/voxel/save/WorldSaveService.cpp
    src/voxel/world/BitPackedArray.cpp
    src/voxel/world/BlockLightCatalog.cpp
    src/voxel/world/Chunk.cpp
    src/voxel/world/BlockEditor.cpp
    src/voxel/world/BlockEditQueue.cpp
    src/voxel/world/BlockEntityBehavior.cpp
    src/voxel/world/ChunkLightData.cpp
    src/voxel/world/ChunkFluidData.cpp
    src/voxel/world/FluidDirtyQueue.cpp
    src/voxel/world/FluidSystem.cpp
    src/voxel/world/FluidGpuSystem.cpp
    src/voxel/world/FlatTerrainGenerator.cpp
    src/voxel/world/LightPropagator.cpp
    src/voxel/world/NoiseTerrainGenerator.cpp
    src/voxel/world/NoiseTerrainWorldShape.cpp
    src/voxel/world/SpaceEnvironment.cpp
    src/voxel/world/TerrainColumnPrepass.cpp
    src/voxel/world/TerrainDefinitionRegistry.cpp
    src/voxel/world/TerrainPipeline.cpp
    src/voxel/world/TerrainPipelineFoliage.cpp
    src/voxel/world/TerrainPipelineStages.cpp
    src/voxel/world/TerrainPipelineStructures.cpp
    src/voxel/world/ChunkJobMailbox.cpp
    src/voxel/world/ChunkDirtyQueue.cpp
    src/voxel/world/ChunkManager.cpp
    src/voxel/world/ChunkPipeline.cpp
    src/voxel/world/Raycast.cpp
    src/voxel/world/ChunkStreamer.cpp
    src/voxel/world/CoreBehaviors.cpp
    src/voxel/world/WorldDelta.cpp
)

target_include_directories(voxel_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${vma_SOURCE_DIR}/include
)

target_compile_definitions(voxel_engine
    PUBLIC
        VOXEL_CHUNK_SIZE=32
    PRIVATE
        VOXEL_SHADER_DIR="${VOXEL_SHADER_OUTPUT_DIR}"
)

add_dependencies(voxel_engine voxel_shaders)

target_link_libraries(voxel_engine
    PUBLIC
        Vulkan::Vulkan
        EnTT::EnTT
    PRIVATE
        glfw
        nlohmann_json::nlohmann_json
        imgui
)

if(VOXEL_ENABLE_ZSTD)
    target_link_libraries(voxel_engine PRIVATE libzstd_static)
    target_compile_definitions(voxel_engine PRIVATE VOXEL_HAS_ZSTD=1)
endif()

if(MSVC)
    target_compile_options(voxel_engine PRIVATE /W4 /permissive-)
    if(VOXEL_WARNINGS_AS_ERRORS)
        target_compile_options(voxel_engine PRIVATE /WX)
    endif()
else()
    target_compile_options(voxel_engine PRIVATE -Wall -Wextra -Wpedantic)
    if(VOXEL_WARNINGS_AS_ERRORS)
        target_compile_options(voxel_engine PRIVATE -Werror)
    endif()
endif()

add_executable(voxel_sandbox
    apps/sandbox/main.cpp
)

target_link_libraries(voxel_sandbox
    PRIVATE
        voxel_engine
)

add_executable(voxel_smoke_tests
    tests/smoke/main.cpp
)

target_link_libraries(voxel_smoke_tests
    PRIVATE
        voxel_engine
)

if(MSVC)
    # The smoke test intentionally exercises many world/chunk systems in one
    # executable. MSVC Debug keeps large stack frames alive conservatively, and
    # Chunk-heavy regression cases can exceed the default 1 MB test stack.
    target_link_options(voxel_smoke_tests PRIVATE /STACK:8388608)
endif()

enable_testing()
add_test(NAME voxel_smoke_tests COMMAND voxel_smoke_tests)
set_tests_properties(voxel_smoke_tests
    PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
