#pragma once

#include <voxel/core/Math.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::player {

struct PlayerAabb {
    core::Vec3 min{};
    core::Vec3 max{};
};

struct PlayerControllerConfig {
    float height{1.8F};
    float radius{0.3F};
    float eyeHeight{1.62F};
    float walkSpeed{4.3F};
    float flySpeed{10.0F};
    float jumpVelocity{8.0F};
    float gravity{24.0F};
};

struct PlayerInput {
    bool forward{};
    bool backward{};
    bool left{};
    bool right{};
    bool jump{};
    bool flyUp{};
    bool flyDown{};
    bool fast{};
    float yawDelta{};
    float pitchDelta{};
};

class PlayerController {
public:
    explicit PlayerController(PlayerControllerConfig config = {});

    void setPosition(core::Vec3 position) noexcept;
    void setPosition(core::DVec3 position) noexcept;
    void setLook(float yawRadians, float pitchRadians) noexcept;
    void setWalkSpeed(float speed) noexcept;
    void setFlySpeed(float speed) noexcept;
    void setGravityScale(float scale) noexcept;
    void setNoclip(bool enabled) noexcept;
    void toggleNoclip() noexcept;

    void tick(const world::ChunkManager& chunks, const PlayerInput& input, float deltaSeconds);

    [[nodiscard]] core::Vec3 position() const noexcept;
    [[nodiscard]] core::DVec3 dPosition() const noexcept;
    [[nodiscard]] core::Vec3 velocity() const noexcept;
    [[nodiscard]] core::Vec3 eyePosition() const noexcept;
    [[nodiscard]] core::DVec3 dEyePosition() const noexcept;
    [[nodiscard]] core::Vec3 forwardVector() const noexcept;
    [[nodiscard]] float yawRadians() const noexcept;
    [[nodiscard]] float pitchRadians() const noexcept;
    [[nodiscard]] bool grounded() const noexcept;
    [[nodiscard]] bool noclip() const noexcept;
    [[nodiscard]] PlayerAabb aabbAt(core::Vec3 feetPosition) const noexcept;
    [[nodiscard]] bool overlapsBlock(world::ChunkCoord chunk, world::BlockCoord block) const noexcept;

    [[nodiscard]] static bool blockIsSolid(BlockStateId block) noexcept;

private:
    [[nodiscard]] core::Vec3 forwardFlat() const noexcept;
    [[nodiscard]] core::Vec3 rightFlat() const noexcept;
    [[nodiscard]] bool collidesAt(const world::ChunkManager& chunks, core::Vec3 feetPosition) const;
    void moveAxis(const world::ChunkManager& chunks, double& component, float delta, int axis);

    PlayerControllerConfig config_{};
    core::DVec3 position_{72.0, 64.0, 88.0};
    core::DVec3 velocity_{};
    float gravityScale_{1.0F};
    float yawRadians_{-2.23F};
    float pitchRadians_{-0.35F};
    bool grounded_{false};
    bool noclip_{false};
};

} // namespace voxel::player
