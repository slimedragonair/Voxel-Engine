#pragma once

#include <cstdint>
#include <optional>

#include <voxel/core/Math.hpp>
#include <voxel/world/BlockState.hpp>
#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::world {

// W0: which block kinds count as "hit" for a raycast.
//
//   SolidOnly       — skips water, leaves (cutout), and other non-solid
//                     terrain. Use for block placement / breaking so the
//                     player can click *through* shallow water onto the
//                     stone below.
//   FluidsOnly      — only fluids count. For tools like a bucket where
//                     you want to interact with the water itself.
//   SolidAndFluid   — anything non-air is a hit. The pre-W0 behavior;
//                     useful for general "what am I looking at" queries.
//   AnyNonAir       — alias for SolidAndFluid, kept for clarity.
enum class RaycastMask : std::uint8_t {
    SolidOnly = 0,
    FluidsOnly,
    SolidAndFluid,
    AnyNonAir = SolidAndFluid,
};

struct VoxelRay {
    core::Vec3 origin{};
    core::Vec3 direction{};
    float maxDistance{8.0F};
    std::uint64_t planetId{};
    RaycastMask mask{RaycastMask::SolidOnly};
};

struct VoxelRaycastHit {
    PlanetCoord position{};
    BlockCoord normal{};
    BlockStateId block{};
    float distance{};
};

class VoxelRaycaster {
public:
    void setFluidBlockType(BlockTypeId type) noexcept { fluidBlockType_ = type; }
    [[nodiscard]] BlockTypeId fluidBlockType() const noexcept { return fluidBlockType_; }

    [[nodiscard]] std::optional<VoxelRaycastHit> cast(const ChunkManager& chunks, const VoxelRay& ray) const;

private:
    BlockTypeId fluidBlockType_{12};
};

// Categorizes a block for raycast masking. Pure function, safe to call
// hot. Future fluid kinds (lava, slime, magic water) extend this without
// touching the raycaster itself.
[[nodiscard]] bool blockMatchesRaycastMask(BlockStateId block, RaycastMask mask) noexcept;
[[nodiscard]] bool blockMatchesRaycastMask(
    BlockStateId block,
    RaycastMask mask,
    BlockTypeId fluidBlockType) noexcept;

} // namespace voxel::world
