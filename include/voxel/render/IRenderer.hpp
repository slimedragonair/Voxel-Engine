#pragma once

#include <string_view>

#include <voxel/platform/IWindow.hpp>

namespace voxel::render {

struct RendererConfig {
    std::string_view appName{"Voxel Engine"};
    int width{1280};
    int height{720};
    platform::IWindow* window{};
    bool enableGpuCulling{false};
    bool compareGpuCulling{false};
};

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void initialize(const RendererConfig& config) = 0;
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void shutdown() = 0;
};

} // namespace voxel::render
