# GpuFluid utilities

Helpers for running the GpuFluid tests on a Mac and turning their frames into videos.

| File | What it does |
|---|---|
| `run_gallery.sh` | Copies `ParticlesTest` next to the engine binaries, deletes the cached gas shader permutations (`GpuFluid/*` and `GpuFluid/Gas/`, never `GpuFluid/Liquid/`; a changed shader is not picked up from them), and runs the tests headless on Metal into an output folder. |
| `make_video.swift` | Encodes folders of numbered PNG or JPG frames into one H.264 mp4. A `title.txt` in a folder captions that clip. Copied from the ClothLab tools. |
| `imgdiff.py` | Mean, max and changed-pixel difference of two PNGs or two folders of PNGs, optionally as a difference image. |

## Build

```
export PATH=~/.local/bin:$PATH DOTNET_ROOT=~/PlasmaTools/dotnet
W=~/.t3/worktrees/PlasmaEngine/gpu-fluid
cd <this package>
PLASMA_SDK_ROOT=$W PLASMA_PACKAGE_PATH=~/PlasmaPackages arch -arm64 plasmabuild build ParticlePlugin -p Mac -c Development --no-color
PLASMA_SDK_ROOT=$W PLASMA_PACKAGE_PATH=~/PlasmaPackages arch -arm64 plasmabuild build ParticlesTest -p Mac -c Development --no-color
```

The engine needs `Player` and `RendererTest` built in `$W` first (`plasmabuild build Player -p Mac -c Development`).

## Run

```
Utilities/GpuFluid/run_gallery.sh ~/FluidLab/runs/P0/run1                      # every "GpuFluid Gallery" sub-test
Utilities/GpuFluid/run_gallery.sh ~/FluidLab/runs/P0/run1 "GpuFluid Gallery"   # same, filter given
PL_GPUFLUID_VIDEO=1 Utilities/GpuFluid/run_gallery.sh ~/FluidLab/runs/P0/video  # also records the video frames
```

Shots go to `<out>/GpuFluidGallery/`, the N0 numbers to `<out>/GpuFluidGallery/N0_Handoff.csv` and the console to
`<out>/console.log`. The script prints the N0 lines and the sub-test results. Always pass a filter: the older particle
image tests need a Jolt fixture this Mac lacks.

## Video

```
cd Utilities/GpuFluid
VIDEO_WIDTH=960 VIDEO_BITRATE=1250000 swift make_video.swift ~/FluidLab/videos/P0-handoff-sphere.mp4 30 <out>/GpuFluidGallery/HandoffSphere
rm <out>/GpuFluidGallery/HandoffSphere/frame_*.png
```

Keep each mp4 under 15 MB, and delete the frames once encoded: disk is tight.

## Sub-tests (P0)

- **N0 Handoff**: the compute-written analytic sphere (`plGpuFluidGasVolumeComponent`, RGBA16F atlas and brick table,
  handed over as `plVDBVolumeGpuRenderData`) against the same sphere baked like the VDB importer does (RGBA8, runtime
  `plVDBVolumeResource`). Checks the GPU's own counts (bricks kept = bricks the bake kept, nothing denied, no NaN) and
  compares the two renders inside the sphere's footprint against the noise floor of two captures of the same volume.
- **Handoff Sphere Video**: with `PL_GPUFLUID_VIDEO=1`, 150 frames of the pulsing GPU sphere beside the baked one, under
  a slow camera arc. Without it the sub-test passes as skipped.

## Sub-tests (P1)

- **Smoke Column**: the gas solver on a fully allocated 4 x 4 x 8 m box at 6.25 cm (8 x 8 x 16 = 1024 bricks), open on
  every side but the floor, fed by a warm sphere source; 300 ticks (150 frames, two 1/60 s ticks each). Checks:
  - N12: the first (fresh-resource) frame publishes nothing; the NaN and overflow counts are 0 on every readback;
  - every brick of the box is simulated, density stays within its clamp;
  - mass grows every half second over the first four seconds (the source is on throughout);
  - the density-weighted centroid rises every quarter second over the first two seconds;
  - the projection cuts the largest |div u| by at least 10x on every readback after the first half second.
  Stills `SmokeColumn_f030/060/090/120/149.png`, every readback in `SmokeColumn.csv`.
- **Smoke Column Video**: with `PL_GPUFLUID_VIDEO=1`, 300 frames (10 s) under a slow camera arc, into
  `GpuFluidGallery/SmokeColumn/`. Without it the sub-test passes as skipped.
- **Gas Safety** (N12 stress): the smoke column with its source driving the faces at 48 m/s, 90 frames. The 32 m/s
  clamp must fire (`speed_clamped` > 0), the overflow and NaN counts stay 0, and every readback's speed is finite. On
  frames 45 to 47 `GpuFluid.Debug.InjectNaN` writes a NaN into the density at the source: the NaN count must go above
  0 for those frames and back to 0 from ten frames later. Every readback in `GasSafety.csv`.

## Sub-tests (P3)

- **N2 Divergence**: the Smoke Column with the Medium multigrid (two full V-cycles per tick):
  L_inf(div after) / L_inf(div before) must stay below 1e-2 on every readback after the first half second. A second
  phase with 12 V-cycles prints the half-precision floor of the stored velocity. `N2_Divergence.csv`. Every other P3
  scene holds the same ratio on its readbacks above 10 1/s (the "N2 divergence ratio" lines): N4, N9, Gas Wind and the
  two videos. (`GpuFluid.Gas.MGShortCycles true`, a second cycle that stops at 4 cubed, misses it under wind: 0.0124.)
- **N3 Poisson**: a manufactured problem with a known solution on 64 cubed (8 x 8 x 8 bricks), every face open, from a
  cold start each frame (`plGpuFluidGasVolumeComponent::m_DebugPoissonTest`). The residual must fall at least 5x per
  V-cycle after the first while above the float floor (`N3_Poisson_Cycles.csv`); then multigrid V-cycles and red-black
  SOR iterations are swept against the GPU time of Gas/Pressure (`N3_Poisson.csv`), and each stage of a V-cycle is timed
  on its own (the "N3 profile" line). No MGPCG rows: the gas pressure is multigrid only; MGPCG is the liquid's solver.
- **N4 Gas Mass**: a closed 2 m box, no dissipation, a source for 0.5 s, 300 ticks; the largest mass drift after 0.7 s
  must stay within the tolerance stored in the test (the measured run plus 50%). `N4_GasMass.csv`. Since P2 the defaults
  are Medium's (velocity semi-Lagrangian) with the closed-box mass correction: largest drift 0.0044. **N4 Gas Mass
  Velocity SL** (`GpuFluid.Gas.VelocityMacCormack 0`, no correction: +16.5%) and **N4 Gas Mass Velocity MacCormack**
  (`1`, no correction: +59.5%) hold the advection's own conservation to its measured values. See
  `s_fN4MassTolerance` for why: the atlas writes truncate on this Mac (now rounded by hand), and the advection gains
  gas in a stirred closed box.
- **N9 Colliders**: a plume centred under a sphere collider, no source noise: the centroid stays within a voxel of the
  middle in x and y, the solid has the sphere's cell count, gas reaches it and gets past it, and the gas the solid
  deletes from its cells stays below 1e-3 of what the source emitted (the "lost to the solids" line; the `emitted` and
  `solid_cleared` CSV columns). **N9 Moving Collider**: the Smoke Around Obstacles scene without recording frames; the
  sweeping box may delete at most 2% of the emitted gas. `N9_MovingCollider.csv`.
- **Gas Wind**: the Smoke Column under a 2 m/s `plSimpleWindWorldModule` wind: the ambient velocity matches and the
  plume bends over.
- **Corridor Smoke Video**, **Smoke Around Obstacles Video**: with `PL_GPUFLUID_VIDEO=1`, 300 and 240 frames into
  `GpuFluidGallery/CorridorSmoke/` and `GpuFluidGallery/SmokeObstacles/`. Look values: `PL_GPUFLUID_TUNE_*` (see the
  scene setups in `GpuFluidGasSolverTests.cpp`).

Gas solver CVars for comparisons (all read every frame): `GpuFluid.Gas.PressureSolver` (0 red-black SOR, 1 multigrid),
`GpuFluid.Gas.PressureCycles`, `GpuFluid.Gas.MGShortCycles`, `GpuFluid.Gas.MGPreSmooth`, `GpuFluid.Gas.MGPostSmooth`,
`GpuFluid.Gas.MGCoarseSweeps`, `GpuFluid.Gas.MGFinalSweeps`, `GpuFluid.Gas.MGDenseGroupCells`,
`GpuFluid.Gas.VelocityMacCormack` (-1 the quality's default: High and up, 0 off, 1 on), `GpuFluid.Gas.MassCorrection`,
`GpuFluid.Gas.ExactSampling`, `GpuFluid.Gas.Vorticity`, `GpuFluid.Gas.VorticityRate`, `GpuFluid.Gas.ProfilePressure`
(each step of Gas/Pressure in a pass of its own), `GpuFluid.Gas.ProfileKernels` (each kernel of Gas/Advect and
Gas/Forces in a pass of its own; the split itself costs about 0.1 ms per pass). They can be given on the command line
after the filter, e.g. `run_gallery.sh out "^N2 Divergence$" -GpuFluid.Gas.PressureCycles 1` (in zsh, split a variable
holding several with `${=args}`).

## Sub-tests (P2)

The grid is sparse unless a volume sets `AllocateAll` (the closed-box mode; the P1 and P3 scenes do, so their numbers
stay those of the whole box). The pool is the quality's cap (Low 512, Medium 1536, High 3072, Ultra 6144) unless
`MaxBricks` is set.

- **N1 Allocation**: a sparse 12 x 12 x 16 m plume box at 12.5 cm (2304 virtual bricks). Every frame the allocator's
  state is read back (`m_bDebugGridReadback`) and checked: page table and active list agree and hold each slot once,
  active + free = pool, A > 0 exactly for allocated bricks with content, every neighbour the last Finalize asked for is
  allocated unless the frame denied a request. Then the source stops and the gas fades fast: 60 ticks later no brick
  may be left. `N1_Allocation.csv` (the `alloc_*` and `denied_b*` columns).
- **N1 Allocation Cap**: the same plume in a 64 brick pool: no NaN or overflow, the pool fills, requests are denied, and
  a bucket is only denied once every request of every later bucket was (bucket 3 first). `N1_AllocationCap.csv`.
- **N1 Allocation Evict**: the plume in a 96 brick pool; once the pool is full the source travels 9 m along +x into
  bricks without a slot. The allocator must evict bricks no source overlaps (idle first, then proximity only, flux,
  content) so that no source request is denied and every frame still emits at least 90% of the median emission before
  the pool filled; the N1 invariants hold on every readback. The `alloc_evicted` and `mass_dropped` CSV columns
  (`N1_AllocationEvict.csv`) count the evicted slots and the gas they still held.
- **N11 Determinism**: the plume five times, hashed every frame in virtual brick order (GasHash): twice with the
  defaults and a 1024 pool, then with `GpuFluid.Gas.ExactSampling` and pools of 1024 and 512, and once more with exact
  sampling, the 1024 pool and the free slots handed out in descending order (`m_bDebugReverseSlots`), so every brick,
  and every slot freed and taken again, lands in another slot than in the ascending run. Each pair must match on every
  frame. Hardware filtering differs in the last bits between pools (the atlas coordinate's rounding depends on the
  slot), so cross-pool determinism needs exact sampling, which costs about 3.6 times the advection. This is an open
  item against the design's N11 (the default mode is deterministic per pool and slot layout only).
- **Smoke Column Sparse**: the Smoke Column twice with the same seed and ticks, the whole box allocated and then sparse.
  The mass curves must agree within 2.4% until the plume reaches the open top (frame 110) and within 14% over the
  run, the last images within 0.95% of white on average (the measured run plus 50%: 1.6%, 9.3% and 0.48-0.63%).
  `SmokeColumnSparse.csv`, `SmokeColumnTwin_Whole.png`, `SmokeColumnTwin_Sparse.png`. The difference is the flow, not
  lost gas: freeing drops nothing here (the plume only grows), but an unallocated neighbour is open air (p = 0, the
  ambient wind), so the sparse plume's boundary is a free surface a brick or two from the gas instead of simulated
  still air; its centroid rises up to 0.18 m slower and less of it reaches the open top. Gas advected into an
  unallocated neighbour is not counted anywhere (the flux lookahead requests those bricks a frame ahead).
- **Moving Emitter Video**: with `PL_GPUFLUID_VIDEO=1`, hero demo (a): a source driving across a 64 x 48 m courtyard at
  25 cm (6144 virtual bricks, Medium at 30 Hz, a pool of 640 so it fills during the clip and the source's bricks
  evict others), walls, pillars, a gatehouse and a well as colliders, a 1.5 m/s wind; `GpuFluid.Debug.Bricks` draws
  the brick edges into the gas from 4.5 to 10 s and `GpuFluid.Debug.BrickStats` shows the allocation. No source
  request may be denied. 450 frames into `GpuFluidGallery/MovingEmitter/`.

Freeing and eviction: a slot is freed after 8 allocation steps without a request in any bucket. When the sources'
pending bricks (bucket 0) outnumber the free slots, GasAlloc evicts slots of bricks no source overlaps for them, lowest
class first (idle, proximity only, flux, content) and in slot order within a class, so a full pool never starves a
source. The gas a freed or evicted brick still held at its last Finalize is counted in `plGpuFluidGasStats::m_fMassDropped`.

Debug CVars: `GpuFluid.Debug.Bricks` (the brick view: the gas's density without its glow, and the edges of every
allocated brick drawn in as a faint gas glowing in its bucket's colour: orange a source's, green content, cyan flux,
blue proximity, grey idle; every allocated brick is marched in this view, also those without content),
`GpuFluid.Debug.BrickStats` (the allocation as on-screen text), `GpuFluid.Debug.BrickLines` (the bricks as debug line
boxes, for the editor; on Metal the VDB march then draws no gas in that view).

## Timing (`GpuFluid Tier Timing`)

```
Utilities/GpuFluid/run_gallery.sh ~/FluidLab/runs/P1/timing "GpuFluid Tier Timing"
CONFIG=Release Utilities/GpuFluid/run_gallery.sh ~/FluidLab/runs/P1/timing-release "GpuFluid Tier Timing"   # the comparable numbers
```

One sub-test per manually picked tier (`Gas Low`, `Gas Medium`, and since P3 `Gas Medium SOR`, Medium with the P1
red-black SOR pressure solver for comparison; the "pressure" lines give Gas/Pressure per tick) on the smoke column, with
`Rendering.Pipeline.Injections` off as the baseline and `Rendering.Profiling.GPUStats` on. 60 warm-up and 180 measured
frames; the median and p90 of every `Gas/*` phase per frame and per run (a tick phase runs once per tick, Finalize once
per frame), µs per brick and run, the tick phases together per tick, and the whole gas frame. The CSV
(`tier-timing-<tier>.csv`) goes to `$W/Output/GpuFluid/` and `<out>/GpuFluid/`, followed by every other GPU pass of the
last frame for scale. Needs the engine's Metal timestamp fence (feature/gpu-fluid 37a09cea); without it Metal reports a
fraction of each pass. Release numbers need Release builds of the engine and the package.

**Gas Medium Sparse** (P2, the performance gate of plan amendment A1.1): the sparse gate scene (`SetupGateScene`, a
16 x 16 x 8 m box at 6.25 cm, two plumes, the Medium pool of 1536) after 150 warm-up frames; the bricks are the mean
over the measured frames, logged with their min and max ("active bricks over the measured frames"). The scene's look
values are `PL_GPUFLUID_TUNE_GATESOURCES`, `GATERADIUS`, `GATEDISSIPATION`, `GATECOOLING` and `GATESPACING`. One tick
per frame instead of two: `-GpuFluid.Debug.StepTime 0.0333334` (the sub-test then fails its two-ticks check). The P2
Release CSVs are in `Timing/`.

The smoke scenes set a few VDB march CVars for their duration (twelve full-resolution self-shadow taps, no ambient
occlusion, half the sky term) and put them back afterwards. Look values can be tried without a rebuild, for instance:

```
PL_GPUFLUID_TUNE_BUOYANCY=8 PL_GPUFLUID_TUNE_FRAMES=300 Utilities/GpuFluid/run_gallery.sh ~/FluidLab/runs/P1/try "^Smoke Column$"
```

The names are in `Tune(...)` calls in `GpuFluidGalleryTest.cpp`. The gallery window is 1280 x 720 (written to the run's
appdata, so the shared FluidGallery project stays at 960 x 540).

## Liquid gallery (`GpuFluid Liquid Gallery`, P6 to P8)

The liquid track runs its test binary as `ParticlesTestLiquid` next to the engine binaries (so it never replaces the
gas track's `ParticlesTest`) and clears the GpuFluid shader cache before each run:

```
cp Bin/Tests/Mac_x64_Development/ParticlesTest $W/Binaries/Mac_Development/ParticlesTestLiquid
find $W/Binaries/Mac_Development/ShaderCache -type d -name GpuFluid -prune -exec rm -rf {} \;
cd $W/Binaries/Mac_Development && DYLD_LIBRARY_PATH="$PWD:<this package>/Bin/Mac_x64_Development" PL_SILENT_ASSERTS=1 \
  ./ParticlesTestLiquid -run -close -noGui -noSave -all -assert 0 -renderer Metal -outputDir <out> -filter "GpuFluid Liquid Gallery"
```

A filter can name one sub-test by a part of its name ("Random Seed", "Medium Room"). Release: the same with
`Mac_Release` and `Mac_x64_Release` (only the package's Release dylib is loaded then; `DYLD_PRINT_LIBRARIES=1` shows it).

| Sub-test | Checks |
|---|---|
| Dam Break | N5 (alive == seeded - drained, volume measure +-3% every tick, rest depth +-3% with the slosh energy taken out and +-5% raw, centroid steps), N2 every tick, N7 against Martin & Moyce |
| Hydrostatic Tank | N6 at a stratified seed with forced substep changes |
| Hydrostatic Tank Random Seed | N6 with every particle anywhere in the box: max \|v\| < 1 cm/s at tick 300 and over the last 60 ticks |
| Pour Into Glass | inflow count, containment, settled depth, N2, a surface mesh every tick |
| Medium Room | the plan's Medium reference load (128 x 96 x 64 at 6.25 cm, 393K particles) on the tier defaults: per-phase GPU time (`MediumRoom_timings.csv`), N2 percentiles, the 3-substep cap |
| No Liquid | the liquid pass runs none of its GPU passes in a view without liquid |
| Dam Break Video, Pour Into Glass Video | frames with `PL_GPUFLUID_VIDEO=1` |

Solver values can be tried without a rebuild through `PL_GPUFLUID_LIQ_<NAME>` (see `ApplyTuningOverrides` in
`GpuFluidLiquidGalleryTest.cpp`), for instance `CYCLES` (MGPCG iterations), `FORCE_SUBSTEPS` (a fixed substep count),
`LIQUID_PHI` (tau of the level-set classification), `LEVEL_SET_ONLY`, `DENSITY_GAIN`, `TWO_SIDED`, `VOLUME_GAIN`,
`PROFILE=1` (P2G's level set and the solve's setup and projection in passes of their own, so each is timed),
`SURFACE_OWN_LEVEL_SET=1` (P7's extra sort for the surface), `RENDER_MODE=1` with `TOP_SPEED` (speed-coloured points).
Timing CSVs of each phase end are in `Timing/` (`P8-release-liquid-*.csv`).

## Offline compile for Vulkan and DX12

`compile_offline.sh <work dir>` compiles every GpuFluid shader (top level, `Gas`, `Liquid`, all permutations) with
the engine's `ShaderCompiler` for VULKAN and DX12 on this Mac (plan amendment A6) and prints one summary line per
platform; the logs stay in the work dir. Build the engine's `ShaderCompiler` target first.
