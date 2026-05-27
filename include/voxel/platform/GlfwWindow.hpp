#pragma once

#include <string>

#include <voxel/platform/IWindow.hpp>

struct GLFWwindow;

namespace voxel::platform {

struct WindowConfig {
    std::string title{"Voxel Engine Finale"};
    int width{1280};
    int height{720};
};

class GlfwWindow final : public IWindow {
public:
    explicit GlfwWindow(WindowConfig config);
    ~GlfwWindow() override;

    GlfwWindow(const GlfwWindow&) = delete;
    GlfwWindow& operator=(const GlfwWindow&) = delete;

    void pollEvents() override;
    void setTitle(std::string_view title) override;
    void setCursorCaptured(bool captured) override;
    [[nodiscard]] bool shouldClose() const override;
    [[nodiscard]] bool keyDown(Key key) const override;
    [[nodiscard]] bool mouseButtonDown(MouseButton button) const override;
    [[nodiscard]] bool cursorCaptured() const override;
    [[nodiscard]] CursorPosition cursorPosition() const override;
    [[nodiscard]] double scrollOffset() const override;
    void clearScrollOffset() override;
    [[nodiscard]] WindowExtent framebufferExtent() const override;
    [[nodiscard]] std::vector<const char*> requiredVulkanExtensions() const override;
    [[nodiscard]] VkSurfaceKHR createVulkanSurface(VkInstance instance) const override;

    // Exposes the underlying GLFW handle for code that integrates with
    // third-party GLFW-aware libraries (e.g. Dear ImGui's GLFW backend).
    // Callers must NOT free or destroy the returned pointer.
    [[nodiscard]] GLFWwindow* nativeHandle() const noexcept { return window_; }

private:
    GLFWwindow* window_{nullptr};
    bool cursorCaptured_{false};
    double scrollAccumulator_{0.0};
};

} // namespace voxel::platform
