# GPU Hybrid Meshing and Terrain Generation - Design

## Goals

This phase starts two GPU compute ports that reuse the Vulkan extension pattern
already proven by `FluidGpuSystem`.

1. Hybrid meshing: the GPU classifies visible voxel faces and emits face
   records. The CPU keeps the final greedy merge and existing
   `ChunkMesh`/upload path. Target: about 10x lower mesh-build CPU time.
2. GPU terrain generation: the GPU fills chunk block-state data from terrain
   settings. The CPU remains authoritative by reading the generated block buffer
   back into `Chunk` before lighting, meshing, saving, or simulation sees it.

Both systems stay feature-flagged and default off until they match current
behavior and the debug overlay proves the frame-time win.

## Shared constraints

- `Chunk` data remains CPU-authoritative.
- Vulkan resources stay hidden behind PIMPL-style implementation files.
- Main-thread Vulkan submission is required; worker threads should request GPU
  jobs and consume completed readbacks, not touch VkDevice directly.
- Existing stale-result protection based on chunk revision, mesh revision, and
  neighbor hashes must continue to gate installs.
- CPU paths remain the fallback and the source of semantic truth during bringup.

## Hybrid meshing

### Current CPU split

`GreedyMesher` does three major jobs:

1. Build a per-cell cache from chunk block data and render catalog.
2. For each face direction, classify visible faces into 2D slice masks.
3. CPU greedy-merge masks into quads, vertices, indices, draw ranges, and
   section metadata.

The GPU should take over step 2 first. Step 3 stays on CPU because it is compact,
branchy, and already integrated with the renderer's packed vertex format.

### GPU input

The first implementation should upload a padded 34x34x34 cell grid for one mesh
job:

```cpp
struct HybridMeshingCell {
    uint32_t materialId; // 0 means air
    uint32_t flags;      // bit 0 occludes, bit 1 fluid, bits 8..9 surface
    uint32_t light;      // packed light or face-shade seed
    uint32_t pad;
};
```

The center chunk occupies `[1..32]` on each axis. The one-cell halo contains the
six face neighbors. Missing neighbors are encoded as air so the conservative
streaming behavior remains unchanged.

### GPU output

The shader emits a compact list:

```cpp
struct HybridFaceRecord {
    uint32_t localFace;  // [face:3][localIndex:15]
    uint32_t materialId;
    uint32_t packedLight;
    uint32_t surface;
};
```

CPU readback reconstructs the same per-face masks `GreedyMesher` already uses,
then runs the existing greedy merge code. The first CPU integration should keep
the old CPU mesher as a per-job fallback whenever the GPU path cannot allocate,
overflows `maxFaces`, or is disabled.

### Runtime sequence

1. Main thread drains mesh queue candidates as today.
2. For candidates selected for hybrid meshing, it captures the same target chunk
   and six-neighbor snapshot used by the CPU worker path.
3. Main thread packs one or more snapshots into GPU input buffers and submits
   `mesh_face_classify.comp`.
4. One-frame-later readback produces face records.
5. A worker or main-thread task runs CPU greedy merge from records into
   `ChunkMesh`.
6. Existing install path validates revision/hash, uploads mesh, stores cache,
   clears dirty flags.

### Bringup phases

Phase M0:
- Add shader contract and design docs.
- Add default-off `useGpuHybridMeshing` config flag.

Phase M1:
- Extract CPU greedy merge so it can consume either CPU-built masks or
  GPU-produced face records.
- Add unit tests comparing CPU mesher output against records converted back to
  masks.

Phase M2:
- Add `HybridMeshingGpuSystem` resources: input buffer, face buffer, readback
  buffers, descriptor set, compute pipeline, command buffers, fences.
- Submit one chunk per tick, read back next tick, CPU-merge, compare with CPU
  result in debug builds.

Phase M3:
- Batch multiple chunks per dispatch frame.
- Add overflow fallback and debug overlay stats:
  `hybrid_mesh_gpu_jobs`, `hybrid_mesh_fallbacks`, `hybrid_mesh_faces`,
  `hybrid_mesh_gpu_us`, `hybrid_mesh_merge_us`.

## GPU terrain generation

### Target shape

The GPU generator fills block states for a 32^3 chunk into a readback buffer.
CPU then imports the block values into `Chunk` and marks the chunk generated.
This preserves the existing downstream systems: save/load, lighting, meshing,
fluid simulation, and block editing all continue to see ordinary `Chunk` data.

### Porting strategy

The CPU terrain pipeline has two workloads:

1. Column world-shape/prepass data: 2D noise, biome, sea/rivers, surface height.
2. Per-block generation: base fill, caves, ores, fluids, structures, foliage.

The first GPU target should be base terrain + fluid fill + cave/ore density.
Structures and foliage remain CPU-side initially because they are sparse,
cross-chunk, and already cheap compared with density sampling.

### GPU output

```cpp
struct TerrainBlockOut {
    uint32_t stateValue; // BlockStateId.value
};
```

One output slot is `32 * 32 * 32 * 4 = 128 KB` per chunk. Double-buffered
readback for 16 in-flight GPU terrain jobs is about 4 MB, acceptable.

### Runtime sequence

1. Chunk pipeline selects generation requests as it does today.
2. If `useGpuTerrainGeneration` is true and the active generator is compatible,
   the main thread submits GPU terrain jobs instead of worker CPU jobs.
3. The GPU fills output block IDs.
4. One-frame-later readback imports the block IDs into a `Chunk`.
5. CPU runs deferred sparse stages that remain CPU-only: structures and foliage.
6. The existing install path stores the generated chunk and enqueues lighting
   and meshing.

### Bringup phases

Phase T0:
- Add shader contract and default-off `useGpuTerrainGeneration` flag.
- Define a small packed settings struct with block IDs, seed, sea level, base
  frequencies, and chunk coord.

Phase T1:
- Implement GPU base-height terrain only.
- Add deterministic tests comparing GPU output against a deliberately simple
  CPU reference generator, not the full `NoiseTerrainGenerator`.

Phase T2:
- Port world-shape signals and prepass interpolation.
- Compare generated surface columns against `NoiseTerrainGenerator` within an
  exact integer surface-height match target.

Phase T3:
- Port cave/ore density grids.
- CPU applies structures and foliage after readback.

Phase T4:
- Batch chunk columns and add debug overlay stats:
  `gpu_terrain_jobs`, `gpu_terrain_fallbacks`, `gpu_terrain_readback_kb`,
  `gpu_terrain_us`.

## Order of implementation

Hybrid meshing should land before GPU terrain generation. It has a tighter seam,
less semantic risk, and immediate payoff in the existing dirty-mesh pipeline.
GPU terrain generation should reuse the same command-buffer/fence/readback
helper style once hybrid meshing proves the generic compute job pattern.

## Validation

- CPU and hybrid meshing must produce the same visible faces for the smoke-test
  chunks, including water seams and static-water-surface suppression.
- Hybrid mesh output may differ in quad merge ordering only if final vertices,
  indices, draw ranges, and rendered result remain equivalent.
- GPU terrain is not allowed to replace `NoiseTerrainGenerator` until sampled
  columns match the CPU generator for fixed seeds and a representative X/Z/Y
  range.
- All flags default false, preserving current behavior for existing users.
