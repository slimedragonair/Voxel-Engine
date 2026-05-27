#pragma once

#include <voxel/core/Math.hpp>
#include <voxel/platform/IWindow.hpp>

namespace voxel::render {

class DebugCamera {
public:
    void setAspect(float aspect) noexcept;
    void setFarPlane(float farPlane) noexcept;
    void setPose(core::Vec3 position, float yawRadians, float pitchRadians) noexcept;
    void setPose(core::DVec3 position, float yawRadians, float pitchRadians) noexcept;
    void updateFromInput(const platform::IWindow& window, float deltaSeconds) noexcept;
    [[nodiscard]] core::Mat4 viewProjection() const noexcept;
    [[nodiscard]] core::Mat4 cameraRelativeViewProjection() const noexcept;
    [[nodiscard]] core::Vec3 position() const noexcept;
    [[nodiscard]] core::DVec3 dPosition() const noexcept;
    [[nodiscard]] core::Vec3 forwardVector() const noexcept;

private:
    [[nodiscard]] core::Vec3 forward() const noexcept;
    [[nodiscard]] core::Vec3 right() const noexcept;

    core::DVec3 position_{72.0, 52.0, 88.0};
    core::Vec3 up_{0.0F, 1.0F, 0.0F};
    float yawRadians_{-2.23F};
    float pitchRadians_{-0.55F};
    float aspect_{16.0F / 9.0F};
    float fovYRadians_{1.04719755F};
    float nearPlane_{0.1F};
    float farPlane_{1024.0F};
    float moveSpeed_{42.0F};
    float turnSpeed_{1.6F};
    float mouseSensitivity_{0.0022F};
    bool hasLastCursor_{false};
    double lastCursorX_{0.0};
    double lastCursorY_{0.0};
};

} // namespace voxel::render
