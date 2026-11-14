#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Vec3.h>
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
  plUInt32 m_uiMaxSubsteps = 4;
  float m_fCflNumber = 1.5f; ///< substeps = ceil(vmax * dt / (cfl * dx))

  plUInt32 m_uiPressureCycles = 3;   ///< multigrid V-cycles per substep
  plUInt32 m_uiPreSmooth = 2;        ///< red-black Gauss-Seidel sweeps before restriction
  plUInt32 m_uiPostSmooth = 2;       ///< and after prolongation
  plUInt32 m_uiCoarsestSweeps = 48;  ///< sweeps on the coarsest level, in one dispatch
  /// Coarse right-hand side = this * sum of the 8 fine residuals. 0.5 (four times the mean) matches the unit-spacing
  /// stencil rediscretised on the coarse level.
  float m_fCoarseScale = 0.5f;

  float m_fTheta = 0.5f;          ///< ghost-fluid fraction of the free surface between a liquid and an air cell centre
  float m_fDensityGain = 0.3f;    ///< k of the local density correction k * max(n/n0 - 1, 0) / dt
  float m_fVolumeGain = 0.4f;     ///< c of the global volume control (fraction of the volume error corrected per substep)

  /// After a reset the first ticks run the most substeps: a column released from rest accelerates at up to g * H / dx
  /// near its foot before the read-back speed can see it.
  plUInt32 m_uiStartupTicks = 10;
  float m_fWallFriction = 0.0f;   ///< 0: free slip, 1: no slip
  float m_fWallMargin = 0.01f;    ///< particles stay this far inside walls and solids, in cells

  plUInt8 m_uiOpenFaces = 0; ///< GPU_FLUID_LIQUID_OPEN_* bits; closed faces are walls

  plHybridArray<plGpuFluidLiquidFill, 4> m_Fills;
  plHybridArray<plGpuFluidLiquidShape, GPU_FLUID_LIQUID_MAX_SHAPES> m_Shapes;

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

  plUInt32 m_uiActiveBricks = 0;   ///< bricks the grid kernels ran on (occupied plus one brick around)
  plUInt32 m_uiOccupiedBricks = 0; ///< bricks holding particles
  plUInt32 m_uiLiquidCells = 0;    ///< cells holding particles

  /// Volume measure in cells: a liquid cell whose neighbours are all liquid or solid counts 1, a surface cell
  /// min(n / n0, 1). Target: (seeded - drained) / n0.
  float m_fVolumeCells = 0.0f;
  float m_fTargetVolumeCells = 0.0f;

  float m_fMaxSpeed = 0.0f;        ///< m/s
  float m_fMeanSpeed = 0.0f;       ///< m/s
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
};

/// What the debug renderer needs to draw a liquid volume's particles this frame.
struct plGpuFluidLiquidPublish
{
  plGALBufferHandle m_hPoints;   ///< float4 per particle: world position, speed
  plGALBufferHandle m_hDrawArgs; ///< {6 * alive, 1, 0, 0}
  plUInt32 m_uiMaxParticles = 0;
  plVec3 m_vDomainOrigin = plVec3::MakeZero();
  plVec3 m_vDomainSize = plVec3::MakeZero();
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
/// - Liquid/P2G: gather of momentum and weight per face in int32 registers, normalise, extrapolate 3 layers;
/// - Liquid/Solve: classify, gravity, right-hand side with density and volume correction, multigrid V-cycles
///   (red-black Gauss-Seidel smoothing, warm started, ghost fluid at the free surface), project, extrapolate;
/// - Liquid/G2P: APIC/FLIP update, RK2 advection, push out of walls and solids.
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

  /// Records this tick's work. pStats are the newest stats read back (or null), which pick the substep count.
  void Record(plRenderGraph& ref_graph, const plGpuFluidLiquidShaders& shaders, const plGpuFluidLiquidFrameInput& input, const plGpuFluidLiquidStats* pStats, plUInt64 uiStatsAge);

  /// Whether a tick has been recorded since the solver was created, so its points may be drawn.
  bool HasRecorded() const { return m_bRecorded; }

  plGALBufferHandle GetStatsBuffer() const { return m_hStats; }
  void GetPublish(plGpuFluidLiquidPublish& out_publish) const;

  /// Decodes a stats readback.
  void DecodeStats(plArrayPtr<const plUInt8> data, plGpuFluidLiquidStats& out_stats) const;

  plVec3U32 GetCells() const { return m_vCells; }
  float GetCellSize() const { return m_fDx; }
  plUInt32 GetLevelCount() const { return m_uiLevelCount; }
  plUInt32 GetSubstepsOfLastRecord() const { return m_uiLastSubsteps; }

private:
  plGALBufferHandle CreateRawBuffer(plUInt64 uiBytes, const char* szName, bool bIndirect = false);

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

  plGALBufferHandle m_hCounters;
  plGALBufferHandle m_hStats;
  plGALBufferHandle m_hShapes;

  // Render-thread state between ticks.
  plUInt32 m_uiCurrentSet = 0;       ///< set holding the particles after the last recorded tick
  plUInt32 m_uiLastResetCounter = 0xFFFFFFFFu;
  plUInt64 m_uiLastTick = 0;
  plUInt32 m_uiTicksSinceReset = 0;
  bool m_bHasTick = false;
  float m_fLastSubstepDt = 0.0f;
  plUInt32 m_uiLastSubsteps = 0;
  plHybridArray<plGpuFluidLiquidShape, GPU_FLUID_LIQUID_MAX_SHAPES> m_UploadedShapes;
  bool m_bShapesUploaded = false;
  plVec3 m_vDomainOrigin = plVec3::MakeZero();
};
