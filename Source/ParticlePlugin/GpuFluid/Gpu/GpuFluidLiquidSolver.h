#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Vec3.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidLiquidSolids.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidScan.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/RendererFoundationDLL.h>
#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidConstants.h>

class plRenderGraph;

/// One fill box of a liquid volume, snapped to whole cells of the domain.
struct plGpuFluidLiquidFill
{
  plVec3U32 m_vCellMin = plVec3U32::MakeZero();
  plVec3U32 m_vCellCount = plVec3U32::MakeZero();
  plVec3 m_vVelocity = plVec3::MakeZero();
  float m_fJitter = 1.0f; ///< 0: particles on a regular lattice, 1: anywhere in their sub-cell
  bool m_bStratified = true; ///< exactly n0 per cell, one per sub-cell; false: every particle anywhere in the box

  /// Fraction (0, 1] of the top cell layer's height that is filled: that layer gets this share of the particles,
  /// placed in its lower part, so the surface can lie inside a cell.
  float m_fTopLayerFill = 1.0f;
};

/// An inflow: a disc of liquid entering at a flow rate with a velocity (a nozzle). Domain-relative metres.
struct plGpuFluidLiquidSource
{
  plVec3 m_vCenter = plVec3::MakeZero();
  plVec3 m_vVelocity = plVec3(0.0f, 0.0f, -1.0f); ///< m/s, also the direction the disc faces
  float m_fRadius = 0.02f;                        ///< m
  float m_fFlowRate = 0.0f;                       ///< m^3/s; 0: the disc's area times the speed
};

/// Everything a liquid volume's GPU work needs for one tick, gathered on the update thread.
struct plGpuFluidLiquidFrameInput
{
  /// A new value steps the simulation once by m_fDt. The same value again (a frame rendered without an update)
  /// steps nothing.
  plUInt64 m_uiTick = 0;

  /// A change empties the volume and seeds the fill boxes again.
  plUInt32 m_uiResetCounter = 0;

  bool m_bSimulate = true;
  float m_fDt = 1.0f / 60.0f;
  plVec3 m_vGravity = plVec3(0.0f, 0.0f, -9.81f);

  /// World position of the domain's minimum corner. The domain is axis aligned in world space.
  plVec3 m_vDomainOrigin = plVec3::MakeZero();

  float m_fFlipBlend = 0.1f;
  /// Substeps per tick: ceil(vmax * dt / (cfl * dx)) between these. vmax is the largest max speed of the last
  /// m_uiSpeedHistory readbacks plus g * dt per frame of their age, so a stale readback right after a calm moment
  /// does not pick too few. The count rises at once and drops by one only after m_uiSubstepHoldTicks ticks in a row
  /// in which one fewer would have been enough with room to spare (the CFL number times m_fSubstepDownMargin). When
  /// the cap is not enough, the tick simulates less time than it was given (slow motion, down to a quarter tick)
  /// instead of breaking the CFL number.
  plUInt32 m_uiMaxSubsteps = 4;
  plUInt32 m_uiMinSubsteps = 1;
  float m_fCflNumber = 1.5f;
  plUInt32 m_uiSpeedHistory = 8;
  plUInt32 m_uiSubstepHoldTicks = 15;
  float m_fSubstepDownMargin = 0.8f;

  /// Pressure solve: MGPCG (conjugate gradients, one multigrid V-cycle as the preconditioner per iteration) or, when
  /// false, plain V-cycles. Either way m_uiPressureCycles V-cycles per substep, a fixed count (no readback). Measured
  /// in P8 (relative residual max|r| / max|rhs| of a tick's last substep, median / 95th percentile / max over the ticks):
  /// - Medium room (128 x 96 x 64, 393K particles): 3 iterations 9.7e-4 / 2.8e-3 / 6.1e-3, 4: 1.9e-4 / 6.9e-4 /
  ///   1.8e-3, 6: 9e-6 / 2.9e-5 / 5.9e-5;
  /// - dam break (2 cm, 230K particles): 3: 2.1e-3 / 9.3e-3 / 4.5e-2, 4: 3.8e-4 / 1.9e-3 / 1.3e-2, 6: 2e-5 / 7.7e-5 /
  ///   5.3e-4.
  /// The volume component takes the tier's count (Medium 4) unless PressureCycles is set.
  bool m_bPcg = true;
  plUInt32 m_uiPressureCycles = 4;

  plUInt32 m_uiPreSmooth = 2;        ///< red-black Gauss-Seidel sweeps before restriction (1: the residual is 25x worse)
  plUInt32 m_uiPostSmooth = 2;       ///< and after prolongation
  /// Sweeps on the coarsest level, in one dispatch of one group. 48 (P7) gave the same residuals at the Medium room
  /// as 16, which saves about 0.5 ms there.
  plUInt32 m_uiCoarsestSweeps = 16;
  /// Coarse right-hand side = this * sum of the 8 fine residuals. 0.5 (four times the mean) matches the unit-spacing
  /// stencil rediscretised on the coarse level.
  float m_fCoarseScale = 0.5f;

  float m_fTheta = 0.5f;          ///< ghost-fluid fraction of the free surface between a liquid and an air cell centre
  /// k of the local density correction: target divergence k * e in 1/s (at most 0.5 / dt), e = rho/rho0 - 1 from a
  /// smooth (quadratic B-spline) particle density over the liquid cells around (LiquidRhs). A rate, and its
  /// equilibrium is e = 0, so the packing it holds does not depend on dt or the substep count.
  float m_fDensityGain = 4.0f;
  /// Deep cells (no air in the 3x3x3 block around) correct both ways: sparse ones pull particles in. Near the surface
  /// only crowding is corrected (a partly filled cell reads sparse). False: one-sided everywhere.
  bool m_bDensityTwoSided = true;
  float m_fDensityMaxError = 0.5f;  ///< |e| is clamped to this
  /// The density correction acts on |e| - this (0: none).
  float m_fDensityDeadZone = 0.0f;
  /// c of the global volume control: target divergence c * (V_target - V) / V_target, in 1/s (the share of the
  /// volume error corrected per second, independent of the substep count). 20: the air an overturning wave traps
  /// (the dam break's surge at the far wall) is pushed out within about a second, so the measure stays within 2%.
  float m_fVolumeGain = 20.0f;

  /// After a reset the first ticks run the most substeps: a column released from rest accelerates at up to g * H / dx
  /// near its foot before the read-back speed can see it.
  plUInt32 m_uiStartupTicks = 10;
  float m_fWallFriction = 0.0f;   ///< 0: free slip, 1: no slip
  float m_fWallMargin = 0.01f;    ///< particles stay this far inside walls and solids, in cells

  plUInt8 m_uiOpenFaces = 0; ///< GPU_FLUID_LIQUID_OPEN_* bits; closed faces are walls

  /// Level set (Zhu & Bridson) from the particles every substep: a cell is liquid when phi < m_fLiquidPhi at its
  /// centre, and the free surface's ghost-fluid theta is (phi_i - tau) / (phi_i - phi_j) per face. False: liquid cells
  /// are the cells with particles and theta is m_fTheta (P6).
  bool m_bLevelSet = true;
  float m_fLevelSetRadius = 1.5f;    ///< kernel radius R in cells (at most 1.5: the gather covers 3x3x3 cells)
  /// r in cells. 0.25 R would put the zero of a flat, full surface where it belongs; a little less keeps the surface
  /// mesh on the particles of a settled pool.
  float m_fParticleRadius = 0.28f;
  bool m_bLevelSetTheta = true;      ///< theta from the level set; false: m_fTheta with the level-set liquid cells
  /// Liquid cells are phi < m_fLiquidPhi alone (P8), not also every cell holding particles (P7's union). The cells with
  /// particles follow the particles' noise at the surface: a cell holding one stray particle beside an empty one at the
  /// same height is a one-cell bump whose pressure pushes sideways into the air next to it, and a half-filled top layer
  /// is a mix of liquid cells and holes, so a resting surface never comes to rest (a randomly seeded pool "boils"). The
  /// level set is a smooth function of the particle positions, so the surface it gives the pressure is too; a particle
  /// outside it is spray and flies ballistically until it lands.
  bool m_bLevelSetOnly = true;
  /// tau (cells): the pressure's free surface is phi = tau, a little outside the level set's zero, whose particle radius
  /// is set for the surface mesh. As the level set is phi = |x - xbar| - r, this is the level set of r + tau (0.65
  /// cells). It has to reach about 0.6 cells: the centre of a half-filled top cell lies about 0.3 cells outside the
  /// mesh's level set, and with a smaller tau some of those cells are liquid and some air (0.2: the random pool still
  /// moves at 2 cm/s; 0.1 and below with the level set alone: the liquid falls apart).
  float m_fLiquidPhi = 0.37f;
  /// Without m_bLevelSetOnly (P7's union): a cell holding particles is also liquid while phi at its centre is below
  /// this (cells).
  float m_fSprayPhi = 1.0e3f;
  /// Without m_bLevelSetOnly: a cell outside phi < m_fLiquidPhi needs this many particles to be liquid.
  plUInt32 m_uiLiquidMinCount = 1;
  /// Particles in air cells fly ballistically (gravity only). False: they take the extrapolated grid velocity.
  bool m_bBallisticAir = true;
  /// Only clear of the surface: a particle in an air cell right above a liquid cell (along gravity) takes the grid
  /// velocity instead. A part-full top layer the level set reads as air otherwise fell, bounced off the liquid below
  /// and fell again, kicks of 0.1-0.2 m/s in a resting tank (P8, random-seed tanks at 16.25 and 16.75 cells).
  bool m_bBallisticClearOfSurface = true;
  float m_fThetaMin = 0.25f;         ///< smallest theta from the level set
  /// Clamped Laplacian steps on the substep's level set before it classifies (pairs; 0: none). They take the particles'
  /// noise out of theta, which otherwise keeps a resting surface jittering.
  plUInt32 m_uiLevelSetSmoothSteps = 0;

  /// Surface mesh (Liquid/Surface, once per frame that steps): the level set again from the final particles, blended
  /// with the last frame's surface carried along the flow, smoothed, then surface nets.
  bool m_bSurface = true;
  /// The surface sorts the final particles again and computes its own level set from them (P7). False: it takes the
  /// last substep's level set, computed from the particles at that substep's start, so the mesh lags the particles by
  /// one substep (1/90 s at 3 substeps) and the frame saves a sort and a level-set gather (about 0.7 ms at Medium).
  bool m_bSurfaceOwnLevelSet = false;
  float m_fSurfaceBlend = 0.3f;          ///< share of the advected last frame (0: no temporal blend)
  plUInt32 m_uiSurfaceSmoothSteps = 4;   ///< clamped Laplacian steps
  float m_fSurfaceSmoothGain = 0.5f;     ///< k of a step
  float m_fSurfaceSmoothClamp = 0.4f;    ///< most a step moves phi, in cells

  plHybridArray<plGpuFluidLiquidFill, 4> m_Fills;
  plHybridArray<plGpuFluidLiquidShape, GPU_FLUID_LIQUID_MAX_SHAPES> m_Shapes;
  plHybridArray<plGpuFluidLiquidSource, 4> m_Sources; ///< at most s_uiMaxSources are used

  /// The static solids baked from the world's collision meshes (null: none). A new pointer is baked again on the GPU.
  plGpuFluidLiquidSolidBakePtr m_pSolidBake;

  /// Open drains: boxes (CenterType.xyz centre, Size.xyz half extents, domain metres). A particle inside one is removed
  /// and counted as drained; a cell whose centre is inside one is air.
  plHybridArray<plGpuFluidLiquidShape, GPU_FLUID_LIQUID_MAX_DRAINS> m_Drains;

  /// Brick and volume sleep (plan section 5): a brick calm for 60 substeps freezes its particles; when no brick holding
  /// particles is awake, no inflow runs and nothing that could wake it changed, the whole volume stops simulating (only
  /// its last surface is drawn) until an input changes.
  bool m_bAllowSleep = true;

  /// Water level probes: up to four columns (x, y cells) whose level (m above the domain floor) the stats report.
  plHybridArray<plVec2U32, GPU_FLUID_LIQUID_MAX_PROBES> m_Probes;

  /// Splits Liquid/P2G (level set apart: "P2G LevelSet") and Liquid/Solve ("Solve Setup": classify, forces, right-hand
  /// side; "Solve": the pressure; "Solve Project": residual, projection, extrapolation) into passes of their own, so
  /// each is timed. Costs a few barriers more.
  bool m_bProfileKernels = false;

  /// Front histogram along +x of particles below this height (domain metres), in bins of this size. 0 bins: off.
  float m_fFrontHeight = 0.0f;
  float m_fFrontBinSize = 0.0f;
};

/// \brief Numbers the GPU counts for a liquid volume, two to three frames late.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidStats
{
  plUInt64 m_uiFrame = 0;          ///< render frame they were recorded in

  plUInt32 m_uiAlive = 0;          ///< particles in the volume
  plUInt32 m_uiSeeded = 0;         ///< seeded since the last reset
  plUInt32 m_uiDrained = 0;        ///< left through an open face
  plUInt32 m_uiLost = 0;           ///< removed for any other reason (must stay 0)
  plUInt32 m_uiTicks = 0;          ///< ticks simulated since the last reset
  plUInt32 m_uiSubstepsTotal = 0;  ///< substeps simulated since the last reset
  plUInt32 m_uiSubsteps = 0;       ///< substeps of the tick that was recorded
  double m_fSimulatedTime = 0.0;   ///< seconds simulated since the last reset (less than ticks * dt in slow motion)

  plUInt32 m_uiActiveBricks = 0;   ///< bricks the grid kernels ran on (occupied plus one brick around)
  plUInt32 m_uiOccupiedBricks = 0; ///< bricks holding particles
  plUInt32 m_uiLiquidCells = 0;    ///< cells holding particles

  /// Volume measure in cells: a liquid cell whose neighbours are all liquid or solid counts 1, a surface cell
  /// min(n / n0, 1). Target: (seeded - drained) / n0.
  float m_fVolumeCells = 0.0f;
  float m_fTargetVolumeCells = 0.0f;

  float m_fMaxSpeed = 0.0f;        ///< m/s
  float m_fMeanSpeed = 0.0f;       ///< m/s
  float m_fMeanSquaredSpeed = 0.0f; ///< (m/s)^2: twice the kinetic energy per unit mass
  plUInt32 m_uiAbove1cm = 0;       ///< particles faster than 1 cm/s
  plUInt32 m_uiAbove1mm = 0;       ///< particles faster than 1 mm/s
  plVec3 m_vCentroid = plVec3::MakeZero(); ///< domain metres

  plUInt32 m_uiNaN = 0;
  plUInt32 m_uiOverflow = 0;       ///< fixed-point or speed clamps

  float m_fRhsMax = 0.0f;          ///< max |rhs| of the last substep (m/s)
  float m_fResidualMax = 0.0f;     ///< max |residual| after its solve (m/s)

  plVec3U32 m_vBoundsMinCell = plVec3U32::MakeZero();
  plVec3U32 m_vBoundsMaxCell = plVec3U32::MakeZero();

  float m_fFrontBinSize = 0.0f;
  plDynamicArray<plUInt32> m_FrontHistogram;

  plUInt32 m_uiSurfaceVertices = 0; ///< surface-nets vertices of the frame (0 when the surface is off)
  plUInt32 m_uiSurfaceQuads = 0;    ///< and quads, both capped at their capacity
  plUInt32 m_uiSurfaceOverflow = 0; ///< vertices and quads that did not fit

  plUInt32 m_uiAwakeBricks = 0;    ///< bricks holding particles that are awake after the last substep
  plUInt32 m_uiSleepingBricks = 0; ///< and asleep (their particles frozen)
  /// Water depth at the probes (m above the domain floor) from the particles of the 3 x 3 cell column around each:
  /// their count over n0 per cell, the volume they stand for spread over the column.
  float m_fProbeLevels[GPU_FLUID_LIQUID_MAX_PROBES] = {};
  float m_fProbePhiLevels[GPU_FLUID_LIQUID_MAX_PROBES] = {}; ///< the same from the level set's lowest zero crossing
  plUInt32 m_uiInSolid = 0;        ///< particle-substeps of the frame left deeper than half a cell inside a solid (N9)
  plUInt32 m_uiSolidCells = 0;     ///< cells the static solids bake made solid (SDF < 0), counted by the last bake

  // Filled on the CPU, current (not read back):
  bool m_bVolumeAsleep = false;     ///< the volume does not simulate (nothing awake, nothing to wake it)
  plUInt64 m_uiTicksAsleep = 0;     ///< ticks skipped asleep since the last reset
  plUInt32 m_uiSolidTriangles = 0;  ///< triangles of the static solids bake on the GPU (0: none)
  plUInt32 m_uiSolidBakes = 0;      ///< bakes run since the volume was created
};

/// What the debug renderer needs to draw a liquid volume's particles this frame.
struct plGpuFluidLiquidPublish
{
  plGALBufferHandle m_hPoints;   ///< float4 per particle: world position, speed
  plGALBufferHandle m_hDrawArgs; ///< {6 * alive, 1, 0, 0}, then {6 * alive, 2, 0, 0} for stereo
  plUInt32 m_uiMaxParticles = 0;
  plVec3 m_vDomainOrigin = plVec3::MakeZero();
  plVec3 m_vDomainSize = plVec3::MakeZero();

  /// The surface mesh, when one was extracted: vertices (float4 domain-relative position, float4 normal), quads (4
  /// vertex indices) and the draw arguments {6 * quads, 1, 0, 0}.
  bool m_bHasSurface = false;
  plGALBufferHandle m_hSurfaceVertices;
  plGALBufferHandle m_hSurfaceQuads;
  plGALBufferHandle m_hSurfaceDrawArgs;
  plUInt32 m_uiMaxSurfaceQuads = 0;
};

/// Shaders of the liquid solver, loaded once per manager.
struct plGpuFluidLiquidShaders
{
  plShaderResourceHandle m_hStatsClear;
  plShaderResourceHandle m_hSeed;
  plShaderResourceHandle m_hSeedFinish;
  plShaderResourceHandle m_hClear;
  plShaderResourceHandle m_hCount;
  plShaderResourceHandle m_hScatter;
  plShaderResourceHandle m_hBricks;
  plShaderResourceHandle m_hP2G;
  plShaderResourceHandle m_hExtrapolate;
  plShaderResourceHandle m_hClassify;
  plShaderResourceHandle m_hForces;
  plShaderResourceHandle m_hRhs;
  plShaderResourceHandle m_hSmoothBricks;
  plShaderResourceHandle m_hSmoothDense;
  plShaderResourceHandle m_hRestrict;
  plShaderResourceHandle m_hProlongBricks;
  plShaderResourceHandle m_hProlongDense;
  plShaderResourceHandle m_hCoarsest;
  plShaderResourceHandle m_hResidual;
  plShaderResourceHandle m_hProject;
  plShaderResourceHandle m_hG2P;
  plShaderResourceHandle m_hRenderPrep;
  plShaderResourceHandle m_hFrameEnd;
  plShaderResourceHandle m_hPcgInit;
  plShaderResourceHandle m_hPcgDot;
  plShaderResourceHandle m_hPcgApply;
  plShaderResourceHandle m_hPcgReduce;
  plShaderResourceHandle m_hPcgUpdateX;
  plShaderResourceHandle m_hPcgUpdateP;
  plShaderResourceHandle m_hLevelSet;
  plShaderResourceHandle m_hSurfaceBlend;
  plShaderResourceHandle m_hSurfaceSmooth;
  plShaderResourceHandle m_hSurfaceVerts;
  plShaderResourceHandle m_hSurfaceQuads;
  plShaderResourceHandle m_hSurfaceArgs;
  plShaderResourceHandle m_hSource;
  plShaderResourceHandle m_hSourceFinish;
  plShaderResourceHandle m_hSolidsClear;
  plShaderResourceHandle m_hSolidsBake;
  plShaderResourceHandle m_hSolidsSign;
  plShaderResourceHandle m_hSleep;
  plShaderResourceHandle m_hProbes;
  plGpuFluidScan m_Scan;

  void Load();
  void Release();
  bool IsLoaded() const { return m_hG2P.IsValid(); }
};

/// \brief The GPU side of one liquid volume: APIC/FLIP particles on a dense MAC grid, run on occupied bricks.
///
/// Per substep (one render-graph pass per phase, so each phase is timed):
/// - Liquid/Sort: clear, count particles per cell (removing those that left), scan, scatter into the other set,
///   active bricks (occupied plus one around) and their dispatch arguments;
/// - Liquid/P2G: gather of momentum and weight per face in int32 registers, normalise, extrapolate 3 layers, and the
///   level set at the cell centres;
/// - Liquid/Solve: classify (level set), gravity, right-hand side with density and volume correction, MGPCG (multigrid V-cycle
///   preconditioner with red-black Gauss-Seidel smoothing; warm started, ghost fluid at the free surface), project,
///   extrapolate;
/// - Liquid/G2P: APIC/FLIP update, RK2 advection, push out of walls and solids.
///
/// Once per frame that steps, Liquid/Surface: sort the final particles, level set, temporal blend with the advected
/// last surface, clamped Laplacian smoothing, surface nets (vertices, quads, draw arguments) over the active bricks.
///
/// Render thread only.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidSolver
{
public:
  plGpuFluidLiquidSolver();
  ~plGpuFluidLiquidSolver();

  /// Creates the GPU resources. False when one could not be created; the solver then stays empty.
  bool Create(const plGpuFluidLiquidSettings& settings);
  void Destroy();
  bool IsCreated() const { return m_bCreated; }

  /// Records this tick's work. pStats are the newest stats read back (or null), which pick the substep count. False
  /// when nothing was recorded (no new tick, or the volume is asleep): the stats buffer then holds the last tick's.
  bool Record(plRenderGraph& ref_graph, const plGpuFluidLiquidShaders& shaders, const plGpuFluidLiquidFrameInput& input, const plGpuFluidLiquidStats* pStats, plUInt64 uiStatsAge, plUInt64 uiFrame);

  bool IsAsleep() const { return m_bAsleep; }
  plUInt64 GetTicksAsleep() const { return m_uiTicksAsleep; }
  plUInt32 GetSolidTriangles() const { return m_bHasSolidSdf ? m_uiSolidTriangles : 0u; }
  plUInt32 GetSolidBakes() const { return m_uiSolidBakes; }

  /// Whether a tick has been recorded since the solver was created, so its points may be drawn.
  bool HasRecorded() const { return m_bRecorded; }

  plGALBufferHandle GetStatsBuffer() const { return m_hStats; }
  void GetPublish(plGpuFluidLiquidPublish& out_publish) const;

  /// Decodes a stats readback.
  void DecodeStats(plArrayPtr<const plUInt8> data, plGpuFluidLiquidStats& out_stats) const;

  plVec3U32 GetCells() const { return m_vCells; }
  float GetCellSize() const { return m_fDx; }
  plUInt32 GetLevelCount() const { return m_uiLevelCount; }
  /// Substeps of the last recorded tick that stepped.
  plUInt32 GetSubstepsOfLastRecord() const { return m_uiLastSubsteps; }

private:
  plGALBufferHandle CreateRawBuffer(plUInt64 uiBytes, const char* szName, bool bIndirect = false, bool bZero = true);

  bool m_bCreated = false;
  bool m_bRecorded = false;
  plGpuFluidLiquidSettings m_Settings;
  plVec3U32 m_vCells = plVec3U32::MakeZero();
  plVec3U32 m_vBricks = plVec3U32::MakeZero();
  float m_fDx = 0.05f;
  plUInt32 m_uiCellCount = 0;
  plUInt32 m_uiFaceCount = 0;
  plUInt32 m_uiBrickCount = 0;
  plUInt32 m_uiCapacity = 0;
  plUInt32 m_uiLevelCount = 1;
  plVec3U32 m_LevelDims[GPU_FLUID_LIQUID_MAX_LEVELS];
  plGpuFluidScan::Layout m_ScanLayout;

  // Particle sets A and B (the sort ping-pongs between them) and their {groups, count} arguments.
  plGALBufferHandle m_hPositions[2];
  plGALBufferHandle m_hVelocities[2];
  plGALBufferHandle m_hAffine[2];
  plGALBufferHandle m_hParticleArgs[2];
  plGALBufferHandle m_hRank;
  plGALBufferHandle m_hPoints;
  plGALBufferHandle m_hDrawArgs;

  // Cells.
  plGALBufferHandle m_hCellCount;
  plGALBufferHandle m_hCellStart;
  plGALBufferHandle m_hCellType;
  plGALBufferHandle m_hPressure;
  plGALBufferHandle m_hRhs;
  plGALBufferHandle m_hScanScratch;

  // Face velocity sets: G (the grid velocity), S (u* for FLIP), T (scratch / the extrapolated result), and validity.
  plGALBufferHandle m_hVelG;
  plGALBufferHandle m_hVelS;
  plGALBufferHandle m_hVelT;
  plGALBufferHandle m_hValid[2];

  // Bricks.
  plGALBufferHandle m_hBrickOccupied;
  plGALBufferHandle m_hBrickActive;
  plGALBufferHandle m_hBrickList;
  plGALBufferHandle m_hBrickArgs;

  // Multigrid levels 1..: solution, right-hand side, type.
  plGALBufferHandle m_hLevelX[GPU_FLUID_LIQUID_MAX_LEVELS];
  plGALBufferHandle m_hLevelB[GPU_FLUID_LIQUID_MAX_LEVELS];
  plGALBufferHandle m_hLevelType[GPU_FLUID_LIQUID_MAX_LEVELS];

  // MGPCG: residual, preconditioned residual, direction, A * direction (per cell), the per-brick dot products and
  // the scalars.
  plGALBufferHandle m_hPcgR;
  plGALBufferHandle m_hPcgZ;
  plGALBufferHandle m_hPcgP;
  plGALBufferHandle m_hPcgQ;
  plGALBufferHandle m_hPcgPartials;
  plGALBufferHandle m_hPcgScalars;

  plGALBufferHandle m_hCounters;
  plGALBufferHandle m_hStats;
  plGALBufferHandle m_hShapes;

  // Level set: the raw phi of the substep (and of the frame's surface), the smoothing scratch, the surface of the
  // last two frames with a per-brick tag of the extraction that wrote it; the surface-nets mesh.
  plGALBufferHandle m_hPhiRaw;
  plGALBufferHandle m_hPhiTmp;
  plGALBufferHandle m_hSurfacePhi[2];
  plGALBufferHandle m_hSurfaceStamp[2];
  plGALBufferHandle m_hSurfaceVertices;
  plGALBufferHandle m_hSurfaceQuads;
  plGALBufferHandle m_hSurfaceCubeMap;
  plGALBufferHandle m_hSurfaceCounters;
  plGALBufferHandle m_hSurfaceDrawArgs;
  plUInt32 m_uiMaxSurfaceVertices = 0;
  plUInt32 m_uiMaxSurfaceQuads = 0;
  plUInt32 m_uiSurfaceTag = 0;      ///< extractions so far; the tag of the next one
  plUInt32 m_uiSurfaceCurrent = 0;  ///< which m_hSurfacePhi holds the last extracted surface
  bool m_bHasSurface = false;

  // Inflow: the share of a particle each source owes from earlier ticks, so a low flow rate still arrives.
  static constexpr plUInt32 s_uiMaxSources = 4;
  float m_SourceCarry[s_uiMaxSources] = {};

  // Render-thread state between ticks.
  plUInt32 m_uiCurrentSet = 0;       ///< set holding the particles after the last recorded tick
  plUInt32 m_uiLastResetCounter = 0xFFFFFFFFu;
  plUInt64 m_uiLastTick = 0;
  plUInt32 m_uiTicksSinceReset = 0;
  bool m_bHasTick = false;
  float m_fLastSubstepDt = 0.0f;
  plUInt32 m_uiLastSubsteps = 0;

  // Max speeds of the last readbacks (a ring, newest at m_uiSpeedNext - 1) and the render frame of the newest.
  static constexpr plUInt32 s_uiMaxSpeedHistory = 16;
  float m_SpeedHistory[s_uiMaxSpeedHistory] = {};
  plUInt32 m_uiSpeedCount = 0;
  plUInt32 m_uiSpeedNext = 0;
  plUInt64 m_uiSpeedFrame = 0;
  plUInt32 m_uiTicksBelow = 0; ///< ticks in a row that wanted fewer substeps than the last

  plHybridArray<plGpuFluidLiquidShape, GPU_FLUID_LIQUID_MAX_SHAPES> m_UploadedShapes;
  bool m_bShapesUploaded = false;
  plVec3 m_vDomainOrigin = plVec3::MakeZero();

  // Static solids: the baked distance per cell, and the bake's uploads (kept until the next bake).
  plGALBufferHandle m_hSolidSdf;
  plGALBufferHandle m_hSolidTriangles;
  plGALBufferHandle m_hSolidBins;
  plGALBufferHandle m_hSolidIndices;
  plGALBufferHandle m_hSolidBricks;
  plGpuFluidLiquidSolidBakePtr m_pBakedSolids; ///< the bake the SDF holds
  bool m_bHasSolidSdf = false;
  plUInt32 m_uiSolidTriangles = 0;
  plUInt32 m_uiSolidBakes = 0;

  // Regions: the drains and the wake boxes, uploaded when they change.
  plGALBufferHandle m_hRegions;
  plHybridArray<plGpuFluidLiquidShape, GPU_FLUID_LIQUID_MAX_REGIONS> m_UploadedRegions;

  // Sleep: per brick whether a particle was faster than the sleep speed this substep, and the calm substeps in a row.
  plGALBufferHandle m_hBrickSpeed;
  plGALBufferHandle m_hBrickCalm;
  bool m_bAsleep = false;
  bool m_bWakePending = true;    ///< the next substep starts every brick awake
  plUInt64 m_uiWakeFrame = 0;    ///< render frame of the last wake; stats from before it cannot put the volume to sleep
  plUInt64 m_uiAsleepKey = 0;    ///< the inputs that can wake the volume, as they were when it fell asleep
  plUInt64 m_uiTicksAsleep = 0;
};
