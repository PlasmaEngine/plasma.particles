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
  - N12: the first (fresh-resource) frame publishes nothing; the NaN count is 0 on every readback;
  - every brick of the box is simulated, density stays within its clamp;
  - mass grows every half second over the first four seconds (the source is on throughout);
  - the density-weighted centroid rises every quarter second over the first two seconds;
  - the projection cuts the largest |div u| by at least 10x on every readback after the first half second.
  Stills `SmokeColumn_f030/060/090/120/149.png`, every readback in `SmokeColumn.csv`.
- **Smoke Column Video**: with `PL_GPUFLUID_VIDEO=1`, 300 frames (10 s) under a slow camera arc, into
  `GpuFluidGallery/SmokeColumn/`. Without it the sub-test passes as skipped.
- **Gas Timing**: 60 warm-up and 180 measured frames of the smoke column with `Rendering.Profiling.GPUStats` on; the
  median and p90 of every `Gas/*` phase (summed over the frame's two ticks) go to `GpuFluidGallery/gas-timing.csv`,
  followed by every other GPU pass of the last frame for scale. Needs the engine's Metal timestamp fence
  (feature/gpu-fluid 37a09cea); without it Metal reports a fraction of each pass.

The smoke scenes set a few VDB march CVars for their duration (twelve full-resolution self-shadow taps, no ambient
occlusion, half the sky term) and put them back afterwards. Look values can be tried without a rebuild, for instance:

```
PL_GPUFLUID_TUNE_BUOYANCY=8 PL_GPUFLUID_TUNE_FRAMES=300 Utilities/GpuFluid/run_gallery.sh ~/FluidLab/runs/P1/try "^Smoke Column$"
```

The names are in `Tune(...)` calls in `GpuFluidGalleryTest.cpp`. The gallery window is 1280 x 720 (written to the run's
appdata, so the shared FluidGallery project stays at 960 x 540).
