# Plan amendments (override design-plan.md where they differ)

## A1: Fable evaluation 1 (2026-11-13)
Source: `fable-eval-1.json`. Scores: G1 1, G2 3.5, G3 3, G4 2, G5 1.5; about 2.5 overall.

1. **Gas order and performance gate**
   - Order: P3 (finish MG + solids, commit) → **P2 sparse allocation right after** → performance gate → P4 fire.
   - Gate:
     - Release engine + package dylibs, the other track idle.
     - Medium gas at a 1536-brick cap with the pool 60% full must measure **≤ 5 ms/frame** before any fire work.
     - If it misses, rewrite the tier table from measurements. Do not carry the estimates.
2. **Liquid**
   - Level-set classification (liquid if count > 0 or φ < 0) lands in P7.
   - "Hydrostatic Tank Random Seed" is **mandatory and ungated** before P8.
   - Replace the rate-based density correction with a proper divergence target, so packing does not depend on substeps.
   - Bound the solve:
     - measure the PCG iterations needed for residual < 1e-3;
     - use a small fixed count, about 3-4 PCG iterations with 1 V-cycle each, instead of 8 V-cycles per substep;
     - cap Medium at 3 substeps, using the slow-motion fallback.
   - Re-measure at the Medium room size (128×96×64, 400K particles) before P8.
3. **G1 slice pulled forward**, right after P3/P7 (new phase **P7b**):
   - `plGpuFluidWorldModule` with CPU `SampleGasVelocity` and `SampleLiquidVelocity` from per-brick mean readbacks;
   - a `ParticleBehavior_GpuFluidVelocity` graph node (drag towards the fluid velocity) for CPU particle systems;
   - an editor-placeable volume and emitter with gizmo and key properties;
   - a 20 s "embers and sparks carried by the smoke" clip.
4. **Four hero demos** are the video priorities. Other gallery scenes are test artefacts, with no polished video needed.
   - (a) **Smoke Around Obstacles + Moving Emitter**: brick overlay, a 64 m domain (sparse + colliders).
   - (b) **Flooding Room**: rendered water, furniture from Jolt collision meshes, a doorway, a drain.
   - (c) **Campfire to Inferno**: emission and fire light.
   - (d) **Particle-effect-driven**: a flamethrower, or a sprinkler against fire.
5. **Hygiene**
   - Merge the liquid branch into `feature/gpu-fluid` at each liquid phase end, and do not let it drift.
   - Commit at every working step; never leave a phase with uncommitted shaders.
   - Run scripts clear the whole GpuFluid shader cache every run (a race between tracks is acceptable).
6. **Portability step every phase**
   - Compile every GpuFluid shader for **VULKAN and DX12 on this Mac** with the engine's offline ShaderCompiler tool (`plShaderCompilerDX12/Vulkan.dylib` are built here).
   - Record the result in the phase report.
   - Add the small tests for the "unverified" items (open faces, drains, obstacles, bounds growth, E5) in the phase that touches them.
7. **Release timing is the only timing**
   - At each phase end run Tier Timing on Release engine + package dylibs, with the other track idle if possible.
   - Commit the CSV.
   - Record measured µs per brick-step and ns per particle-substep.
8. **Visual fixes before the fire video**
   - E4: smoke shadows onto opaque surfaces.
   - Plume-top streaking: check the velocity advection on the open top face; MacCormack velocity on Medium+.
   - Fix AO brick seams in the march instead of overriding CVars per test.
