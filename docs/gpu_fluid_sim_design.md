# GPU Fluid Simulation — Design

## Goal

Move the per-tick fluid simulation work (~4.5 ms p99 today, all on main thread) to
a Vulkan compute shader. Keep the existing mesher / block-data pipeline unchanged;
the GPU sim writes a *change events* list that the CPU consumes each frame and
applies via the same `setBlockSilently` + `markMeshDirtyNoRevision` paths the CPU
sim uses today.

## Constraints we must preserve

1. **Block data stays CPU-authoritative**. Workers and the mesher pipeline read
   `Chunk::blockData()` on every chunk build. We don't want to rewrite the
   mesher.
2. **Player edits remain immediate**. Breaking a block adjacent to water must
   show flow next tick, not 2-3 frames later. The CPU→GPU upload of edits has
   to round-trip in one frame.
3. **Falling-only semantics first**. The current sim is `setBlockSilently` of
   water into the air cell below. Horizontal spread is a later phase. Don't
   over-engineer the shader for rules we don't have yet.
4. **No allocation-per-edit on hot path**. Player edits push to a queued upload
   batch; we don't reallocate the GPU buffer every tick.

## High-level architecture

```
                       ┌────────────────────────┐
                       │  CPU main thread       │
                       │                        │
   player edit ───────▶│  ChunkFluidEditQueue   │──upload──┐
                       │  (push fluid edits)    │          │
                       └────────────────────────┘          │
                                                            ▼
                       ┌────────────────────────────────────────┐
                       │   GPU — fluid storage buffer pool      │
                       │                                        │
                       │   [slot 0]  [slot 1]  [slot 2]  ...    │
                       │   32 KB     32 KB     32 KB            │
                       │   each slot = one chunk's fluid state  │
                       └────────────────────────────────────────┘
                                            │
                                            ▼
                       ┌────────────────────────────────────────┐
                       │   Compute shader: fluid_sim.comp       │
                       │                                        │
                       │   - Local workgroup = 8×8×8 = 512      │
                       │   - Dispatched 4×4×4 per chunk slot    │
                       │   - Reads slot + 6 neighbour slots     │
                       │   - Writes new state to back-buffer    │
                       │   - Appends to "events" SSBO           │
                       └────────────────────────────────────────┘
                                            │
                                            ▼
                       ┌────────────────────────┐
                       │  CPU readback          │
                       │  events list →         │
                       │  setBlockSilently +    │
                       │  markMeshDirty         │
                       └────────────────────────┘
```

## Data layout

### Slot pool (GPU storage buffer)

```cpp
// Sized for ~100 simultaneously-active fluid chunks. Slots are 32 KB each
// (32^3 bytes — one byte per cell, same packing as ChunkFluidData).
constexpr uint32_t kFluidSlotCount = 128;
constexpr uint32_t kFluidSlotBytes = 32 * 32 * 32;  // 32 KB
constexpr uint32_t kFluidPoolBytes = kFluidSlotCount * kFluidSlotBytes;  // 4 MB

// One storage buffer, slots accessed by base offset = slotIndex * kFluidSlotBytes.
struct FluidSlotPool {
    VkBuffer        buffer;       // host-visible coherent OR device-local + staging
    VmaAllocation   allocation;
    uint32_t        slotsAllocated;
};
```

Each slot is laid out as:

```
[byte 0 ... byte (32^3 - 1)]    // cell bytes, same packing as ChunkFluidData
```

The cell byte packing matches `ChunkFluidData`:
- bits 0..3 = level
- bit 4    = falling
- bit 5    = oceanLocked

### Slot directory (CPU + GPU)

```cpp
// CPU side: ChunkCoord -> slot index
std::unordered_map<ChunkCoord, uint32_t, ChunkCoordHash> slotForChunk_;

// CPU side: free list of available slots
std::vector<uint32_t> freeSlots_;
```

The GPU shader needs to find neighbour slots. Two options:
- **Option A (simpler)**: dispatch *per chunk* and pass neighbour slot indices as
  push constants (24 bytes for 6 neighbours).
- **Option B (more general)**: maintain a GPU-side directory buffer that maps
  ChunkCoord → slot, but ChunkCoord is 24 bytes which makes for a clunky GPU
  hash structure.

**Decision: Option A.** One dispatch per chunk slot per tick, with push constants
carrying the 6 neighbour slot indices (or `0xFFFFFFFF` sentinel if no neighbour).

### Block-state slot (GPU storage buffer)

The shader needs to know "is this cell air?" to know if water can fall into it.
We don't need the full block palette — just air/non-air, or air/water/solid (2
bits per cell).

```cpp
// Tightly packed: 4 cells per byte = 8192 bytes per chunk slot.
constexpr uint32_t kBlockBitsSlotBytes = (32 * 32 * 32 * 2) / 8;  // 8 KB
```

The CPU maintains a parallel block-bits buffer for each active fluid chunk, kept
in sync when chunks are edited or generated. This is ~1 MB total for 128 slots.

**Encoding per cell (2 bits)**:
- `00` = air
- `01` = water
- `10` = solid (anything else)
- `11` = reserved

### Events buffer (GPU → CPU)

The shader writes a list of changes for the CPU to apply:

```cpp
struct FluidEventEntry {
    uint32_t packedCellAndType;  // [type:4][chunkSlot:12][localIndex:16]
    uint32_t newCellByte;        // the new cell value to apply
};

struct FluidEventBuffer {
    uint32_t          count;          // atomic counter, written by shader
    uint32_t          pad[3];
    FluidEventEntry   entries[16384]; // bounded; overflow → CPU re-runs next tick
};
```

Event types:
- `0` = drained (cell became air)
- `1` = carved-falling (water added to cell)
- `2` = level-changed (existing water changed level)

CPU reads `count`, iterates `count` entries, applies via existing block edit paths.

## Per-frame sequence

```
=== Main thread, top of tick (after streaming, before sim) ===

1. CPU collects all chunks with fluid + neighbours.
   - For each: if not already slot-resident, allocate slot, upload current
     ChunkFluidData + block bits.
2. CPU uploads any player edits since last tick (small per-cell writes).
3. CPU records: vkCmdFillBuffer (clear event count to 0).
4. For each active fluid chunk: cmdDispatch(fluid_sim.comp, 4, 4, 4) with push
   constants for the 6 neighbour slots.
5. cmdPipelineBarrier (shader-write → host-read for event buffer).
6. cmdCopyBuffer(event buffer → host-visible readback buffer).
7. Submit & wait (or use fence with one-frame delay).

=== Next tick (or end of current tick if not pipelined) ===

8. CPU reads back event buffer.
9. For each event: apply via setBlockSilently / markMeshDirtyNoRevision.
10. (Optional) Pipeline: most events apply 1 frame late — acceptable for water.
```

## Sync model: one-frame pipelined

Naïve "dispatch then wait" stalls the main thread on the GPU.

Better: **one-frame pipelined readback**. Tick N's compute results are read back
on tick N+1. Cost: water flow appears 1 frame later (1/60 sec) — invisible.

```cpp
struct FluidFrameContext {
    VkBuffer        eventBuffer;      // GPU storage
    VkBuffer        eventReadback;    // host-visible, populated by cmdCopyBuffer
    VkFence         fence;            // signaled when frame's compute is done
};
std::array<FluidFrameContext, 2> frames_;  // double-buffered
```

## CPU↔GPU sync points

1. **Edit upload (CPU → GPU)**: every frame the CPU may upload changes from
   player edits or BFS unlocking. Batched into a single `vkCmdUpdateBuffer` or
   transfer-from-staging op at the start of the compute pass.
2. **Event readback (GPU → CPU)**: one frame late, via fenced host-visible
   buffer. CPU walks the event list and applies via existing paths.
3. **Slot allocation / eviction**: when a chunk enters/leaves the fluid pool,
   CPU does a one-shot upload (slot init) or just frees the slot index.

## Feature flag

The new system is gated behind `ApplicationConfig::useGpuFluidSim` (defaults to
`false` until proven). Both code paths coexist for the next phase. CPU sim stays
in tree until the GPU path proves equivalent behaviour on the test suite.

## Out of scope (for this first port)

- **Horizontal flow** — current CPU sim is falling-only. Same for GPU sim.
- **Ocean BFS unlocking** — `activateOceanEdge` stays CPU-side (rare, big batches).
- **Save format changes** — fluid state at save time is read from `g_store`
  (CPU mirror) which we keep updated via the event-apply path.
- **Multi-frame coherent simulation** — each tick is independent; no carry-over
  state beyond what's in the slot pool.

## What we keep from the CPU sim

- `ChunkFluidData` byte packing (now also the GPU slot layout)
- `FluidDirtyQueue` — still drives "which chunks need simulating this tick"
- Player edit hooks (`wake`, `activateOceanEdge`) — they enqueue chunks for GPU
  dispatch instead of (or in addition to) the CPU queue
- The event-apply step uses `Chunk::setBlockSilently` + `markMeshDirtyNoRevision`
  unchanged

## Open questions

1. **What's the upper bound on simultaneously active fluid chunks?** If >128 we
   need a bigger pool or eviction policy. Need to measure.
2. **Coalescing edits within a frame**: if the player breaks two blocks in the
   same chunk, do we upload once or twice? Should be once (deduplicate by chunk).
3. **Determinism**: GPU compute with floats is hard to make bit-identical to CPU.
   Falling-water uses only integer logic, so this should be fine — but flag for
   verification.

## Validation plan

Before flipping the flag in production:

1. **Headless smoke test**: run both CPU and GPU sims on the same input cells
   for 100 ticks, assert identical state.
2. **In-game A/B**: feature flag toggle at runtime, eyeball water behaviour
   matches.
3. **Perf measurement**: confirm sim_ms < 0.5 ms p99 (target).
