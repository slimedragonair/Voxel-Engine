#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <voxel/core/Types.hpp>
#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::automation {

enum class KineticRole : std::uint8_t {
    Transfer,
    Source,
    Consumer
};

enum class RotationDirection : std::int8_t {
    Clockwise = 1,
    CounterClockwise = -1
};

struct KineticBlockDefinition {
    bool enabled{};
    KineticRole role{KineticRole::Transfer};
    float sourceRpm{};
    float stressCapacity{};
    float stressDemand{};
    float gearRatio{1.0F};
    bool breaksOnOverload{true};
    bool axisAware{false};
};

class KineticBlockCatalog {
public:
    void set(BlockTypeId type, KineticBlockDefinition definition);
    [[nodiscard]] KineticBlockDefinition get(BlockStateId state) const noexcept;
    [[nodiscard]] KineticBlockDefinition getByType(BlockTypeId type) const noexcept;
    [[nodiscard]] bool hasEnabledEntries() const noexcept;

private:
    std::vector<KineticBlockDefinition> entries_;
};

struct KineticNodeDebug {
    world::PlanetCoord position{};
    BlockStateId block{};
    KineticRole role{KineticRole::Transfer};
    float solvedRpm{};
    bool overloaded{};
    std::uint64_t networkId{};
    std::int8_t direction{1};
};

struct KineticNetworkDebug {
    std::uint64_t id{};
    std::uint32_t nodeCount{};
    std::uint32_t sourceCount{};
    std::uint32_t consumerCount{};
    float rpm{};
    float stressDemand{};
    float stressCapacity{};
    bool overloaded{};
    world::PlanetCoord representativeNode{};
    std::int8_t direction{1};
};

struct KineticSolveResult {
    std::vector<KineticNetworkDebug> networks;
    std::vector<KineticNodeDebug> nodes;
    std::uint32_t overloadedNetworks{};
};

class KineticNetworkSolver {
public:
    [[nodiscard]] KineticSolveResult solve(const world::ChunkManager& chunks, const KineticBlockCatalog& catalog) const;

    void markDirty(world::PlanetCoord position);
    void markDirty(world::ChunkCoord coord);
    [[nodiscard]] bool hasDirtyPositions() const noexcept;
    void clearDirty();
    [[nodiscard]] KineticSolveResult solveDirty(const world::ChunkManager& chunks, const KineticBlockCatalog& catalog);

    void setBlockDisabled(world::PlanetCoord position, bool disabled);
    [[nodiscard]] bool isBlockDisabled(world::PlanetCoord position) const;

private:
    struct DirtyKey {
        std::int64_t x{};
        std::int64_t y{};
        std::int64_t z{};
        [[nodiscard]] friend bool operator==(const DirtyKey& lhs, const DirtyKey& rhs) noexcept
        {
            return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
        }
    };
    struct DirtyKeyHash {
        [[nodiscard]] std::size_t operator()(const DirtyKey& k) const noexcept
        {
            return static_cast<std::size_t>(
                (static_cast<std::uint64_t>(k.x) * 73856093ULL)
                ^ (static_cast<std::uint64_t>(k.y) * 19349663ULL)
                ^ (static_cast<std::uint64_t>(k.z) * 83492791ULL));
        }
    };
    std::unordered_map<DirtyKey, bool, DirtyKeyHash> dirtyPositions_;
    std::unordered_map<DirtyKey, bool, DirtyKeyHash> disabledPositions_;
};

} // namespace voxel::automation
