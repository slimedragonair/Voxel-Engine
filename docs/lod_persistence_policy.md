# LOD Persistence Policy

## The user-visible problem

When the player moves away from a chunk, the engine currently:
1. Streamer marks the chunk out-of-range.
2. Mesh is removed from the GPU.
3. **The chunk just disappears** — the player sees a gap where terrain used to be.

This is acceptable in classic chunk-only engines (Minecraft) but unacceptable
for AetherForge because:

- LOD2 cluster meshes exist precisely so distant terrain stays visible.
- A "fade out" / "pop out" between LOD0 eviction and LOD2 arrival breaks
  spatial continuity, especially noticeable during fast flight or ship travel.

## The invariant

> **A chunk's GPU mesh MUST NOT be removed until the containing cluster's
> mesh is live on the GPU.**

This guarantees there is never a frame where the player sees nothing where
terrain should be. The transition is:

```
  Frame N    : LOD0 chunk mesh rendered (cluster mesh may or may not exist)
  Frame N+1  : LOD2 cluster mesh installed (chunk mesh still rendered, depth-
               buffer covers cluster behind it)
  Frame N+k  : Chunk leaves LOD0/1 distance band, chunk mesh evicted,
               cluster mesh takes over the area's pixels
```

The depth buffer naturally resolves overdraw: when both LOD0 and LOD2 are
present at the same world location, LOD0 wins (same depth, drawn second OR
slightly biased forward). There's no visible flicker because both
representations occupy the same volume.

## Implementation responsibilities

### `ClusterRenderer` (this PR — Phase 1C-2)

- Renders LOD2 meshes independently of chunk meshes.
- Does NOT remove a cluster mesh just because some chunks in it leave LOD0.
- Tracks resident clusters via `uploadedClusters_`.

### Chunk eviction (Phase 1D — `ChunkStreamer` / `Application`)

- When deciding to evict a chunk's MESH (separate from evicting its block
  data — block data has its own eviction policy):
  - **Block**: don't remove `uploadedMeshes_[coord]` unless one of:
    1. The containing cluster has a live mesh in `ClusterRenderer::uploadedClusters_`.
    2. The chunk is being unloaded entirely (out of even the LOD3 region band).
  - **Trigger**: when a chunk enters the LOD2 distance band, the streamer
    should pre-emptively enqueue a cluster mesh build for its containing
    cluster. By the time the chunk crosses the LOD0→LOD2 threshold, the
    cluster should already be queued or built.

### Cluster mesh scheduling (Phase 1D)

- Cluster mesh build priority should match the distance of its containing
  chunks. Closer = sooner.
- A cluster mesh becomes "stale" when any contained chunk's revision
  changes. The streamer should re-enqueue at low priority — visible quality
  degrades briefly but is fine for distant LOD2.

## What NOT to do

- **Don't render the same area twice intentionally**: depth buffer handling
  is fine for the brief transition window, but rendering BOTH LOD0 and LOD2
  for the entire LOD2 ring all the time wastes GPU. The renderer's draw
  pass should skip LOD2 clusters whose contained chunks are ALL LOD0-mesh
  resident.

- **Don't pre-emptively build LOD2 for every cluster**: this kills cold
  start. Only schedule cluster builds for clusters whose chunks are about
  to leave LOD0 (i.e., distance > LOD1 max).

- **Don't tie cluster lifetime to a single chunk's lifecycle**: a cluster
  outlives all its chunks. It evicts only when the player moves so far
  that LOD3 region representation takes over.

## State transitions per chunk

```
       ┌──────────────────────────┐
       │      Active (LOD0)       │
       │  - block data resident   │
       │  - chunk mesh on GPU     │
       └────────────┬─────────────┘
                    │  player moves out to LOD2 distance
                    ▼
       ┌──────────────────────────┐
       │   Persisting transition  │  ← chunk mesh STAYS, cluster build queued
       │  - block data resident   │
       │  - chunk mesh on GPU     │
       │  - cluster mesh queued   │
       └────────────┬─────────────┘
                    │  cluster mesh becomes live
                    ▼
       ┌──────────────────────────┐
       │    Cluster-only (LOD2)   │
       │  - block data optional   │
       │  - chunk mesh REMOVED    │
       │  - cluster mesh on GPU   │
       └────────────┬─────────────┘
                    │  player moves out to LOD3 distance
                    ▼
       ┌──────────────────────────┐
       │     Region (LOD3)        │
       │  - cluster mesh REMOVED  │
       │  - region patch on GPU   │
       └──────────────────────────┘
```

The middle "persisting transition" state is the key to user-visible
continuity. It exists as long as needed for the cluster mesh build to
finish — usually 1-2 frames, but could be longer under heavy load.
