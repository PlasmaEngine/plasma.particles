# GpuFluid: a 3D GPU fluid system for plasma.particles. Final architecture and phase plan

Base design: proposal **engine**. Grafts come from **scale** (numerics, allocator, budget, tests) and **look** (visuals, demo checks). Every judge must-fix item is resolved here.

## 0. Facts this plan relies on (checked in source, gpu-fluid @ 2ca1bb20)

| # | Fact | Consequence |
|---|---|---|
| F1 | plasma.fluid is installed on this Mac and has a macOS runtime component. | All names use the `GpuFluid` prefix and never start with `Fluid`. This covers RTTI names (`plGpuFluid*`), shaders (`Shaders/GpuFluid/`), permutation vars (`GPU_FLUID_*`), cvars (`GpuFluid.*`), stats (`GpuFluid/`) and graph nodes (`…_GpuFluid*`). |
| F2 | Frame CB slots 1, 10 and 12 are free; slots 0, 2–9, 11 and 13–15 are used. | `plGpuFluidFrameConstants` takes **slot 12**, and only once a material samples the fluid (P13). Solver constants use `CONSTANT_BUFFER2(…, 2, BG_DRAW_CALL)`. |
| F3 | Baked brick tables are `R8G8B8A8_UNORM` (`VDBVolumeAsset.cpp:173,258`). Brick mean is floored at 1/255 (`VDBVolumeAsset.cpp:141`, `Max(1u, …)`). The shader decodes `round(rgb*255)` and treats `a>0` as occupied. | The sim writes its own **RGBA16F** indirection: RGB = atlasBrick/255 (exact under half precision), A = max(mean, floor). Baked wisps do not vanish. |
| F4 | At lod≥1 the sampler returns `float4(A,0,0,0)`, so temperature and emission are lost on coarse taps and froxels. | Fire volumes force `plVDBVolumeRenderMode::RayMarch`. A uses the same units as atlas R. The floor is `1e-4 × preset density scale`. |
| F5 | The atlas cap is **204 bricks/axis**: 2048 texels / 10 stored voxels. | The pool atlas is sized within this cap. |
| F6 | Three passes resolve VDB bindings: `VDBVolumePass` (:312–337), `VolumetricFogPass` (:156–165) and `PathTracingPass` (:905, which locks `binding.m_hVolume` of slot 0). | E1 routes all three through one helper. |
| F7 | A runtime `plVDBVolumeResource` is not forced immutable, since only the loader sets `m_bImmutable`. Its textures are still created with init data, and it holds one texture per frame slot. | It works only as a fallback, with a copy from the ping-pong atlas. E1 is done in P0. |
| F8 | `plJoltStaticActorComponent::OnMsgExtractGeometry` answers `ExtractWorldGeometry(…, CollisionMesh)` (:287–300). | The static SDF needs no engine hook and no Jolt adapter. |
| F9 | `GetObjectGravity` does not exist. Gravity comes from `plPhysicsWorldModuleInterface::GetGravity()` (`Core/Interfaces/PhysicsWorldModule.h:70`). The Jolt update counter exists only on `plJoltWorldModule`. | Impulses are applied every PreAsync frame as F·Δt, with no Jolt link. |
| F10 | PreRender render graphs run before the views (`RenderGraphManager.cpp:799`). | Results appear in the same frame, barriers are tracked, and each pass is timed automatically. |
| F11 | `plGPUParticleRenderData` is package-owned and takes raw GAL buffers. | Whitewater and the debug point view reuse the existing renderer and OIT. |
| F12 | `XRRenderPipeline` has no `plVDBVolumePass`. | Gas in XR renders through froxels only. This is documented, not fixed. |
| F13 | Disk has **11 GB** free. | Encode each sequence, then delete its frames. Each mp4 stays under 15 MB. |

## 1. Module layout

Package `plasma.particles`, folder `Source/ParticlePlugin/GpuFluid/`. Every `.cpp` gets `PL_STATICLINK_FILE` and an entry in the PCH reference block.

| File | Classes and role |
|---|---|
| `GpuFluidDeclarations.h` | `plGpuFluidQuality` (Low, Medium, High, Ultra), `plGpuFluidKind`, and the versioned structs `plGpuFluidGasSettings` and `plGpuFluidLiquidSettings` |
| `Resources/GpuFluidPresetResource.{h,cpp}` | `plGpuFluidPresetResource`, creatable at runtime for tests. Presets: Smoke, Campfire, Inferno, Explosion, Steam; Water, Muddy Water |
| `Components/GpuFluidGasVolumeComponent` | Extents, voxel size, preset, quality override, open or closed per face, priority, LodDistance, AlwaysSimulate, debug view. Emits `plVDBVolumeGpuRenderData`. |
| `Components/GpuFluidLiquidVolumeComponent` | Box extents (dense domain), dx, preset, initial fill box, open or closed faces, AlwaysSimulate (default **true**), debug view |
| `Components/GpuFluidEmitterComponent` | Sphere, Box, Cylinder, Cone or Nozzle. Gas: density, temperature, fuel, velocity, curl-noise jitter, or a `plVDBVolumeResource` source. Liquid: inflow (L/s) or drain. |
| `Components/GpuFluidColliderComponent` | Analytic box, sphere, capsule or plane; static flag; friction. Dirty-gated by `HasMovedSince`. |
| `Components/GpuFluidBodyComponent` | Buoyancy opt-in: density override, Cd, linear and angular damping |
| `Components/GpuFluidFireLightComponent` | P12 polish. Drives a point light from the hot-brick centroids. |
| `World/GpuFluidWorldModule` | PreAsync, before Jolt (priority > −100000), runs in the editor. Gathers inputs into a double-buffered `plGpuFluidFrameInput`, runs physics queries, applies buoyancy, keeps the sim tick and `ResetSimulationClock()`. Implements `plGpuFluidWorldModuleInterface` with CPU `SampleGasVelocity(pos)` and `SampleLiquidVelocity(pos)`. Never implements `plWaterWorldModuleInterface`. |
| `Gpu/GpuFluidGpuManager` | One per render world. Uses the BeginRender handler and builds **one PreRender graph per frame**. Follows the retire-not-free rule and publishes nothing on a frame with fresh resources. |
| `Gpu/GpuFluidSparseGrid` | Page table, request masks, scan allocator, active list, neighbour table, indirect arguments |
| `Gpu/GpuFluidScan` | Shared block scan and indirect-argument writer (folds counts above 65535 into 2D) |
| `Gpu/GpuFluidPressure` | Sparse MG for gas; dense MG/MGPCG for liquid; fixed-order reductions |
| `Gpu/GpuFluidGasSolver`, `Gpu/GpuFluidLiquidSolver` | The two solvers |
| `Gpu/GpuFluidSolids` | Static SDF bake and dynamic shape list |
| `Gpu/GpuFluidSurface` | Level set, smoothing, surface nets |
| `Gpu/GpuFluidWhitewater` | Spray, foam and bubble pool in the 64 B `plGPUParticle` layout |
| `Gpu/GpuFluidReadback` | Fence-retired ring of 3 in flight: stats, bounds, buoyancy rows, splash events, coarse velocity |
| `Gpu/GpuFluidBudget` | Tier tables, cost model, auto-scaler |
| `Rendering/GpuFluidLiquidPass` | `: plForwardRenderPass`, at BeforeTransparent, `m_fOrder` after plasma.water |
| `Rendering/GpuFluidUnderwaterPass` | At AfterTransparentResolved |
| `Rendering/GpuFluidFrameDataExtractor` | Registered through `m_ExtractorTypes` |
| `Behavior/ParticleBehavior_GpuFluidVelocity`, `_GpuFluidSource`; `Emitter/ParticleEmitter_GpuFluidEvents` | Particle graph nodes, discovered through reflection |

**Shaders** live in `Assets/Shaders/GpuFluid/`:
- `GpuFluidConstants.h` (`PL_SHADER_STRUCT`);
- `GpuFluidSparse.hlsli` (page lookup, neighbour loads, MAC sampling);
- `GpuFluidScan.hlsli`;
- `GpuFluidWaterOptics.hlsli` (optics extracted from plasma.water, generalised from Z-up);
- `Gas*.plShader`, `Liquid*.plShader`, `Surface*.plShader`;
- `PermutationVars/GPU_FLUID_{GAS,LIQUID,SURF}_PASS.plPermVar`, `GPU_FLUID_MACCORMACK`.

**Editor:** the "GpuFluidPreset" asset lives in `EditorPluginParticle`.

**Tests** live in `Source/ParticlesTest/GpuFluid/`, with unity build off: `GpuFluidGalleryTest.cpp`, `GpuFluidNumericTests.cpp`, `GpuFluidTierTiming.cpp`. Data is in `Data/UnitTests/GameEngineTest/FluidGallery/`.

**Scripts** live in `Utilities/GpuFluid/`:
- `run_gallery.sh`, which deletes `ShaderCache/METAL/Shaders/GpuFluid`, swaps in Release dylibs for timing, and sets `PL_GPUFLUID_VIDEO=1`;
- `make_video.swift` (AVFoundation, PNG to mp4, deletes the frames);
- `imgdiff.py`.

## 2. Engine branch `feature/gpu-fluid` (small, general)

| ID | Change | Size |
|---|---|---|
| **E1** (P0) | `plVDBVolumeGpuRenderData : plVDBVolumeRenderData`, a derived class so the base layout and baked volumes are untouched. It carries `m_hTexture`, `m_hIndirection` (GAL), `m_vBrickGrid`, `m_vAtlasBricks`, `m_BoundsUVW`, `m_fVoxelSize` and `m_uiResolution`. The extractor `plDynamicCast`s it and fills `plPerVDBVolumeData`. `plVDBVolumeBinding` gains `m_hGpuTexture` and `m_hGpuIndirection`, plus `plVDBVolumeBinding::ResolveTextures(hTex, hInd)`, used by VDBVolumePass, VolumetricFogPass, the shadow-moment path and PathTracingPass. PathTracingPass uses the GPU handles when `m_hVolume` is invalid and never locks an invalid handle. Bump the `-dev` ABI tag. | about 150 lines |
| **E2** (P0) | Test project `Data/UnitTests/GameEngineTest/FluidGallery/`, copied from WaterGallery. Data only. | data |
| **E3** (P13, optional) | Publish the `Velocity` stream at BeforeTransparent so the liquid can write motion vectors. | small |
| **E4** (P12, optional) | VDB moment shadows readable from `Lighting.h`, so smoke shadows opaque surfaces. | small |
| **E5** (P0, bug fix) | `GPUParticleSDF.plShader:14` declares `SceneDepth` while the pass binds `ParticleSceneDepth` (`GPUParticlePass.cpp:322`). Rename it to match, in whichever repository owns the file. | 1 line |

No "software scene wanted" hook is needed (F8).

**Fallback if E1 slips by more than a day:** a runtime `plVDBVolumeResource` with an RGBA16F atlas and indirection created with `m_bAllowUAV=true`, `m_bImmutable=false` and no mips, zeroed by compute before the first publish. A copy pass moves the current ping-pong side into it each frame. Bounds stay at the whole grid. Before relying on this, verify in P0 that the creation path accepts UAV textures.

## 3. Sparse gas grid

**Brick layout**
- 8³ cells with a 1-voxel apron, stored as 10³, matching `VDB_BRICK_*`.
- A pool index *i* maps to atlas brick `(i%Ax, (i/Ax)%Ay, i/(Ax·Ay))`, with each axis ≤ 204.
- Atlases are created at the tier maximum when the volume is activated, then never resized. Auto-scaling lowers a cap inside the pool.

**Virtual grid**
- Fixed in world space per volume. Size = authored extent / (8·voxel), at most 128³ bricks (1024³ voxels).

| Resource | Format | Size |
|---|---|---|
| `PageTable` | `RWStructuredBuffer<uint>`: pool index or `0xFFFFFFFF` | 4 B × virtual bricks (8 MB at 128³) |
| `Indirection` | `Texture3D` RGBA16F, UAV, virtual-grid sized | 8 B × virtual bricks |
| `RequestMask[4]` | `RWByteAddressBuffer`, 1 bit per virtual brick per priority bucket | 4 × 256 KB at 128³ |
| `FreeMask` | 1 bit per pool slot | — |
| `BrickMeta` | `{uint virtualPacked; half maxDensity, maxTemp, maxSpeed, mean; uint idleSteps}` | 16 B × pool |
| `ActiveList`, `NewList` | `uint` pool indices | 4 B × pool |
| `NeighborTable` | 27 × `uint` per active slot (~0 when missing) | 108 B × pool |
| `IndirectArgs` | `uint3` × 8 kernels | — |
| `VelocityAtlas[2]` | `Texture3D` RGBA16F. **MAC-staggered inside the texel**: x = u at the i−½ face, y = v at j−½, z = w at k−½, w = scratch | 8 KB/brick each |
| `ScalarAtlas[2]` | RGBA16F: R density, G temperature normalised to [0,1] (preset T_max), B fuel, A burn rate. This is the VDB atlas, with channels Density\|Temperature | 8 KB/brick each |
| `ScratchAtlas` | RGBA16F: MacCormack φ̂ and curl | 8 KB/brick |
| `EmissionAtlas` (fire presets, P12) | RGBA16F, VDB_CHANNEL_EMISSION | 8 KB/brick |
| `Pressure`, `Divergence`, `Residual` | `RWStructuredBuffer<float>`, 512 per slot, no apron | 6 KB/brick |
| `MG` intra-brick levels | 4³ + 2³ + 1 per slot × {p, r, rhs} | 0.9 KB/brick |
| `MGBrickGrid` | Dense R32F over the virtual brick grid × {p, r, rhs}, then dense halvings down to 4³ | ≤ 3 × 8 MB |
| `Flags` | uint per 4 cells (solid, inside-collider) | 0.5 KB/brick |
| `Stats` | 256 B: bounds min/max (uint atomics), active, free, denied, NaN count, overflow, mass (fixed point) | 256 B |

**Memory:** about 47 KB per brick (55 KB with emission).
- Medium, 1536 bricks: 72 MB.
- High, 3072 bricks: 144 MB.
- Ultra, 6144 bricks: 289 MB.

**Marking (face flux and proximity, not 26-neighbour dilation).** A brick requests neighbour *n* across a face, edge or corner when either:
- content (max of density, temperature or fuel above `GpuFluid.Gas.AllocThreshold`, default 1e-3) lies within 2 voxels of the shared boundary; or
- outward face velocity × dt × 2 reaches into *n*.

Requests go into priority buckets:

| Bucket | Meaning |
|---|---|
| 0 | Emitter overlap (from the CPU bin list) |
| 1 | Self, currently alive |
| 2 | Flux into neighbour |
| 3 | Proximity only |

**Allocation, deterministic.** All atomics are order-independent `InterlockedOr` and `InterlockedAdd` on counts.
1. **Free.** A brick with `idleSteps ≥ 8` and no bucket-0/1 request is freed: its page entry is cleared and its free bit set.
2. **Count.** Popcount each bucket's request words for unallocated bricks only, then run a block scan over the 4 buckets concatenated in bucket order.
3. **Assign.** A scan over `FreeMask` gives the k-th free slot to the k-th request. Requests take slots in bucket order, then virtual index, so **denial hits bucket 3 first, then 2**. Bucket 0 is denied only when the pool is exhausted. The denied count goes to Stats and `GpuFluid/BricksDenied`.
4. **Clear new bricks.** All **10³** texels in every atlas, including the apron (a reused slot keeps stale data), plus the pressure and MG entries. Velocity is set to ambient wind, scalars to 0.
5. **Rebuild.** Compact `ActiveList`, rebuild `NeighborTable`, write `IndirectArgs`, and write the `Indirection` texels for changed bricks.

Slot assignment is deterministic, so the state hash can be taken on atlases directly. N5 still hashes in virtual order, so it stays valid across pool sizes.

**Halo rule.** An explicit `GasHalo` kernel fills the apron from neighbour interiors through `NeighborTable`. Where a neighbour is missing:
- scalars get boundary values (0);
- velocity gets ambient wind on open faces, or zero normal velocity on closed faces.

Passes never update a field in place where neighbours read its apron: either they ping-pong, or they write interiors only and are followed by a halo pass. Fused scattered-apron writes (scale's ~30% bandwidth saving) are a P12 optimisation behind the N10 seam test.

## 4. Gas solver: passes in order

The gas runs at a fixed tick of 1/60 s (1/30 s on Low). At most 2 ticks run per frame; beyond that the sim runs in slow motion. `GpuFluid.Debug.StepTime` overrides the tick.

Groups are `[numthreads(8,8,8)]`, one group per active brick, through `DispatchIndirect`, unless noted. There is one render-graph pass per **phase**, so there is one Metal timestamp per phase. Dispatches inside a phase are separated by explicit `BufferBarrier` and `TextureBarrier` calls; these are no-ops on Metal.

**Phase Gas/Alloc**

| # | Kernel | Groups | Notes |
|---|---|---|---|
| G0 | `GasMark` | per brick | Writes requests from the last Finalize maxima |
| G1 | `GasEmitterMark` | 64 threads × emitter bricks | CPU-binned (brick, source) list, ≤ 8K pairs |
| G2 | `GasFree` | 64 threads, 64 bricks per group | Idle-age logic |
| G3 | `ScanBlocks` / `ScanTop` / `ScanApply` | 256 threads | Over 4 × 64K words at the maximum grid size |
| G4 | `GasAssign` | 256 threads | k-th free slot |
| G5 | `GasBuildLists` | 256 threads | Active list, neighbour table, indirect args (2D fold) |
| G6 | `GasClearNew` | per new brick | Full 10³ clear |

**Phase Gas/Emit**

| # | Kernel | Notes |
|---|---|---|
| G7 | `GasEmit` | One group per (brick, source) pair. Smooth falloff with curl-noise jitter. Adds density, temperature, fuel and velocity, plus `divSrc`. Sources can sample a baked `plVDBVolumeResource` (`SampleVDBVolume`). Particle splats from `_GpuFluidSource` go into a fixed-point accumulator (`RWByteAddressBuffer`, 2¹⁶ scale, ≤ 16K splats/frame/volume) and are resolved here. Then `GasHalo`. |

**Phase Gas/Advect**

| # | Kernel | Notes |
|---|---|---|
| G8 | `GasAdvectVelocity` | RK2 backtrace. Each MAC component is sampled with hardware trilinear at its half-cell offset (3 fetches). A backtrace into an unallocated brick reads ambient wind. With MacCormack: φ̂ goes to Scratch, then `GasHalo(Scratch)`, then a correction pass writes VelocityAtlas[next], clamped to the min/max of the 8 source texels. |
| G9 | `GasAdvectScalars` | Same scheme for scalars. Semi-Lagrangian on Low, MacCormack from Medium. **Density is clamped to the preset's `DensityMax`** after correction, so `m_fMaxDensity` stays a true majorant. |
| — | `GasHalo` | Both atlases |

**Phase Gas/Forces**

| # | Kernel | Notes |
|---|---|---|
| G10 | `GasCurl` | Medium and up: curl into Scratch, then `GasHalo(Scratch)` |
| G11 | `GasForces` | Buoyancy (α·(T−T_amb) − β·ρ)·(−ĝ), with ĝ from `GetGravity()` (default −Z). Vorticity confinement ε·Δx·(N×ω), with ε of 0.3 for smoke and 0.6 for fire. Wind from `plWindWorldModuleInterface`. Force fields from `plParticleWorldModule::m_ForceFieldData`. Combustion where T > T_ign: `burn = min(fuel, rate·dt)`, `fuel -= burn`, `T += heat·burn`, `ρ += soot·burn`, `divSrc += expansion·burn`. Cooling T_amb + (T−T_amb)·e^(−c·dt). Dissipation. Final density clamp. Writes the other velocity side, then `GasHalo`. |
| G12 | `GasSolids` | Static SDF (dense R16F at 2× gas dx, ≤ 256³) and up to 64 dynamic shapes. Faces inside a solid take the solid's normal velocity. Scalars inside solids are cleared. |

**Phase Gas/Pressure**

| # | Kernel | Notes |
|---|---|---|
| G13 | `GasDivergence` | Exact MAC divergence `(u[i+1]−u[i]+…)/Δx − divSrc`. The +1 face comes from the apron. |
| G14 | `GasMG` V-cycle, warm-started from the last tick | **Smoother:** red-black GS on a 10³ groupshared tile (4 KB, halo frozen), 2 sweeps per launch. **Restrict** 8³ → 4³ → 2³ → 1 inside the brick, then into `MGBrickGrid`. **Dense levels** halve down to 4³; a coarse cell is fluid if any child is. **Prolong** back up, then post-smooth. **Boundaries:** unallocated neighbour = **Dirichlet p=0** (open air, about 8 voxels beyond the dilated region); closed domain face = Neumann; solid = Neumann. A closed-box mode allocates the whole box. |
| G15 | `GasProject` | `u -= Δt/ρ·∇p` on faces, then `GasHalo` |

**Phase Gas/Finalize**

| # | Kernel | Notes |
|---|---|---|
| G16 | `GasFinalize` | Per-brick max of density, temperature and speed, and mean density. Indirection A = max(mean, 1e-4·DensityScale) if max > threshold, else 0. Atomic min/max of brick bounds. Mass in fixed point. NaN count; a NaN texel is zeroed. Optional EmissionAtlas: R = smoke + flame opacity, GBA = HDR emission from a 256-entry Planck LUT per preset × intensity + burn boost. Stats are copied with `CopyBufferRegion` into the readback ring. |

**VDB handoff (every frame)**
- The component emits `plVDBVolumeGpuRenderData` with:
  - atlas = the current ScalarAtlas side (or EmissionAtlas with the Emission flag);
  - the Indirection texture;
  - channels Density\|Temperature;
  - `m_fMaxDensity` = preset `DensityMax`.
- Bounds are the readback AABB (2–3 frames late), dilated by `maxSpeed × latency × dt + 1 brick` and clamped to the domain. The whole grid is used until the first readback arrives.
- Render mode: RayMarch for presets with temperature (fire), otherwise Auto.
- A volume with fresh resources publishes nothing for one frame.
- Simulated volumes are capped at `GpuFluid.MaxActiveVolumes.Gas = 4` of the 16 slots. They carry a priority, and `Renderer/VDBVolumes/DroppedOverSlotLimit` is watched.

## 5. Liquid solver

**Method:** APIC on a **dense MAC domain with sparse brick dispatch**, plus a FLIP blend f ∈ [0, 0.3] (default 0.1; preset "Splash" goes up to 0.3). Linear kernel per MAC component, with D = Δx²/4. The quadratic B-spline kernel is an option on Ultra.

**Domain:** an authored box, at most 256×192×128 cells (each axis ≤ 2048 on the surface grid). Work runs only on occupied bricks (8³ cells) plus a 1-brick dilation. A sparse liquid domain is post-1.0 (see Open decisions).

**Particle storage (SoA, ping-ponged for the sort)**

| Buffer | Format | Bytes |
|---|---|---|
| Position | float3 (domain-relative metres; 16-bit fixed is rejected because slow seepage below 0.2 mm/step would stall) | 12 |
| Velocity | float3 | 12 |
| C + flags | 5 × uint: 9 halves, then a 16-bit field of flags (alive, band, sleeping) and age | 20 |

That is 44 B per particle, 88 B with the sorted copy: 1M = 88 MB, 2M = 176 MB, 4M = 352 MB.

**Grid fields (dense, buffers unless noted):**
- u, v, w (R32F), and u*, v*, w* for FLIP;
- p, div;
- MG levels (+1/7);
- cellStart/count (uint);
- flags (packed);
- φ as a `Texture3D` R16F on the surface grid.

That is about 41 B per cell:

| Tier | Domain | Cells | Grid memory |
|---|---|---|---|
| Medium | 128×96×64 | 786K | 32 MB |
| High | 192×128×96 | 2.36M | 97 MB |
| Ultra | 256×192×128 | 6.3M | 258 MB |

High adds the 2× φ grid (38 MB).

**Fixed-point rules:**
- Velocity is clamped at **32 m/s**, and the APIC term at |C·(x_i−x_p)| ≤ 16 m/s.
- Momentum sums use int32 at 2¹⁶ (worst case 64 particles × 48 m/s × 65536 = 2.0e8 < 2³¹). Weight sums use 2²⁴ (64 × 2²⁴ = 1.07e9).
- Clamped contributions increment `Stats.overflow`.
- There are no 64-bit atomics and no float atomics anywhere.

**Passes per substep.** Particle kernels use 256 threads with `DispatchIndirect` from the alive count. Grid kernels use one 8×8×8 group per active brick.

**Phase Liquid/Sort**

| # | Kernel | Notes |
|---|---|---|
| L0 | `LiqSources` | Inflow: spawn stratified particles in emitter cells whose count is below n₀, jitter `hash(tick, sourceId, cell)`, inflow velocity. Increments `seeded` (uint). |
| L1 | `LiqCount` | Per particle: kill if outside the domain or inside a drain (increments `drained`); `InterlockedAdd(cellCount)` |
| L2 | `ScanCells` | Block scan to `cellStart` |
| L3 | `LiqScatter` | To the sorted arrays. Order inside a cell does not matter, because the sums below are integer. |
| L4 | `LiqBricks` | Occupancy, dilation, active list, indirect args |

**Phase Liquid/P2G**

| # | Kernel | Notes |
|---|---|---|
| L5 | `LiqP2G` | **Gather.** Each thread owns one cell's u, v and w faces, loops over particles in the 2×2×2 cell neighbourhood of each face (from `cellStart`) and sums in int32 registers. It needs no atomics and no groupshared memory, and is bit-deterministic. It also writes particle count to a density ratio n/n₀. `GpuFluid.Liquid.P2GMode=1` selects a groupshared-tile scatter (10³ × 6 uint = 24 KB) as an optimisation measured in P5; gather stays the default unless it is more than 1.3× slower. |
| L6 | `LiqNormalize` | Normalize, copy u*, extrapolate 3 layers into air (3 iterations, ping-pong) |

**Phase Liquid/Solve**

| # | Kernel | Notes |
|---|---|---|
| L7 | `LiqClassify` | Solid if SDF < 0; liquid if count > 0 (or φ < 0 in a band interior); else air. Solid face fractions (Batty) from the SDF. |
| L8 | `LiqForces` | Gravity from `GetGravity()`. Solid face velocities: static = 0; dynamic = v + ω × r. |
| L9 | `LiqRHS` | −div + local correction −k·max(n/n₀ − 1, 0)/dt with k = 0.3, clamped. Global volume control c·(V_target − V)/V_target, where V_target = (seeded − drained) × V_particle, exact as integers. Drain sinks. |
| L10 | `LiqPressure` | Dense geometric MG: RBGS 2 pre and 2 post sweeps; fine levels dispatched over active bricks; coarse levels dense down to 4³. Ghost-fluid Dirichlet with θ from φ. Warm-started. High and up use **MGPCG** with one V-cycle as preconditioner, dot products by fixed-order tree reductions (256-thread partials, then one group), and α/β kept in a GPU buffer, so there is no readback. |
| L11 | `LiqProject` | Then extrapolate again |

**Phase Liquid/G2P**

| # | Kernel | Notes |
|---|---|---|
| L12 | `LiqG2P` | Manual 8-tap trilinear. New velocity v = lerp(v_APIC, v_p + (u − u*), f); update C; RK2 advection through the grid. SDF push-out along ∇SDF, remove the inward normal velocity, apply friction. Whitewater potentials (trapped air, crest, kinetic energy) go to the spawn buffer. Sleep: a brick with max\|v\| < 2 cm/s for 60 steps, with no emitter or moving solid overlapping it, freezes its particles. |

**Substeps:** n = ceil(vmax·dt / (1.5Δx)) from the readback vmax, capped per tier. Over the cap the sim runs in slow motion. The volume sleeps when every brick sleeps; it then costs only rendering and keeps its cached mesh.

**Narrow band (High and up, P12):** particles are kept within 4 cells of φ=0. The interior keeps grid velocity (semi-Lagrangian advected) and an advected grid φ. **The pressure solve still spans every interior liquid brick**, so solve cost follows water volume. The tier ms numbers include this, and interior bricks stay active.

## 6. Liquid surface and rendering

**Phase Liquid/Surface (once per frame)**

| # | Kernel | Notes |
|---|---|---|
| S0 | `SurfLevelSet` | Zhu–Bridson weighted-average φ from sorted particles, radius 1.3h, on the surface grid (1× sim; 2× in the band on High) |
| S1 | `SurfSmooth` ×2 | Clamped Laplacian (\|Δφ\| ≤ 0.4h), then a temporal blend φ = 0.7·φ_new + 0.3·advect(φ_prev) |
| S2 | `SurfNetsVerts` | Per surface cell: one vertex at the mean of edge crossings. `InterlockedAdd` on the vertex counter; the vertex index goes into a cell map (uint). The vertex stores position, normal from ∇φ, velocity and foam. |
| S3 | `SurfNetsQuads` | Per crossing edge: one quad of 4 vertex indices |
| S4 | `SurfDrawArgs` | `{6 × quads, 1, 0, 0}`, capped at 1M quads |

Drawing uses `DrawInstancedIndirect` with vertex pulling (`BindNullMeshBuffer`): the vertex shader reads the quad and vertex buffers. There is no index buffer and there are no mesh shaders.

**`plGpuFluidLiquidPass`** runs at BeforeTransparent, `m_fOrder` after plasma.water. It binds Color and DepthStencil, reads ResolvedDepth and GBuffer1, and publishes `plasma.particles/LiquidExitDepth`.
1. Copy scene colour (pre-exposed; `SceneColorToAbsolute`) and depth.
2. Draw back faces into an R16F linear-depth target (thickness, Low and Medium).
3. Draw front faces with depth write:
   - Fresnel with IoR 1.333; probe reflection plus SSR;
   - refraction offset by N × thickness, with a foreground reject;
   - Beer–Lambert exp(−σₐ·L) with σₐ = (0.45, 0.07, 0.04)/m plus an in-scatter tint;
   - analytic sun term in the `EvaluateWaterSunShadow` style;
   - foam from the whitewater splat grid with breakup noise.

   On High, L comes from a refracted **φ march** that skips empty bricks, capped by `GpuFluid.Liquid.RefractSteps` (16 on High; 0 means back-face thickness). It is also an auto-scaler lever.
4. Back faces in the colour pass shade the interface from below, with Snell's window and total internal reflection.

Because the liquid writes depth before the transparent pass, fog, the VDB march and OIT composite at its depth for free.

**`plGpuFluidUnderwaterPass`** runs at AfterTransparentResolved. A 1-thread compute in the PreRender graph samples φ at the camera into a 16 B buffer, so no readback is needed. If the camera is inside the liquid, the pass applies absorption fog and distortion up to `LiquidExitDepth`.

**Known gap:** no motion vectors until E3 (P13), so fast liquid ghosts under TAA. The fallback is a reactive mask written by the liquid pass.

## 7. Colliders and coupling

**Static SDF**
1. At activation, on a property change, or on `GpuFluid.Rebake`: `ExtractWorldGeometry(CollisionMesh)`, clipped on the CPU to the domain plus margin and binned per brick (CPU task).
2. GPU pass `SolidsBake`, one 8×8×8 group per brick: exact unsigned distance to the binned triangles of the brick and its neighbours, within a 3-cell band.
3. Sign from the angle-weighted pseudonormal, plus a **minimum solid thickness of 0.5·dx**, so open-plane room walls work.
4. Output is dense R16F: at liquid dx for the liquid; at 2× gas dx (≤ 256³) for the gas.

Code-built meshes without a `plCpuMeshResource` give no triangles, so tests use Jolt static actors or analytic colliders.

**Dynamic shapes**
- Sources, deduplicated per actor, at most 64:
  - `QueryShapesInBox(Dynamic|Character|Ragdoll|Debris)` on the domain box. Each gives an OBB from `GetWorldSpaceBounds`; character controllers become capsules from `GetGlobalBounds`.
  - Authored `plGpuFluidColliderComponent` shapes.
  - Kinematic bodies.
- Linear and angular velocity come from transform deltas (angle-axis of the rotation delta over dt).
- The list is uploaded as a transient structured buffer. Each brick builds a 64-bit shape mask (two uints) by AABB reject, then evaluates the shapes analytically per voxel as SDF-min plus solid velocity.
- Bodies push liquid and stir smoke.

**Buoyancy** (up to 32 tracked bodies: those with `plGpuFluidBodyComponent`, or automatic dynamic bodies in the liquid)
1. `LiqBuoyancy`: one 256-thread group per body over the cells of its shape gives `{V_sub, centre of buoyancy, mean relative fluid velocity}`.
2. **Only ρ·g·V_sub is used**, never ∑p·n·dA as well, which would count the hydrostatic part twice.
3. F = ρ·g·V_sub·(−ĝ) + ½ρ·C_d·A·|Δv|Δv + c_lin·m·Δv. The torque comes from the CB offset, with angular damping.
4. Readback is 3 frames in flight. In PreAsync the force is low-pass filtered (τ = 0.1 s), clamped to ρ·g·V_body·1.2, and sent as `plMsgPhysicsAddImpulse` (type 0) at the CB with impulse = F·Δt_frame, every frame. Sleeping bodies are skipped.
5. Displacement back into the liquid comes from the dynamic solids.

N6 checks that the bobbing amplitude does not grow.

## 8. Particle-effect integration (G1)

| Node | Context | Behaviour |
|---|---|---|
| `_GpuFluidVelocity` | Update | Drag towards fluid velocity. CPU systems sample a per-brick mean velocity readback (≤ 4096 × 16 B, every 4 frames). **GPU-lowered systems** get a package kernel (`ParticleGpuFluidDrag`) that runs on their pool in the PreRender graph, sampling the gas atlas or liquid grid at full resolution; the engine shader is unchanged. |
| `_GpuFluidSource` | Update | Each particle splats density, temperature, fuel and velocity (embers, missile trails, flamethrower), or spawns liquid (hose). At most 16K per frame per volume. GPU pools are read directly. |
| `_GpuFluidEvents` | Spawn emitter | The top 64 splash or impact events per frame (by energy) are read back and spawn CPU particles. The same events drive sounds and decals. |

**Whitewater** follows Ihmsen 2012:
- spawned from the G2P potentials;
- classified by φ: spray is ballistic with drag, foam is advected on the surface with a lifetime, bubbles are buoyant;
- held in a package pool in the **64 B `plGPUParticle` layout** with an alive list and draw arguments, drawn through `plGPUParticleRenderData`, so the existing OIT path applies;
- foam and bubbles also splat (fixed point) into an R32UI foam grid read by the surface shader.

**Spray cools fire:** spray particles inside an active gas brick subtract temperature and fuel through the splat accumulator. This is the sprinkler demo.

## 9. Quality tiers

The numbers are estimates for a base M4 (10 GPU cores, ~120 GB/s) at the **reference load**, not at the cap:
- gas: the pool 60% full;
- liquid: an 8×6 m room filled to 0.5 m at 4 particles per cell (~400K particles on Medium).

The cost model is 1.1 µs per brick-step, plus 0.6 µs per V-cycle, plus 0.3 µs for MacCormack, plus 2.2 ns per particle-substep. P5 replaces these with Release measurements.

| | Low | Medium (M4 default) | High (M4 Pro target) | Ultra (capture only) |
|---|---|---|---|---|
| Gas brick cap (voxels) | 512 (0.26M) | 1536 (0.79M) | 3072 (1.6M) | 6144 (3.1M) |
| Gas tick | 30 Hz | 60 Hz | 60 Hz | 60 Hz, 2 ticks |
| Advection | SL | MacCormack, vorticity | MacCormack, vorticity | MacCormack |
| Gas pressure | 1 V(1,1) | 1 V(2,2) | 2 V(2,2) | 3 V(2,2) |
| Gas sim ms | 0.6 | 2.5 | 4.5 | 9 |
| Liquid domain / dx (room) | 96×64×64 / 10 cm | 128×96×64 / 6.25 cm | 192×128×96 / 5 cm | 256×192×128 / 4 cm |
| Particles per cell / cap | 4 / 256K | 4 / 1M | 8, band 4 / 2M | 8, band 4 / 4M |
| Substeps max | 1 | 2 | 3 | 3 |
| Liquid pressure | MG 2 V | MG 3 V | MGPCG 4 | MGPCG 8 |
| Surface grid / thickness | 1× / back face | 1× / back face | 2× band / φ march 16 | 2× / φ march 24 |
| Whitewater cap | 0 | 64K | 256K | 1M |
| Liquid sim + surface ms | 1.2 | 3.0 | 6.0 | 14 |
| Render ms (1080p, gas + liquid) | 0.8 | 1.5 | 2.5 | 3.5 |
| **Default `GpuFluid.Budget.GpuMs`** | **3** | **7** | **13** | 0 (off) |
| Gas / liquid memory | 24 / 20 MB | 72 / 120 MB | 144 / 310 MB | 289 / 870 MB |

**Budget defaults equal the sum of the tier rows** for a scene with one gas and one liquid volume, so a demo does not degrade at once. Mobile is out of scope: it is not testable here.

## 10. Budget and auto-scaling

- **Measurement:** per-phase render-graph timers (Gas/Alloc, Emit, Advect, Forces, Pressure, Finalize; Liquid/Sort, P2G, Solve, G2P, Surface; Render), read when `Rendering.Profiling.GPUStats` is on. Otherwise the cost model is calibrated by a timing sample every 120 frames.
- **Split:** the budget is divided across active volumes by Priority, then camera distance.
- **Controller:** after 30 frames over budget it takes one lever step, in this order (cheapest loss first), with 10% hysteresis and 60 frames between steps:
  1. MacCormack off;
  2. pressure cycles −1;
  3. φ-march steps down, then back-face thickness;
  4. whitewater cap ×½;
  5. surface grid to 1×;
  6. gas tick 60 → 30 Hz;
  7. gas brick cap −25% (Bucket-3 bricks evicted first);
  8. allocation threshold ×2;
  9. liquid max substeps −1;
  10. VDB step size ×1.5.

  It recovers in reverse order. It never reallocates pools.
- **LOD:** beyond `LodDistance` a volume runs at half rate with 2·dt; beyond 2× it freezes. Off-screen for 30 frames means quarter rate. **`AlwaysSimulate`** (default on for liquid) keeps full rate, so floods and buoyancy that gameplay depends on never pause.
- **Limits:** `GpuFluid.MaxActiveVolumes.Gas=4`, `.Liquid=2`.
- **Tier change:** a tier change that alters voxel size or pool size recreates the volume on a background frame and publishes nothing that frame. Metal zero-fills shared buffers on the CPU, so pools are created only at activation or on a tier change.

## 11. Verification

**Numeric tests** (`GpuFluidNumericTests`, GPU integer reductions read back). Every gallery image test also asserts numbers.

| ID | Test | Pass criterion |
|---|---|---|
| N0 | Handoff | A compute-written analytic sphere (E1) renders within the `imgdiff` threshold of a baked sparse .nvdb of the same sphere. This validates RGBA16F indirection, apron and A. |
| N1 | Allocation invariants | Page table and active list agree, each slot exactly once; active + free = pool; A > 0 exactly when allocated and non-empty; content near a face implies the neighbour is allocated unless denied > 0; zero bricks 60 ticks after emitters stop; a 64-brick cap degrades without NaN, and denial order is bucket 3 first |
| N2 | Divergence | Gas: exact MAC L∞(div)/L∞(div_pre) < 1e-2 after 2 V-cycles. Liquid: < 1e-3 relative. Both use the same discrete operator as the projection. |
| N3 | Poisson | 64³ known solution; residual cut per V-cycle ≤ 0.2; convergence-per-ms CSV for RBGS, MG and MGPCG |
| N4 | Gas mass | Closed box (fully allocated, Neumann faces, dissipation off), 300 ticks. The tolerance is **set from the first measured run + 50%** and stored in the test. |
| N5 | Liquid volume | Particle delta == seeded − drained, exact. Dam-break volume within ±3% over 600 steps. Flood level Q·t/A_floor within ±5%. Drain empties the tank. |
| N6 | Hydrostatic | max\|v\| < 1 cm/s after 300 ticks; surface vertex height variance < 0.05h |
| N7 | Dam break | Front position within 10% of Martin & Moyce at t* = 1, 2, 3 |
| N8 | Buoyancy | Body at ρ=500 settles at 50% ± 10% submerged; bobbing amplitude decreases monotonically after 2 s (Jolt via `PL_PARTICLESTEST_PLUGIN_DIRS`) |
| N9 | Colliders | No particle with SDF < −0.5Δx; plume centroid x-drift < 1 voxel around a symmetric obstacle |
| N10 | Seams | Uniform translation of a Gaussian across brick boundaries: max error at apron cells ≤ 1.5× interior error. Gates fused apron writes. |
| N11 | Determinism | Two runs give bitwise-equal hashes of the gas atlases (virtual order) and the liquid grid plus sorted particle positions; also across pool sizes while under budget |
| N12 | Safety | NaN count 0; overflow count 0 at the 32 m/s clamp stress test; a fresh-resource frame publishes nothing |

**Gallery** (`plGpuFluidGalleryTest`, WaterGallery pattern):
- fixed 1/30 s step, seeded, `ResetSimulationClock`;
- reference PNGs at frames 30, 90 and 180;
- `PL_GPUFLUID_VIDEO=1` saves `frame_%05d`, which `make_video.swift` encodes before deleting the frames.

Scenes:
1. Smoke Column
2. Corridor Smoke (ceiling layer through doorways)
3. Smoke Around Obstacles
4. Campfire to Inferno
5. Fireball Explosion
6. Sim vs Baked .nvdb (side by side)
7. Moving Emitter (512 m domain, brick overlay)
8. Dam Break
9. Flooding Room (pipe inflow, furniture, opening door, drain)
10. Pour Into Glass
11. Wave Tank (paddle)
12. Buoyant Crates
13. Underwater Dive
14. Sprinklers vs Fire
15. Tier Sweep

**Timing** (`GpuFluidTierTiming`): Release dylibs, `Rendering.Pipeline.Injections` off as the baseline, 60 warm-up and 180 measured frames. Reports median and p90 per phase, µs per brick-step and ns per particle-substep, and writes a CSV to `Output/GpuFluid/`.

## 12. Phase plan

Each phase ends with an mp4, a commit (`!F`/`!B` format) and a push of both the package branch and `feature/gpu-fluid`.

| # | Work | Exit (video to record) | Cut if slipping |
|---|---|---|---|
| P0 | E1, E2, E5; package skeleton, manager, empty PreRender graph, readback ring; scripts (run, video, imgdiff, cache delete); analytic compute volume | **"Handoff Sphere"**: a pulsing analytic sphere through the VDB march beside the baked sphere; N0 passes | E1 → runtime-resource fallback (F7) |
| P1 | Gas on a fully allocated brick box: atlases, MAC velocity, SL advection, `GasHalo`, emit, buoyancy, divergence, warm RBGS, project, Finalize (A floor, density clamp, NaN guard), phase timers, manual-tier TierTiming skeleton | **"Smoke Column"**: self-shadowed plume under the sun; N12; first CSV | none |
| P2 | Sparse allocation: mark by flux and proximity, priority buckets, scan assign, free hysteresis, full clear, neighbour table, indirect args with 2D fold, readback bounds and dilation, brick debug view | **"Moving Emitter"** with the brick overlay; N1, N11 | hysteresis tuning |
| P3 | MG (intra-brick 4³/2³/1 levels + brick grid), MacCormack + Scratch, vorticity, wind, force fields, analytic colliders, open and closed faces; Poisson test and convergence CSV | **"Corridor Smoke"** and **"Smoke Around Obstacles"**; N2, N3, N4, N9 | MacCormack (keep SL) |
| P4 | Combustion, expansion, presets (resource + runtime creation), full emitter component, .vdb-driven source, fire forces RayMarch | **"Campfire to Inferno"**, **"Fireball Explosion"**, **"Sim vs Baked .nvdb"** | expansion term |
| P5 | Tier tables, budget split, auto-scaler levers, LOD, AlwaysSimulate, gas sleep, P2G-mode bench hook; Release TierTiming | **"Tier Sweep"** with an ms overlay; CSV committed | auto-scaler (manual tiers only) |
| P6 | Liquid core: dense MAC domain, particle SoA, sort, gather P2G, extrapolation, classify, MG pressure with ghost fluid, G2P, analytic SDF, closed box; debug points through `plGPUParticleRenderData` | **"Dam Break (points)"**; N5 (volume), N6, N7 | FLIP blend |
| P7 | Level set, clamped and temporal smoothing, surface nets, LiquidPass (back-face thickness, refraction, absorption, Fresnel, SSR/probe, sun, depth write) | **"Dam Break (rendered)"** and **"Pour Into Glass"** | temporal blend |
| P8 | Static SDF bake from CollisionMesh (pseudonormal + min thickness), inflow and drain, local + global volume correction, CFL substeps, brick and volume sleep | **"Flooding Room"**: pipe inflow, slosh, drain; flood-level test | drains (kill planes instead) |
| P9 | Dynamic shapes (query + authored, angular velocity), buoyancy reduction, impulses, damping | **"Buoyant Crates"**, **"Wave Tank"**, and a door pushing smoke; N8 | buoyancy torque |
| P10 | Whitewater pool and renderer, foam grid, underwater pass with camera φ | **"Pour Tank with Foam"** and **"Underwater Dive"** | bubbles |
| P11 | Particle nodes (Velocity, Source, Events), GPU-lowered drag kernel, spray cools fire, embers advected | **"Sprinklers vs Fire"** and effect-driven smoke | GPU drag (CPU readback only) |
| P12 | Polish and scale (each item cuttable): MGPCG on High, narrow band, 2× φ band grid, φ-march refraction, emission atlas + Planck LUT, fire lights, E4, fused apron writes behind N10 | **"Burning Building"** and a High-tier flood within budget | each item independently |
| P13 | Production: editor properties, ranges and gizmos, GpuFluidPreset asset in EditorPluginParticle, serialisation versions, frame CB slot 12 + material node if wanted, E3 motion vectors, HTML docs in `Documentation/` (JoshDT standard), image references, DX12/Vulkan shader compiles here; PC compile, run and full regression (later, on the PC) | **Final demo reel**; all tests pass on Metal, and PC results recorded | E3 (reactive mask), gizmos |

## 13. Risks and mitigations

1. **VDB contract** (16 shared slots, A semantics, coarse taps lose temperature, F6 consumers). E1 and N0 in P0; a cap of 4 sim volumes with priority; fire forced to RayMarch; A floor in density units.
2. **Bandwidth on a base M4.** RGBA16F everywhere except pressure; measure in P1 and P5 on Release; conservative Medium tier.
3. **Dispatch and encoder overhead on a serial Metal encoder** (~45 dispatches per gas tick at Medium). Everything runs on `DispatchIndirect` with no CPU stall; kernels are fused in P12; timing is per phase only.
4. **Halo correctness.** An explicit halo pass first, ownership rules, full 10³ clears, N10 before any fused writes.
5. **Fixed-point overflow.** Stated clamps and scales, an overflow counter, and a stress test.
6. **Groupshared and occupancy.** The default P2G is gather (no groupshared); the 24 KB tile is opt-in after measurement; smoother tiles are 4 KB.
7. **Metal allocation costs** (CPU zero-fill, no transient aliasing). Pools are allocated once per tier, Private textures are cleared by compute, and fresh frames publish nothing.
8. **Liquid schedule depth.** A dense domain first; narrow band, MGPCG and sparse liquid only after the P8 flood video.
9. **Dense liquid cost while the room is mostly air.** Sparse brick dispatch at fine levels; coarse levels are small.
10. **TAA ghosting.** E3 in P13, or a reactive mask.
11. **Naming and package coexistence** (plasma.fluid with a macOS runtime, plasma.water). The GpuFluid prefix, CB slot 12, no `plWaterWorldModuleInterface`, and LiquidPass `m_fOrder` after water.
12. **iOS and portability.** No mesh shaders, no draw-indirect-count, no texture or 64-bit atomics, no RG11B10 UAVs; early simulator smoke test of `RWTexture3D<float4>` RGBA16F; DX12 group fold.
13. **Determinism across vendors.** Fixed-order tree reductions and integer sums; determinism is tested per device only.
14. **Disk and stale caches.** Encode then delete frames, mp4 under 15 MB, and the scripts delete the GpuFluid shader cache. CPU timings come only from Release builds.

## Open decisions (recommended default in bold)

1. **Liquid domain:** a dense authored box up to 256×192×128 for 1.0, with the sparse liquid domain (scale's design) after 1.0 → **dense for 1.0**.
2. **2× gas scalar upres** (Flow-style; ~49K render bricks and ~800 MB at High) → **out of scope for 1.0**.
3. **XR:** gas renders through froxels only (F12) → **accept and document**, rather than adding `plVDBVolumePass` to XRRenderPipeline.
4. **Ultra tier** → **capture-only** (offline videos), not a realtime target.
5. **Tier change at runtime** that alters dx → **allowed**, recreating the volume with one blank frame.
6. **Default FLIP blend** → **0.1**, with a 0.3 "Splash" preset.