# AetherForge: Infinite Creation

Working title for the Vulkan-first, `32^3` chunk-based voxel game engine
Using Mostly AI coding, im not a coder, but i am learning things so its fun.

## Current Scope

- Clean module boundaries for core, data registries, world/chunks, rendering, meshing, save/load, automation, physics, magic, and networking.
- GLFW platform window with a Vulkan surface.
- Vulkan heartbeat renderer that uploads chunk meshes, draws flat terrain chunks, clears, and presents the swapchain.
- Integer world coordinates with camera-relative rendering planned.
- Palette-backed chunk storage with dirty flags and revision tracking.
- Data-driven item registry and inventory backend with a 12x10 backpack,
  12-slot hotbar, equipment slots, accessories, slot metadata, and generous
  stack limits for building-heavy play.
- Inventory movement rules cover stack merge, split-half, place-one,
  compatible swaps, quick movement between inventories, and versioned JSON
  persistence under `player/inventory.json` for future player save files.
- Vulkan renderer boundary and render graph placeholders.
- CPU greedy meshing boundary for the first voxel renderer milestone.
- Region save-store boundary for chunk persistence and future delta/network workflows.

Advanced gameplay is intentionally not implemented yet. TODO markers are left where future rendering, physics, automation, magic, and networking systems should attach.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\voxel_sandbox.exe
```

On single-config generators, the executable may be under `build/voxel_sandbox`.

The sandbox opens a window and runs until the window is closed. For automated smoke runs:

```powershell
.\build\Debug\voxel_sandbox.exe --frames 240
```

Dependencies:
- Vulkan SDK must be installed and discoverable by CMake.
- GLFW is fetched by CMake through `FetchContent` when configuring.
- Vulkan Memory Allocator is fetched by CMake through `FetchContent` and is
  used for renderer-owned buffers/images with `VMA_MEMORY_USAGE_AUTO*` policies.
- GLSL shaders in `assets/shaders` are compiled to SPIR-V under `build/shaders`.
- Zstd is optional. Pass `-DVOXEL_ENABLE_ZSTD=ON` to fetch `facebook/zstd v1.5.6` and compress chunk save bodies; the default (`OFF`) writes the same versioned binary format uncompressed.

## Migration note

Saves from Phase D and earlier (`VCHK` magic) are not compatible with the
Phase E `VXC2` palette + bit-packed format. **Delete `saves/dev_world/` once**
after upgrading; chunks will regenerate from the current world seed and be
saved in the new format on the next edit.

## Current Visual Prototype

Phase A is underway:
- Window loop: implemented.
- Vulkan clear color: implemented.
- Debug camera: implemented with keyboard movement and basic mouse look.
- GPU mesh upload: implemented with device-local buffers fed through staging.
- Draw flat terrain chunks: implemented.
- Depth buffer: implemented.

Player controls:
- `W/A/S/D`: move.
- Mouse movement: look around.
- `Space`: jump.
- `Left Shift`: fast movement.
- `C`: toggle freecam/debug camera.
- `V`: toggle player noclip/fly mode.
- `I`: toggle the first visual inventory overlay.
- Mouse capture is enabled by default for first-person testing.
- `Esc`: release the mouse cursor; click the sandbox window to capture it again.
- In noclip, `Q/E` move down/up.
- The player now waits for a resolved terrain spawn before normal gravity takes
  over, which avoids dropping through not-yet-resident startup chunks.

Freecam controls:
- `W/A/S/D`: move.
- `Q/E`: move down/up.
- Arrow keys or mouse movement: turn/pitch.
- `Left Shift`: fast movement.

Prototype block interaction:
- Left mouse: break selected block.
- Right mouse: place the selected block on the selected face.
- Placement is rejected when the target block overlaps the player collision box.
- Reach is limited to 6 blocks in player mode.
- The selected target block is shown with a lightweight renderer-owned debug
  outline.
- A renderer-owned 2D overlay draws the crosshair, 12-slot hotbar, selected
  slot highlight, and a first-pass inventory grid/panel.
- Creative prototype startup seeds the player inventory hotbar with the
  matching block-item stacks and adds a small set of resource/automation stacks
  to the backpack so the overlay reflects real inventory data.
- `1`: stone.
- `2`: dirt.
- `3`: grass.
- `4`: glass.
- `5`: water.
- `6`: coal ore.
- `7`: iron ore.
- `8`: creative motor.
- `9`: wooden gear.
- `0`: mechanical press.
- `-`: clutch.
- `=`: millstone.
- `F`: dump the full automation snapshot to the log (every kinetic network).
- The window title shows the selected hotbar block, camera mode, frame time,
  resident chunk count, and mesh cache size.
- Edits are queued through a small main-thread edit batch, mark chunks dirty,
  trigger remesh/relight work, and debounce saves instead of saving every click.

Still intentionally deferred:
- Text/icons/tooltips for the hotbar and inventory overlay.
- Inventory drag/drop controls, sorting, search, and application-level
  inventory save/load calls during world startup/shutdown.
- Materials and texture atlas.
- Chunk draw batching/indirect draws.
- Dedicated transparency pass, AO polish, and advanced interaction.

Phase B progress:
- True greedy opaque meshing: implemented.
- Material draw ranges: implemented on CPU mesh data.
- Block render catalog for opaque/cutout/transparent classification: implemented.
- Basic solid/air and non-occluding face culling: implemented.
- Transparent mesh ranges: implemented on CPU mesh data; dedicated transparency render order/pass is TODO.
- Basic lighting/AO: face-shade placeholder packed; real AO/light sampling is TODO.

Phase C foundation progress:
- Voxel raycast utility for block selection.
- Block editor for place/break deltas.
- Dirty chunk revision bumps for remesh.
- Border neighbor remesh marking.
- Dirty chunk save service.
- Sandbox mouse break/place wiring.

Phase D automation prototype:
- Kinetic block metadata for source/transfer/consumer blocks.
- Gear-touch/facing-adjacent connection rule.
- RPM/stress/capacity propagation.
- Overload detection and failure flagging.
- Log-based automation debug snapshot.

Phase E performance foundation:
- Real worker-thread JobSystem with four priority queues and `waitAll()`.
- Async chunk generation + meshing through a snapshot-and-mailbox pattern; Vulkan upload remains main-thread only.
- Palette + bit-packed chunk storage (`Palette<T>` + `BitPackedArray`) replacing the flat block vector; an all-air chunk is now ~4 KB.
- Deterministic value-noise terrain (`NoiseTerrainGenerator`) with hills and 3D caves; `FlatTerrainGenerator` retained for tests.
- Versioned binary save format (`VXC2`) carrying palette + bit-packed indices; Zstd compression opt-in via `-DVOXEL_ENABLE_ZSTD=ON` (CMake fetches `facebook/zstd v1.5.6`). Old Phase D `VCHK` saves are dropped — delete `saves/dev_world/` after upgrading.
- Chunk-local sunlight + blocklight propagation with intra-chunk BFS, plus a basic occluder-aware sample baked into the mesher's `packedLight`; Phase F extends this across chunk borders.

Phase F world scale + lighting polish:
- Cross-chunk lighting BFS. `ChunkLightData` is persisted per chunk; the main-thread `LightPropagator` reads neighbour chunks' baked light as boundary seeds and cascades dirty flags when borders change. Snapshots into mesh workers carry the chunk's frozen lightData.
- Mesh seam culling. The mesher now consumes a `ChunkNeighborhood` (the target chunk plus its 6 face neighbours). Boundary faces against solid neighbours are culled; when a neighbour first arrives, the existing chunk is automatically re-meshed.
- GPU upload via staging. Chunk meshes live in device-preferred memory and are
  fed through the shared per-frame staging arena.
- Render distance separates horizontal from vertical (`renderDistanceChunks` × `verticalRenderDistanceChunks`). The sandbox accepts `--render-distance N` and `--vertical-distance N`. Stress-tested at h=16 / v=2 (5445 chunks).
- Noise terrain polish: `core:coal_ore`, `core:iron_ore`, and `core:water` blocks; cave threshold tuned; below sea-level air is now filled with water.
- Automation debug visualisation: per-network detail in the periodic log (network id, source/consumer counts, RPM, stress%, representative coord). Press **F** in the sandbox for an on-demand full snapshot dump.

Phase G runtime stability + instrumentation:
- Runtime stats print compact interval summaries and a shutdown summary: frame time, planned requests, generation/mesh submitted/completed, stale mesh discards, lighting recomputes, neighbour-install remeshes, GPU uploads, staging bytes, resident chunks, mesh cache entries, and in-flight job counts.
- Timed stats now include terrain generation, lighting propagation, mesh build, mesh snapshot creation, GPU upload, save/load, and worker queue wait average/max timings.
- Mesh work is keyed by target coord + save revision + render-only mesh revision/neighbour revision hash. Workers still consume immutable chunk snapshots; stale results are discarded before GPU upload.
- Mesh dirty and lighting dirty no-revision paths are tested so neighbour installs and lighting cascades do not create save churn, while block edits still bump revisions and save dirtiness.
- Vulkan mesh buffer replacement now uses a small deferred deletion queue retired by frame index/fence instead of stalling the whole device per replacement.
- Upload guardrails skip identical mesh revision uploads and report total uploaded mesh bytes at shutdown.
- Debug camera now has basic mouse look for windowed testing.
- Noise terrain generation now uses column range fills for common grass/dirt/stone/water spans, coarse deterministic cave sampling, and narrower ore/cave checks. Generated chunks are mesh/lighting dirty, but not save-dirty.
- Terrain generation skips cave/ore work for chunks proven to be entirely above
  terrain and sea level, and limits coarse cave sampling to vertical layers that
  can actually contain carved stone.
- Stream priority favours near horizontal chunks and chunks close to the camera Y plane before farther vertical layers.
- Terrain column prepass cache stores 32x32 world-shape metadata per horizontal
  chunk column keyed by seed + terrain version: continentalness, erosion,
  peaks/valleys, temperature, humidity, weirdness, ocean depth, biome ID,
  surface kind, beach mask, river-candidate mask, and sea mask. Visible
  generation uses cached columns when available and falls back to direct
  deterministic generation on misses.
- Noise terrain now separates 2D world shape from 3D underground detail. The
  prepass decides continents, deep oceans, shelves, beaches, roughness, climate,
  and biome; the existing 3D layer still handles caves and ore density.
- World-shape prepass signals are sampled on a coarse 4-block grid and
  bilinearly interpolated before surface/biome decisions. This keeps decisions
  deterministic while avoiding full FBM evaluation for every x/z block column.
- Cave generation samples a coarse 3D density grid and trilinearly
  interpolates density before thresholding. It does not upsample final
  air/solid decisions.
- Coal and iron ore use the same coarse-density/interpolate-then-threshold
  approach, with cheap coarse-cell rejection so cells that cannot cross an ore
  or cave threshold skip per-voxel interpolation.
- Low-priority terrain prepass jobs build column metadata only; they do not create chunks, mutate `ChunkManager`, light, mesh, upload, or save.
- Runtime stats now include terrain prepass submitted/completed jobs, cache hits/misses/entries, prepass build avg/max, and generation-from-prepass vs direct avg/max timings.

Phase I gameplay/performance polish:
- Terrain-aware spawn resolving keeps the player frozen until the startup
  terrain column is resident and a safe standing surface is found.
- Mouse sensitivity and player walk/fly speeds are configurable through
  `ApplicationConfig`.
- Block edits flow through a main-thread edit queue. Dirty mesh/light/save
  work is coalesced for stats, and chunk saves flush on an interval and at
  shutdown.
- Streaming priority now includes a small forward-camera bias while keeping
  nearest chunks dominant.
- Mesh result installation and GPU uploads have per-frame budgets. Empty
  meshes and duplicate revisions are skipped, far mesh cache entries are
  evicted, and retired GPU buffers still use the deferred deletion queue.
- Runtime stats include accepted/rejected edits, dirty coalescing, save queue
  length, save flushes, upload deferrals, and frame-budget saturation hints.
- Save loading indexes existing chunk files and avoids repeated absent-file
  probes while streaming newly generated terrain.

Phase L frametime stability:
- Lighting and mesh work now flow through explicit deduped dirty queues instead
  of full resident-chunk scans. Queue processing is nearest-first and guarded by
  millisecond budgets plus in-flight caps.
- Chunk warmup is tuned for faster visible terrain while moving: generation
  dispatch/install, mesh dispatch/install, and upload budgets are larger by
  default, completed generation and mesh results install nearest to the player
  first. Runtime rendering uses shader-side lighting by default so chunk
  visibility no longer waits for propagated CPU light data.
- CPU lighting propagation is retained for validation and comparison behind
  `--cpu-lighting`. In that mode it runs on worker-owned frozen snapshots,
  mirroring the mesh worker model; the main thread validates revision/hash keys
  before installing light data and discards stale results without mutating live
  chunks.
- Chunk generation dispatch has an in-flight cap so streaming cannot bury the
  worker pool while the player moves.
- Terrain generation dispatch now batches requested vertical slices of the
  same X/Z column into one worker job. Storage, meshing, saving, and rendering
  remain `32^3` chunks, but `NoiseTerrainGenerator` can reuse one horizontal
  column prepass across the batch. This reduces job overhead and repeated
  prepass work without changing `ChunkManager` ownership.
- Mesh arena uploads are batched into the renderer's normal frame command
  buffer. Chunk upload calls copy into the current frame's staging slot and
  return; the frame submit performs copy-before-draw ordering and the staging
  slot is reused only after its frame fence signals.
- Renderer buffer/image allocations now go through Vulkan Memory Allocator
  using modern `VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE` /
  `VMA_MEMORY_USAGE_AUTO_PREFER_HOST` selection. Runtime resource retirement
  remains frame/fence ordered; shutdown still tears down Vulkan objects in
  dependency order.
- GPU/CPU chunk-section culling now keeps draw origin separate from cull
  bounds. Vertices remain chunk-local while each vertical section culls against
  its own `32x16x32` bounds.
- GPU compute culling is now the sandbox default. `--cpu-cull` forces the CPU
  indirect-command fill fallback, and `--gpu-cull-compare` also computes the
  CPU frustum count and reports delayed GPU/CPU mismatch counters.
- GPU culling now compacts visible opaque/cutout/transparent section draws into
  per-frame indirect command ranges and executes them through
  `vkCmdDrawIndexedIndirectCount`, avoiding full-scene draw-command walks after
  the cull shader runs. Runtime logs report `gpu_draw_cmds`.
- Scene-buffer synchronization is incremental per frame slot: chunk
  upload/remesh/unload stamps only changed scene entries, and runtime logs
  report `scene_sync(entries/full)` so full-table copies are visible.
- Fragment lighting evaluation is shader-side by default: face normal, material
  emission, height, and mesher face shade/AO are combined in `voxel.frag`.
  This removes runtime light propagation and lit-remesh churn from the normal
  streaming path.
- `--cpu-lighting` restores propagated `ChunkLightData` for profiling and
  regression tests. That path still fast-paths uniform opaque chunks and simple
  uniform transparent full-sky chunks, and uses packed vector frontiers instead
  of tuple queues for BFS propagation.
- Mesh dispatch no longer waits for lighting in the default shader-lighting
  path. CPU-lighting mode may still remesh chunks when propagated light data
  finishes.
- Dirty chunk saves are now worker-backed: the main thread snapshots dirty
  chunks and clears their save flag, while worker jobs serialize/write immutable
  chunk copies with their own save-store instance.
- The default sandbox profile targets common 8-core / 16-thread systems by
  reserving CPU for the main/render/driver threads while using up to 12 worker
  threads for chunk work. Passing `--workers N` scales generation, mesh,
  install, and upload budgets from that worker count. In-flight generation is
  intentionally capped below the raw worker queue capacity so long Debug
  terrain jobs do not monopolize CPU time during fast movement.
- `--fast-streaming` switches the sandbox to a higher-throughput profile for
  flying/profiling runs: it raises worker-backed generation/mesh in-flight
  caps, mesh install/upload budgets, and the stream-dispatch scan budget. Use
  it with `--workers N` when you want chunk catch-up speed over the smoothest
  possible frame pacing.
- Streaming dispatch gives nearby/visible requests a fresh first pass every
  frame before advancing the rolling far/backfill cursor, so distant backlog
  cannot occupy all generation slots while close chunks are still missing.
- Static-ocean streaming now pins a small vertical sea-level band, not just one
  chunk layer. The band is prioritized by X/Z distance so water surfaces and
  the first underwater slab appear earlier during high-speed flight while the
  engine still stores/renders ordinary `32^3` chunks.
- Mesh dispatch also guarantees a small minimum batch each tick so dirty-queue
  sorting cannot starve visible terrain meshing.
- Mesh dispatch is generation-backlog aware: when generation jobs or completed
  generation installs are pending, only a smaller near-mesh lane stays high
  priority while far/backfill mesh jobs yield to chunk generation.
- Streaming request planning is cached per chunk/settings/quantized forward
  direction. Horizontal chunk-center movement reuses the sorted request list by
  translating coordinates instead of rebuilding it, and generation result
  installation is count- and time-budgeted per frame. Deferred generation
  payloads stay in the mailbox and remain marked in-flight until installed,
  preventing duplicate jobs while avoiding large stream-stage bursts.
- When streaming has caught up and the player remains in the same chunk, the
  dispatch path enters a short idle-rescan mode instead of checking the resident
  request set every frame.
- Chunk loading from existing save files now happens inside chunk worker jobs
  through direct per-file reads. The main thread only installs completed chunk
  payloads, so saved terrain does not block the streaming stage with synchronous
  disk IO during fast movement.
- Unresolved spawn probing now scans loaded vertical chunks instead of every
  world-Y block and is throttled while waiting for terrain. Player collision
  resolution uses a bounded binary search instead of tiny incremental backoff.
- Runtime stats now include lighting job submitted/completed/stale counts,
  dirty queue lengths, upload batch counts/bytes, chunks made drawable, renderer
  fence wait timing, GPU cull stats, scene-sync counts, active worker count,
  backlog gauges, per-frame stage timing, and throttled slow-frame breakdowns.
  The sandbox accepts `--workers N` for CPU tuning, `--slow-frame-ms N` to
  change the slow-frame log threshold, `--fast-streaming` for higher chunk
  throughput, `--space-far-plane N` to tune space visibility distance, and
  `--cpu-cull` / `--gpu-cull-compare` to validate GPU-driven culling.
- The F3 ImGui profiler keeps a two-minute rolling history, records spike rows
  above a configurable threshold, splits stream timing into plan/dispatch/
  prepass/pipeline/enqueue sub-stages, and can write a CSV capture to
  `logs/perf_capture.csv` for longer movement/loading tests. The same F3 view
  exposes live runtime settings for render/simulation/physics distance, space
  far plane, worker-thread count, generation/mesh budgets, and GPU upload
  budget; the 65% and 70% CPU buttons resize the worker pool using detected
  hardware concurrency.
- Revised Space Phase A is wired as the natural-space layer: altitude changes
  gravity/atmosphere state, space shows a HUD/starfield, water is capped below
  the atmosphere, space chunks skip planetary prepass work, and far visibility
  is handled by the camera far plane instead of streaming more chunks. Space
  Phase B has started with mineable voxel asteroids using dedicated resource
  blocks: `core:space_rock`, `core:rich_metal_ore`,
  `core:aether_crystal_ore`, and `core:compressed_ice`. Existing machine
  recipes can process those resources into common metals, `core:aether_crystal`,
  `core:cryo_ice`, and `core:space_alloy_ingot`. The space feature generator
  also emits rare planet-sector mini-systems with one planet descriptor,
  companion moons, and occasional ring debris. Moons are voxel-generated as
  cratered dead bodies with cave pockets and class-biased resources; planets
  are deterministic descriptors with body class, gravity, atmosphere,
  landability, roughness, ocean/ice coverage, resource richness, and life-signal
  hints. Landable planets now generate first-pass voxel world bodies from those
  profiles, including deterministic surface bands for polar caps, dry ocean
  basins, volcanic fields, crystal crust, and life-bearing regions; gas giants
  remain descriptor-only.

## Validate

The full gate (configure, build, tests, windowed sandbox run):

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\voxel_sandbox.exe --frames 240
```

Expect: 100% tests passing. The sandbox prints periodic `Runtime stats` lines
and a shutdown summary with resident chunks, mesh cache entries, in-flight
generation/mesh counts, save queue activity, budget saturation hints, and total
uploaded mesh MB. Exact chunk counts depend on `--render-distance` and
`--vertical-distance`; use `--workers N --fast-streaming` to tune worker
pressure during local profiling. When a frame exceeds the slow-frame threshold, the sandbox prints a
`Slow frame` line with stage timings for streaming, player/input, mesh install,
mesh dispatch, lighting, save, simulation, and render work.

Useful streaming stress checks:

```powershell
.\build\Debug\voxel_sandbox.exe --frames 600
.\build\Debug\voxel_sandbox.exe --frames 2000 --render-distance 16 --vertical-distance 2
.\build\Debug\voxel_sandbox.exe --frames 2000 --render-distance 16 --vertical-distance 2 --workers 16 --fast-streaming
```

To verify the Zstd opt-in path (requires network access to fetch `facebook/zstd`):

```powershell
cmake -S . -B build-zstd -DVOXEL_ENABLE_ZSTD=ON
cmake --build build-zstd --config Debug
ctest --test-dir build-zstd -C Debug --output-on-failure
```

The same tests run; saved chunk bodies are Zstd-compressed and the shutdown
log reports `zstd=on`.
