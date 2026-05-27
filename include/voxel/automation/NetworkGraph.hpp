#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <voxel/world/Coordinates.hpp>

namespace voxel::automation {

enum class NetworkKind : std::uint8_t {
    Kinetic,
    Fluid,
    Thermal,
    Electrical,
    Mana
};

struct NetworkNode {
    std::uint64_t id{};
    world::BlockCoord position{};
    NetworkKind kind{NetworkKind::Kinetic};
    std::string registryId;
};

struct NetworkEdge {
    std::uint64_t from{};
    std::uint64_t to{};
};

class NetworkGraph {
public:
    void addNode(NetworkNode node);
    void addEdge(NetworkEdge edge);
    void markDirty();
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] const std::vector<NetworkNode>& nodes() const noexcept;

    // TODO(automation): Add typed solvers for RPM/stress, pressure, heat, electricity, and mana.
    // TODO(automation): Rebuild only dirty connected components after block changes.

private:
    std::vector<NetworkNode> nodes_;
    std::vector<NetworkEdge> edges_;
    bool dirty_{false};
};

} // namespace voxel::automation

