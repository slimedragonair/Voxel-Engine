#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

namespace voxel::platform {

enum class Key {
    W,
    A,
    S,
    D,
    Q,
    E,
    Left,
    Right,
    Up,
    Down,
    LeftShift,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    Digit0,
    Minus,
    Equal,
    Space,
    C,
    V,
    F,
    I,
    R,
    F3,    // debug overlay toggle
    F9,    // M2: World Manager overlay toggle
    Escape
};

enum class MouseButton {
    Left,
    Right
};

struct WindowExtent {
    std::uint32_t width{};
    std::uint32_t height{};
};

struct CursorPosition {
    double x{};
    double y{};
};

class IWindow {
public:
    virtual ~IWindow() = default;

    virtual void pollEvents() = 0;
    virtual void setTitle(std::string_view title) = 0;
    virtual void setCursorCaptured(bool captured) = 0;
    [[nodiscard]] virtual bool shouldClose() const = 0;
    [[nodiscard]] virtual bool keyDown(Key key) const = 0;
    [[nodiscard]] virtual bool mouseButtonDown(MouseButton button) const = 0;
    [[nodiscard]] virtual bool cursorCaptured() const = 0;
    [[nodiscard]] virtual CursorPosition cursorPosition() const = 0;
    [[nodiscard]] virtual double scrollOffset() const = 0;
    virtual void clearScrollOffset() = 0;
    [[nodiscard]] virtual WindowExtent framebufferExtent() const = 0;
    [[nodiscard]] virtual std::vector<const char*> requiredVulkanExtensions() const = 0;
    [[nodiscard]] virtual VkSurfaceKHR createVulkanSurface(VkInstance instance) const = 0;
};

} // namespace voxel::platform
