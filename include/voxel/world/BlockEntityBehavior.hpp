#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <voxel/automation/KineticNetwork.hpp>
#include <voxel/core/Types.hpp>
#include <voxel/data/BlockRegistry.hpp>
#include <voxel/data/ItemRegistry.hpp>
#include <voxel/data/RecipeRegistry.hpp>
#include <voxel/inventory/Inventory.hpp>
#include <voxel/world/ChunkManager.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::world {

struct BlockEntityTickContext {
    const data::ItemRegistry& itemRegistry;
    const data::RecipeRegistry& recipeRegistry;
    const data::BlockRegistry& blockRegistry;
    inventory::InventoryManager& inventoryManager;
    automation::KineticNetworkSolver& kineticSolver;
    ChunkManager& chunks;
    world::PlanetCoord position;
    float dt{1.0F / 60.0F};
    Tick tick{0};
};

class IBlockEntityBehavior {
public:
    virtual ~IBlockEntityBehavior() = default;

    virtual void tick(BlockEntityTickContext& context) = 0;
    virtual void onNeighborChange(BlockEntityTickContext& context, BlockCoord normal) { (void)context; (void)normal; }
    virtual void onLoad(BlockEntityTickContext& context) { (void)context; }
    virtual void onUnload(BlockEntityTickContext& context) { (void)context; }

    virtual std::vector<std::uint8_t> serialize() const { return {}; }
    virtual bool deserialize(const std::vector<std::uint8_t>& data) { (void)data; return true; }
};

using BlockEntityBehaviorFactory = std::function<std::unique_ptr<IBlockEntityBehavior>()>;

class BlockEntityTypeRegistry {
public:
    void registerType(const std::string& typeId, BlockEntityBehaviorFactory factory);
    [[nodiscard]] std::unique_ptr<IBlockEntityBehavior> create(const std::string& typeId) const;
    [[nodiscard]] bool hasType(const std::string& typeId) const;

private:
    std::unordered_map<std::string, BlockEntityBehaviorFactory> factories_;
};

struct BlockEntityTickStats {
    std::size_t entitiesTicked{0};
    std::size_t entitiesSkipped{0};
    double tickMs{0.0};
    bool budgetSaturated{false};
};

class BlockEntityTickScheduler {
public:
    void initialize(const data::BlockRegistry& blocks, const BlockEntityTypeRegistry& types);

    // `shouldTickChunk` (if set) is queried per-chunk before the
    // O(32³) block scan. Returning false skips the chunk for this
    // tick — its block entities effectively pause until the predicate
    // returns true again. The Application supplies a predicate that
    // returns isChunkInActiveSim(coord), implementing the LOD1 sim
    // throttle: LOD1 chunks (between simulationDistance and
    // renderDistance) freeze their block-entity ticks until the
    // player gets closer.
    //
    // Default-null predicate = original behavior (tick every chunk).
    BlockEntityTickStats tick(BlockEntityTickContext& contextTemplate,
                              const ChunkManager& chunks,
                              const data::BlockRegistry& blockRegistry,
                              double budgetMs,
                              std::function<bool(ChunkCoord)> shouldTickChunk = {});

    void markDirty(world::ChunkCoord coord);
    void setBudgetMs(double budgetMs) noexcept { budgetMs_ = budgetMs; }

    IBlockEntityBehavior* getOrCreateBehavior(const PlanetCoord& pos, const std::string& typeId);
    void removeBehavior(const PlanetCoord& pos);

private:
    struct BlockEntityKey {
        std::int64_t chunkX{};
        std::int64_t chunkY{};
        std::int64_t chunkZ{};
        std::int32_t blockX{};
        std::int32_t blockY{};
        std::int32_t blockZ{};

        [[nodiscard]] friend bool operator==(const BlockEntityKey& lhs, const BlockEntityKey& rhs) noexcept
        {
            return lhs.chunkX == rhs.chunkX && lhs.chunkY == rhs.chunkY && lhs.chunkZ == rhs.chunkZ
                && lhs.blockX == rhs.blockX && lhs.blockY == rhs.blockY && lhs.blockZ == rhs.blockZ;
        }
    };

    struct BlockEntityKeyHash {
        [[nodiscard]] std::size_t operator()(const BlockEntityKey& k) const noexcept
        {
            const auto h = static_cast<std::size_t>(
                (static_cast<std::uint64_t>(k.chunkX) * 73856093ULL)
                ^ (static_cast<std::uint64_t>(k.chunkY) * 19349663ULL)
                ^ (static_cast<std::uint64_t>(k.chunkZ) * 83492791ULL)
                ^ (static_cast<std::uint64_t>(k.blockX) * 50331653ULL)
                ^ (static_cast<std::uint64_t>(k.blockY) * 3089)
                ^ (static_cast<std::uint64_t>(k.blockZ) * 997));
            return h;
        }
    };

    static BlockEntityKey posToKey(const PlanetCoord& pos)
    {
        return {pos.chunk.x, pos.chunk.y, pos.chunk.z, pos.block.x, pos.block.y, pos.block.z};
    }

    const BlockEntityTypeRegistry* typeRegistry_{nullptr};
    double budgetMs_{2.0};
    std::vector<world::ChunkCoord> dirtyChunks_;
    std::unordered_map<BlockEntityKey, std::unique_ptr<IBlockEntityBehavior>, BlockEntityKeyHash> behaviors_;
};

} // namespace voxel::world
