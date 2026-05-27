#include <voxel/world/BlockEntityBehavior.hpp>

#include <chrono>

#include <voxel/core/Logger.hpp>

namespace voxel::world {

void BlockEntityTypeRegistry::registerType(const std::string& typeId, BlockEntityBehaviorFactory factory)
{
    factories_[typeId] = std::move(factory);
}

std::unique_ptr<IBlockEntityBehavior> BlockEntityTypeRegistry::create(const std::string& typeId) const
{
    const auto it = factories_.find(typeId);
    if (it == factories_.end()) {
        return nullptr;
    }
    return it->second();
}

bool BlockEntityTypeRegistry::hasType(const std::string& typeId) const
{
    return factories_.contains(typeId);
}

IBlockEntityBehavior* BlockEntityTickScheduler::getOrCreateBehavior(const PlanetCoord& pos, const std::string& typeId)
{
    const auto key = posToKey(pos);
    const auto it = behaviors_.find(key);
    if (it != behaviors_.end()) {
        return it->second.get();
    }
    auto behavior = typeRegistry_->create(typeId);
    if (behavior == nullptr) {
        return nullptr;
    }
    auto* raw = behavior.get();
    behaviors_.emplace(key, std::move(behavior));
    return raw;
}

void BlockEntityTickScheduler::removeBehavior(const PlanetCoord& pos)
{
    behaviors_.erase(posToKey(pos));
}

void BlockEntityTickScheduler::initialize(const data::BlockRegistry& blocks, const BlockEntityTypeRegistry& types)
{
    typeRegistry_ = &types;
}

BlockEntityTickStats BlockEntityTickScheduler::tick(BlockEntityTickContext& contextTemplate,
                                                    const ChunkManager& chunks,
                                                    const data::BlockRegistry& blockRegistry,
                                                    double budgetMs,
                                                    std::function<bool(ChunkCoord)> shouldTickChunk)
{
    BlockEntityTickStats stats;
    if (typeRegistry_ == nullptr) {
        return stats;
    }

    const auto start = std::chrono::steady_clock::now();

    chunks.forEach([&](const Chunk& chunk) {
        if (stats.budgetSaturated) {
            return;
        }

        const auto elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsedMs >= budgetMs) {
            stats.budgetSaturated = true;
            return;
        }

        const auto& coord = chunk.coord();
        // R0-LOD plan: LOD1 sim throttle. Chunks outside the active
        // sim band (provided by the predicate) skip their O(32³)
        // block scan entirely. They retain their state — block
        // entities just don't advance until the player gets closer.
        if (shouldTickChunk && !shouldTickChunk(coord)) {
            ++stats.entitiesSkipped;
            return;
        }
        for (std::uint8_t y = 0; y < ChunkSize; ++y) {
            for (std::uint8_t z = 0; z < ChunkSize; ++z) {
                for (std::uint8_t x = 0; x < ChunkSize; ++x) {
                    const auto stateId = chunk.blockAt(x, y, z);
                    const auto typeId = blockTypeOf(stateId);
                    const auto* blockDef = blockRegistry.registry().byRuntimeId(typeId.value);
                    if (blockDef == nullptr || !blockDef->hasBlockEntity || blockDef->blockEntityType.empty()) {
                        continue;
                    }

                    const PlanetCoord pos{0, {}, {coord.x, coord.y, coord.z},
                        {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)}};

                    auto* behavior = getOrCreateBehavior(pos, blockDef->blockEntityType);
                    if (behavior == nullptr) {
                        ++stats.entitiesSkipped;
                        continue;
                    }

                    contextTemplate.position = pos;
                    behavior->tick(contextTemplate);
                    ++stats.entitiesTicked;
                }
            }
        }
    });

    const auto end = std::chrono::steady_clock::now();
    stats.tickMs = std::chrono::duration<double, std::milli>(end - start).count();
    return stats;
}

void BlockEntityTickScheduler::markDirty(ChunkCoord coord)
{
    dirtyChunks_.push_back(coord);
}

} // namespace voxel::world
