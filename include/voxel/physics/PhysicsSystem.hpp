#pragma once

#include <voxel/core/Types.hpp>

namespace voxel::physics {

class PhysicsSystem {
public:
    void initialize();
    void tick(Tick tick);

    // TODO(physics): Integrate Jolt or equivalent rigid-body backend behind this boundary.
    // TODO(physics): Add voxel collision generation, simulation islands, ships, and contraption colliders.
};

} // namespace voxel::physics

