# GpuFluid performance research (2026-11-14)

This doc collects published techniques for a performance pass on GpuFluid, and says what each would change in our design. It draws on `DesignPlan.md` sections 3 to 6 and 9 to 10, `Handoff.md`, the Gas and Liquid shaders, `GpuFluidGasSolver.cpp`, and the public NVIDIA Flow source (BSD-3, `NVIDIA-Omniverse/PhysX/flow`).

**How to read the numbers.**
- "Expected gain" is an estimate, either from the cited work or from fetch and dispatch counts of our kernels. It is not a measurement.
- Our baseline timings come from an RTX 4080 that another app was using at about 44% (Handoff). Absolute numbers may be inflated by up to about 2×. The ratios between passes are more trustworthy.
- **[unverified]** means I could not check the claim against a primary source in this session, or it rests on my reading of our code without profiling.

## Diagnosis from reading our code (do this first)

A 4080 has about 700 GB/s. ~1000 bricks is 512K interior cells. One 7-point sweep over 512K fp32 cells moves about 10 to 15 MB, so it should take **about 15 to 25 µs**. Our timings are 10 to 50 times what bandwidth alone predicts. That points to structure, not arithmetic:

1. **Pressure is probably dispatch- and barrier-bound** [unverified, needs a per-dispatch capture].
   - Each V-cycle records about 11 per-colour brick dispatches (fine, mid and coarse, each split by brick colour).
   - The dense levels add about 10 dispatches each: 2×2 pre-smoothing half-sweeps, restrict, prolong, 2×2 post-smoothing.
   - One single-group `DENSE_CYCLE` dispatch then runs the whole tail of the cycle on **one SM**.
   - Every dispatch is followed by a full UAV barrier. That gives roughly 30 to 40 serialized dispatches per cycle and 60 to 80 per tick.
   - Each barrier drains the GPU, so at a ~20 to 40 µs floor per dispatch, 3 ms is explained without any of the arithmetic.
   - Dense level D0 covers the **whole virtual brick grid**, not just the active bricks.
2. **Advection over-fetches** (read from `GasAdvect` and `GpuFluidSparse.hlsli`).
   - Velocity is MAC-staggered inside one RGBA16F texel. Each velocity sample is therefore 3 full 8-byte trilinear fetches, and each uses one channel. 3D trilinear on a 64-bit format runs at roughly quarter rate.
   - The RK2 trace takes 2 velocity samples, so tracing each face costs 6 fetches. Velocity advection is about 21 fetches per cell, the scalars about 7.
   - MacCormack repeats the trace and adds 8 `Load`s per limited field.
   - Samples that leave the home brick take a page-table lookup and a dependent fetch.
   - In total that is about 90 texture operations per cell.
3. **Aprons and halo passes.**
   - A 10³ slot holds 8³ cells, so 49% of every atlas texel is apron. Each atlas costs 1.95× its interior in memory and in bandwidth.
   - A tick runs about 5 `GasHalo` passes (after Emit, SL, MC, Curl and Forces/Project). Each rewrites ~488 texels per brick per atlas.
4. **Liquid P2G gather scales with particles per cell squared, locally.**
   - Each cell thread walks the 3×3×3 cell neighbourhood for its 3 faces, so it loads 27 × ppc particles of 44 B.
   - At 4 to 8 ppc that is 5 to 10 KB of particle reads per cell, and every particle is read about 27 times.
   - Over-packed cells push this to quadratic blow-up. That is the 125 ms P2G seen in the Flooding Room test.

**Action:** take one PIX (DX12) or Nsight Graphics GPU Trace capture of a Medium tick on an otherwise idle GPU before changing anything. Check the barrier count, per-dispatch duration and occupancy. Items 1 and 2 decide which of the rows below come first.

## Prioritized table

| # | Technique | Bottleneck | Source | Change to our design | Expected gain | Effort | Risk |
|---|---|---|---|---|---|---|---|
| 1 | Fewer, fatter MG dispatches: multi-sweep smoothing in groupshared with a 2-cell halo, sparse coarse levels over active bricks only, no single-group tail | Gas pressure (dispatch/barrier bound) | McAdams, Sifakis, Teran, "A parallel multigrid Poisson solver for fluids simulation on large grids", SCA 2010, https://doi.org/10.2312/SCA/SCA10/065-074 ; NVIDIA Flow `Pressure.cpp` (sparse levels, 1 pre + 1 post Jacobi, 4 at coarsest), https://github.com/NVIDIA-Omniverse/PhysX/tree/main/flow | Fuse pre-smoothing, residual and restriction into one launch over all bricks (overlapping-halo block Jacobi or Chebyshev; no colour split). Stop dense levels at an *active-brick* sparse level (one more 2× level inside the page table), and run the tail from a small dense level using a few groups, not one | Pressure 2 to 4× (3.0 → ~0.8 to 1.5 ms) [estimate] | M | Brick-face error that the colour split was added to avoid; overlap plus coarse correction should hide it. Verify against the residual |
| 2 | One V-cycle, warm start, Jacobi or Chebyshev smoother instead of RBGS | Gas pressure | Flow source (one V-cycle with Jacobi; 40-iteration Jacobi when MG is off); Adams, Brezina, Hu, Tuminaro, "Parallel multigrid smoothing: polynomial versus Gauss–Seidel", J. Comput. Phys. 2003, https://doi.org/10.1016/S0021-9991(03)00194-3 | Medium: 1 V(1,1) or V(2,2) warm-started (we already warm start). Chebyshev degree 2 to 3 inside a groupshared tile is order-independent and colour-free | Halves pressure by itself; stacks with #1 | S | Smoke tolerates residual divergence; a closed box shows mass drift. Keep 2 cycles for closed-box mode |
| 3 | Velocity and pressure at half the density resolution ("coarse velocity"), density advected at full resolution | Gas pressure, advect, forces | Flow settings `densityCellSize` ("velocity cell size is twice density cell size"), https://docs.omniverse.nvidia.com/extensions/latest/ext_fluid-dynamics/settings.html ; Lentine, Zheng, Fedkiw, "A novel algorithm for incompressible flow using only a coarse grid projection", SIGGRAPH 2010, https://doi.org/10.1145/1778765.1778851 | A second, coarser brick grid (velocity brick = 2× density brick in world size) with its own page table, or 4³ velocity cells per 8³ density brick. Density and temperature backtrace through a trilinear sample of the coarse velocity. Vorticity confinement or curl noise restores fine motion | Velocity, forces and pressure work ÷8 by cell count; whole gas tick ~3 to 5× [estimate] | L | Loses sub-brick swirls, so look-dev must accept it. Changes the MAC/allocator layout. This is the biggest structural lever and the main reason Flow-class cost is possible |
| 4 | Velocity fetch diet: centre-averaged trace velocity, split u/v/w R16F, Euler or clamped trace on Low | Gas advect | Selle et al., "An unconditionally stable MacCormack method", J. Sci. Comput. 2008, https://doi.org/10.1007/s10915-007-9166-4 ; Flow `AdvectionCommon.hlsli` (one collocated float4 velocity fetch, single-step displacement, clamped) | (a) The trace samples a collocated velocity (one RGBA16F fetch per RK stage instead of 3). (b) Or store u, v, w in three R16F atlases, so each staggered sample is a 2-byte full-rate fetch. (c) Optionally trace once per cell centre and reuse the departure offset for the 3 faces (first-order approximation) | Velocity advection ~2 to 3× (21 → ~9 fetches per cell); with (c) ~4× | S–M | (a) and (c) add a small dissipation; check against the vortex-ring test. (b) doubles the count of texture bindings |
| 5 | Apron-free bricks: each kernel loads its 10³ (or 12³) tile into groupshared from the neighbour table, then does manual trilinear from groupshared; out-of-tile traces fall back to the page table | Gas halo passes, memory | Wu, Truong, Hoetzlein et al., "Fast Fluid Simulations with Sparse Volumes on the GPU", Eurographics 2018, https://doi.org/10.1111/cgf.13350 (GVDB bricks, apron handling) [details unverified]; Hoetzlein, "GVDB: Raytracing sparse voxel database structures on the GPU", HPG 2016, https://doi.org/10.2312/hpg.20161197 | Remove `GasHalo` (about 5 passes a tick) and store 8³ slots. The VDB renderer needs aprons for hardware filtering, so keep aprons on the **render** scalar atlas only, filled once in Finalize | Gas memory about −45%; halo passes gone (est. 15 to 30% of the tick) [estimate] | M–L | Groupshared trilinear is ALU-heavier; traces longer than the tile need the global path (rare at 60 Hz CFL ≤ 2) |
| 6 | Scatter P2G into a groupshared tile with int32 atomics, after a warp-level segmented reduction over sorted particles | Liquid P2G | Gao, Wang, Wu, Pradhana, Sifakis, Yuksel, Jiang, "GPU optimization of material point methods", SIGGRAPH Asia 2018, https://doi.org/10.1145/3272127.3275044 (PDF https://people.csail.mit.edu/kuiwu/GPU_MPM/GPU_MPM.pdf); Wang et al., "A massively parallel and scalable multi-GPU material point method", SIGGRAPH 2020, https://doi.org/10.1145/3386569.3392442 | `P2GMode=1` already exists. Make it the default once wave reduction is added (`WaveActiveSum` within a wave run of identical cell keys). **Integer atomics are order-independent, so the result stays bit-deterministic.** Our "no atomics" rule only needs "no float atomics" | P2G 3 to 10× at 4 to 8 ppc; removes the over-packed blow-up (linear instead of ~27×ppc reads) [estimate; Gao reports scatter with reduction beating gather] | M | 24 KB groupshared limits occupancy (see area 6); a 16-bit-key wave match costs a few instructions |
| 7 | Restricted tall cells for deep, calm water | Liquid solve and particles for floods | Chentanez & Müller, "Real-time Eulerian water simulation using a restricted tall cell grid", SIGGRAPH 2011, https://matthias-research.github.io/pages/publications/tallCells.pdf ; Irving, Guendelman, Losasso, Fedkiw, "Efficient simulation of large bodies of water by coupling two and three dimensional techniques", SIGGRAPH 2006, https://doi.org/10.1145/1141911.1141960 | Per column: one tall cell below a band of N regular cells around the surface (N about 8 to 12). The pressure stencil changes at the tall/regular junction (the paper gives the operator and a dedicated multigrid). Particles only in the band (as in #8) | For a 1 to 2 m deep flood at 4 cm (25 to 50 layers): cells and solve 2.5 to 5×; much less for 0.5 m | L | New discretisation, and ghost-fluid and solids must work on tall cells. Gives little when the water is shallow compared with the band |
| 8 | Narrow-band FLIP (particles only within ~3 to 4 cells of φ=0; grid-advected interior) | Liquid particle cost | Ferstl, Ando, Wojtan, Westermann, Thuerey, "Narrow Band FLIP for Liquid Simulations", Eurographics 2016, https://doi.org/10.1111/cgf.12825 | Already planned for High (P12). Bring it to Medium for the flood preset; it pairs with #7 | Particles 3 to 5× fewer in deep water (paper reports large particle reductions) [factor unverified] | M | Interior level-set drift. Needs the φ re-seed rules from the paper |
| 9 | Larger liquid CFL plus fewer sorts | Liquid substeps | Bridson, *Fluid Simulation for Computer Graphics*, 2nd ed. 2015 (FLIP is stable at CFL > 1); Gao 2018 (sort-free substeps, re-bin only) | Raise the substep CFL from 1.5Δx to about 3Δx with RK2 particle advection and SDF push-out. Fully re-sort the payload every 2 to 4 substeps; in between, update only `cellStart` and a cell-key index (4 B per particle) | Substeps about ½ (2 to 5 → 1 to 3); sort cost about −50% | S | Thin walls (0.5·dx minimum) could be tunnelled; clamp the displacement per substep |
| 10 | Shallow-water or height-field layer for large flooded floors | Flooding room | Chentanez & Müller, "Real-time simulation of large bodies of water with small scale details", SCA 2010, https://doi.org/10.2312/SCA/SCA10/197-206 [DOI from memory, unverified]; Irving et al. 2006 (above) | A 2D SWE grid (cell ~4 to 8 cm) for spreading sheet flow, with 3D FLIP only at inflows, falls and bodies, and two-way coupling at the band | A flood floor becomes a 2D cost: 30K columns × a few µs-scale passes, i.e. **≪ 1 ms** [estimate] | L | Coupling seams are visible; no overturning in SWE regions. This is a gameplay design choice as much as a solver one |
| 11 | MGPCG → plain warm-started MG V-cycles at game tolerance; fold the dot products into the V-cycle kernels | Liquid solve | McAdams et al. 2010 (MGPCG); Flow (plain MG) | Each PCG iteration is a V-cycle plus 2 tree reductions (3+ dispatches each). Try 2 to 3 warm-started V-cycles on Medium and compare volume loss | Solve 1.5 to 2× [estimate] | S | The free surface converges worse with plain MG; keep MGPCG on High |
| 12 | Half-precision storage of pressure, residual and MG levels (fp32 math) | Gas and liquid bandwidth | Flow `enableLowPrecisionVelocity` (8-bit velocity and divergence), settings page above | p and r as R16F or 16-bit fixed relative to a per-brick scale; keep rhs fp32 | Bandwidth of MG ~½; helps only once dispatch-bound is fixed | S | Stagnating residual near 1e-3; fine for smoke, test on liquids |
| 13 | Dispatch fusion: Advect SL + MacCormack in one launch via groupshared φ̂; Curl + Forces + Solids in one launch; Divergence folded into Fine (already done) | Gas, all phases | Flow (AdvectionVelocity1/2 is a 2-pass MacCormack, curl and confinement are 2 passes: no better than ours here); general GPU practice | Fusion needs neighbour φ̂ values, so it depends on #5 (tile in groupshared). Forces/Curl/Solids fuse easily | 10 to 20% of the gas tick [estimate] | M | Larger kernels lose occupancy; measure |
| 14 | 30 Hz gas tick on Medium with render-side temporal smoothing | Gas, total | Design plan already uses 30 Hz on Low; general practice | Make 30 Hz the Medium default with the 2·dt CFL clamp; the VDB march interpolates or relies on TAA | Gas cost ½ | S | Fast fire looks steppy; keep 60 Hz for hero presets |
| 15 | Async compute overlap | Frame-level | Vendor guidance (AMD GPUOpen, NVIDIA "Advanced API performance: async compute and overlap", https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap/) | Run the gas tick on the async queue against shadow and depth rasterization | 0 to 30% of fluid time hidden [uncertain] | M | Our own async-compute review found no speed-up on this engine; low priority until the passes are bandwidth-bound |

S = days, M = 1 to 2 weeks, L = several weeks.

## Per-area notes

### 1. Sparse gas at game cost

**What Flow actually does** (read from the open-source shaders, `flow/source/nvflow/`):
- **Blocks.** Density blocks are 32×16×16 cells by default (16×8×8 with `enableSmallBlocks`). Velocity cells are twice the density cell size, so a velocity block holds 1/8 of the cells.
- **Velocity layout.** Velocity is **collocated** float4 (no MAC). The semi-Lagrangian step takes one velocity fetch at the cell and one clamped displacement, then one value fetch, in `semiLagrangian4f`.
- **MacCormack** is a soft blend, `predict + 0.5·blendFactor·(old − predictRev)`, gated by a threshold, rather than a full limiter.
- **Pressure** packs p and div as float2 in one 3D texture. Every level is sparse, each with its own block table. The fine levels use 1 Jacobi pre-sweep and 1 post-sweep, the coarsest 4. There is one V-cycle. Groups are 128 threads, and every kernel goes through `NvFlowSingleVirtualToReal`, a block-table indirection.
- **Velocity precision.** There is an 8-bit velocity and divergence option.

So Flow's cost model rests on four choices: coarse velocity, a single cheap V-cycle, collocated velocity, and one fetch per sample. None of them is a clever kernel. It trades accuracy for cost at each of those points (rows 2, 3 and 4).

**GVDB and Wu et al. 2018.**
- Bricks are sized for raymarching and simulation together. The pressure solve is a matrix-free multigrid-preconditioned solver on the sparse topology.
- The abstract reports large speedups over PCG and MGPCG. Brick size and apron details are **[unverified]**; I read only the abstract and metadata.
- https://www.researchgate.net/publication/325488464_Fast_Fluid_Simulations_with_Sparse_Volumes_on_the_GPU

**NanoVDB.**
- Museth, "NanoVDB: A GPU-friendly and portable VDB data structure for real-time rendering and simulation", SIGGRAPH 2021 Talks, https://doi.org/10.1145/3450623.3464653.
- It is a read-mostly, linearised VDB. It is useful as an export target (Flow builds NanoVDB for export), but it is not a simulation layout because topology changes need a rebuild. There is no change for us.

**SPGrid.**
- Setaluri, Aanjaneya, Bauer, Sifakis, "SPGrid: A sparse paged grid structure applied to adaptive smoke simulation", SIGGRAPH Asia 2014, https://doi.org/10.1145/2661229.2661269.
- It uses 4 KB pages with Morton-interleaved channels and relies on the virtual-memory hardware. The GPU lesson is channel-interleaving and page-sized blocks, so that a stencil touches few pages.
- Our 3D-texture atlas gets tiling from the hardware swizzle. However, 10³ slots do not align to power-of-two tiles, so a brick may straddle more tiles than it needs **[unverified; measure L1/L2 hit rate in Nsight]**. Apron-free 8³ slots (row 5) align naturally.

**Brick size.**
- 8³ with a 1-cell apron is the worst case for overhead (1.95×). The alternatives are 16³ with an apron (1.42×) or 8³ without one (1.0×, row 5).
- Larger bricks waste interior cells at plume edges. Flow accepts that with 32×16×16 blocks.
- **Recommendation:** keep 8³ allocation granularity, drop the apron, and let kernels assemble tiles in groupshared.

### 2. Pressure solves for realtime

**Dispatch structure is the first-order problem.** The fine-level smoother itself is a reasonable design: a 10³ groupshared tile with block red-black. The cost is in:
- splitting every level by colour, which doubles the launches;
- running dense levels over the full virtual brick grid;
- a full barrier between each step;
- the single-group tail.

**Options, in order of preference:**
- **Overlapping-halo smoothing.** Load a 12³ tile (2-cell halo) and do 2 Jacobi or Chebyshev sweeps per launch, recomputing the halo redundantly. All bricks run in one launch, with no colour split. This is communication-avoiding smoothing as in McAdams 2010's parallel MG (they use damped Jacobi and boundary GS for parallelism).
- **Chebyshev smoothers.** They need only matrix-vector products and so are order-independent and deterministic. Adams et al. 2003 show they are competitive with GS on parallel hardware.
- **Sparse coarse levels.** Continue coarsening sparsely inside the page-table hierarchy, as Flow does, instead of going dense over the virtual grid. Only switch to dense when the level is ≤ 16³, and run the tail with ~8 to 16 groups and a grid barrier through dispatch splits, not one group.
- **Budget.** A fixed 1 V-cycle plus warm start is the game norm (Flow). Pressure error then shows up as slight compressibility, which smoke hides.

**Coarse projection.** Lentine et al. 2010 project on a coarse grid and keep fine-scale velocity detail through a fine-grid correction. Flow's 2× velocity cell is the simple form of the same idea. Expect the biggest single gain (row 3).

**Adaptive or power-diagram solvers.** Aanjaneya, Gao, Liu, Batty, Sifakis, "Power diagrams and sparse paged grids for high resolution adaptive liquids", SIGGRAPH 2017, https://doi.org/10.1145/3072959.3073625. This is offline-oriented and too complex for the gain at our sizes. **Not recommended now.**

**Half precision.** Use it for storage only. It is worth doing after the dispatch fix, because only then does bandwidth matter (row 12).

### 3. Advection cost

**MacCormack versus BFECC.**
- BFECC (Kim, Liu, Llamas, Rossignac, "FlowFixer: Using BFECC for fluid simulation", Eurographics Workshop on Natural Phenomena 2005) costs 3 semi-Lagrangian steps against MacCormack's 2.
- Limited MacCormack (Selle 2008) is the cheaper of the two. **Keep MacCormack**, and consider Flow's thresholded blend, which needs no 8-texel range load if the threshold suffices visually.
- Our limiter loads 8 texels per field. HLSL has no `Gather` for 3D textures, so the way to cut that is to reuse the loads from a groupshared tile (row 5).

**Advection-reflection.**
- Zehnder, Narain, Thomaszewski, "An advection-reflection solver for detail-preserving fluid simulation", SIGGRAPH 2018, https://doi.org/10.1145/3197517.3201324.
- It advects for a half step, reflects with a projection, then advects again. That is **two pressure solves per step**, in exchange for about 100× less energy loss.
- It would let us drop vorticity confinement and perhaps MacCormack. It only pays off once pressure is cheap. **Revisit after rows 1 to 3.**

**Fetch count is the main cost.** See the diagnosis and row 4.
- Cheapest change: one centre-averaged velocity field (computed in the Project pass at no extra launch) used for every trace.
- 3D trilinear on RGBA16F is roughly ¼ rate on NVIDIA and AMD **[order of magnitude from vendor texture-rate tables; not measured]**. An R16F per component is full rate and quarter the cache footprint.

**Upres.** Advecting density at 2× the velocity resolution is row 3. Particle-based detail (e.g. wavelet turbulence) costs more than it saves for us.

### 4. Liquid at game cost

**P2G.**
- Gao 2018 establishes the GPU recipe: particles sorted by cell, a warp-level segmented reduction over runs of equal cell keys, then few atomics into a shared-memory block, then a block write-out.
- Wang 2020 extends it to multi-GPU with an "array-of-structs-of-arrays" layout.
- Our gather is deterministic but reads every particle ~27 times. Our integer fixed-point sums make **atomic scatter deterministic too**, because integer addition is associative. So the determinism argument for gather goes away.
- Fallback that keeps gather: first stage the brick's particles (plus a one-cell shell) into groupshared, so each particle is read from memory once per brick. That is a 2 to 4× gain [estimate], but groupshared size caps it at ~8 ppc.

**Over-packing.**
- The 125 ms P2G is a symptom of particles clustering. Two fixes:
  - reseeding or culling to a ppc cap per cell in the Count pass (delete or merge beyond ~2× n₀);
  - the density-correction term already in the RHS.
- Scatter makes the cost linear even when cells over-pack.

**Sorting.**
- A counting sort is the right algorithm. The cost is in moving 44 to 88 B per particle each substep.
- Sort the index (key, particle index) every substep, but move the payload only every N substeps, or when the coherence metric (mean jump distance in the sorted order) degrades. Gao 2018 keeps particle order across steps and re-bins.

**Narrow band and tall cells.**
- For a flooding room the expensive part is the **calm interior**: particles, P2G, G2P and the solve on every interior cell.
- Narrow-band FLIP removes the interior particles. Tall cells remove the interior *cells*. Together they make cost scale with surface area, not volume. Chentanez & Müller 2011 reported real-time rates on 2011 hardware with tall-cell water.
- The design plan says "pressure solve still spans every interior liquid brick" for narrow band. Tall cells are the fix for that line.

**Shallow water.**
- A floor flood, a few cm to ~30 cm deep and spreading, is the regime where 3D FLIP wastes most of its work.
- Use a 2D SWE grid (row 10), with 3D only near inflows, falls and bodies. This could take the flood from 18 ms to ~2 to 4 ms total [estimate, strongly scene-dependent].

**Sleeping and adaptive substeps.** These already exist. Make sure sleep also skips P2G and the solve on sleeping bricks, not only G2P **[not checked in code]**.

### 5. Scalability and budgeting

**Quality tiers in shipped realtime fluids.**
- Flow exposes block size, velocity precision, velocity substeps and a pool cap (settings page above). These are the same levers as our auto-scaler, plus **resolution ratio** and **precision**, which we lack. Add both.
- Unreal Niagara Fluids is positioned for cinematics and high-end rather than shipping gameplay budgets **[unverified; check the Epic docs]**.
- EmberGen is an offline-authoring realtime tool, not a game runtime budget reference.
- I found no public per-frame ms figures for Flow in shipped games **[unverified]**.

**Budget arithmetic for 1 to 2 ms of gas on mid-range hardware (~RTX 3060 / RX 6600, ~350 to 450 GB/s).**
- With row 3, 1000 density bricks means about 64K velocity cells. A V(1,1) cycle on that is about 10 dispatches of ~10 µs, once the dispatch floor is cut.
- Density MacCormack over 512K cells is about 0.2 to 0.4 ms.
- That puts 1 to 1.5 ms per tick within reach **[estimate]**. It is out of reach without rows 1 to 4.

**LOD and time-slicing.** Already planned (half rate, frozen, off-screen ¼). Add one more: **distance-based resolution ratio**, so far volumes run velocity at 4× density cells.

**Async compute.** See row 15. This engine's own review found no gain, so treat it as last.

### 6. GPU engineering specifics

**Occupancy against groupshared.**
- `GasMGFine` uses about 8 KB groupshared for 512 threads, which is fine.
- The 24 KB P2G scatter tile limits a 4080 SM (100 KB) to ~4 groups of 512.
- 8³ groups of 512 threads are big. 256-thread groups (two per brick, slab-split as advection already does) often schedule better on AMD (wave64) and Apple GPUs **[measure per platform]**.

**Wave intrinsics.**
- `WavePrefixSum` and `WaveActiveSum` cut the allocator scans and the PCG dot products to one dispatch plus a tiny top level.
- `WaveActiveSum` and `WaveReadLaneAt` give the P2G segmented reduction.
- Wave sizes differ (32 NVIDIA, 32/64 AMD, 32 Apple). Results must not depend on lane order for floats, and our integer sums are safe.
- Microsoft Shader Model 6 wave intrinsics: https://github.com/microsoft/DirectX-Specs/blob/master/d3d/HLSL_SM_6_0_WaveIntrinsics.md

**Dispatch-count reduction.**
- The big win is fewer barriers (rows 1 and 13).
- Persistent-thread megakernels with global barriers are unsafe on DX12/Vulkan/Metal without forward-progress guarantees, so avoid them. A few groups looping over a small level, with a dispatch split per sync, is the safe form.
- `DispatchIndirect` is already used well.
- Batch the per-colour dispatch pairs into one, using a colour bit in the group ID.

**RGBA16F 3D textures against structured buffers.**
- Keep textures where hardware trilinear is used (advection value fetches, rendering).
- Use structured or typed buffers with a Morton or brick-linear layout for stencil-only fields (pressure already does this).
- The MAC-in-one-texel layout is the costly middle ground: it pays texture filtering cost on four channels and uses one (row 4).

**Determinism (N11).** Exact sampling (8 `Load`s) costs about 8× the hardware fetch. Make sure it stays a test-only path, as it is today (`GpuFluid.Gas.ExactSampling` default off).

## First five things to try (gain per effort)

**Step 0 is measurement, before any of the five.** Take a PIX or Nsight capture of a Medium gas tick and a liquid substep on an idle GPU, and list the dispatches, barriers, per-dispatch µs and the top kernel's limiter. It costs about a day and decides whether #1 below is right.

1. **Cut pressure dispatches and barriers** (rows 1 and 2).
   - Merge the colour-split launches.
   - Make the dense levels sparse or active-only.
   - Replace the single-group tail.
   - Go to one warm-started V-cycle on Medium.
   - Expected 2 to 4× on Gas/Pressure. Effort S–M.
2. **Velocity fetch diet in advection** (row 4).
   - Write a collocated centre velocity in Project, and trace through it with one fetch per RK stage.
   - Optionally split u/v/w into R16F.
   - Expected 2 to 3× on Gas/Advect. Effort S.
3. **Scatter P2G with wave reduction and int32 groupshared atomics** (row 6), plus a ppc cap in Count.
   - It stays deterministic and fixes the 125 ms over-pack case.
   - Expected 3 to 10× on Liquid/P2G. Effort M.
4. **Liquid CFL to ~3Δx, and payload sort every N substeps** (row 9), plus plain warm-started MG on Medium (row 11).
   - Expected about 2× on liquid per frame. Effort S.
5. **Half-resolution velocity and pressure for gas** (row 3).
   - This is the structural step that makes a 1 to 2 ms gas budget plausible on mid-range GPUs.
   - Expected 3 to 5× on the gas tick on top of 1 and 2. Effort L.
   - Prototype it on the AllocateAll box first.

**Next after these, for the flood:** tall cells (row 7) with narrow-band FLIP on Medium (row 8). If design accepts it, add a 2D shallow-water floor layer (row 10). Those are the only items that change how flood cost scales (volume against surface or area). The five above only change the constant.
