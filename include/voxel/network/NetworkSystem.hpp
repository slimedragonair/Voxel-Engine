#pragma once

#include <voxel/core/Types.hpp>

namespace voxel::network {

class NetworkSystem {
public:
    void initialize();
    void tick(Tick tick);

    // TODO(network): Add authoritative server model, chunk delta replication, entity interpolation, and prediction hooks.
};

} // namespace voxel::network

