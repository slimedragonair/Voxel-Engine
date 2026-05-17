#include <voxel/player/PlayerController.hpp>

#include <algorithm>
#include <cmath>

#include <voxel/world/CoordinateUtils.hpp>

namespace voxel::player {

namespace {

constexpr float kEpsilon = 0.001F;
constexpr std::uint32_t kWaterBlockId = 12;
constexpr int kCollisionSearchIterations = 10;

} // namespace

PlayerController::PlayerController(PlayerControllerConfig config)
    : config_(config)
{
}

void PlayerController::setPosition(core::Vec3 position) noexcept
{
    position_ = position;
    velocity_ = {};
    grounded_ = false;
}

void PlayerController::setLook(float yawRadians, float pitchRadians) noexcept
{
    yawRadians_ = yawRadians;
    pitchRadians_ = std::clamp(pitchRadians, -1.45F, 1.45F);
}

void PlayerController::setWalkSpeed(float speed) noexcept
{
    config_.walkSpeed = std::max(0.0F, speed);
}

void PlayerController::setFlySpeed(float speed) noexcept
{
    config_.flySpeed = std::max(0.0F, speed);
}

void PlayerController::setNoclip(bool enabled) noexcept
{
    noclip_ = enabled;
    if (noclip_) {
        velocity_ = {};
        grounded_ = false;
    }
}

void PlayerController::toggleNoclip() noexcept
{
    setNoclip(!noclip_);
}

void PlayerController::tick(const world::ChunkManager& chunks, const PlayerInput& input, float deltaSeconds)
{
    yawRadians_ += input.yawDelta;
    pitchRadians_ = std::clamp(pitchRadians_ + input.pitchDelta, -1.45F, 1.45F);

    core::Vec3 wish{};
    if (input.forward) {
        wish += forwardFlat();
    }
    if (input.backward) {
        wish += forwardFlat() * -1.0F;
    }
    if (input.right) {
        wish += rightFlat();
    }
    if (input.left) {
        wish += rightFlat() * -1.0F;
    }

    wish = core::normalize(wish);
    const float speed = (noclip_ ? config_.flySpeed : config_.walkSpeed) * (input.fast ? 2.0F : 1.0F);

    if (noclip_) {
        core::Vec3 fly = wish;
        if (input.flyUp || input.jump) {
            fly.y += 1.0F;
        }
        if (input.flyDown) {
            fly.y -= 1.0F;
        }
        fly = core::normalize(fly);
        position_ += fly * (speed * deltaSeconds);
        velocity_ = {};
        grounded_ = false;
        return;
    }

    velocity_.x = wish.x * speed;
    velocity_.z = wish.z * speed;
    if (input.jump && grounded_) {
        velocity_.y = config_.jumpVelocity;
        grounded_ = false;
    }
    velocity_.y -= config_.gravity * deltaSeconds;

    grounded_ = false;
    moveAxis(chunks, position_.x, velocity_.x * deltaSeconds, 0);
    moveAxis(chunks, position_.z, velocity_.z * deltaSeconds, 2);
    moveAxis(chunks, position_.y, velocity_.y * deltaSeconds, 1);
}

core::Vec3 PlayerController::position() const noexcept
{
    return position_;
}

core::Vec3 PlayerController::velocity() const noexcept
{
    return velocity_;
}

core::Vec3 PlayerController::eyePosition() const noexcept
{
    return position_ + core::Vec3{0.0F, config_.eyeHeight, 0.0F};
}

core::Vec3 PlayerController::forwardVector() const noexcept
{
    return core::normalize({
        std::cos(pitchRadians_) * std::sin(yawRadians_),
        std::sin(pitchRadians_),
        std::cos(pitchRadians_) * std::cos(yawRadians_)
    });
}

float PlayerController::yawRadians() const noexcept
{
    return yawRadians_;
}

float PlayerController::pitchRadians() const noexcept
{
    return pitchRadians_;
}

bool PlayerController::grounded() const noexcept
{
    return grounded_;
}

bool PlayerController::noclip() const noexcept
{
    return noclip_;
}

PlayerAabb PlayerController::aabbAt(core::Vec3 feetPosition) const noexcept
{
    return {
        {feetPosition.x - config_.radius, feetPosition.y, feetPosition.z - config_.radius},
        {feetPosition.x + config_.radius, feetPosition.y + config_.height, feetPosition.z + config_.radius}
    };
}

bool PlayerController::overlapsBlock(world::ChunkCoord chunk, world::BlockCoord block) const noexcept
{
    const auto world = world::toWorldBlock(chunk, block);
    const auto box = aabbAt(position_);
    return box.max.x > static_cast<float>(world.x)
        && box.min.x < static_cast<float>(world.x + 1)
        && box.max.y > static_cast<float>(world.y)
        && box.min.y < static_cast<float>(world.y + 1)
        && box.max.z > static_cast<float>(world.z)
        && box.min.z < static_cast<float>(world.z + 1);
}

bool PlayerController::blockIsSolid(BlockStateId block) noexcept
{
    return block.value != world::AirBlockState.value && block.value != kWaterBlockId;
}

core::Vec3 PlayerController::forwardFlat() const noexcept
{
    return core::normalize({std::sin(yawRadians_), 0.0F, std::cos(yawRadians_)});
}

core::Vec3 PlayerController::rightFlat() const noexcept
{
    return core::normalize(core::cross(forwardFlat(), {0.0F, 1.0F, 0.0F}));
}

bool PlayerController::collidesAt(const world::ChunkManager& chunks, core::Vec3 feetPosition) const
{
    const auto box = aabbAt(feetPosition);
    const int minX = static_cast<int>(std::floor(box.min.x));
    const int maxX = static_cast<int>(std::floor(box.max.x - kEpsilon));
    const int minY = static_cast<int>(std::floor(box.min.y));
    const int maxY = static_cast<int>(std::floor(box.max.y - kEpsilon));
    const int minZ = static_cast<int>(std::floor(box.min.z));
    const int maxZ = static_cast<int>(std::floor(box.max.z - kEpsilon));

    for (int z = minZ; z <= maxZ; ++z) {
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const auto local = world::toChunkLocal(x, y, z);
                const auto* chunk = chunks.find(local.chunk);
                if (chunk == nullptr) {
                    continue;
                }
                if (blockIsSolid(chunk->blockAt(local.local.x, local.local.y, local.local.z))) {
                    return true;
                }
            }
        }
    }
    return false;
}

void PlayerController::moveAxis(const world::ChunkManager& chunks, float& component, float delta, int axis)
{
    if (delta == 0.0F) {
        return;
    }

    const float original = component;
    component += delta;
    if (!collidesAt(chunks, position_)) {
        return;
    }

    float lo = 0.0F;
    float hi = 1.0F;
    float best = 0.0F;
    for (int i = 0; i < kCollisionSearchIterations; ++i) {
        const float t = (lo + hi) * 0.5F;
        component = original + (delta * t);
        if (collidesAt(chunks, position_)) {
            hi = t;
        } else {
            best = t;
            lo = t;
        }
    }
    component = original + (delta * best);

    if (axis == 1) {
        if (velocity_.y < 0.0F) {
            grounded_ = true;
        }
        velocity_.y = 0.0F;
    } else if (axis == 0) {
        velocity_.x = 0.0F;
    } else {
        velocity_.z = 0.0F;
    }
}

} // namespace voxel::player
