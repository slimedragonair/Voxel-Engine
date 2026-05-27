# Advanced Voxel Engine Architecture

## Game Identity Target

**Genre/aesthetic:** block-based fantasy + technology + adventure + action + automation.

**Core fantasy:** a world where the player can mine, build, automate, sail, fly, leave the atmosphere, land on planets, discover magic, then merge magic with machinery into magitech contraptions, ships, factories, and weapons.

**Technical north star:** large-scale voxel worlds with high render distance, fast traversal, planetary coordinate consistency, modding-first data systems, multiplayer-friendly saves, and robust but practical development tooling.

---

# 1. Architecture Overview

Use a **data-oriented modular engine** with a strict separation between simulation, rendering, streaming, data/modding, and editor tooling.

```text
Game Layer
 ├─ Gameplay Systems
 │   ├─ Survival / Combat / Items / Progression
 │   ├─ Magic / Magitech
 │   ├─ Automation / Contraptions
 │   ├─ Ships / Vehicles / Spacecraft
 │   └─ Weather / Biomes / World Rules
 │
Engine Core
 ├─ ECS / Object Registry
 ├─ Job System
 ├─ Event Bus
 ├─ Asset Manager
 ├─ Data Registry
 ├─ Networking Layer
 ├─ Save / Compression Layer
 └─ Debug / Profiler / Console
 │
World Layer
 ├─ Planet / Region / Chunk Streaming
 ├─ Terrain Generation
 ├─ Structure Generation
 ├─ Voxel Storage
 ├─ Block State System
 ├─ Lighting Data
 ├─ Fluid Data
 └─ World Transition System
 │
Simulation Layer
 ├─ Physics
 ├─ Water Simulation
 ├─ Contraption Solver
 ├─ Ship Solver
 ├─ Entity Simulation
 ├─ Particles / Debris
 └─ Multiplayer Prediction / Reconciliation
 │
Rendering Layer
 ├─ Vulkan Render Graph
 ├─ Chunk Meshing
 ├─ GPU Culling
 ├─ Indirect Draw System
 ├─ Lighting / Atmosphere
 ├─ Water Rendering
 ├─ Particle Rendering
 ├─ UI Renderer
 └─ Debug Visualization
```

The engine should not be built as one giant system. It should be built as many specialized pipelines connected by stable data contracts.

---

# 2. Recommended Core Stack

## Language

**Best practical choice:** C++20 or C++23.

Reasons:
- Best Vulkan ecosystem.
- Easy to integrate physics libraries.
- Good control over memory and threading.
- Good long-term fit for engine development.

Alternative: Rust is safer but will slow early progress if the goal is fast custom-engine iteration.

## Rendering

- Vulkan-first renderer.
- Bindless-style resource model where supported.
- GPU-driven draw submission.
- Compute-based chunk meshing and culling.
- Render graph for frame organization.
- Async compute later for meshing, particles, lighting, or water.

## Physics

Use a proven rigid-body engine first, then add custom voxel/ship/contraption layers on top.

Recommended:
- **Jolt Physics** for rigid bodies, characters, collision, vehicles, and multithreaded simulation.
- Custom voxel collision generation for chunks and contraptions.
- Custom buoyancy/ship/spacecraft layer.

Do not write a full rigid-body engine from scratch at the start.

## Compression

Use:
- Zstd for chunk save compression.
- Optional custom palette + RLE + bit-packing before Zstd.
- Region-file layout similar in spirit to Minecraft Anvil, but designed for async streaming and multiplayer delta updates.

## Modding

Use a two-layer system:

1. **Data mods:** JSON/TOML/RON/YAML-like files for blocks, items, recipes, biomes, structures, loot, spells, UI skins, sounds, and progression.
2. **Code mods:** Lua, C#, or WASM sandbox for custom behavior.

Recommended early path:
- JSON data registry first.
- Lua scripting second.
- WASM or C# later if deeper modding is needed.

---

# 3. World Scale Model

The biggest mistake would be using one global `float3` position for everything. For planetary consistency, use a multi-layer coordinate system.

## Coordinate Types

```cpp
struct ChunkCoord {
    int64_t x;
    int64_t y;
    int64_t z;
};

struct BlockCoord {
    int32_t x;
    int32_t y;
    int32_t z;
};

struct RegionCoord {
    int64_t x;
    int64_t y;
    int64_t z;
};

struct PlanetCoord {
    uint64_t planet_id;
    int64_t region_x;
    int64_t region_y;
    int64_t region_z;
    int32_t chunk_x;
    int32_t chunk_y;
    int32_t chunk_z;
    int32_t block_x;
    int32_t block_y;
    int32_t block_z;
};

struct RenderPos {
    float x;
    float y;
    float z;
};
```

## Main Rule

The world uses integer coordinates. Rendering uses camera-relative floats.

```text
Natural World Position = Planet ID + Region + Chunk + Local Block
Render Position        = World Position - Floating Origin
Physics Position       = Local simulation island position
Network Position       = Quantized world-space or island-space transform
```

## Floating Origin

The player/camera stays near `(0,0,0)` in render space.

When the camera moves too far from origin:
- Shift the render origin.
- Rebase local physics islands.
- Keep true world coordinates unchanged.
- Never rewrite saved world coordinates because of render rebasing.

## Planetary Consistency

Use planet-local coordinate spaces:

```text
Universe
 └─ Star System
     └─ Planet
         └─ Region
             └─ Chunk
                 └─ Block
```

For early development, planets can be flat voxel worlds with sky/space transitions. Later, add spherical projection or cube-sphere planet sectors.

---

# 4. Chunk Model

You requested **32x32 chunks**. I recommend treating that as horizontal size, but choosing vertical strategy carefully.

## Option A: 32x32x32 Cubic Chunks

Best for:
- Space structures.
- Floating islands.
- Underground terrain.
- Ships and contraptions.
- Easy meshing.

Recommended for this engine.

```text
Chunk = 32 x 32 x 32 blocks = 32,768 blocks
```

Pros:
- Great for 3D streaming.
- Good for caves and space.
- Clean neighbor logic.

Cons:
- More chunk objects.
- Needs good streaming and batching.

## Option B: 32x32x256 Column Chunks

Best for Minecraft-like terrain.

Pros:
- Simpler surface terrain.

Cons:
- Worse for ships, space, floating structures, huge caves, vertical planets.

## Recommendation

Use **32³ chunk bricks** internally.
For terrain generation, group them into vertical columns or regions.

---

# 5. Voxel Storage

Each chunk should not store raw full block structs. Use palettes and compact arrays.

```cpp
struct Chunk {
    ChunkCoord coord;
    ChunkState state;
    Palette<BlockStateId> block_palette;
    BitPackedArray block_indices;
    OptionalLightData light;
    OptionalFluidData fluid;
    DirtyFlags dirty;
    uint64_t revision;
};
```

## Block Data Split

Separate block identity from block state.

```text
Block Type:     stone, oak_log, copper_pipe, mana_crystal
Block State:    orientation, powered, waterlogged, growth stage, heat, charge
Block Entity:   inventory, custom machine data, script state
```

Example:

```json
{
  "id": "core:copper_pipe",
  "display_name": "Copper Pipe",
  "solid": true,
  "opaque": false,
  "mesh": "pipe_connected",
  "states": {
    "north": [false, true],
    "south": [false, true],
    "east": [false, true],
    "west": [false, true],
    "up": [false, true],
    "down": [false, true]
  },
  "tags": ["pipe", "contraption_connectable", "wrench_rotatable"],
  "components": [
    { "type": "fluid_transport", "capacity": 1000 },
    { "type": "thermal_conductor", "conductivity": 0.55 }
  ]
}
```

---

# 6. Chunk Streaming for 128–256 Render Distance

A 256 chunk render distance with 32-block chunks means an enormous visible radius. Full simulation of that area is impossible. The solution is level-of-detail and separated simulation ranges.

## Distance Rings

```text
Ring 0: 0–8 chunks
 - Full blocks
 - Full entities
 - Physics active
 - Fluids active
 - Machines active

Ring 1: 8–32 chunks
 - Full mesh
 - Limited entities
 - No detailed physics except important objects
 - Reduced machine ticking

Ring 2: 32–96 chunks
 - Chunk mesh or simplified mesh
 - No regular simulation
 - Impostor structures
 - Cached lighting

Ring 3: 96–256 chunks
 - Terrain LOD mesh
 - Atmosphere/fog hides detail
 - No block-level data loaded unless required
```

## Key Principle

Render distance and simulation distance must be separate settings.

```text
Render distance:      128–256 chunks
Simulation distance:  8–24 chunks
Physics distance:     4–12 chunks
Network interest:     per-player dynamic radius
Save active range:    dirty chunks only
```

## Streaming Pipeline

```text
Player movement
 -> Predict needed chunks based on velocity
 -> Request chunk metadata
 -> Load/generate low LOD first
 -> Load full chunk if near
 -> Build mesh on worker/GPU
 -> Upload mesh buffer
 -> Register draw command
 -> Evict far chunk when memory budget exceeded
```

At high speed, the engine should stream in a cone ahead of movement, not just a sphere around the player.

```text
Priority = distance_score + camera_direction_score + velocity_prediction_score + gameplay_importance_score
```

---

# 7. Vulkan Renderer Architecture

## Renderer Goals

- GPU-driven rendering.
- Chunk visibility culling on GPU.
- Low CPU draw-call overhead.
- Fast chunk mesh updates.
- Support atmosphere, water, particles, voxel debris, UI, and debug tools.

## Frame Graph

```text
Frame Start
 ├─ Upload / staging flush
 ├─ GPU culling pass
 ├─ Depth prepass or visibility pass
 ├─ Main voxel opaque pass
 ├─ Contraption / entity pass
 ├─ Water pass
 ├─ Transparent / particles pass
 ├─ Lighting / volumetrics / atmosphere
 ├─ Post-processing
 ├─ UI pass
 └─ Debug overlays
```

## Chunk Rendering Strategy

Early version:
- CPU greedy meshing.
- One mesh per chunk or chunk section.
- Indirect draw batching.

Mid version:
- Worker-thread meshing.
- Persistent mapped staging buffers.
- GPU frustum/occlusion culling.
- Meshlet-like chunk clusters.

Advanced version:
- Compute or mesh-shader based meshing/culling where supported.
- GPU-driven indirect draw list.
- Chunk LODs.

## Mesh Types

```text
Opaque voxel mesh      -> greedy meshed quads
Cutout voxel mesh      -> leaves, grates, crystals
Transparent mesh       -> glass, water surface, magic fields
Animated block mesh    -> machines, gears, belts
Contraption mesh       -> moving block structures
Debris mesh            -> temporary voxel fragments
LOD terrain mesh       -> far terrain simplification
```

## Chunk Mesh Data

```cpp
struct VoxelVertex {
    uint32_t packed_pos;      // local x/y/z + face index
    uint32_t packed_uv;       // atlas tile + uv
    uint32_t packed_light;    // sunlight + block light + AO
    uint32_t packed_material; // block/material id
};
```

Use packed vertices aggressively. Voxel meshes are bandwidth-heavy, not math-heavy.

---

# 8. Lighting and Atmosphere

## Lighting Targets

You need two lighting systems:

1. **Voxel gameplay lighting** for caves, torches, machines, magic crystals.
2. **Visual lighting** for atmosphere, shadows, weather, planets, water reflections, and magic effects.

## Practical Lighting Stack

Early:
- Sunlight flood-fill per chunk/region.
- Block light flood-fill.
- Ambient occlusion baked into chunk mesh.
- Cascaded shadow maps for sun.
- Fog for distance hiding.

Mid:
- Clipmap-style global illumination approximation.
- Volumetric fog.
- Screen-space reflections for water.
- Local light clustering/tiled lighting.

Advanced:
- Sparse voxel lighting cache.
- DDGI/probe volumes for settlements/dungeons.
- Ray query effects for shadows/reflections if hardware allows.

## Atmosphere

Use a physically inspired but game-controlled atmosphere model.

```text
Planet altitude
 -> air density
 -> fog amount
 -> sky color
 -> sun scattering
 -> cloud density
 -> storm intensity
```

For the 10,000k-block atmosphere escape target, do not simulate full atmosphere at block resolution. Treat altitude as a continuous planetary parameter.

```text
0–2,000 blocks:       ground weather, fog, clouds
2,000–10,000 blocks:  thin atmosphere, high clouds, horizon haze
10,000+ blocks:       space rendering, stars, planetary curve/fake horizon
```

Note: “10,000k blocks” literally means 10,000,000 blocks. If you mean 10,000 blocks, that is much more practical. Architecturally, both work if altitude is coordinate-based and rendering is LOD-based.

---

# 9. Physics Architecture

## Physics Must Be Split Into Simulation Islands

Do not simulate the entire world as one physics scene.

```text
Physics Scene
 ├─ Local player island
 ├─ Ship island
 ├─ Contraption island
 ├─ Spacecraft island
 ├─ Dungeon/combat island
 └─ Far objects as simplified transforms
```

## Voxel Collision

Static terrain:
- Generate collision meshes from chunks.
- Use simplified greedy collision boxes when possible.
- Use full triangle mesh only where needed.

Dynamic contraptions/ships:
- Convert connected block structure into compound colliders.
- Cache collider until blocks change.
- Rebuild only dirty sections.

## Buildable Ships

A ship is a dynamic block structure with its own local voxel grid.

```text
Ship Entity
 ├─ Ship Grid
 ├─ Block Palette
 ├─ Mass Properties
 ├─ Compound Collider
 ├─ Thruster / Sail / Rudder Components
 ├─ Buoyancy Samplers
 ├─ Damage Model
 ├─ Local Entities / Players
 └─ Network Replication State
```

## Ship Mass Calculation

```text
mass = sum(block_mass)
center_of_mass = weighted average of block positions
inertia_tensor = approximated from block distribution
buoyancy_points = generated from hull bounds / water contact cells
```

## Player on Moving Ship

Use one of these approaches:

### Option A: Parent player to ship local frame

Best for block-building games.

```text
Player world transform = Ship world transform * Player local transform
```

Pros:
- Stable walking on large ships.
- Easier building while ship moves.

Cons:
- Needs careful network and camera handling.

### Option B: Pure physics contact

Pros:
- Physically natural.

Cons:
- Player slides/jitters on moving voxel ships unless heavily tuned.

Recommendation: use local-frame parenting for players standing on ships/contraptions.

## Space Ships

Space ships use the same block-grid ship system but with different force sources.

```text
Water ship: buoyancy + drag + sails/rudders/engines
Space ship: thrusters + torque + inertia + gravity wells
Air ship: lift + drag + balloons/magic/thrusters
```

---

# 10. Automation, Contraptions, and Infrastructure Systems

## Automation Identity

Automation should feel like:

```text
Minecraft readability
+ Create-style mechanical satisfaction
+ visible factory logistics
+ later magitech conversion
+ eventual mobile bases and ships
```

The system should be deep because of combinations, not because the player has to solve hardcore engineering math.

## Core Automation Pillars

```text
1. Kinetic machines come first.
2. RPM/stress is simple and readable.
3. Gear touches = works.
4. Belts are visually satisfying near the player.
5. Pipes have medium-depth pressure behavior.
6. Electricity is clean/simple power throughput.
7. Mana becomes the late-game programmable resource.
8. Programming stays magic-only.
9. Moving contraptions can become ships/mobile factories later.
10. Machines do not have normal wear/durability.
```

## Infrastructure Types

The game should eventually support several resource networks:

```text
Kinetic power:    shafts, gears, belts, RPM, stress
Fluid power:      pipes, pumps, tanks, pressure, filters
Thermal power:    heat, cooling, smelting, steam generation
Steam power:      boilers, turbines, pistons
Electrical power: wires, batteries, generators, clean power units
Mana power:       crystals, batteries, conduits, runes, spell machines
```

These systems should use a shared graph/network foundation where possible.

```text
Network node:    block or entity that participates in a network
Network edge:    connection between nodes
Network type:    kinetic, fluid, electrical, thermal, mana
Network solver:  specialized logic for that resource type
Dirty rebuild:   network recalculates only when changed
```

This allows early kinetic networks to be built first, then fluids, electricity, and mana can reuse parts of the same architecture.

---

## 10.1 Kinetic Power System

### Player-Facing Model

Use a Create-like abstraction:

```text
RPM
Stress units
Gear ratios
Direction
Capacity
Overload
```

Avoid exposing full torque, inertia, rotational acceleration, or advanced friction math to the player. The goal is simple and fun.

### Kinetic Concepts

```text
Source:     creates rotation
Consumer:   uses stress to perform work
Transfer:   shaft/gear/belt moves rotation
Modifier:   changes speed/direction/ratio
Limiter:    clutch/brake/fuse controls flow
Storage:    flywheel buffers rotational energy visually/gameplay-wise
```

### Mechanical Sources

Early sources:

```text
Hand crank
Treadmill
Water wheel
Windmill
Weighted crank
Basic engine
```

Mid sources:

```text
Steam engine
Flywheel engine
Large water turbine
Mechanical alternator
```

Late/magitech sources:

```text
Mana motor
Gravity rotor
Rune flywheel
Magitech turbine
Ship reactor drive shaft
```

### Kinetic Network Data

```cpp
struct KineticNode {
    BlockEntityId id;
    BlockCoord pos;
    Axis axis;
    float rpm;
    float stress_capacity;
    float stress_usage;
    float gear_ratio;
    RotationDirection direction;
    KineticMaterialTier material_tier;
};

struct KineticNetwork {
    NetworkId id;
    vector<KineticNodeId> nodes;
    float source_capacity;
    float total_stress;
    float network_rpm;
    bool overloaded;
    uint64_t revision;
};
```

### Gear Connection Rules

Use loose Minecraft-like rules:

```text
Gear touches = connects.
Shaft touches compatible shaft = connects.
Belt connects valid pulleys.
Gearbox redirects axis.
Clutch can disconnect network.
```

Do not require complex support bearings, tooth compatibility, belt tension, or real alignment math early.

### Material Tiers

Material tiers should add depth without adding simulation complexity.

```text
Wood:       low stress capacity, cheap, early
Stone:      slow/heavy, durable-looking, primitive
Copper:     decent transfer, good with early machines
Iron:       standard industrial tier
Steel:      high capacity
Arcane alloy: accepts mana modifiers
Void alloy: unstable but powerful
```

Material affects:

```text
max stress capacity
max safe RPM
efficiency modifier
overload tolerance
mana compatibility later
```

### Overload Behavior

No normal durability/wear. Machines do not slowly degrade from regular use.

Failures happen from overload, bad design, or dangerous late-game systems.

Kinetic overload should usually:

```text
Stop network
Play breaking/strain effects
Optionally pop a weak part
Damage selected components
Require player repair/replacement
```

Not every overload should explode.

---

## 10.2 Belts and Item Logistics

### Belt Feel

Belts should have the full visible Create-style feel near players:

```text
Visible items ride on belts.
Items can be grabbed by arms.
Presses visibly process items.
Mixers visibly consume ingredients.
Sorters visibly redirect items.
Factories look alive.
```

This is important for game feel.

### Hybrid Simulation

For performance, belts should use two simulation modes.

```text
Near/player-visible:
 - visible item proxies
 - animation
 - collision-lite placement
 - machine interaction timing

Far/unloaded/abstracted:
 - throughput packets
 - inventory deltas
 - production summaries
 - no per-item visible simulation
```

The gameplay result should remain consistent even if the visual representation changes.

### Belt Data Model

```cpp
struct BeltSegment {
    BlockCoord pos;
    Direction direction;
    float speed;
    vector<BeltItemProxy> visible_items;
    ThroughputBuffer abstract_buffer;
};

struct BeltItemProxy {
    ItemStack stack;
    float belt_position;
    EntityVisualId visual;
};
```

### Logistics Blocks

Early:

```text
Belt
Chute
Hopper
Funnel
Depot
Mechanical arm
Basic filter
```

Mid:

```text
Smart splitter
Weighted sorter
Item elevator
Package crate
Mechanical inserter
Storage interface
```

Late:

```text
Mana sorter
Dimensional input/output
Rune-filtered logistics
Portal-linked storage bus
```

---

## 10.3 Fluid and Pipe System

Fluid should be medium-depth, not hardcore simulation.

### Player-Facing Model

```text
Pressure
Capacity
Pump rate
Distance loss
Filters
Branch behavior
```

### Fluid Network Concepts

```text
Pipe:       transports fluid
Pump:       adds pressure/flow
Tank:       stores fluid
Valve:      controls flow
Filter:     only allows selected fluids
Junction:   branches flow
Machine:    consumes/produces fluid
```

### Pipe Rules

```text
Each pipe network has fluid type or mixed/contaminated state.
Pumps create pressure.
Long distance reduces effective flow.
Branches split flow according to priority/resistance.
Tanks buffer pressure and volume.
Filters prevent unwanted fluids.
```

Avoid full fluid dynamics. Use graph-based flow solving.

```cpp
struct FluidNode {
    BlockEntityId id;
    FluidRole role;
    float capacity;
    float stored_amount;
    float pressure;
    float max_flow_rate;
    Optional<FluidId> filter;
};

struct FluidNetwork {
    NetworkId id;
    FluidId fluid;
    float total_capacity;
    float total_volume;
    vector<FlowEdge> edges;
};
```

### Fluid Progression

```text
Water pipes
Steam pipes
Oil/fuel pipes
Coolant pipes
Liquid mana later
Reactive fluids late-game
```

---

## 10.4 Thermal and Steam Systems

Thermal should support smelting, boilers, cooling, weather interactions, and later magitech reactors.

### Thermal Model

Keep it simple:

```text
heat units
temperature bands
heat transfer rate
cooling rate
safe operating range
overheat threshold
```

### Steam System

Steam bridges thermal and kinetic.

```text
Water + heat -> steam pressure
Steam pressure -> piston/turbine rotation
Excess pressure -> vent/safety valve
Bad setup -> shutdown or component damage
```

Steam machines:

```text
Boiler
Steam pipe
Steam piston
Steam turbine
Pressure valve
Condenser
Heat exchanger
Industrial furnace
```

---

## 10.5 Electrical System

Electricity should be clean and simple.

### Player-Facing Model

```text
Power units
Throughput
Generation
Consumption
Battery storage
Network capacity
```

Do not expose voltage/amperage/wire-loss complexity by default.

### Electrical Blocks

```text
Generator
Wire
Relay
Battery
Motor
Lamp
Sensor
Control block
Electric furnace
Shield emitter later
```

### Electrical Role

Electricity should become useful after mechanical/steam tech, but before full magitech.

```text
Kinetic -> generator -> electricity
Electricity -> motor -> kinetic
Electricity -> sensors/control
Electricity -> compact machines
Electricity -> ship systems
```

---

## 10.6 Mana and Magitech Automation

Mana is discovered through exploration, not used heavily in early automation.

Progression:

```text
Early:   kinetic machines
Mid:     thermal, steam, fluids, basic electricity
Magic:   mana crystals and mana batteries
Late:    mana conduits and spell machines
Endgame: wireless/beam/rune networks and programmable magitech
```

Magitech converts mana into other forms:

```text
mana -> kinetic motion
mana -> heat
mana -> electricity
mana -> shields
mana -> portals
mana -> flight/lift
mana -> dimensional storage
mana -> automation commands
```

Programming remains magic-only. Non-magic automation uses simple binary control, sensors, sequencers, and mechanical/electrical logic.

---

## 10.7 Logic and Control System

### Early Logic

Use simple binary logic:

```text
on/off
pulse
delay
toggle
inverter
basic sensor
button/lever/pressure plate
```

Avoid analog redstone complexity early.

### Mid Logic

```text
sequencer
counter
clock
proximity sensor
inventory sensor
fluid sensor
stress sensor
pressure sensor
ship docking sensor
```

### Late Magic Logic

True programming unlocks through rune/mana systems only.

```text
Rune controller
Spell program block
Programmable spell core
Magitech automation brain
Ship spell-control matrix
```

This keeps early automation approachable and reserves deep programming for advanced magic players.

---

## 10.8 Moving Contraptions

Contraptions are block structures that can be extracted into moving entities.

### Stage Progression

```text
Stage 1: rotating/piston contraptions
Stage 2: gantries/elevators/drills
Stage 3: block-built ships
Stage 4: mobile factories/spacecraft
```

### Contraption Entity

```text
Contraption Entity
 ├─ Local block grid/block list
 ├─ Transform
 ├─ Kinetic/fluid/electric/mana networks
 ├─ Collider
 ├─ Render mesh
 ├─ Attachment points
 ├─ Active machines
 ├─ Inventory/fluid/mana storage
 └─ Network replication state
```

### Movement Rules

```text
Rotating contraptions rotate around bearings.
Piston contraptions move linearly.
Gantries move along rails.
Drills physically destroy blocks one by one.
Ships/mobile bases use local voxel grids.
```

### Collision Philosophy

If a moving contraption hits terrain or another solid object:

```text
It damages itself.
It may stop or jam.
It does not casually erase the world.
Drills are the intended block-breaking interface.
```

This prevents griefy accidental terrain destruction while still allowing tunnel bores and mining rigs.

### Drilling Rules

Drills should physically destroy blocks one by one.

```text
Drill contacts block
 -> check drill strength vs block hardness
 -> consume kinetic stress/energy
 -> damage/harvest block
 -> emit particles/debris
 -> output drops to inventory/belt/chute
 -> update chunk mesh/lighting
```

Batch updates internally for performance, but present the gameplay as real block-by-block mining.

---

## 10.9 Ships, Mobile Bases, and Spacecraft

### Ship Architecture

Use a hybrid model.

```text
Normal block build can become a ship/mobile grid if valid.
Dedicated ship cores can define/control ship structures.
Shipyards/docking blocks can help scan and validate builds.
```

This preserves building freedom while still giving the engine a clean conversion point.

### Ship Size Target

Target medium to large ships, with huge ships possible but not the primary design target.

```text
Small:       rafts, boats, skiffs, scout craft
Medium:      cargo ships, airships, mining ships
Large:       mobile bases, frigates, factory ships
Huge:        rare late-game carriers/city ships/orbital structures
```

Huge builds need strict performance budgets and may be server-configurable.

### Arcade-Stable Walking

Players on ships should use local-frame stability.

```text
Player standing on ship:
 - parent movement to ship local frame
 - stable walking
 - no constant sliding
 - camera remains comfortable
```

This is more important than raw physical realism.

### Machinery on Moving Ships

All machinery should eventually work while moving.

This means ships are not just vehicles; they are mobile factories and bases.

Supported on ships long-term:

```text
belts
shafts
gears
pipes
steam
electricity
mana conduits
spell machines
storage
workers
logic
weapons
thrusters
reactors
```

Engine implication: ships need their own local simulation island and local infrastructure networks.

```text
Ship local grid
 -> local kinetic network
 -> local fluid network
 -> local electrical network
 -> local mana network
 -> local machine ticking
 -> transform to world for rendering/physics
```

### Ship-to-World Interaction

```text
Docking ports connect ship networks to base networks.
Cargo interfaces transfer items/fluids/mana.
Ship drills mine terrain.
Ship weapons cast/projectile effects.
Ship engines consume kinetic/electric/mana resources.
```

---

## 10.10 Chunk Loaders and Offline Automation

### Chunk Loader Block

Chunk loaders should keep the chunk they are in loaded.

Base rule:

```text
Each player has 12 base chunk loaders.
Server/admin can configure the limit.
Singleplayer can raise/lower the limit.
Each chunk loader is expensive to craft/power.
```

Recommended constraints:

```text
Only loads its own chunk by default.
Consumes power/mana/fuel.
Can be disabled by server config.
Has visual/debug UI showing active loaded chunk.
Cannot bypass global server budget.
```

Possible upgrades later:

```text
+1 adjacent chunk with expensive upgrade
claimed-base loader
ship loader core
orbital station loader
admin/world-rule loader
```

### Offline Simulation Stages

```text
Early:
 - only loaded chunks run machines
 - chunk loaders keep limited chunks active

Mid:
 - simple catch-up simulation for farms/basic machines
 - calculate elapsed time when chunk reloads

Late:
 - abstract simulation for claimed bases
 - server-configurable industry budgets
 - important factories can continue while player explores other planets
```

### Why This Matters

Without limits, chunk loaders can destroy server performance. With configurable budgets, players with strong hardware can “let them cook” while servers can stay stable.

---

## 10.11 Worker Entities

Worker entities should be physical pathfinding helpers, not purely abstract UI entries.

Types:

```text
Golems
Magical drones
Homunculi
```

Roles:

```text
haul items
operate simple machines
repair damaged contraptions
load/unload ships
farm/harvest
guard areas
maintain mana batteries
perform ritual tasks
```

Simulation rules:

```text
Workers physically pathfind when nearby/loaded.
Workers can use abstract task completion when far away in claimed/loaded bases later.
Workers need task stations and permissions.
Workers should not require programming; they use job orders.
```

Pathfinding must be budgeted heavily.

```text
Nearby: full pathfinding
Medium: coarse pathfinding/nav regions
Far/offline: abstract work ticks only if eligible
```

---

## 10.12 Automation Roadmap

### Automation Phase 1: Kinetic Core

Goal: first wow prototype.

```text
Water wheel
Shaft
Gear
Gearbox
Clutch
Mechanical press
Mill/crusher
Belt
Depot
Simple item input/output
RPM/stress UI
Overload stop/damage
```

This should create the first satisfying factory line.

### Automation Phase 2: Visible Logistics

```text
Belts with visible items
Chutes/funnels
Mechanical arms
Filters
Splitters
Storage interfaces
Press/saw/mixer chains
```

### Automation Phase 3: Moving Contraptions

```text
Mechanical bearing
Piston
Gantry
Elevator
Drill
Tunnel bore
Contraption extraction/entity system
Self-damage collision behavior
```

### Automation Phase 4: Fluids/Thermal/Steam

```text
Pipes
Pumps
Tanks
Valves
Filters
Boilers
Steam engines
Steam pistons
Heat machines
```

### Automation Phase 5: Electricity

```text
Generator
Wire
Battery
Motor
Relay
Electric sensors
Compact electric machines
```

### Automation Phase 6: Ships/Mobile Factories

```text
Ship core
Shipyard validator
Local ship grid
Stable walking
Ship networks
Docking ports
Ship drills
Ship storage/logistics
Ship engines
```

### Automation Phase 7: Mana/Magitech

```text
Mana crystals
Mana batteries
Atmospheric mana extractor
Mana motor
Mana furnace
Rune clutch
Spell turret
Spell machine
Mana conduits
```

### Automation Phase 8: Endgame Industry

```text
Magic-powered ships
Floating bases
Portal networks
Mana reactors
Orbital factories
Programmable magitech controller
Dimensional storage logistics
Spell-powered production lines
```

---

# 11. Water System

The water system should have layers. Do not start with full 3D fluid simulation everywhere.

## Water Layers

```text
Layer 1: Voxel water occupancy
 - Water blocks/cells
 - Flow direction
 - Source/sink rules
 - Gameplay interactions

Layer 2: Surface simulation
 - Waves
 - Wind response
 - Storm visuals
 - Boat interaction visuals

Layer 3: Physics interaction
 - Buoyancy
 - Drag
 - Current forces
 - Impact splashes

Layer 4: Weather/ocean macro system
 - Tide level
 - Storm intensity
 - Wind field
 - Wave direction
 - Ocean biome parameters
```

## Early Water

Use simplified cellular water:
- Water level per cell, 0–8 or 0–15.
- Downward flow first.
- Horizontal spread second.
- Chunk-local active water queue.

## Ocean Water

For seas and oceans, use a heightfield/spectral-looking surface visually, while gameplay water remains voxel-based near shore/player.

```text
Far ocean: shader waves + height sampling
Near shore: voxel water cells
Ship physics: buoyancy samplers query visual/physical water height
Storms: wind field modifies wave amplitude and direction
```

This gives Valheim-like sailing feel without simulating every water voxel in the ocean.

---

# 12. Terrain Generation

## Terrain Goals

- Natural depth.
- Large caves.
- Mountains, valleys, rivers, oceans.
- Fantasy biomes.
- Ancient ruins and tech/magic structures.
- Planet-specific identities.

## Recommended Generator Pipeline

```text
World seed
 -> Planet profile
 -> Climate maps
 -> Continental shape
 -> Height / density fields
 -> Erosion approximation
 -> Biome assignment
 -> Cave fields
 -> Ore/resource fields
 -> Structure candidates
 -> Local chunk generation
 -> Decoration pass
 -> Lighting init
```

## Data-driven Planet Profiles

```json
{
  "id": "core:storm_moon",
  "gravity": 0.72,
  "sea_level": 91,
  "atmosphere_height": 10000,
  "temperature_bias": -0.3,
  "humidity_bias": 0.8,
  "terrain": {
    "continental_scale": 0.0008,
    "mountain_scale": 0.006,
    "cave_density": 0.42,
    "erosion_strength": 0.65
  },
  "biomes": [
    "core:mist_forest",
    "core:basalt_cliffs",
    "core:mana_marsh"
  ]
}
```

## AI/ML Terrain

Use AI/ML carefully. Do not put a neural network in the main chunk-generation hot path early.

Best use cases:
- Offline generation of biome masks.
- Learning terrain style from heightmap examples.
- Structure placement suggestions.
- Generating prefab variants.
- World-gen assistant tool for designers.

Runtime terrain should remain deterministic and fast.

Possible architecture:

```text
ML Generator Tool
 -> outputs biome masks / macro maps / structure layout hints
 -> saved as deterministic world-gen assets
 -> runtime generator uses these maps + procedural rules
```

---

# 13. Structure and Prefab Generation

Structures should be modular and rule-based.

## Prefab Types

```text
Static prefab: fixed ruin, tree, tower
Jigsaw prefab: rooms/corridors assembled by sockets
Grammar prefab: generated village, dungeon, factory
Biome decoration: rocks, plants, crystals
Quest structure: deterministic important location
Player blueprint: saved build/ship/contraption
```

## Structure File Example

```json
{
  "id": "core:ancient_mana_tower",
  "type": "jigsaw_structure",
  "size_limit": [96, 160, 96],
  "spawn_rules": {
    "biomes": ["core:mana_marsh", "core:mist_forest"],
    "min_distance_from_spawn": 2000,
    "rarity": 0.015
  },
  "pieces": [
    "core:mana_tower/base",
    "core:mana_tower/mid",
    "core:mana_tower/top",
    "core:mana_tower/bridge"
  ],
  "connectors": ["door", "stair", "bridge", "shaft"]
}
```

## Blueprint System

Blueprints should be first-class:
- Player-made ships.
- Contraptions.
- Factories.
- Structures.
- Dungeon rooms.
- Modded schematics.

Blueprint format should store:
- Blocks.
- Block states.
- Block entities.
- Connections.
- Metadata.
- Required mods.

---

# 14. Save System and Multiplayer-Friendly World Data

## Save Model

Use append-friendly region files with chunk records.

```text
World Folder
 ├─ world.json
 ├─ registries/
 ├─ planets/
 │   └─ planet_0001/
 │       ├─ regions/
 │       │   ├─ r.0.0.0.vregion
 │       │   └─ r.0.0.1.vregion
 │       ├─ entities/
 │       ├─ ships/
 │       └─ maps/
 └─ players/
```

## Chunk Save Pipeline

```text
Chunk changed
 -> mark dirty
 -> serialize chunk delta or full chunk
 -> palette encode
 -> bit-pack
 -> optional RLE
 -> Zstd compress
 -> write async to region file
 -> update index table
```

## Multiplayer Save Principle

Every meaningful world change should be representable as either:

```text
BlockDelta
BlockEntityDelta
EntityDelta
ContraptionDelta
ShipDelta
FluidDelta
RegistrySync
```

This makes save, undo, replay, and networking share the same change model.

## Delta Example

```cpp
struct BlockDelta {
    PlanetId planet;
    ChunkCoord chunk;
    uint16_t local_index;
    BlockStateId old_state;
    BlockStateId new_state;
    uint64_t tick;
    PlayerId author;
};
```

## Multiplayer Architecture

Use authoritative server simulation.

```text
Client
 ├─ prediction for player movement
 ├─ local chunk cache
 ├─ interpolation for entities/ships
 └─ visual-only particles/weather

Server
 ├─ authoritative world state
 ├─ chunk streaming per player interest
 ├─ physics authority for ships/contraptions
 ├─ anti-cheat validation
 └─ save writer
```

For co-op, this can still run as a local listen server.

---

# 15. World Transition: Ground to Space to Planet Landing

The base-game space identity, feature taxonomy, generation weights, and phase
plan are defined in `docs/space_design_bible.md`. In short: space is mostly
natural wilderness; active civilizations are not common base-game content; the
player builds the industrial network.

Do not load a separate “space game.” Use a layered world transition model.

## Altitude Bands

```text
Ground Band
 - Full voxel terrain
 - Weather
 - normal gravity
 - dense atmosphere

Upper Atmosphere Band
 - reduced terrain detail
 - cloud layers
 - atmospheric drag changes
 - horizon effects

Near Space Band
 - planet below as LOD sphere/terrain patch
 - ships use space flight physics
 - atmosphere mostly visual

Orbital / Space Band
 - star system coordinates
 - planets as celestial bodies
 - ship interiors remain local voxel grids

Planet Landing Band
 - select landing target
 - stream planet terrain under ship
 - transition from orbital LOD to chunk terrain
```

## Key Design

The player should always be inside a local simulation bubble.

```text
Player/ship local bubble
 -> nearby chunks/entities/physics active
 -> far planet/space rendered as LOD
 -> true position stored in high-precision universe coordinates
```

## 10,000 Block Escape Atmosphere

If escape altitude is 10,000 blocks:
- This is feasible.
- 32-block chunks means roughly 312 vertical chunks.
- You only need a small vertical column active around the player.

If escape altitude is 10,000,000 blocks:
- Still possible architecturally.
- Must be almost entirely coordinate/LOD based.
- Do not stream actual block chunks through the whole height.

---

# 16. Magic and Magitech System

## Core Magic Identity

Magic is not divided into traditional schools like fire, frost, storm, or nature. Instead, magic comes from **Mana Particles**: fundamental programmable particles woven into existence.

Mana is a universal energetic substrate that can be absorbed, shaped, compressed, emitted, stored, or converted. In practical gameplay terms, mana can affect:

```text
Matter state
Energy transfer
Motion/force
Heat
Light
Mass-like behavior
Block transformation
Entity effects
Space/distance relationships
Storage/dimensional pockets
Machine behavior
Automation signals
```

The fantasy is not “learn fire magic.” The fantasy is “learn the language and physics of mana well enough to rewrite small pieces of reality.”

This makes the system feel like **deep magic engineering** and **science-like magic**, while still allowing player-cast spells, tools, machines, spell cannons, portals, floating bases, and magitech factories.

## Mana as a Programmable Particle

Mana should behave like a field/particle resource that exists in the world and inside living entities.

```text
World mana field:      ambient particles in air/biomes/ruins
Entity mana pool:      mana absorbed and stored by player/entities
Tool mana buffer:      mana stored inside crafted focuses/spell cores
Machine mana battery:  industrial-scale mana storage
Dense mana crystal:    solid/mineral form of mana concentration
Unstable mana:         poorly shaped or overloaded mana behavior
```

## Mana Flow Model

Mana should have practical engineering properties:

```text
Quantity:      how much mana is available
Density:       how concentrated it is
Purity:        how efficient/stable it is
Flow rate:     how quickly it moves through tools/conduits
Resonance:     compatibility with runes/materials/machines
Waste:         mana lost as heat/light/particles/noise
Stability:     chance of failure, miscast, or overload
```

These properties give machines, tools, and player progression meaningful roles without needing traditional magic schools.

## Same Underlying System, Multiple Delivery Methods

All magic uses the same effect engine. The difference is how the effect is delivered.

```text
Body-cast magic:
 - Player directly shapes mana from their internal pool.
 - Early game.
 - High waste.
 - Lower precision.
 - More risk when overloaded.

Tool/focus magic:
 - Wand, staff, glove, spell core, rune lens, crystal focus.
 - Mid game.
 - Less waste.
 - Higher effect strength.
 - Better targeting and stability.

Machine-cast magic:
 - Spell turret, ritual device, mana reactor, portal anchor, factory block.
 - Mid/late game.
 - Large effects.
 - Automatable.
 - Can consume batteries, crystals, or atmospheric mana.

Programmatic magic:
 - Player writes or assembles custom spell logic from discovered rune-language primitives.
 - Advanced optional layer for system-heavy players.
 - Should be powerful but sandboxed.
```

## Three-Layer Spell Creation Model

Magic should be understandable early, deep later, and extremely deep for players who want to engineer custom spells.

### Layer 1: Basic Spell Components

Early game players discover runes and unlock spell components.

```text
Trigger
Target
Shape
Effect
Modifier
Cost
Visual
Failure rule
```

Example:

```text
Cast trigger
 -> Ray target
 -> Push force
 -> Low cost
 -> Short cooldown
```

This creates simple player-cast spells without requiring coding knowledge.

### Layer 2: Modular Spell Graphs

Mid-game players assemble spells visually or through structured components.

```text
Input Node
 -> Mana Budget Node
 -> Targeting Node
 -> Condition Node
 -> Matter/Force/Heat/Light Effect Node
 -> Modifier Node
 -> Safety Node
 -> Output Node
```

Example graph:

```text
On Cast
 -> If target is water block
 -> Apply freeze-state conversion
 -> Spawn mist particles
 -> Consume mana based on volume
 -> If insufficient mana, weaken effect instead of exploding
```

Spell graphs should be serializable as data so they can be saved, shared, modded, and placed inside machines.

### Layer 3: Programmatic Spell Language

Advanced players can discover the deeper “language of magic” through books, ruins, tomes, ancient machines, research puzzles, and magical computers.

This should be a small custom language or safe scripting layer, not arbitrary unsafe code.

Design goals:

```text
Readable enough for curious players
Limited enough to sandbox safely
Powerful enough for custom spell engineering
Deterministic enough for multiplayer
Cost-analyzed before execution
Works with spell cores and machines
Can be discovered progressively through gameplay
```

Example style:

```text
spell PushLift {
  budget mana <= 80
  target ray range 24

  if target.block.has_tag("movable") {
    force.up(12)
    force.away_from(caster, 4)
  }

  safety weaken_on_overload
}
```

Example machine spell:

```text
spell FactorySortPulse {
  budget mana <= battery.input(20)
  target area radius 5

  for block in area.blocks {
    if block.has_tag("ore") {
      move(block).toward(marker("crusher_input"))
    }
  }

  cooldown 40 ticks
  safety stop_on_low_mana
}
```

The spell language should never allow unrestricted loops, memory access, file access, or native calls. It should compile into a limited internal spell bytecode or effect graph.

## Spell Execution Pipeline

```text
Spell request
 -> Validate caster/tool/machine permissions
 -> Resolve spell graph or spell program
 -> Estimate mana cost and risk
 -> Check available mana/buffer/tool capacity
 -> Apply safety policy
 -> Execute effect commands
 -> Emit particles/audio/lighting
 -> Apply block/entity/world deltas
 -> Save/network replicate deterministic deltas
```

## Spell Effect Command Types

The internal engine should execute spells through a limited command set.

```text
Entity commands:
 - damage
 - heal
 - push/pull
 - shield
 - debuff/buff
 - teleport short distance
 - summon worker entity

Block commands:
 - transform block
 - move block
 - place temporary block
 - break/soften block
 - heat/cool block
 - charge block
 - stabilize/unstabilize block

Physics commands:
 - apply force
 - apply torque
 - reduce/increase gravity-like effect
 - suspend/freeze motion

Energy commands:
 - emit light
 - create heat
 - convert mana to kinetic/electric/thermal output
 - charge battery

Spatial commands:
 - link portal anchors
 - create dimensional storage access
 - compress storage space
 - mark teleport destination

Automation commands:
 - trigger machine
 - alter RPM/stress behavior
 - move items/blocks
 - control rune logic
 - pulse mana network
```

## Failure States

Magic failures should have consequences but should not usually destroy large builds. The game should punish bad engineering without making players afraid to experiment.

### Soft Failure

```text
Spell fizzles
Mana is wasted
Tool overheats briefly
Cooldown increases
Effect is weakened
```

### Personal Overload

If the player attempts to cast beyond their mana pool or body capacity:

```text
Mana drains to zero
Player enters over-mana state
Small self-damage or exhaustion
Temporary debuffs
Reduced movement/casting efficiency
Blurred/warped visual effect
```

Possible debuffs:

```text
Mana Sickness: slower mana regeneration
Arcane Fatigue: slower movement/action speed
Nerve Static: unstable aim/casting
Reality Burn: mild damage over time
Cognitive Haze: longer spell cooldowns
```

### Tool Failure

If a focus/tool/spell core is overloaded:

```text
Tool cracks
Tool loses durability
Tool temporarily locks
Stored mana vents as particles
Spell core burns out
Rarely: tool breaks completely
```

### Machine Failure

Machines should prefer safe shutdown over catastrophic explosions.

```text
Mana conduit trips
Battery vents mana
Reactor enters cooldown
Rune matrix desynchronizes
Machine jams
Output becomes inefficient
```

Large destructive failure should exist only for explicitly dangerous late-game systems like unstable reactors, void devices, or boss/ritual events.

## Mana Extraction and Storage

Ambient mana particles can be harvested later through technology.

```text
Atmospheric Mana Extractor
 - Pulls low-density mana particles from the air.
 - Works better in high-mana biomes, storms, ruins, leyline zones, and upper atmosphere.
 - Slowly charges mana batteries.
 - Can be upgraded with lenses, filters, crystals, and rune arrays.

Mana Battery
 - Stores mana for machines/tools.
 - Has capacity, flow rate, purity, and stability.

Mana Crystal
 - Dense natural or crafted mana storage.
 - Used for spell cores, reactors, portal anchors, and high-end machines.
```

## Progression Path

```text
1. Player senses/absorbs small amounts of mana naturally.
2. Player finds ruins/tomes/runes.
3. Basic body-cast spells unlock.
4. Research unlocks spell components.
5. Player crafts spell cores/focus tools to reduce waste.
6. Player builds mana batteries and simple mana machines.
7. Player discovers modular spell graphs.
8. Player discovers programmatic spell language fragments.
9. Player automates spell effects through magitech blocks.
10. Player builds reactors, portals, spell cannons, floating bases, dimensional storage, and magic-powered ships.
```

## Early/Mid/Late Magic Roadmap

### Early Game

```text
Body-cast mana
Small internal mana pool
Simple discovered runes
Basic modular spells
Low block manipulation
High mana waste
Soft failure states
Ruins teach first concepts
```

Early spells should include:

```text
Push/pull object
Light pulse
Heat small area
Cool/freeze small water patch
Short-range blink or dash
Basic shield
Block soften/mining assist
Minor repair/stabilize
```

### Mid Game

```text
Crafted focus tools
Spell cores
Mana batteries
Atmospheric mana extractor
Mana conduits
Spell graph editor
Machine-cast spell blocks
Basic spell turrets
Mana-powered contraptions
```

Mid-game capabilities:

```text
Move blocks in limited patterns
Charge machines
Automate simple spell effects
Create defensive barriers
Power ship devices
Stabilize floating blocks/platforms
Build small portal links
```

### Late Game

```text
Programmatic spell language
Magitech factories
Spell cannons
Magic-powered ships
Floating sky bases
Advanced block-moving machines
Summoned worker entities
Dimensional storage
Portal networks
Mana reactors
```

Late-game capabilities:

```text
Automated mining/sorting with spells
Spell-programmed factories
Long-distance logistics
Large defensive shields
Ship-mounted spell engines/cannons
Floating base stabilization
Dimensional item storage
Portal transportation
```

## Magitech Integration

Magic should integrate with automation by treating mana as another engineered resource, but deeper and more flexible than electricity.

```text
Kinetic system: motion, RPM, stress
Fluid system: liquids, pressure, transport
Thermal system: heat, smelting, phase change
Electrical system: circuits, batteries, machines
Mana system: programmable effect energy
Magitech: conversion layer between mana and all other systems
```

Examples:

```text
Mana motor: converts mana to kinetic rotation
Rune clutch: engages/disengages shafts based on spell logic
Spell press: applies programmed matter-state change to items
Mana furnace: converts mana to heat
Gravity stabilizer: supports floating platforms
Portal anchor: links two spatial coordinates
Spell cannon: machine-casts projectile/beam spells
Summoner block: creates worker entities from mana + materials
Dimensional storage core: stores items in linked pocket space
```

## Magic Data Model

```cpp
struct ManaContainer {
    float amount;
    float capacity;
    float purity;
    float max_input_rate;
    float max_output_rate;
    float stability;
};

struct SpellCore {
    SpellGraphId graph;
    ManaContainer buffer;
    float efficiency;
    float max_complexity;
    float overload_tolerance;
    FailurePolicy failure_policy;
};

struct SpellExecutionContext {
    EntityId caster;
    Optional<EntityId> tool;
    Optional<BlockEntityId> machine;
    PlanetId planet;
    WorldPosition origin;
    ManaContainer* mana_source;
    uint64_t tick;
};
```

## Critical Engine Requirements for Magic

Because magic can modify blocks and interact with machines, it must use the same world-delta system as building, mining, machines, explosions, and multiplayer.

```text
SpellBlockDelta
SpellEntityDelta
SpellForceEvent
SpellParticleEvent
SpellLightEvent
SpellMachineSignal
SpellNetworkEvent
```

Rules:

```text
Spells cannot directly mutate world memory.
Spells emit validated commands.
Commands become deltas.
Deltas are saved and replicated.
Expensive spell effects must have budgets.
Block-changing spells must be chunk-aware.
Programmatic spells must be deterministic.
```

This keeps magic powerful without destroying performance, saves, or multiplayer.

---

# 17. UI System

Build two UI layers.

## Dev UI

For early development:
- ImGui or similar immediate-mode UI.
- Debug panels.
- Entity inspector.
- Chunk viewer.
- Registry browser.
- Profiler overlay.
- Console/commands.
- World-gen preview tools.

## Game UI

For actual player-facing UI:
- Retained-mode UI tree.
- CSS-like styling or data-driven styles.
- Theme/skinning support.
- Controller support.
- Localization.
- Animation support.

## UI Architecture

```text
UI Document
 ├─ Layout tree
 ├─ Style classes
 ├─ Data bindings
 ├─ Input events
 ├─ Animation tracks
 └─ Render commands
```

Example:

```json
{
  "id": "core:inventory_screen",
  "root": {
    "type": "panel",
    "class": "inventory_root",
    "children": [
      { "type": "slot_grid", "bind": "player.inventory.main" },
      { "type": "equipment_panel", "bind": "player.equipment" }
    ]
  }
}
```

---

# 18. Voxel Particles and Debris

## Particle Types

```text
GPU particles: sparks, dust, magic motes, snow, rain
Voxel debris: block fragments, explosions, mining chips
Physics debris: limited rigid chunks near player
Weather particles: rain, ash, leaves, embers
```

## Debris Rules

Never make every block fragment a real physics object forever.

Use tiers:

```text
Tier 1: visual-only GPU particles
Tier 2: short-lived collider debris near player
Tier 3: actual dropped item/entity if important
```

## Voxel Destruction

```text
Block breaks
 -> spawn visual voxel fragments
 -> spawn dust particles
 -> optionally spawn item entity
 -> update chunk mesh
 -> update lighting
 -> send BlockDelta
```

For explosions:
- Batch block changes.
- Rebuild affected chunk meshes once.
- Spawn particles from aggregated destruction volume.

---

# 19. Data and Modding Architecture

## Registry System

Everything gameplay-facing should be registry-based.

```text
Registries
 ├─ blocks
 ├─ items
 ├─ recipes
 ├─ biomes
 ├─ structures
 ├─ entities
 ├─ spells
 ├─ particles
 ├─ sounds
 ├─ UI screens
 ├─ loot tables
 ├─ machines
 ├─ fluids
 ├─ materials
 └─ dimensions/planets
```

## Load Order

```text
Base engine registries
 -> Core game data
 -> DLC data
 -> Mods
 -> User config overrides
```

## Mod Safety

Each registry entry needs:
- Namespace.
- Version.
- Dependency list.
- Schema validation.
- Conflict handling.
- Stable numeric runtime IDs generated at load.

```json
{
  "mod_id": "skyforge",
  "version": "0.1.0",
  "depends": ["core >= 0.1.0"],
  "registries": ["blocks", "items", "spells", "machines"]
}
```

## Code Mods

Early:
- Data-only mods.

Mid:
- Lua scripts for block/entity behavior.

Advanced:
- WASM sandbox for high-performance safe mod plugins.

Do not allow arbitrary native DLL mods until the engine is stable.

---

# 20. Threading and Job System

This engine needs a strong job system early.

## Worker Jobs

```text
Chunk generation
Chunk meshing
Chunk compression/decompression
Lighting updates
Structure placement
Pathfinding
AI ticks
Physics broad prep
Network serialization
Save writing
Texture/asset streaming
```

## Main Thread Should Handle

```text
Input
High-level gameplay orchestration
Render frame submission
UI event dispatch
Audio command submission
```

## Job Priority

```text
Critical: chunks directly around player, physics colliders, network state
High: chunks in movement direction, visible chunk meshes
Medium: lighting, structure details, nearby decorations
Low: far LOD, background saves, analytics, cache warming
```

---

# 21. Performance Budgets

For this project to survive, set hard budgets.

## 60 FPS Target

```text
Frame time: 16.67 ms
CPU main thread: 2–5 ms
Render submission: 1–3 ms
GPU render: 6–12 ms
Physics: 1–4 ms
Gameplay simulation: 1–3 ms
UI/audio/etc: 1 ms
```

## Chunk Budgets

```text
Chunk full data size target:       4 KB–64 KB compressed depending complexity
Chunk mesh rebuild target:         under 1–4 ms on worker for normal chunks
Chunks meshed per frame:           budgeted, not unlimited
Chunk uploads per frame:           memory-budgeted
Active full-sim chunks:            small radius only
Far render chunks:                 LOD/impostor only
```

## Important Optimization Rules

- Never tick every block.
- Never simulate all rendered chunks.
- Never rebuild a chunk mesh more than once per frame.
- Never save synchronously on the main thread.
- Never use one draw call per chunk if avoidable.
- Never allocate heavily during frame gameplay.
- Never let mods bypass performance budgets without profiling hooks.

---

# 22. MVP Build Order

## Phase 0: Engine Skeleton

Goal: open a window, render a triangle/cube, load JSON registry.

Build:
- Window/input.
- Vulkan device/swapchain.
- Basic render graph.
- Asset manager.
- Logger.
- Profiler markers.
- Job system skeleton.
- JSON registry loader.

## Phase 1: Basic Voxel World

Goal: walk around a generated voxel terrain world.

Build:
- 32³ chunks.
- Block registry.
- Chunk storage.
- Simple terrain generator.
- CPU greedy meshing.
- Chunk streaming radius 8–16.
- Basic save/load.
- Camera-relative rendering.

## Phase 2: Performance Foundation

Goal: make the voxel world fast enough to scale.

Build:
- Async chunk generation.
- Async meshing.
- Chunk mesh upload queue.
- GPU frustum culling.
- Indirect draw batching.
- Compressed region files.
- Basic lighting.
- Debug chunk viewer.

## Phase 3: Gameplay Foundation

Goal: actual playable survival/adventure loop.

Build:
- Player controller.
- Inventory.
- Block placing/breaking.
- Items/tools.
- Crafting.
- Basic enemies/entities.
- UI system v1.
- Data-driven recipes and blocks.

## Phase 4: Contraptions

Goal: Create-like mechanics.

Build:
- Mechanical graph.
- Shafts/gears/belts.
- Stress/speed solver.
- Moving contraption entity extraction.
- Machine blocks.
- Wrench/tool interactions.

## Phase 5: Ships and Water

Goal: build a block ship and sail it.

Build:
- Dynamic ship grid.
- Compound collider generation.
- Mass/inertia calculation.
- Buoyancy points.
- Water height queries.
- Player local-frame ship walking.
- Ship saving/network state.

## Phase 6: Space Transition

Goal: fly high and transition to near-space.

Build:
- High-altitude atmosphere bands.
- Space rendering.
- Ship flight mode swap.
- Planet LOD from high altitude.
- Coordinate rebasing stress tests.

## Phase 7: Magic and Magitech

Goal: magic becomes part of automation.

Build:
- Spell registry.
- Effect graph.
- Mana resource network.
- Magic machines.
- Magitech crafting progression.
- Spell VFX system.

## Phase 8: Multiplayer

Goal: co-op world with chunk streaming and ships.

Build:
- Authoritative server.
- Client prediction.
- Chunk replication.
- Delta protocol.
- Entity interpolation.
- Ship/contraption replication.
- Save integration.

---

# 23. Recommended Minimum Prototype

Do not start with space, AI terrain, storms, and multiplayer all at once.

The best first playable prototype:

```text
Single player
Flat/terrain voxel world
32³ chunks
Vulkan rendering
32 chunk render distance
8 chunk sim distance
JSON block registry
Block place/break
Basic lighting
Save/load
Simple mechanical shaft/gear network
Simple boat/raft made of blocks
```

Once that works, expand.

---

# 24. Biggest Risk Areas

## Risk 1: Too Much Scope

This is several games worth of systems. Keep a vertical slice small.

## Risk 2: Render Distance Expectations

128–256 chunk render distance is possible visually, but not at full block detail and simulation. It requires LOD, fog, culling, and strict budgets.

## Risk 3: Moving Block Ships

Buildable moving voxel ships are hard because they touch physics, rendering, networking, saving, and player movement.

## Risk 4: Water Physics

Full voxel water everywhere is a trap. Use hybrid water.

## Risk 5: Modding Too Early

Data-driven architecture early is good. Full code modding before gameplay stabilizes can slow development.

## Risk 6: Multiplayer Retrofits

Even if multiplayer comes later, design saves and world changes as deltas from the beginning.

---

# 25. Final Recommended Engine Shape

The engine should be built around these pillars:

```text
1. Integer world coordinates + camera-relative rendering.
2. 32³ voxel chunks with palette compression.
3. Separate render distance from simulation distance.
4. Vulkan GPU-driven chunk renderer.
5. Data-driven registries for blocks/items/spells/machines.
6. Jolt-style rigid-body physics plus custom voxel ship/contraption layers.
7. Hybrid water: voxel near player, shader/heightfield ocean far away.
8. Chunk delta save/network model.
9. Modular terrain + prefab generation.
10. Magic as an effect graph that later connects to automation.
```

If you build those pillars first, the later fantasy/tech/space/magitech systems will have a strong foundation instead of becoming isolated hacks.
