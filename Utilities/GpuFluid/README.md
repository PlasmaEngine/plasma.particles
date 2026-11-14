# GpuFluid utilities

Helpers for running the GpuFluid tests on a Mac and turning their frames into videos.

| File | What it does |
|---|---|
| `run_gallery.sh` | Copies `ParticlesTest` next to the engine binaries, deletes the cached GpuFluid shader permutations (a changed shader is not picked up from them), and runs the tests headless on Metal into an output folder. |
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
