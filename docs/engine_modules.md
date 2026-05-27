# Engine Module Map

## Core

Owns application boot, logging, event dispatch, jobs, ticks, profiling hooks, and stable primitive IDs.

Current implementation:
- Provides root-relative paths for assets and saves.
- Runs the sandbox loop until window close, with an optional `--frames N` automation limit.
- Tracks lightweight runtime counters for frame time, chunk request planning,
  async generation/meshing, stale mesh discards, lighting recomputes,
  neighbour remesh churn, GPU uploads, staging bytes, residency, cache size,
  and in-flight work.
- Tracks timing totals/averages/maxima for terrain generation, lighting
  propagation, mesh build, mesh snapshot creation, GPU upload, save/load, and
  worker queue wait.

Current implementation:
- `JobSystem` runs a real worker-thread pool with four priority queues
  (Critical/High/Medium/Low), `submit<Fn>` returning a `std::future`, and a
  re-entrant-safe `waitAll()` guarded against being called from a worker.
- Workers default to `max(1, hardware_concurrency()-1)`; configurable via `start(N)`.

Future work:
- Work-stealing across worker queues; profiler markers; developer console.
- Hot-reload notifications for assets and data registries.

## Platform

Owns OS window integration and renderer surface creation.

Current implementation:
- Uses GLFW behind `IWindow`.
- Creates a Vulkan-compatible window and surface.
- Provides framebuffer extent and required instance extensions to the renderer.
- Provides keyboard, mouse-button, cursor-position polling, and cursor capture
  for the sandbox prototype.
- Provides a `setTitle` hook used for lightweight hotbar/mode/performance feedback.

Future work:
- Add input events and cursor capture modes.
- Add resize/minimize handling.
- Add platform file dialogs and monitor/window mode settings.

## Data

Owns namespaced identifiers, registries, runtime ID assignment, and future data/mod loading.

Current implementation:
- Loads block, item, and recipe registries from JSON data files.
- Item definitions carry data-driven stack limits, tags, tool metadata, and
  optional block placement IDs.

Future work:
- Validate schemas before registration.
- Sort mods by dependencies.
- Preserve stable save/network remaps across versions.

## Inventory

Owns item stacks, slot rules, player storage layout, and shared container foundations for machines, vehicles, ships, and future UI.

Current implementation:
- `ItemStack` supports 32-bit stack counts so generous building stacks do not
  overflow backend storage.
- `Inventory` stores per-slot metadata: `SlotKind`, locked flag, and favorite
  flag. Generic/hotbar slots accept all items; fuel/equipment/accessory/spell
  slots validate by item tags.
- Inventory operations cover the first UI-required movement rules: merge a
  carried stack into a slot, place one item, split half, swap compatible slots,
  and quick-move a slot into another inventory without losing items.
- `PlayerInventory` now exposes a 12x10 backpack grid, 12-slot hotbar,
  equipment slots, armor slots, six accessory slots, and an offhand slot.
- Player inventory can serialize/deserialize to a versioned JSON debug format
  using item identifiers instead of runtime IDs.
- `PlayerInventorySaveService` persists that JSON under
  `<world>/player/inventory.json`, keeping player state separate from chunk
  region data.
- Core item data uses larger stack limits: common blocks and ores stack to 999,
  resources to 500, torches to 250, machines/automation parts to 64, and tools
  remain single-stack.

Future work:
- Wire player inventory save/load into application world startup/shutdown.
- Add sort/search/filter/favorite behavior in the UI layer.
- Add container-specific filters for machine inputs/outputs, armor,
  backpacks, spell slots, fuels, vehicles, and automation tooling.
- Build the actual inventory UI after the backend movement rules are stable.

## World

Owns integer coordinates, `32^3` chunk storage, dirty flags, chunk residency, and streaming request planning.

Current implementation:
- Plans chunk requests around an integer chunk center.
- `ChunkPipeline::processRequestsAsync` dispatches generation jobs to the
  JobSystem and drains completed chunks from `ChunkJobMailbox` on the main
  thread. Synchronous `processRequests` is retained for tests/headless paths.
- `NoiseTerrainGenerator` produces deterministic 2D world shape plus 3D caves
  and ore density from the hash-based value-noise utility in `core::Math`;
  `FlatTerrainGenerator` is still available.
- Phase G optimized `NoiseTerrainGenerator` around one surface-height sample
  per x/z column, common vertical range fills, coarse deterministic cave
  sampling, and ore noise only in valid stone/depth bands. Generated chunks do
  not become save-dirty.
- Terrain generation now performs chunk-level extremity checks so high air
  chunks skip cave/ore work entirely, and partially solid chunks sample only
  coarse cave layers that can intersect stone.
- `TerrainColumnPrepassCache` stores 32x32 world-shape metadata per horizontal
  chunk column: surface heights, continentalness, erosion, peaks/valleys,
  temperature, humidity, weirdness, ocean depth, biome ID, surface kind,
  beach/river-candidate masks, and sea masks. Entries are keyed by seed and
  terrain version. Full chunk generation can use cached column metadata while
  still producing the same deterministic output as the direct path.
- The first continent/ocean pass uses continentalness bands for deep ocean,
  ocean, continental shelf, beach/coast, and land. Land height combines detail
  noise, erosion roughness, and peaks/valleys mountain uplift; ocean floors are
  shaped separately from land terrain.
- The 2D world-shape signals are sampled on a coarse 4-block grid per
  horizontal chunk column, then bilinearly interpolated before the generator
  resolves surface kind, biome, and surface height. This avoids upsampling
  final block decisions while cutting repeated FBM calls in the prepass.
- Cave carving samples a coarse 3D density grid and trilinearly interpolates
  density per voxel before thresholding. Final air/solid/fluid decisions are
  still made after interpolation.
- Coal and iron ore generation use coarse 3D density grids as well. Before
  interpolating, the generator checks the 8 corners of the active coarse cell;
  if none can exceed the relevant threshold, it skips the voxel-level density
  calculation for that cave or ore layer.
- The sandbox schedules low-priority terrain prepass jobs for nearby horizontal
  columns. These jobs build metadata only and never touch `ChunkManager`, Vulkan,
  lighting, meshing, uploads, or saves.
- `Palette<T>` + `BitPackedArray` store palette indices with variable bit
  width (`bitsRequiredForPaletteSize` clamps to >= 1).
- `ChunkLightData` (sky/block in low/high nibble) + `LightPropagator` do
  sky and block-light flood-fill with neighbour boundary seeds on the main thread.
- `StreamingSettings` separates horizontal render distance from vertical render distance.
- Streaming request priority includes nearest-first ordering, Y-slice weighting,
  and a small camera-forward bias that cannot outrank nearby chunks.
- Static water can pin a small sea-level vertical band through
  `pinnedVerticalChunkY`/`pinnedVerticalChunkRadius`, keeping ocean surface and
  near-underwater chunks in the high-priority stream set during high-altitude
  movement.
- `BlockEditQueue` batches main-thread break/place requests before they hit
  `BlockEditor`, allowing edit acceptance/rejection and dirty coalescing stats.
- Border-neighbour edits mark mesh/lighting dirty without bumping save revision
  or save dirtiness in chunks whose blocks did not change.

Future work:
- Sun direction-aware diagonal sky.
- Fluid sidecars and block entity storage.
- Split simulation distance, physics distance, and network interest from render residency.
- Route all block mutations through `WorldDeltaBatch`.

## Render

Owns the Vulkan-first renderer boundary, frame graph, chunk mesh contracts, and eventual GPU-driven draw flow.

Current implementation:
- Emits greedy-merged opaque voxel quads for non-air blocks.
- Packs local vertex position, face index, corner index, and material ID into the mesh contract.
- Stores CPU mesh draw ranges grouped by surface type and material ID.
- Uses a block render catalog for opaque, cutout, and transparent surface classification.
- Packs a simple face-shade value into vertex light data as the placeholder for AO/lighting.
- Creates a Vulkan instance, surface, physical/logical device, swapchain, render pass, framebuffers, command buffers, and frame sync.
- Compiles GLSL shaders through CMake.
- Uses a keyboard + mouse-look debug camera.
- Allocates renderer-owned buffers/images through Vulkan Memory Allocator using
  modern `VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE` and
  `VMA_MEMORY_USAGE_AUTO_PREFER_HOST` policies.
- Uploads generated chunk meshes to device-local GPU buffers via a
  `StagingArena`: two 16 MB slots (one per in-flight frame) with bump
  allocation per upload; the slot is reset after its frame fence signals.
  Eliminates per-upload VkBuffer creation/destruction.
- Retires runtime resources through frame/fence-ordered queues; shutdown
  teardown remains dependency ordered.
- Skips duplicate uploads for mesh revisions already resident on the GPU.
- Draws a selected-block debug outline through a tiny line-list debug pipeline
  owned by the renderer boundary.
- Supports removing uploaded meshes when CPU mesh-cache entries are evicted
  outside the active streaming radius.
- Draws flat terrain chunks with depth testing, clears, and presents the swapchain each frame.
- Camera can follow the first-person player pose or switch back to freecam/debug camera mode.
- Scene/cull entries store chunk draw origin separately from section bounds.
  Vertices stay chunk-local, while the two vertical mesh sections are culled as
  `32x16x32` boxes.
- GPU compute culling is the sandbox default. `--cpu-cull` forces the CPU
  indirect-command fill fallback, and `--gpu-cull-compare` keeps the CPU
  frustum count for delayed validation against the GPU-written indirect
  commands.
- GPU culling now compacts visible opaque/cutout/transparent section draws into
  fixed per-surface indirect ranges and uses `vkCmdDrawIndexedIndirectCount`
  with a GPU-written count buffer. The cull dispatch runs before the render
  pass, then a compute-to-indirect/vertex barrier makes the compact commands
  and origin buffer visible to graphics.
- Scene buffers sync incrementally per frame slot using per-entry generation
  stamps. Chunk upload/remesh/unload updates only touched entries; runtime
  stats report `scene_sync(entries/full)` so accidental full-table copies are
  visible during profiling.
- Runtime fragment lighting is evaluated in `voxel.frag` by default from face
  normal, material emission, height, and mesher face shade/AO. CPU-propagated
  `ChunkLightData` remains available behind the sandbox `--cpu-lighting` flag
  for comparison and regression coverage.
- A small renderer-owned 2D UI overlay path draws untextured rectangles for
  the crosshair, hotbar, selection highlight, and first-pass inventory panel.
  This keeps early visuals behind the renderer boundary without introducing a
  full UI framework yet.

Future work:
- Add swapchain recreation for resize/minimize.
- Add materials, texture atlas, upload batching, and robust chunk draw submission.
- Move meshing to worker jobs, then explore GPU meshing later.
- Add text rendering, icons, nine-slice panels, optional Hi-Z occlusion
  culling, atmosphere, water, particles, and richer debug overlays.

## Roadmap

Phase A - See something:
- Window loop: done.
- Vulkan clear color: done.
- Debug camera: keyboard controls and basic mouse look done.
- GPU mesh upload: device-local staged path done.
- Draw flat terrain chunks: done.
- Depth buffer: done.

Phase B - Make chunks usable:
- True greedy meshing: done for opaque full-cube faces.
- Material buckets: CPU draw ranges done.
- Face culling rules: basic solid/air plus non-occluding render catalog done.
- Transparency queue: CPU transparent ranges done, dedicated render pass/order still TODO.
- Basic lighting/AO: face-shade placeholder packed, real AO/light sampling still TODO.

Phase C - Make world interaction real:
- Raycast block selection: world DDA utility done.
- Place/break blocks: world edit service and sandbox mouse wiring done.
- Dirty chunk remesh: edit dirtiness and revision bumps done.
- Save modified chunks: dirty-save service done.
- Neighbor chunk border remesh: border neighbor dirty marking done.
- Player-overlap placement guard: block placement is rejected when the target voxel overlaps the player AABB.

Phase D - Automation prototype:
- Mechanical network graph: kinetic solver done.
- Gear-touch connection rule: face-touching kinetic blocks done.
- RPM/stress/capacity propagation: done.
- Overload failure: overload flagging done, physical block break effects TODO.
- Debug overlay: log-based snapshot done, on-screen overlay TODO.

Phase F - World scale + lighting polish:
- Cross-chunk lighting BFS via `LightPropagator` reading neighbour chunks; lightData persisted per chunk (`std::unique_ptr<ChunkLightData>`).
- Mesh seam culling: mesher takes a `ChunkNeighborhood` so boundary faces against solid neighbours are culled. `ChunkPipeline` marks neighbours mesh-dirty when a new chunk installs.
- GPU mesh uploads use device-preferred shared arenas fed by the renderer's
  per-frame staging arena. Debug one-shot uploads still use
  `VulkanRenderer::uploadViaStaging`.
- `StreamingSettings` split: `renderDistanceChunks` (X/Z) × `verticalRenderDistanceChunks` (Y). Sandbox accepts `--render-distance N` and `--vertical-distance N`.
- Terrain polish: `core:coal_ore`, `core:iron_ore`, `core:water`; cave threshold tuned; below-sea-level air is filled with water.
- Automation debug detail: `KineticNetworkDebug.representativeNode`, `KineticNodeDebug.networkId`, per-network log lines, on-demand `F` key dump.

Phase G - Runtime stability + perf instrumentation:
- `RuntimeStats` logs compact interval and shutdown summaries for chunk request planning, async generation/meshing, stale mesh discards, lighting recompute count, neighbour-install remesh count, GPU uploads, staging bytes, residency, mesh cache size, and in-flight work.
- Runtime timing stats include terrain generation, lighting propagation, mesh build, mesh snapshot creation, GPU upload, save/load, and queue wait average/max values.
- Terrain prepass stats include jobs submitted/completed, cache hits/misses/entries, prepass build timing, and generation-from-prepass vs direct timing.
- Mesh jobs are keyed by coord + save revision + render-only mesh revision/neighbour revision hash. This preserves the single-writer `ChunkManager` model while workers operate only on frozen snapshots.
- Stale mesh results are discarded before staging upload if the target chunk revision or neighbour revision hash has changed.
- Mesh cache currentness respects no-revision mesh dirtiness so neighbour installs can force seam remeshes without save revision churn.
- Vulkan mesh buffer replacement no longer calls `vkDeviceWaitIdle`; old vertex/index buffers are retired and destroyed after the frame fence makes them safe.
- Stream priority now favours nearest chunks and chunks close to the current Y slice before farther vertical layers.
- Tests cover dirty-flag no-revision behavior, duplicate mesh job suppression, and stale snapshot detection.

Phase I - Basic gameplay/performance polish:
- Terrain-aware player spawn resolving waits for resident startup terrain before
  enabling normal gravity.
- Window title feedback reports camera mode, noclip/grounded state, selected
  hotbar slot, frame time, resident chunks, and mesh cache entries.
- Block edits are queued and saves are debounced. Dirty saves flush on a frame
  interval and on shutdown, with a per-flush cap.
- Main-thread work budgets cap lighting propagation, mesh result installs, GPU
  upload bytes, and save flushes.
- Low-priority terrain prepass jobs throttle themselves when visible generation
  or mesh work is backed up.
- Mesh cache entries outside the active streaming radius are evicted and their
  GPU buffers are retired through the deferred deletion path.

Phase H - Playable first-person prototype:
- `PlayerController` owns position, velocity, yaw/pitch, gravity, jump, grounded state, noclip, and AABB collision against solid voxel blocks.
- `PlayerSpawnResolver` finds a safe terrain surface from resident chunks before
  the sandbox enables normal player movement.
- Sandbox camera follows the player by default; `C` toggles freecam, and `V` toggles player noclip/fly mode.
- Creative hotbar maps number keys to block state IDs for stone, dirt, grass,
  glass, water, ores, and early automation blocks. `-` and `=` select the
  11th and 12th slots. Selection is logged and
  mirrored in the window title and the rectangle-based hotbar overlay.
- The sandbox seeds the player inventory backend with creative hotbar item
  stacks and a small starter set of resources/automation parts so UI visuals
  are driven by actual inventory data rather than placeholder rectangles.
- Existing DDA raycast + `BlockEditor` remain the interaction path. Reach is capped and block edits still go through dirty/save/mesh/light flags.

Phase J - GPU-driven chunk rendering (in progress):
- J1: shared mesh arenas. `BufferArena` is a free-list sub-allocator over a
  single device-local VkBuffer; chunk vertex/index data lives in two big
  arenas (256 MB + 64 MB). Each chunk owns a `BufferArena::Slice` instead of
  its own buffer allocation. The draw loop now binds vertex/index
  arenas once per frame and uses `vertexOffset` / `firstIndex` per chunk.
- J2a: CPU frustum culling. `core::extractFrustumPlanes` extracts six
  Gribb–Hartmann planes from the view-projection; `core::aabbIntersectsFrustum`
  uses the positive-vertex test (one dot per plane). Chunks outside the
  frustum are skipped in the draw loop and counted in
  `RuntimeCounters.chunksDrawn` / `chunksCulled`.
- J2b: SSBO + `vkCmdDrawIndexedIndirect`. A new `voxel_chunk.vert` reads
  chunk origin from a host-visible storage buffer indexed by
  `gl_InstanceIndex`. The CPU writes one `VkDrawIndexedIndirectCommand` and
  one `vec4` origin per visible chunk per frame; the GPU then executes them
  with a single indirect draw call. Requires the `multiDrawIndirect` device
  feature. The debug-line pipeline still uses the original push-constant
  shader (`voxel.vert`).
- J3: GPU compute culling. A new `voxel_cull.comp` reads a per-frame
  *scene buffer* (one entry per uploaded chunk: origin, indexCount,
  firstIndex, vertexOffset), extracts the 6 view-projection planes inside
  the shader, runs the positive-vertex AABB test, and writes the
  `VkDrawIndexedIndirectCommand` array. Culled chunks get `instanceCount = 0`
  so the GPU skips them inside the single `vkCmdDrawIndexedIndirect`. The
  CPU now does **zero per-chunk work per frame** in the draw path; the scene
  buffer is rewritten only when the resident chunk set changes. Compute
  → draw synchronisation is handled by a single
  `vkCmdPipelineBarrier(COMPUTE_SHADER → DRAW_INDIRECT | VERTEX_SHADER)`.

- J3b: GPU indirect draw compaction. The cull shader atomically appends visible
  opaque, cutout, and transparent section commands into fixed ranges
  `[0, max)`, `[max, 2*max)`, and `[2*max, 3*max)`. A tiny count buffer drives
  `vkCmdDrawIndexedIndirectCount`, so draw execution follows the visible
  command count instead of the full scene section count. `--gpu-cull-compare`
  validates compact visible-section counts against the CPU frustum path.

Phase K - Multi-frame-in-flight pipelining:
- `VulkanRenderer::kFramesInFlight = 2`. Each frame slot owns its own
  command buffer, in-flight fence, image-available / render-finished
  semaphores, plus its own copies of every buffer the CPU writes per frame:
  indirect-command buffer, chunk-origin SSBO, and scene buffer. A shared
  descriptor pool allocates `kFramesInFlight` chunk-set + cull-set descriptor
  sets (each pointing at its frame's buffers). The cull pipeline,
  pipeline layouts, and descriptor set *layouts* remain shared.
- `beginFrame` waits only on the current frame slot's fence, allowing the
  CPU to queue the next frame's work while the GPU is still executing the
  previous one. `currentFrame_` rotates mod `kFramesInFlight` after each
  submit/present.
- The scene buffer is rebuilt **per frame, on demand**, but now uses
  incremental updates: `upsertSceneEntry` / `removeSceneEntry` modify a
  single entry in an authoritative array and bump a generation counter;
  rebuilds cheaply `memcpy` the current array. This replaces the previous
  O(n) full-recompute path.
- The biggest payoff is when CPU mesh/light/stream work takes longer than
  GPU draw — frame N's GPU pass overlaps frame N+1's CPU work, eliminating
  the previous `vkWaitForFences` stall at frame start.
- J4: per-surface batching — opaque and transparent each have their own
  indirect draw call with the correct render order. The compute cull shader
  writes two `VkDrawIndexedIndirectCommand` entries per chunk (first half =
  opaque, second half = transparent); `recordFrameCommands` issues two
  `vkCmdDrawIndexedIndirect` calls at different offsets into the same
  indirect buffer.


Phase L - Frametime and framerate stability:
- GPU culling runs before the render pass and writes compact indirect ranges
  plus compact origin entries. Opaque, cutout, and transparent sections each
  use an indirect-count draw with fixed range offsets in the same command
  buffer, preserving render order without CPU-side command compaction.
- `ApplicationConfig::ChunkWorkBudget` has millisecond budgets for lighting, mesh install, upload submission, and dirty queue processing. The world loop favours smooth frames over fastest chunk warmup.
- `ChunkDirtyQueue` owns deduped lighting/mesh queues keyed by chunk coord and revision metadata. The app feeds these queues from generation installs, block edits, lighting completion, neighbour dirtiness, and stale-result retries.
- Shader-side lighting is the default runtime path. `Application` suppresses
  lighting queue submission, builds meshes without `ChunkLightData`, and avoids
  lit-remesh churn while chunks stream in.
- CPU lighting remains available with `--cpu-lighting`. That path follows the
  worker snapshot/mailbox pattern: main-thread snapshots target + 6 neighbours,
  workers compute `ChunkLightData`, and the main thread validates
  `LightingJobKey` before install. Stale light results are counted and
  discarded without mutating live chunks.
- The CPU lighting path fast-paths uniform opaque chunks to all-dark light data
  and full-sky uniform transparent chunks to filled skylight where neighbour
  block light cannot enter. BFS frontiers use packed vector storage to reduce
  per-cell overhead.
- Mesh job dispatch is intentionally ordered before optional CPU lighting
  dispatch so chunks become visible without waiting for expensive light
  propagation. CPU lighting completion can still queue a no-save remesh.
- Streaming request lists are cached and refreshed only when the player changes
  chunk, streaming settings change, or the quantized forward direction refresh
  interval expires. The app also filters a small candidate list for
  `ChunkPipeline` so saturated generation queues do not rescan the full radius
  every frame.
- Generation result installation is capped by
  `ChunkPipelineSettings::maxGenerationInstallsPerTick`, smoothing worker
  completion bursts across frames while preserving the main-thread-owned
  `ChunkManager`.
- Completed generation results are drained nearest to the active streaming
  center first, so fast movement does not let far completed chunks delay
  visible nearby chunks.
- Spawn resolution scans the loaded chunk column directly and unresolved
  attempts are throttled, preventing missing-terrain startup from charging the
  player/input stage every frame. Player collision uses bounded binary-search
  resolution along each axis instead of incremental epsilon stepping.
- The default sandbox profile targets common 8-core / 16-thread machines by
  reserving CPU for the main/render/driver threads while using up to 12 worker
  threads for chunk work. Passing `--workers N` explicitly scales generation,
  mesh, install, and upload budgets from that worker count, while keeping
  `ChunkManager` installs and Vulkan ownership on the main thread. In-flight
  generation uses a lower multiplier than the worker count so long terrain
  jobs cannot saturate every core and starve the main/render thread.
- `--fast-streaming` opts into a higher-throughput profile for profiling and
  rapid flight. It increases generation/mesh in-flight caps, mesh install and
  upload budgets, and the stream-dispatch scan budget while keeping all live
  world mutation on the main thread.
- Streaming dispatch prioritizes nearby/visible requests every frame before
  advancing the far/backfill cursor, preventing distant backlog from consuming
  all generation slots while close holes are still missing.
- Sea-level streaming pins a narrow vertical water band as high-priority
  visual work. This reduces ocean surface/near-seafloor pop-in without changing
  the engine's `32^3` chunk storage, renderer chunk size, or save format.
- Streaming request planning caches the sorted radius and translates it across
  horizontal chunk-center movement when settings and forward bucket are stable,
  avoiding a full radius rebuild/sort during fast flight.
- Once dispatch finds no generation work and the player stays in the same
  chunk, streaming enters an idle-rescan mode for a few frames to avoid
  repeated resident-chunk scans in steady state.
- Existing chunk saves are loaded by chunk worker jobs via direct target-file
  reads. The main thread no longer performs synchronous chunk file IO in the
  stream stage for the normal sandbox path.
- Terrain generation batches requested vertical chunks for the same horizontal
  column into one worker job. The worker still returns ordinary `32^3`
  `GeneratedChunkResult` payloads, but `NoiseTerrainGenerator::generateColumn`
  reuses one `TerrainColumnPrepass` for the batch. `ChunkManager`, meshing,
  saving, and Vulkan upload remain main-thread/subchunk based.
- Completed generation installs are now limited by both count and milliseconds
  per frame. Deferred payloads are requeued into `ChunkJobMailbox` and remain
  in the generation in-flight set until installed, so smoothing the stream
  stage does not open duplicate generation submissions.
- Newly installed chunks enter the mesh dirty queue with explicit high
  priority, while neighbour-only remeshes get a lower boost. Existing queued
  entries keep the strongest priority when coalesced.
- Mesh dispatch guarantees a small minimum submission batch each tick,
  preventing dirty-queue sorting from starving meshing on high-core-count
  machines.
- Mesh dispatch also yields to generation backlog. While generated chunks are
  still in flight or waiting to install, mesh jobs stop using the critical
  worker lane, keep only very near meshes high priority, and push far/backfill
  meshes behind generation work.
- Generation, lighting, and mesh dispatch have simple in-flight caps so worker queues cannot grow without bound while streaming catches up.
- Mesh arena uploads are batched into the normal frame command buffer. Staging buffers are kept alive by the renderer and retired via frame fences; the draw command buffer records copy-before-draw ordering instead of waiting on a fence per buffer copy.
- Mesh result installation is also nearest-first, and lighting jobs run at
  medium priority so generation and meshing can win the worker pool during
  rapid travel.
- Vulkan Memory Allocator owns renderer buffer/image memory selection and
  suballocation; new paths use modern `VMA_MEMORY_USAGE_AUTO*` allocation hints.
- After GPU upload, CPU vertex/index data is freed immediately via swap-clear
  to halve per-chunk resident memory.
- Runtime stats include dirty queue lengths, lighting jobs submitted/completed/stale, upload batch counts/bytes, upload queue length, chunks made drawable, renderer fence wait timing, worker count, worker/mailbox backlog gauges, per-stage frame timing, and throttled slow-frame breakdowns. The sandbox CLI adds `--workers N`, `--fast-streaming`, `--slow-frame-ms N`, and `--space-far-plane N`.
- Slow-frame diagnostics split the main loop into streaming/chunk-pipeline, player/input/interaction, mesh install, mesh dispatch, lighting install/dispatch, save flushing, simulation, and render stages. This keeps interval logs compact while still identifying the exact subsystem responsible for frame spikes.
- The F3 ImGui profiler keeps two minutes of rolling history, tracks worst-frame
  and configurable spike records, exports CSV captures to
  `logs/perf_capture.csv`, and breaks streaming into request-plan,
  dispatch-scan, terrain-prepass scheduling, chunk-pipeline, and neighbor-enqueue timings.
  The F3 runtime settings window live-edits render/simulation/physics distance,
  the space far plane, worker-thread count, generation/mesh budgets, and GPU
  upload budget. Its 65% and 70% CPU buttons resize the worker pool from
  detected hardware concurrency while keeping Vulkan ownership on the main
  thread.
- Space Phase A owns the natural-space boundary: altitude state, gravity
  falloff, atmosphere fade, water cutoff, deterministic sectors, starfield,
  configurable far plane, and empty-space prepass skipping. Space Phase B has
  started with append-only asteroid mining blocks (`space_rock`,
  `rich_metal_ore`, `aether_crystal_ore`, `compressed_ice`) and feature-specific
  material distributions for metal, crystal, ice, comet, ring-debris, and plain
  asteroid clusters. Phase B resource recipes use the existing machine data
  path: mill rich metal, extract aether crystal, crush compressed ice, and press
  space alloy. The generator also emits rare planet-sector mini-systems:
  one planet descriptor, deterministic companion moons, and occasional ring
  debris. Planet and moon descriptors carry body class, gravity, atmosphere,
  landability, roughness, ocean/ice coverage, resource richness, and life-signal
  hints. Moons are chunk-generated as cratered/cavernous dead bodies with
  profile-driven roughness and class-biased resources. Landable planets now
  have a first-pass voxel body generator using body-class material profiles
  and deterministic surface bands; gas giants remain descriptor-only.

Phase E - Performance foundation:
- Real worker-thread `JobSystem` with four priority queues and `waitAll()`; re-entrant submit is supported.
- Async chunk pipeline: generation + meshing dispatch through `ChunkJobMailbox`. World/ChunkManager stays main-thread-owned; workers receive immutable snapshots and produce result payloads.
- Palette + `BitPackedArray` chunk storage replaces flat block vectors. Public `blockAt`/`setBlock` API is unchanged. `bitsRequiredForPaletteSize` clamps to >= 1 so single-entry palettes still have valid storage.
- Chunk block data lives behind `shared_ptr<ChunkBlockData>` for O(1) snapshot
  copies: worker snapshots share the backing storage; `ensureUniqueBlockData()`
  deep-copies on the first write after a snapshot, keeping per-worker memory
  overhead minimal.
- Deterministic value-noise terrain (`NoiseTerrainGenerator`) for hills + caves. `FlatTerrainGenerator` is still available for tests.
- Versioned binary save format (`VXC2`) carrying palette + bit-packed sections. `VOXEL_ENABLE_ZSTD` CMake option (default OFF) opts into Zstd compression by fetching `facebook/zstd v1.5.6`.
- Chunk-local sunlight + blocklight propagation via `LightPropagator` and `BlockLightCatalog`. Mesher samples baked light into `packedLight`; Phase F extends lighting across chunk borders.

## Save

Owns save-store contracts and future region-file chunk persistence.

Current implementation:
- `VXC2` versioned binary chunk file: magic, version, flags, coord, revision,
  body. Body contains the palette + bit-packed index buffer directly (no
  per-block enumeration on disk).
- Runtime save flushing snapshots dirty chunks on the main thread, clears their
  save flags, then schedules worker jobs to serialize/write immutable snapshots
  through a separate `RegionFileStore`.
- `RegionFileStore` indexes existing chunk filenames on startup and skips disk
  opens for chunks that have never been saved. This keeps fast streaming from
  spending main-thread time probing thousands of absent save files.
- Zstd compression is opt-in via the `VOXEL_ENABLE_ZSTD` CMake option; when
  enabled, the body is compressed and the `FlagZstdCompressed` body flag is
  set. The default build writes uncompressed bodies in the same format.
- Saves chunks marked dirty by world edits and clears only their save-dirty
  flag. Old `VCHK` saves are dropped — delete `saves/dev_world/` once after
  upgrading.

Future work:
- Region-file aggregation (one file per N chunks) instead of one-file-per-chunk.
- Async save compaction and retry/error reporting.
- Delta-based format for multiplayer replication.

## Automation

Owns shared graph foundations for kinetic, fluid, thermal, electrical, and mana networks.

Current implementation:
- Defines kinetic source, transfer, and consumer block components.
- Builds mechanical networks from face-touching kinetic blocks.
- Propagates network RPM from sources.
- Sums stress demand and capacity.
- Flags overloaded networks and overload-sensitive nodes.
- Emits a log-based automation debug snapshot from the sandbox loop.

Future work:
- Implement dirty connected-component rebuilds.
- Add gear ratio/axis compatibility, belts, clutches, visual rotation, and real block failure effects.
- Reuse graph topology for pipes, wires, heat, and mana conduits.

## Physics

Owns the physics facade and future simulation island model.

Future work:
- Integrate Jolt behind the current boundary.
- Generate voxel collision meshes or greedy collision boxes.
- Support ships, contraptions, character controllers, and local-frame walking.

## Magic

Owns future spell graphs, mana containers, and validated magic world commands.

Future work:
- Add spell registry and deterministic effect graph execution.
- Compile programmatic magic to sandboxed bytecode or graph commands.
- Emit world deltas rather than mutating chunk memory directly.

## Network

Owns future multiplayer boundaries.

Future work:
- Add authoritative server tick model.
- Replicate chunk deltas and entity transforms.
- Support prediction, reconciliation, and interest management.
