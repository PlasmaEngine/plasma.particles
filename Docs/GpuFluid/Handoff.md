# GpuFluid: state and open items (handoff, 2026-11-14)

GpuFluid is a 3D GPU fluid system in plasma.particles:
- sparse, VDB-style gas (smoke, fire, explosions), rendered through the engine's VDB volume march;
- APIC/FLIP liquid with a surface-nets surface (a room that floods).

Read these first:
- `DesignPlan.md`: the design of record.
- `PlanAmendments.md`: changes after Fable evaluation 1. They override the design plan where the two differ.

## Repositories
**Engine:** PlasmaEngine2 on Gitea, branch `feature/gpu-fluid`. It adds:
- **E1**: `plVDBVolumeGpuRenderData`, so the VDB pass, fog pass and path tracer can draw GPU-written atlas and indirection textures;
- **E2**: the test project `Data/UnitTests/GameEngineTest/FluidGallery`;
- **E5**: the GPUParticleSDF binding fix;
- a Metal timestamp fence fix.

The ABI epoch is 8 (`SdkVersion.cpp`), so every package must be rebuilt against this engine.

**This package:**

| Branch | What it holds |
|---|---|
| `feature/gpu-fluid` | Gas track; liquid merged in up to P7 |
| `feature/gpu-fluid-liquid` | Liquid track up to P8 stage A |
| `wip/gpu-fluid-p2-fixes` | Edits cut off by session restarts; never built or tested; review before use |
| `wip/gpu-fluid-p8b-flood` | Same as above |

## Phase state
**Done:**
- P0: hand-off sphere.
- P1: gas on a fully allocated box.
- P3:
  - multigrid pressure with a block red-black smoother: 1.77 ms per tick at 1024 bricks in Release, against 6.2 ms with SOR;
  - MacCormack, vorticity, wind;
  - analytic and moving colliders;
  - physics bodies.
- P6: liquid core, with MGPCG pressure and a dam break within 4% of Martin & Moyce (using that paper's time alignment).
- P7: level set, surface nets, liquid pass.
- P8 stage A:
  - level-set classification fixes the random-seed tank;
  - a divergence-target density correction;
  - Medium tier caps;
  - the liquid pass costs nothing when there is no liquid.

**P2 (sparse allocation):**
- Built: priority buckets, a deterministic scan allocator, freeing, the neighbour table, the brick view, N1, N11, faster kernels, the closed-box mass fix.
- Release timing at Medium:
  - 4.48 ms/frame at one tick per frame (gate: 5 ms);
  - 8.35 ms/frame at two ticks per frame (the test harness runs a fixed 1/30 s step).
- The review fixes below are open.

**P8 stage B (flooding room):** open. A partial attempt is on `wip/gpu-fluid-p8b-flood`: static SDF bake, sleep, probes.

**After that:**
- **P7b:** the G1 slice:
  - a world module with CPU velocity sampling;
  - a `ParticleBehavior_GpuFluidVelocity` node;
  - an editor-placeable volume and emitter;
  - an embers clip.
- P4 fire.
- P5 tiers and budget.
- P9 bodies and buoyancy.
- P10 whitewater and underwater.
- P11 particle nodes.
- P12 polish.
- P13 production.
- The four hero demos in `PlanAmendments.md`.

**Never run:** DX12 and Vulkan. Offline shader compiles for both pass on the Mac (`Utilities/GpuFluid/compile_offline.sh`). Run both galleries on both backends first, and watch for:
- typed RGBA16F 3D UAV loads;
- push-constant blocks;
- an RWByteAddressBuffer passed as a function parameter;
- the 2D-folded indirect arguments.

## Open review findings: P2

**Major**
1. **The brick view never draws bricks kept only by flux, proximity or idleness.** Their brick-table A is 0, so the march skips them.
   - Fix: in brick-view mode, give every active brick a non-zero A.
   - Fix: tell the buckets apart with distinct colours.
2. **Hero demo (a) "Moving Emitter":** when the brick overlay turns on, the smoke nearly disappears.
   - The ground shadow still shows the plume, so the view path and the shadow path read different data.
   - Fix: find the root cause, or composite the edges over the real atlas.
   - Then re-render.
3. **The gate miss needs its A1.1 follow-up.**
   - Fix: rewrite the tier rows and cost model from the P2 CSVs (brick cap, tick rate, V-cycles, velocity MacCormack per tier).
   - Fix: state which tier settings meet 5 ms/frame.
4. **A full pool starves sources permanently.** Nothing evicts idle or proximity-only bricks for bucket-0 requests; the moving-emitter scene gets there in about 25 s.
   - Fix: evict for bucket-0 requests.
   - Fix: add a test where a source enters a full pool.
5. **Sparse volumes get no mass correction.** Only the AllocateAll box does.
   - Fix: add a sparse-vs-whole-box twin of Smoke Column and compare mass and image.
   - Fix: count the mass that freeing drops.

**Minor**
- N11 cannot catch slot-order dependence; add a run with a different slot history.
- GasAlloc needs a texture UAV barrier on the indirection texture before DEBUG and after INIT (a DX12/Vulkan hazard).
- Only new bricks' aprons are filled after allocation; halo the neighbours too.
- Finalize timing claim: time Finalize alone at 1 and 2 ticks per frame.
- Stamp the real build configuration into the timing CSV headers.

## Open review findings: P8 stage A

**Major**
- **The Medium liquid timings (8.9 ms) do not change with the substep count, so they do not measure the work.**
  - Re-measure against an independent GPU clock: command buffer GPU start and end times, Xcode Metal System Trace, PIX on DX12.

**Minor**
- **The two-sided density correction packs a settled pour about 2% tighter.**
  - Fix: find the cells near solids that read ρ < 1 at rest.
- **τ = 0.37 was tuned for one fill level.**
  - Fix: add random-seed hydrostatic variants with the top layer 0.25 and 0.75 full.

## Engine bugs worked around on Metal (worth checking elsewhere)
- Virtual shadow maps draw a dark line in inside corners. The liquid scenes use cascaded shadows.
- The adaptive voxel OIT draws magenta pane edges. The liquid scenes use sorted transparency.

## Working rules (the user's)
**Commits**
- One line, prefixed `!F`, `!B`, `!R` or `!N`.
- No co-author trailer; never mention an agent, AI or Claude.
- Stage explicit paths only: `Bin/` and `Intermediate/` are not ignored.

**Phases**
- Every phase ends with an mp4 video. Stills are taken from that mp4.
- Timing comes from Release builds only.
- DX12 and Vulkan shader compiles run every phase.
- Fable evaluates against the goals at milestones.
- Reports stay honest.

**Progress page:** https://claude.ai/artifact/1dxUFAJjjU9HaBCx8fsKyT, holding the plan, status board, videos and evaluation.
- Read it before republishing, then republish to the same URL.
- Each mp4 must be under 15 MB.
- Videos also go to a shareable "GpuFluid Videos" folder and get posted in chat.
