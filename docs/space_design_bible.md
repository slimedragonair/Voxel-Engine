# AetherForge: Infinite Creation - Space Design Bible

This document is the base-game direction for space generation and space-era
gameplay. It is intentionally conservative: space should feel like wilderness,
not a crowded sci-fi map.

## Core Identity

Space is not civilization. Space is wilderness.

The base-game universe should feel quiet, dangerous, resource-rich, lonely,
ancient, mostly untouched, procedural, and player-shaped. The player is one of
the few active forces building, mining, exploring, automating, and changing it.

The main fantasy is:

```text
The universe is not full of active civilizations.
The player becomes the source of industry, infrastructure, bases, ships,
colonies, automation, and long-distance networks.
```

This fits the voxel/automation model: an asteroid belt should ask "what can I
build here?", not "who already lives here?"

## Base-Game Rules

1. Space is mostly natural.
2. Active intelligent civilizations do not commonly exist in base-game space.
3. Most structures in space should be player-made.
4. Ancient ruins exist, but they are rare and usually planetary.
5. Derelict ships exist, but they are rare events.
6. Life exists on some planets, but not everywhere.
7. Asteroids, moons, planets, and resources are the main exploration content.
8. Mods can add crowded sci-fi content without changing the base game.

## What Space Contains

Common features:
- Asteroids
- Meteor clusters
- Ice fields
- Dead rocky moons
- Dust clouds
- Comets
- Ore-rich rocks
- Crystal formations
- Radiation zones
- Gas pockets
- Planetary rings
- Empty sectors

Uncommon features:
- Large asteroid caverns
- Frozen water deposits
- Rare mineral clusters
- Ancient crater ruins on planets
- Alien fossils
- Strange biome planets
- Underground planetary ruins
- Natural wormholes or anomalies

Rare features:
- Derelict ship
- Ancient alien ruin
- Dead megastructure fragment
- Buried temple on a moon
- Strange living asteroid
- Alien creature planet
- Artifact site

Extremely rare features:
- Ancient civilization homeworld
- Shattered artificial moon
- Planet-sized fossil structure
- Deep-space anomaly
- Void-touched asteroid field
- Ancient gateway ruin

## What Base-Game Space Avoids

These are good mod content, but they should not define the base game:
- Pirate camps
- Active mining outposts
- Orbital elevators
- Space highways
- Ship graveyards
- Active space empires
- Frequent artificial stations
- Busy NPC trade routes
- Modern alien cities

The target tone is closer to Minecraft cave exploration, Subnautica loneliness,
No Man's Sky scale, and Create-style player industry, but with fewer NPC-made
things and more natural mystery.

## Generation Ratios

For a typical star system:

```text
80.0% natural empty/debris/asteroid/planet content
12.0% biological or natural anomalies
 6.0% ancient ruins/fossils/artifacts
 1.5% derelict ships
 0.5% special legendary discoveries
```

For sectors:
- Most sectors are empty or contain small natural objects.
- Some sectors contain asteroid clusters, comets, dust, or ring debris.
- Rare sectors contain an ancient ruin or derelict.
- Very rare sectors contain a major anomaly or unique planet.

Exploration should stay exciting without turning every few minutes into another
hand-authored dungeon.

## Feature Taxonomy

Use "features" rather than "structures" for space generation.

```cpp
enum class SpaceFeatureType {
    Empty,
    AsteroidCluster,
    IceField,
    MetalRichAsteroids,
    CrystalAsteroids,
    DustCloud,
    Comet,
    Planet,
    Moon,
    RingDebris,
    NaturalAnomaly,
    AncientRuin,
    DerelictShip,
    BiologicalSignal,
    ArtifactSite
};

enum class FeatureOrigin {
    Natural,
    AncientExtinctCivilization,
    DerelictUnknown,
    Biological,
    PlayerMade,
    Modded
};
```

The generator should be able to answer:

```text
This is natural.
This is ancient.
This is player-made.
This is modded.
```

Base-game generation mostly uses `Natural`, with rare
`AncientExtinctCivilization`, `DerelictUnknown`, and `Biological` entries.

## Star System Profile

```cpp
struct StarSystemGenerationProfile {
    float asteroidDensity;
    float planetCount;
    float moonDensity;
    float lifePlanetChance;
    float ruinChance;
    float derelictChance;
    float anomalyChance;
    float activeCivilizationChance; // base game: 0.0f
};
```

Recommended base-game defaults:
- `activeCivilizationChance = 0.0f`
- `derelictChance = very low`
- `ruinChance = low`
- `lifePlanetChance = uncommon`
- `asteroidDensity = medium/high`
- `anomalyChance = low/medium`

Normal system ranges:
- Planets: 3-9
- Moons: 0-20, depending on planets
- Asteroid belts: 0-3
- Comet paths: 0-4
- Major ruin sites: 0-2
- Derelict ships: 0-1
- Life planets: 0-2
- Dead/resource planets: common
- Gas giants: common enough, mostly not landable

Most systems should have no derelict ship, maybe no major ruins, and mostly
natural bodies. Special systems can bend the weights toward ruins, alien
biology, rare resources, and dangerous anomalies.

## Player-Made Space Civilization

The game should support player-built:
- Space stations
- Asteroid mines
- Orbital bases
- Shipyards
- Fuel depots
- Moon colonies
- Teleport or jump networks
- Orbital factories
- Asteroid bases

Generated stations and active outposts are not the base-game answer. The player
creates the civilized network.

## Exploration Loop

```text
Leave planet
Scan nearby natural space
Find asteroid field
Mine rare resources
Find dead moon
Land and explore caves
Rarely discover fossil, ruin, artifact, or derelict
Build outpost or station
Upgrade ship
Travel farther
Find more extreme planets
```

## Planets

Planets are the centerpiece. Space stations are not.

Dead planets should be common:
- Dead rocky planet
- Frozen moon
- Lava world
- Acid desert
- Cratered moon
- Dust planet
- Metal-rich world
- Toxic atmosphere planet
- Ocean-but-no-land planet

Dead planet gameplay:
- Mining
- Survival
- Caves
- Ancient ruins
- Rare ores
- Harsh weather
- Low oxygen
- High radiation

Life-bearing planets should be rarer and exciting:
- Jungle planet
- Fungal world
- Ocean life planet
- Giant tree planet
- Crystal-biome planet
- Swamp planet
- Alien grassland
- Bioluminescent moon

Life planet gameplay:
- Alien creatures
- Dangerous plants
- Unique food/resources
- Biological materials
- Taming later, if it fits
- Rare magic/nature resources

## Ruins

Ancient ruins should usually be on planets or moons. They should be buried,
broken, weathered, silent, partially procedural, rare, and planet-bound.

Good ruin forms:
- Buried city fragment
- Temple under sand
- Overgrown alien ruin
- Frozen ancient ruin
- Collapsed underground vault
- Ancient planetary machine
- Shattered monument
- Stone/metal hybrid ruin
- Half-submerged ruin
- Crystalized remains
- Ancient observatory
- Fossilized biome structure

The civilization is gone. The player finds traces, not active society. The
question should remain: is this technology, magic, or both?

## Derelict Ships

Derelicts exist, but should be rare enough to feel like events.

Good derelict types:
- Ancient alien vessel
- Lost human-like expedition ship
- Organic ship husk
- Small broken probe
- Crashed colony ark
- Burned-out unknown craft

Frequency guideline:
- Zero or one derelict per star system.
- Sometimes none.
- A derelict should be an event, not scenery.

Derelict gameplay:
- Explore in zero-G
- Cut through doors
- Find old logs or artifacts
- Salvage rare parts
- Fight environmental hazards
- Maybe encounter alien parasites or creatures
- Power is dead or unstable
- No active crew/faction by default

## Modding Registry

Base-game space stays natural, but the engine should expose data hooks for
mods that want busy sci-fi content.

```cpp
struct SpaceFeatureDefinition {
    std::string id;
    FeatureOrigin origin;
    SpaceFeatureType type;
    float spawnWeight;
    std::vector<std::string> allowedSystemTags;
    std::vector<std::string> allowedPlanetTags;
    bool baseGameEnabled;
    bool moddedOnly;
};
```

Base-game definitions:
- Natural asteroid fields
- Dead planets
- Life planets
- Ancient ruins
- Rare derelicts

Modded definitions:
- Modern stations
- Factions
- Shipyards
- Outposts
- Pirates
- Trade networks
- Alien cities
- Space wars

## Implementation Phases

Phase Space A - Natural Space Layer:
- Escape Y
- Gravity falloff
- Atmosphere falloff
- Space sector system
- Starfield
- Natural asteroid fields
- No derelicts or ruins yet

Implemented foundation:
- Altitude-driven near-space/space state, gravity falloff, atmosphere fade, and
  water-spread ceiling at the atmosphere boundary.
- Deterministic sector features and sparse natural asteroid fields above the
  atmosphere.
- Space HUD status, starfield, and a configurable space far plane that extends
  visibility without increasing chunk streaming radius.
- Background terrain prepass skips space chunks, and `--space-test-start`
  starts near a resource-bearing natural feature for profiling.

Phase Space B - Asteroid Mining:
- Voxel asteroids
- Ore generation
- Ice asteroids
- Crystal asteroids
- Zero-G mining
- Space resource progression

Started foundation:
- Asteroids now use append-only space resource block IDs:
  `core:space_rock`, `core:rich_metal_ore`, `core:aether_crystal_ore`, and
  `core:compressed_ice`.
- Metal-rich, crystal, ice, comet, ring-debris, and plain asteroid clusters
  bias their material distribution differently so mining targets have clear
  resource identity.
- Large anchor rocks can contain deterministic cavern pockets, giving early
  zero-G mining spaces without adding ruins or active civilization content.
- Existing machines now have data-driven Phase B recipes: rich metal ore can be
  milled into common metals, aether crystal ore extracts `core:aether_crystal`,
  compressed ice crushes into `core:cryo_ice`, and the press can combine rich
  metal with aether crystal into `core:space_alloy_ingot`.

Next systems before visuals:
- Add a block loot/drop contract so mined asteroid blocks can drop refined
  resource items instead of only themselves.
- Add scanner/navigation metadata for asteroid feature type and resource
  richness.
- Add zero-G traversal/mobility rules around asteroid chunks.

Phase Space C - Dead Moons:
- Small landable moons
- Weak gravity
- No atmosphere
- Crater terrain
- Rare minerals
- Moon caves

Started generation foundation:
- `SpaceEnvironment` now emits rare deterministic `Moon` and `Planet`
  features alongside asteroid fields and other natural features.
- A planet sector now behaves like a tiny deterministic local system: one
  planet descriptor, one to three companion moon descriptors, and sometimes a
  ring-debris feature.
- Planet and moon descriptors carry deterministic body classes and physical
  hints. Current classes are dead rocky, frozen, metal-rich, volcanic, toxic,
  ocean, gas giant, life-bearing, and crystal. These drive later planet-local
  terrain and already bias moon resource generation.
- Planet and moon descriptors also carry generation profile hints: landability,
  surface roughness, ocean/ice coverage, resource richness, and life signal.
  These keep the future landing generator data-driven instead of re-deriving
  planet behavior from raw feature type.
- `Moon` features are voxel-generated when streaming chunks intersect them:
  rough dead-rock sphere, crater cuts, cave pockets, and sparse ice/metal/
  crystal resource distribution. Moon roughness and resources now come from
  the deterministic moon profile instead of fixed thresholds.
- Landable `Planet` features now generate first-pass voxel world bodies when
  streamed chunks intersect them. Gas giants remain descriptor-only. This is a
  bridge toward the full planet-local generator: roughness, resource richness,
  ocean coverage, life signal, and body class drive temporary material bands
  such as polar caps, dry ocean basins, volcanic fields, crystal crust, and
  life-bearing grass/snow regions. Oceans stay non-fluid for now so water
  cannot spread into space.

Phase Space D - Player Space Infrastructure:
- Player-built space platforms
- Player docking
- Artificial gravity blocks
- Oxygen systems
- Ship power
- Asteroid base support

Phase Space E - Life and Dead Planets:
- Planet definitions
- Landing transition
- Dead planets
- Life planets
- Alien biomes
- Alien creatures on specific worlds

Phase Space F - Rare Ancient Content:
- Ancient ruins
- Buried vaults
- Artifact systems
- Rare derelict ships
- Extinct civilization lore

Keep ancient and derelict content late so it stays special.
