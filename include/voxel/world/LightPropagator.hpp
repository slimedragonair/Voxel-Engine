#pragma once

#include <voxel/world/BlockLightCatalog.hpp>
#include <voxel/world/Chunk.hpp>
#include <voxel/world/ChunkLightData.hpp>
#include <voxel/world/ChunkManager.hpp>

namespace voxel::world {

// Cross-chunk-aware lighting solver (Phase F1).
//
// The propagator reads neighbour chunks from the supplied `ChunkManager` to
// seed sky and block light at the target chunk's boundaries:
//   * Sky enters from the +Y neighbour's bottom row of `lightData()`. If the
//     +Y neighbour has no `lightData` yet (uncomputed), open sky is assumed
//     so the surface lights up immediately on first generation.
//   * Block light entering through any face is seeded from the adjacent
//     cell of the neighbour's `lightData()`. Neighbours without lightData
//     contribute nothing (they'll cascade in later via dirty flags).
//
// The propagator is read-only with respect to neighbour chunks. Cascading
// re-lighting is driven by callers comparing the new lightData boundary
// values to the old and marking neighbours `dirty.lighting`.
class LightPropagator {
public:
    void propagate(const Chunk& target,
                   const ChunkManager& chunks,
                   const BlockLightCatalog& catalog,
                   ChunkLightData& out) const;

    // Convenience overload used by tests + offline tools: propagates a chunk
    // in isolation with the previous "open sky from above, no neighbours"
    // semantics. Equivalent to calling propagate() with an empty manager.
    void propagateIsolated(const Chunk& target,
                           const BlockLightCatalog& catalog,
                           ChunkLightData& out) const;
};

} // namespace voxel::world
