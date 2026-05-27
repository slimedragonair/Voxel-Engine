#include <voxel/render/DebugCamera.hpp>

#include <algorithm>
#include <cmath>

namespace voxel::render {

void DebugCamera::setAspect(float aspect) noexcept
{
    if (aspect > 0.0F) {
        aspect_ = aspect;
    }
}

void DebugCamera::setFarPlane(float farPlane) noexcept
{
    farPlane_ = std::clamp(farPlane, nearPlane_ + 1.0F, 5.0e8F);
}

void DebugCamera::setPose(core::Vec3 position, float yawRadians, float pitchRadians) noexcept
{
    position_ = {static_cast<double>(position.x), static_cast<double>(position.y), static_cast<double>(position.z)};
    yawRadians_ = yawRadians;
    pitchRadians_ = std::clamp(pitchRadians, -1.45F, 1.45F);
    hasLastCursor_ = false;
}

void DebugCamera::setPose(core::DVec3 position, float yawRadians, float pitchRadians) noexcept
{
    position_ = position;
    yawRadians_ = yawRadians;
    pitchRadians_ = std::clamp(pitchRadians, -1.45F, 1.45F);
    hasLastCursor_ = false;
}

void DebugCamera::updateFromInput(const platform::IWindow& window, float deltaSeconds) noexcept
{
    const float speed = moveSpeed_ * (window.keyDown(platform::Key::LeftShift) ? 3.0F : 1.0F);
    const float movement = speed * deltaSeconds;
    const float rotation = turnSpeed_ * deltaSeconds;
    if (window.cursorCaptured()) {
        const auto cursor = window.cursorPosition();
        if (hasLastCursor_) {
            const auto dx = std::clamp(cursor.x - lastCursorX_, -250.0, 250.0);
            const auto dy = std::clamp(cursor.y - lastCursorY_, -250.0, 250.0);
            yawRadians_ += static_cast<float>(-dx) * mouseSensitivity_;
            pitchRadians_ -= static_cast<float>(dy) * mouseSensitivity_;
        }
        lastCursorX_ = cursor.x;
        lastCursorY_ = cursor.y;
        hasLastCursor_ = true;
    } else {
        hasLastCursor_ = false;
    }

    if (window.keyDown(platform::Key::Left)) {
        yawRadians_ -= rotation;
    }
    if (window.keyDown(platform::Key::Right)) {
        yawRadians_ += rotation;
    }
    if (window.keyDown(platform::Key::Up)) {
        pitchRadians_ += rotation;
    }
    if (window.keyDown(platform::Key::Down)) {
        pitchRadians_ -= rotation;
    }

    pitchRadians_ = std::clamp(pitchRadians_, -1.45F, 1.45F);

    const auto forwardVector = forward();
    const auto rightVector = right();

    if (window.keyDown(platform::Key::W)) {
        position_.x += static_cast<double>(forwardVector.x * movement);
        position_.y += static_cast<double>(forwardVector.y * movement);
        position_.z += static_cast<double>(forwardVector.z * movement);
    }
    if (window.keyDown(platform::Key::S)) {
        position_.x -= static_cast<double>(forwardVector.x * movement);
        position_.y -= static_cast<double>(forwardVector.y * movement);
        position_.z -= static_cast<double>(forwardVector.z * movement);
    }
    if (window.keyDown(platform::Key::D)) {
        position_.x += static_cast<double>(rightVector.x * movement);
        position_.y += static_cast<double>(rightVector.y * movement);
        position_.z += static_cast<double>(rightVector.z * movement);
    }
    if (window.keyDown(platform::Key::A)) {
        position_.x -= static_cast<double>(rightVector.x * movement);
        position_.y -= static_cast<double>(rightVector.y * movement);
        position_.z -= static_cast<double>(rightVector.z * movement);
    }
    if (window.keyDown(platform::Key::E)) {
        position_.y += static_cast<double>(movement);
    }
    if (window.keyDown(platform::Key::Q)) {
        position_.y -= static_cast<double>(movement);
    }
}

core::Mat4 DebugCamera::viewProjection() const noexcept
{
    const auto pos = position();
    const auto target = pos + forward();
    return core::multiply(
        core::perspectiveVulkanReversedZ(fovYRadians_, aspect_, nearPlane_, farPlane_),
        core::lookAt(pos, target, up_));
}

core::Mat4 DebugCamera::cameraRelativeViewProjection() const noexcept
{
    const core::Vec3 origin{0.0F, 0.0F, 0.0F};
    const auto target = forward();
    return core::multiply(
        core::perspectiveVulkanReversedZ(fovYRadians_, aspect_, nearPlane_, farPlane_),
        core::lookAt(origin, target, up_));
}

core::Vec3 DebugCamera::position() const noexcept
{
    return {static_cast<float>(position_.x), static_cast<float>(position_.y), static_cast<float>(position_.z)};
}

core::DVec3 DebugCamera::dPosition() const noexcept
{
    return position_;
}

core::Vec3 DebugCamera::forwardVector() const noexcept
{
    return forward();
}

core::Vec3 DebugCamera::forward() const noexcept
{
    return core::normalize(core::Vec3{
        std::cos(pitchRadians_) * std::sin(yawRadians_),
        std::sin(pitchRadians_),
        std::cos(pitchRadians_) * std::cos(yawRadians_)
    });
}

core::Vec3 DebugCamera::right() const noexcept
{
    return core::normalize(core::cross(forward(), up_));
}

} // namespace voxel::render
