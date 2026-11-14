GpuFluid > Editor workflow

# GpuFluid: setting up liquid and gas in a scene

GpuFluid simulates liquid (water filling rooms, pouring, draining) and gas (smoke, steam) on the GPU inside box-shaped
volumes. A volume holds the simulation; everything that adds, removes or blocks fluid is a separate component placed in
the scene. A modifier acts on every volume whose box it sits in, so nothing needs wiring up: move a source into a
volume and it pours there.

## Concepts

- **Volume.** `GpuFluid Liquid Volume` or `GpuFluid Gas Volume`. Its box (Extents) is the only place fluid can exist.
  The liquid box is always aligned to the world axes and centred on the object; the gas box follows the object's
  rotation and scale.
- **Modifiers.** Liquid: `GpuFluid Liquid Source` (pours in), `GpuFluid Liquid Drain` (takes out),
  `GpuFluid Liquid Fill` (liquid present at the start). Gas: `GpuFluid Gas Emitter`. A modifier outside every volume
  is drawn red with "Outside any liquid volume" (or gas volume) while you edit.
- **Solids.** The liquid flows around the scene's collision geometry: static physics shapes, greyboxes with Generate
  Collision, and static `GpuFluid Collider` components. These are baked into a distance field when the volume starts.
  A static collider that moves or changes bakes again by itself; for other geometry set the cvar `GpuFluid.Rebake` to a
  new value. The gas sees `GpuFluid Collider` components (static or moving) and dynamic physics bodies.
- **Sleep.** Calm liquid falls asleep brick by brick and costs nothing until something wakes it (a source, an open
  drain, a solid or gravity changing).

## Setting up a flooding room

1. Build the room with collision: greyboxes with Generate Collision on, or meshes with static physics shapes.
2. Add an object with `GpuFluid Liquid Volume`. Set Extents to cover the room (the box shows in the viewport) and
   CellSize to the detail you need (4 cm for a room, 6.25 cm for larger spaces).
3. Add an object with `GpuFluid Liquid Source` where the water comes in. Turn it so its arrow (+X) points the way the
   water should leave. Set Radius and Speed; leave FlowRate at 0 to pour the disc's area times the speed (the label
   shows the litres per second), or set the litres per second yourself.
4. Add a `GpuFluid Liquid Drain` where the water leaves: a thin box on the floor, or a box through a wall. An open drain
   is drawn filled dark.
5. Press Play or Simulate. Water pours, finds its level and runs out through the drain.
6. To check the solids, switch the viewport's render mode to **Fluid > Liquid Static Solids**: every object the liquid
   flows around is drawn in colour. Geometry missing there has no collision, so water passes through it.

## Starting with water already there

Add a `GpuFluid Liquid Fill` and size its box over the area to fill. It is seeded when the simulation starts or
resets; moving or resizing it in the editor restarts the volumes it overlaps, so the viewport shows the starting state.

## Flooding large rooms: flood basins

Particles everywhere cost too much for a big room (a 15 x 15 m hall knee-deep is 112 m^3 of water, millions of
particles). A `GpuFluid Flood Basin` keeps such water as a level instead: the volume poured in, spread over the room's
free floor, with a simulated wave surface on top. Particle water is kept for where the water really moves in 3D, such as a
burst pipe's jet, and drains into the basin where it lands.

1. Add an object with `GpuFluid Flood Basin` over the room. Its box's bottom is the floor; size Extents to the room's
   inside and as tall as the water may get. Set InitialLevel for water present at the start.
2. Furniture, pillars and a raised floor take their share of the room when they have collision (the same geometry the
   liquid bakes): water rises faster around them and stops at a stage it does not reach.
3. Sources in the box pour straight into the basin. For a visible jet, put the source inside a `GpuFluid Liquid Volume`
   that covers the stream down to the floor: its water joins the basin below the basin's level.
4. Drains in the box empty it while open (an orifice: discharge coefficient x area x sqrt(2 g head)).
5. Rooms next to each other are a basin each. A `GpuFluid Flood Opening` box across the doorway (overlapping both basins;
   its bottom is the sill) lets the higher water flow into the lower until they stand level. Open closes it (a door).
6. Waves come from jets landing, moving `GpuFluid Collider`s crossing the water (a floating crate, a character's legs) and
   streams through openings, and reflect off walls and furniture. WaveStrength, WaveDamping and MaxWaveHeight tune them.

Gameplay reads the water with `GetWorldLevel()`, `GetLevel()` and `GetVolume()`; `AddVolume()` and `AddWave()` change it.
The stats `GpuFluid/Basin/<object name>/Level` and `Volume` show it. While editing, the basin draws its level and volume.

Not simulated: water spreading across a dry floor (the basin rises everywhere at once), breaking waves, water flowing over
water, and an underwater view. The sample project `GpuFluidFloodHall` shows a 15 x 15 m hall and a side room flooding to
about knee height.

### GpuFluid Flood Basin

| Property | Default | Description |
| --- | --- | --- |
| Extents | 15 x 15 x 2 m | The room; the box's bottom is the floor. |
| ColumnSize | 0.05 m | Wave grid resolution (also of the solid profile). |
| InitialLevel | 0 m | Water depth at the start and after a reset. |
| DrainCoefficient | 0.6 | Discharge coefficient of its drains and openings. |
| WaveDamping | 0.003 | Share of the wave motion lost per step. |
| MaxWaveHeight | 0.12 m | Waves are clamped to it. |
| WaveStrength | 1 | Scales every wave source. |
| Absorption, ScatterColor, SurfaceRoughness, RefractionStrength | flood water | The look, as on a liquid volume. |

### GpuFluid Flood Opening

| Property | Default | Description |
| --- | --- | --- |
| HalfExtents | 0.3 x 0.6 x 1 m | The opening; it must overlap two basins. Its bottom is the sill, its top caps the flow. |
| Open | on | Whether water passes (plMsgSetPlaying too). |

## Smoke

1. Add an object with `GpuFluid Gas Volume` and size its Extents.
2. Add a `GpuFluid Gas Emitter` inside it. Shape Sphere or Box; Velocity is in the emitter's space (its default pushes
   up). Several emitters can feed one volume.
3. Solids: `GpuFluid Collider` components in the box; moving ones push the gas.

## Gameplay

Sources, drains and gas emitters react to the `plMsgSetPlaying` message: play starts a source or emitter and opens a
drain, stop does the reverse. The `Flowing`, `Open` and `Emitting` properties are the same switches.

## Debug views

The viewport's render mode menu has a **Fluid** group. Each mode affects only that viewport; the cvars do the same in
every view, including the Player and shipped builds' console.

| Render mode | Cvar | Shows |
| --- | --- | --- |
| Liquid Particles | `GpuFluid.Liquid.RenderMode 1` | particles coloured by speed instead of the surface (the cvar also stops building the surface) |
| Liquid Particles And Surface | `GpuFluid.Liquid.RenderMode 2` | particles inside the surface |
| Liquid Static Solids | `GpuFluid.Liquid.ShowSolids 1` | the triangles the solids bake caught, one colour per object |
| Liquid Sleep | `GpuFluid.Liquid.ShowSleep 1` | particles orange when awake, blue when asleep |
| Gas Bricks | `GpuFluid.Debug.Bricks 1` | the gas bricks allocated, edges coloured by what keeps them |

`GpuFluid.Liquid.DebugTopSpeed` sets the speed (m/s) at the top of the particle colour ramp (deep blue, cyan, white);
`GpuFluid.Liquid.DebugPointRadius` the particle size. The stats `GpuFluid/Liquid/*` (particles, substeps, active,
awake and sleeping bricks, solid cells) are in the engine's stats.

## Component reference

### GpuFluid Liquid Volume

| Property | Default | Description |
| --- | --- | --- |
| Extents | 4 x 3 x 2 m | Size of the box, rounded up to whole bricks of 8 cells. |
| CellSize | 0.0625 m | Grid cell size. Halving it costs about eight times as much. |
| ParticlesPerCell | 4 | Particles per full cell (4 or 8). |
| MaxParticles | 1M | Particle budget; sources stop adding when it is full. |
| Quality | Medium | Substeps and pressure iterations of the tier. |
| OpenFaces | none | Sides of the box the liquid can leave through. |
| BakeStaticSolids | on | Bake the scene's collision geometry into solids. |
| AllowSleep | on | Calm bricks fall asleep. |
| Absorption, ScatterColor, SurfaceRoughness, RefractionStrength | clear water | The look of the surface. |
| FlipBlend, MaxSubsteps, PressureCycles, WallFriction | tier | Solver settings; 0 takes the quality tier's. |
| AlwaysSimulate | on | Off: a volume nobody has looked at for 30 frames holds still. |

### GpuFluid Liquid Source

| Property | Default | Description |
| --- | --- | --- |
| Radius | 0.05 m | Radius of the nozzle disc, across the object's +X. |
| Speed | 2 m/s | Speed of the water leaving it along +X. |
| FlowRate | 0 l/s | Litres per second; 0 pours the disc's area times the speed. |
| Flowing | on | Whether it pours. |

A volume takes up to four sources; the disc centre must be inside the volume.

### GpuFluid Liquid Drain

| Property | Default | Description |
| --- | --- | --- |
| HalfExtents | 0.2 x 0.2 x 0.05 m | Half size of the box. A rotated drain uses its world-axis bounds. |
| Open | on | Whether it takes liquid out. |

### GpuFluid Liquid Fill

| Property | Default | Description |
| --- | --- | --- |
| HalfExtents | 0.5 m | Half size of the box. A rotated fill uses its world-axis bounds. |
| Velocity | 0 | Starting velocity of the liquid, in the object's space. |
| Jitter | 1 | 0 places particles on a regular lattice, 1 anywhere in their sub-cell. |
| Stratified | on | One particle per sub-cell; off: anywhere in the cell. |

### GpuFluid Collider

| Property | Default | Description |
| --- | --- | --- |
| Shape | Box | Sphere, Box, Capsule (along +Z) or Plane (solid below the object, against +Z). |
| HalfExtents / Radius / HalfHeight | 0.5 m | The sizes the shape uses (the others are hidden). |
| Static | off | Never moves. Static colliders are baked into the liquid's solids; moving ones affect only gas. |

### GpuFluid Gas Emitter

| Property | Default | Description |
| --- | --- | --- |
| Shape | Sphere | Sphere or Box (in the gas volume's axes). |
| Radius / HalfExtents | 0.5 m / 0.4 x 0.4 x 0.2 m | Size of the shape. |
| Softness | 0.2 m | Width of the soft edge. |
| Rate | 5 /s | Density added per second. |
| Temperature | 1 | Heat added (makes the gas rise). |
| Velocity | 0, 0, 2 m/s | Velocity it drives the gas with, in the object's space. |
| Noise, NoiseScale | 0.9, 0.3 m | Break-up of the emission into puffs. |
| Emitting | on | Whether it emits. |

## Troubleshooting

| Problem | Cause and fix |
| --- | --- |
| Water passes through a wall or furniture | It has no collision. Check with Liquid Static Solids; add collision or a static GpuFluid Collider, then rebake (`GpuFluid.Rebake`). |
| A modifier is drawn red | It is outside every volume's box. Move it in or grow the volume. |
| A source pours nothing | Its disc centre is outside the volume, Flowing is off, the volume already has four sources, or MaxParticles is reached. |
| The water stops moving after a while | It fell asleep (check with Liquid Sleep). Anything that disturbs it wakes it. |
| Water in slow motion | The jet is faster than the substeps allow; raise MaxSubsteps or lower the speed. |

See also: `Handoff.md` (state of the work), `DesignPlan.md` (how the solver works).
