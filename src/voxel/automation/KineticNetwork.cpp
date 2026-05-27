#include <voxel/automation/KineticNetwork.hpp>

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <voxel/world/BlockState.hpp>
#include <voxel/world/CoordinateUtils.hpp>

namespace voxel::automation {

namespace {

struct WorldBlockKey {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};

    [[nodiscard]] friend bool operator==(WorldBlockKey lhs, WorldBlockKey rhs) noexcept
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }
};

struct WorldBlockKeyHash {
    [[nodiscard]] std::size_t operator()(WorldBlockKey key) const noexcept
    {
        const auto x = static_cast<std::uint64_t>(key.x);
        const auto y = static_cast<std::uint64_t>(key.y);
        const auto z = static_cast<std::uint64_t>(key.z);
        return static_cast<std::size_t>((x * 73856093ULL) ^ (y * 19349663ULL) ^ (z * 83492791ULL));
    }
};

struct IndexedNode {
    WorldBlockKey key{};
    world::PlanetCoord position{};
    BlockStateId block{};
    KineticBlockDefinition definition{};
    world::BlockAxis axis{world::BlockAxis::Y};
};

WorldBlockKey toKey(world::ChunkCoord chunk, world::BlockCoord local)
{
    return {
        chunk.x * world::ChunkSize + local.x,
        chunk.y * world::ChunkSize + local.y,
        chunk.z * world::ChunkSize + local.z
    };
}

WorldBlockKey toKey(const world::PlanetCoord& pos)
{
    return {
        pos.chunk.x * world::ChunkSize + pos.block.x,
        pos.chunk.y * world::ChunkSize + pos.block.y,
        pos.chunk.z * world::ChunkSize + pos.block.z
    };
}

struct FaceAxis {
    int dx;
    int dy;
    int dz;
    world::BlockAxis perpendicularAxis;
};

constexpr FaceAxis kFaces[6] = {
    { 1,  0,  0, world::BlockAxis::X},
    {-1,  0,  0, world::BlockAxis::X},
    { 0,  1,  0, world::BlockAxis::Y},
    { 0, -1,  0, world::BlockAxis::Y},
    { 0,  0,  1, world::BlockAxis::Z},
    { 0,  0, -1, world::BlockAxis::Z},
};

[[nodiscard]] bool axesConnect(world::BlockAxis fromAxis, world::BlockAxis toAxis, world::BlockAxis faceAxis)
{
    if (fromAxis == toAxis) {
        return faceAxis == fromAxis;
    }
    return faceAxis == fromAxis || faceAxis == toAxis;
}

[[nodiscard]] bool canConnect(const IndexedNode& from, const IndexedNode& to, const FaceAxis& face)
{
    if (!from.definition.axisAware && !to.definition.axisAware) {
        return true;
    }
    if (!from.definition.axisAware) {
        return face.perpendicularAxis == to.axis;
    }
    if (!to.definition.axisAware) {
        return face.perpendicularAxis == from.axis;
    }
    if (from.axis == to.axis) {
        return face.perpendicularAxis == from.axis;
    }
    return face.perpendicularAxis == from.axis || face.perpendicularAxis == to.axis;
}

} // namespace

void KineticBlockCatalog::set(BlockTypeId type, KineticBlockDefinition definition)
{
    if (entries_.size() <= type.value) {
        entries_.resize(static_cast<std::size_t>(type.value) + 1U);
    }
    entries_[type.value] = definition;
}

KineticBlockDefinition KineticBlockCatalog::get(BlockStateId state) const noexcept
{
    return getByType(world::blockTypeOf(state));
}

KineticBlockDefinition KineticBlockCatalog::getByType(BlockTypeId type) const noexcept
{
    if (type.value < entries_.size()) {
        return entries_[type.value];
    }
    return {};
}

bool KineticBlockCatalog::hasEnabledEntries() const noexcept
{
    for (const auto& entry : entries_) {
        if (entry.enabled) {
            return true;
        }
    }
    return false;
}

KineticSolveResult KineticNetworkSolver::solve(const world::ChunkManager& chunks, const KineticBlockCatalog& catalog) const
{
    if (!catalog.hasEnabledEntries()) {
        return {};
    }
    std::vector<IndexedNode> indexedNodes;
    std::unordered_map<WorldBlockKey, std::size_t, WorldBlockKeyHash> lookup;

    chunks.forEach([&](const world::Chunk& chunk) {
        if (chunk.state() == world::ChunkState::Empty || chunk.state() == world::ChunkState::Requested) {
            return;
        }

        for (int z = 0; z < world::ChunkSize; ++z) {
            for (int y = 0; y < world::ChunkSize; ++y) {
                for (int x = 0; x < world::ChunkSize; ++x) {
                    const auto block = chunk.blockAt(x, y, z);
                    const auto definition = catalog.get(block);
                    if (!definition.enabled) {
                        continue;
                    }

                    const world::BlockCoord local{x, y, z};
                    const auto key = toKey(chunk.coord(), local);

                    DirtyKey dk{key.x, key.y, key.z};
                    if (const auto it = disabledPositions_.find(dk); it != disabledPositions_.end() && it->second) {
                        continue;
                    }

                    const auto index = indexedNodes.size();
                    auto node = IndexedNode{key, {0, {}, chunk.coord(), local}, block, definition};
                    if (definition.axisAware) {
                        node.axis = world::axisOf(block);
                    }
                    indexedNodes.push_back(std::move(node));
                    lookup.emplace(key, index);
                }
            }
        }
    });

    KineticSolveResult result;
    result.nodes.reserve(indexedNodes.size());
    std::vector<bool> visited(indexedNodes.size(), false);

    for (std::size_t start = 0; start < indexedNodes.size(); ++start) {
        if (visited[start]) {
            continue;
        }

        std::vector<std::size_t> component;
        std::queue<std::size_t> queue;
        queue.push(start);
        visited[start] = true;

        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();
            component.push_back(current);

            const auto& currentNode = indexedNodes[current];
            const auto key = currentNode.key;

            for (const auto& face : kFaces) {
                const WorldBlockKey neighbor{key.x + face.dx, key.y + face.dy, key.z + face.dz};
                const auto found = lookup.find(neighbor);
                if (found == lookup.end() || visited[found->second]) {
                    continue;
                }

                const auto& neighborNode = indexedNodes[found->second];
                if (!canConnect(currentNode, neighborNode, face)) {
                    continue;
                }

                visited[found->second] = true;
                queue.push(found->second);
            }
        }

        KineticNetworkDebug network;
        network.id = static_cast<std::uint64_t>(result.networks.size() + 1U);
        network.nodeCount = static_cast<std::uint32_t>(component.size());
        if (!component.empty()) {
            network.representativeNode = indexedNodes[component.front()].position;
        }

        float sourceRpmSum = 0.0F;
        for (const auto nodeIndex : component) {
            const auto& node = indexedNodes[nodeIndex];
            network.stressCapacity += node.definition.stressCapacity;
            network.stressDemand += node.definition.stressDemand;

            if (node.definition.role == KineticRole::Source) {
                ++network.sourceCount;
                sourceRpmSum += node.definition.sourceRpm;
            } else if (node.definition.role == KineticRole::Consumer) {
                ++network.consumerCount;
            }
        }

        network.rpm = network.sourceCount > 0 ? sourceRpmSum / static_cast<float>(network.sourceCount) : 0.0F;
        network.overloaded = network.stressDemand > network.stressCapacity || network.sourceCount == 0;
        if (network.overloaded) {
            ++result.overloadedNetworks;
            network.rpm = 0.0F;
        }
        network.direction = static_cast<std::int8_t>(RotationDirection::Clockwise);

        for (const auto nodeIndex : component) {
            const auto& node = indexedNodes[nodeIndex];
            float nodeRpm = network.rpm;
            std::int8_t nodeDir = network.direction;

            if (node.definition.role == KineticRole::Transfer && node.definition.gearRatio != 1.0F) {
                nodeRpm *= node.definition.gearRatio;
                nodeDir = -nodeDir;
            } else if (node.definition.gearRatio != 1.0F) {
                nodeRpm *= node.definition.gearRatio;
            }

            result.nodes.push_back({
                node.position,
                node.block,
                node.definition.role,
                nodeRpm,
                network.overloaded && node.definition.breaksOnOverload,
                network.id,
                nodeDir
            });
        }

        result.networks.push_back(network);
    }

    return result;
}

void KineticNetworkSolver::markDirty(world::PlanetCoord position)
{
    const auto key = toKey(position);
    dirtyPositions_[{key.x, key.y, key.z}] = true;
}

void KineticNetworkSolver::markDirty(world::ChunkCoord coord)
{
    for (int z = 0; z < world::ChunkSize; ++z) {
        for (int y = 0; y < world::ChunkSize; ++y) {
            for (int x = 0; x < world::ChunkSize; ++x) {
                const auto key = toKey(coord, {x, y, z});
                dirtyPositions_[{key.x, key.y, key.z}] = true;
            }
        }
    }
}

bool KineticNetworkSolver::hasDirtyPositions() const noexcept
{
    return !dirtyPositions_.empty();
}

void KineticNetworkSolver::clearDirty()
{
    dirtyPositions_.clear();
}

KineticSolveResult KineticNetworkSolver::solveDirty(const world::ChunkManager& chunks, const KineticBlockCatalog& catalog)
{
    clearDirty();
    return solve(chunks, catalog);
}

void KineticNetworkSolver::setBlockDisabled(world::PlanetCoord position, bool disabled)
{
    const auto key = toKey(position);
    disabledPositions_[{key.x, key.y, key.z}] = disabled;
}

bool KineticNetworkSolver::isBlockDisabled(world::PlanetCoord position) const
{
    const auto key = toKey(position);
    const auto it = disabledPositions_.find({key.x, key.y, key.z});
    return it != disabledPositions_.end() && it->second;
}

} // namespace voxel::automation
