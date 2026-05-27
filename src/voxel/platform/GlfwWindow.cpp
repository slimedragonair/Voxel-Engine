#include <voxel/platform/GlfwWindow.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdexcept>

namespace voxel::platform {

namespace {

class GlfwLifetime {
public:
    GlfwLifetime()
    {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }
    }

    ~GlfwLifetime()
    {
        glfwTerminate();
    }
};

GlfwLifetime& glfwLifetime()
{
    static GlfwLifetime lifetime;
    return lifetime;
}

} // namespace

GlfwWindow::GlfwWindow(WindowConfig config)
{
    (void)glfwLifetime();
    if (glfwVulkanSupported() != GLFW_TRUE) {
        throw std::runtime_error("GLFW reports Vulkan is not supported");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetScrollCallback(window_, [](GLFWwindow* w, double /*xoffset*/, double yoffset) {
        auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(w));
        if (self != nullptr) {
            self->scrollAccumulator_ += yoffset;
        }
    });
}

GlfwWindow::~GlfwWindow()
{
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
}

void GlfwWindow::pollEvents()
{
    glfwPollEvents();
}

void GlfwWindow::setTitle(std::string_view title)
{
    glfwSetWindowTitle(window_, std::string{title}.c_str());
}

void GlfwWindow::setCursorCaptured(bool captured)
{
    if (cursorCaptured_ == captured) {
        return;
    }
    cursorCaptured_ = captured;
    glfwSetInputMode(window_, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (captured && glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
}

bool GlfwWindow::shouldClose() const
{
    return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

bool GlfwWindow::keyDown(Key key) const
{
    int glfwKey = GLFW_KEY_UNKNOWN;
    switch (key) {
    case Key::W:
        glfwKey = GLFW_KEY_W;
        break;
    case Key::A:
        glfwKey = GLFW_KEY_A;
        break;
    case Key::S:
        glfwKey = GLFW_KEY_S;
        break;
    case Key::D:
        glfwKey = GLFW_KEY_D;
        break;
    case Key::Q:
        glfwKey = GLFW_KEY_Q;
        break;
    case Key::E:
        glfwKey = GLFW_KEY_E;
        break;
    case Key::Left:
        glfwKey = GLFW_KEY_LEFT;
        break;
    case Key::Right:
        glfwKey = GLFW_KEY_RIGHT;
        break;
    case Key::Up:
        glfwKey = GLFW_KEY_UP;
        break;
    case Key::Down:
        glfwKey = GLFW_KEY_DOWN;
        break;
    case Key::LeftShift:
        glfwKey = GLFW_KEY_LEFT_SHIFT;
        break;
    case Key::Digit1:
        glfwKey = GLFW_KEY_1;
        break;
    case Key::Digit2:
        glfwKey = GLFW_KEY_2;
        break;
    case Key::Digit3:
        glfwKey = GLFW_KEY_3;
        break;
    case Key::Digit4:
        glfwKey = GLFW_KEY_4;
        break;
    case Key::Digit5:
        glfwKey = GLFW_KEY_5;
        break;
    case Key::Digit6:
        glfwKey = GLFW_KEY_6;
        break;
    case Key::Digit7:
        glfwKey = GLFW_KEY_7;
        break;
    case Key::Digit8:
        glfwKey = GLFW_KEY_8;
        break;
    case Key::Digit9:
        glfwKey = GLFW_KEY_9;
        break;
    case Key::Digit0:
        glfwKey = GLFW_KEY_0;
        break;
    case Key::Minus:
        glfwKey = GLFW_KEY_MINUS;
        break;
    case Key::Equal:
        glfwKey = GLFW_KEY_EQUAL;
        break;
    case Key::Space:
        glfwKey = GLFW_KEY_SPACE;
        break;
    case Key::C:
        glfwKey = GLFW_KEY_C;
        break;
    case Key::V:
        glfwKey = GLFW_KEY_V;
        break;
    case Key::F:
        glfwKey = GLFW_KEY_F;
        break;
    case Key::I:
        glfwKey = GLFW_KEY_I;
        break;
    case Key::R:
        glfwKey = GLFW_KEY_R;
        break;
    case Key::F3:
        glfwKey = GLFW_KEY_F3;
        break;
    case Key::Escape:
        glfwKey = GLFW_KEY_ESCAPE;
        break;
    }

    return glfwGetKey(window_, glfwKey) == GLFW_PRESS;
}

bool GlfwWindow::mouseButtonDown(MouseButton button) const
{
    int glfwButton = GLFW_MOUSE_BUTTON_LEFT;
    switch (button) {
    case MouseButton::Left:
        glfwButton = GLFW_MOUSE_BUTTON_LEFT;
        break;
    case MouseButton::Right:
        glfwButton = GLFW_MOUSE_BUTTON_RIGHT;
        break;
    }

    return glfwGetMouseButton(window_, glfwButton) == GLFW_PRESS;
}

bool GlfwWindow::cursorCaptured() const
{
    return cursorCaptured_;
}

CursorPosition GlfwWindow::cursorPosition() const
{
    CursorPosition position{};
    glfwGetCursorPos(window_, &position.x, &position.y);
    return position;
}

WindowExtent GlfwWindow::framebufferExtent() const
{
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

std::vector<const char*> GlfwWindow::requiredVulkanExtensions() const
{
    std::uint32_t count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    if (extensions == nullptr || count == 0) {
        throw std::runtime_error("GLFW did not provide Vulkan instance extensions");
    }
    return {extensions, extensions + count};
}

VkSurfaceKHR GlfwWindow::createVulkanSurface(VkInstance instance) const
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create GLFW Vulkan surface");
    }
    return surface;
}

double GlfwWindow::scrollOffset() const
{
    return scrollAccumulator_;
}

void GlfwWindow::clearScrollOffset()
{
    scrollAccumulator_ = 0.0;
}

} // namespace voxel::platform
