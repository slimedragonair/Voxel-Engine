#include <voxel/world/FluidSystem.hpp>

#include <algorithm>
#include <array>
#include <unordered_map>

#include <voxel/world/BlockState.hpp>
#include <voxel/world/Chunk.hpp>
#include <voxel/world/ChunkFluidData.hpp>
#include <voxel/world/CoordinateUtils.hpp>

namespace voxel::world {

namespace {

// Tiny external store for per-chunk fluid data. Keeping it outside the Chunk
// class avoids touching the save format until the simulation proves itself.
// Memory: one map entry per chunk that has any water (or has had any) — a
// fully oceanic chunk uses ~32 KB; non-fluid chunks use zero.
//
// NB: this is a process-wide singleton because FluidSystem is owned by the
// Application and the simulation is single-threaded for v1. If we move sim
// to workers, this becomes a member with the appropriate locking.
struct FluidDataStore {
    std::unordered_map<ChunkCoord, std::unique_ptr<ChunkFluidData>, ChunkCoordHash> data;

    ChunkFluidData& getOrCreate(ChunkCoord coord)
    {
        auto& slot = data[coord];
        if (!slot) {
            slot = std::make_unique<ChunkFluidData>();
        }
        return *slot;
    }

    ChunkFluidData* find(ChunkCoord coord) noexcept
    {
        const auto it = data.find(coord);
        return it == data.end() ? nullptr : it->second.get();
    }
};

FluidDataStore g_store;

constexpr std::array<BlockCoord, 4> kHorizontalDeltas{{
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 0, 1}, { 0, 0,-1},
}};

[[nodiscard]] bool isWaterBlock(BlockStateId state, std::uint32_t waterValue) noexcept
{
    return state.value != 0u && (state.value >> 16) == (waterValue >> 16);
}

[[nodiscard]] bool isAirBlock(BlockStateId state) noexcept
{
    return state.value == AirBlockState.value;
}

[[nodiscard]] bool fluidAllowedAtWorldY(const FluidSystemSettings& settings, std::int32_t worldY) noexcept
{
    return !settings.maxActiveWorldY.has_value() || worldY <= *settings.maxActiveWorldY;
}

// Wake a cell (chunk + local) into the queue, deriving a priority from the
// cell's chunk distance to the active center.
void wakeCell(FluidDirtyQueue& q, ChunkCoord chunk, BlockCoord local, float priority)
{
    q.enqueue(FluidQueueKey{chunk, local}, priority);
}

} // namespace

FluidSystem::FluidSystem(FluidSystemSettings settings)
    : settings_(settings)
{
}

void FluidSystem::wake(ChunkCoord chunkCoord, BlockCoord local, float priority)
{
    const auto world = toWorldBlock(chunkCoord, local);
    if (!fluidAllowedAtWorldY(settings_, world.y)) {
        return;
    }
    wakeCell(queue_, chunkCoord, local, priority);
}

std::size_t FluidSystem::activateOceanEdge(ChunkManager& chunks, ChunkCoord originChunk,
                                            BlockCoord originLocal, int radius)
{
    std::size_t unlocked = 0;
    if (radius <= 0) {
        return 0;
    }

    // Simple BFS bounded by radius. Each step explores 6 neighbours; we only
    // queue water cells that are currently ocean-locked.
    struct Node {
        ChunkCoord chunk;
        BlockCoord local;
        int hops;
    };
    std::vector<Node> frontier;
    frontier.push_back({originChunk, originLocal, 0});

    std::unordered_map<FluidQueueKey, std::uint8_t, FluidQueueKeyHash> visited;
    visited[FluidQueueKey{originChunk, originLocal}] = 0;

    constexpr std::array<BlockCoord, 6> kFaceDeltas{{
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1},
    }};

    while (!frontier.empty()) {
        Node node = frontier.back();
        frontier.pop_back();
        if (node.hops > radius) {
            continue;
        }
        auto* chunk = chunks.find(node.chunk);
        if (chunk == nullptr) {
            continue;
        }
        const auto worldHere = toWorldBlock(node.chunk, node.local);
        if (!fluidAllowedAtWorldY(settings_, worldHere.y)) {
            continue;
        }
        const auto block = chunk->blockAt(node.local.x, node.local.y, node.local.z);
        if (!isWaterBlock(block, settings_.waterBlockValue)) {
            continue;
        }
        auto* fluidData = g_store.find(node.chunk);
        if (fluidData == nullptr || !fluidData->oceanLockedAt(node.local.x, node.local.y, node.local.z)) {
            // Not locked, or no fluid data yet (so by definition unlocked).
            // Still wake the cell because it may need to flow now that a
            // neighbour was removed.
            wakeCell(queue_, node.chunk, node.local, static_cast<float>(node.hops));
            // Continue exploring even unlocked water — there may be locked
            // cells deeper in the body.
        } else {
            fluidData->setOceanLocked(node.local.x, node.local.y, node.local.z, false);
            ++unlocked;
            wakeCell(queue_, node.chunk, node.local, static_cast<float>(node.hops));
        }

        // Expand the frontier with neighbouring water cells.
        for (const auto& d : kFaceDeltas) {
            const auto neighbourWorld = BlockCoord{worldHere.x + d.x, worldHere.y + d.y, worldHere.z + d.z};
            if (!fluidAllowedAtWorldY(settings_, neighbourWorld.y)) {
                continue;
            }
            const auto neighbourLocal = toChunkLocal(neighbourWorld.x, neighbourWorld.y, neighbourWorld.z);
            FluidQueueKey key{neighbourLocal.chunk, neighbourLocal.local};
            const auto [it, inserted] = visited.try_emplace(key, static_cast<std::uint8_t>(node.hops + 1));
            if (inserted) {
                frontier.push_back({neighbourLocal.chunk, neighbourLocal.local, node.hops + 1});
            }
        }
    }

    return unlocked;
}

FluidSimStats FluidSystem::tick(ChunkManager& chunks, ChunkCoord center)
{
    FluidSimStats stats{};
    if (settings_.waterBlockValue == 0u || queue_.size() == 0) {
        return stats;
    }

    const auto batch = queue_.popClosest(center, settings_.maxCellsPerTick);
    for (const auto& item : batch) {
        ++stats.cellsProcessed;
        const auto chunkCoord = item.key.chunk;
        const auto local = item.key.local;
        auto* chunk = chunks.find(chunkCoord);
        if (chunk == nullptr) {
            // Chunk evicted while the cell was queued; just drop it.
            continue;
        }
        const auto block = chunk->blockAt(local.x, local.y, local.z);
        if (!isWaterBlock(block, settings_.waterBlockValue)) {
            // No longer water — drop.
            continue;
        }
        // The cell below: if it's air, the water falls into it.
        const auto worldHere = toWorldBlock(chunkCoord, local);
        if (!fluidAllowedAtWorldY(settings_, worldHere.y)) {
            continue;
        }
        const BlockCoord belowWorld{worldHere.x, worldHere.y - 1, worldHere.z};
        if (!fluidAllowedAtWorldY(settings_, belowWorld.y)) {
            continue;
        }
        const auto belowLocal = toChunkLocal(belowWorld.x, belowWorld.y, belowWorld.z);
        auto* belowChunk = chunks.find(belowLocal.chunk);
        if (belowChunk == nullptr) {
            // Can't see below — leave the cell for later (next tick).
            queue_.enqueue(item.key, item.priority);
            ++stats.cellsRequeued;
            continue;
        }
        const auto belowBlock = belowChunk->blockAt(
            belowLocal.local.x, belowLocal.local.y, belowLocal.local.z);
        if (isAirBlock(belowBlock)) {
            // Carve falling water into the cell below.
            const auto waterState = BlockStateId{settings_.waterBlockValue};
            belowChunk->setBlockSilently(belowLocal.local.x, belowLocal.local.y, belowLocal.local.z, waterState);
            auto& belowFluid = g_store.getOrCreate(belowLocal.chunk);
            belowFluid.setCell(belowLocal.local.x, belowLocal.local.y, belowLocal.local.z,
                               makeFluidCell(0, /*falling=*/true, /*oceanLocked=*/false));
            belowChunk->markMeshDirtyNoRevision();
            ++stats.cellsCarved;

            // The newly carved cell needs to look for falling continuation.
            wakeCell(queue_, belowLocal.chunk, belowLocal.local, item.priority);

            // The source cell stays water for now (deep ocean / replenishing
            // body). For a future "consume on flow" pass, set it to air here.
            continue;
        }

        // Horizontal flow (v2). Fall didn't work — the cell below is solid.
        // Check the 4 same-Y neighbours; for any air cell, carve water into
        // it and wake it so it continues the chain next tick.
        //
        // Behaviour: this implements Minecraft-style "finite source + flooding
        // into voids". A water cell at the shoreline of an ocean propagates
        // into adjacent air cells. Spread is bounded by `maxCellsPerTick`
        // because each carved cell wakes only its own neighbours next tick —
        // the propagation rate is one block per tick.
        //
        // The carved cell is NOT marked `falling`. The next tick it will
        // re-check its own below cell (still solid usually), and may itself
        // continue horizontal flow. Levels are not yet tracked; everything
        // is full source water. A future pass will introduce level decay.
        for (const auto& d : kHorizontalDeltas) {
            const BlockCoord neighbourWorld{
                worldHere.x + d.x, worldHere.y + d.y, worldHere.z + d.z};
            if (!fluidAllowedAtWorldY(settings_, neighbourWorld.y)) {
                continue;
            }
            const auto neighbourLocal = toChunkLocal(
                neighbourWorld.x, neighbourWorld.y, neighbourWorld.z);
            auto* nChunk = chunks.find(neighbourLocal.chunk);
            if (nChunk == nullptr) {
                continue;
            }
            const auto nBlock = nChunk->blockAt(
                neighbourLocal.local.x, neighbourLocal.local.y, neighbourLocal.local.z);
            if (!isAirBlock(nBlock)) {
                continue;
            }
            // Carve a full-level (non-falling) water cell into the air gap.
            const auto waterState = BlockStateId{settings_.waterBlockValue};
            nChunk->setBlockSilently(
                neighbourLocal.local.x, neighbourLocal.local.y, neighbourLocal.local.z,
                waterState);
            auto& nFluid = g_store.getOrCreate(neighbourLocal.chunk);
            nFluid.setCell(
                neighbourLocal.local.x, neighbourLocal.local.y, neighbourLocal.local.z,
                makeFluidCell(0, /*falling=*/false, /*oceanLocked=*/false));
            nChunk->markMeshDirtyNoRevision();
            ++stats.cellsCarved;
            // Wake the new cell so it can fall or continue spreading next tick.
            wakeCell(queue_, neighbourLocal.chunk, neighbourLocal.local, item.priority);
        }
    }
    return stats;
}

} // namespace voxel::world
