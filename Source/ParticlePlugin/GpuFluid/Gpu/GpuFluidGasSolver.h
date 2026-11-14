#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/StaticArray.h>
#include <Foundation/Math/Vec3.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidSparseGrid.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>

/// Most solids a gas volume takes per tick (GPU_FLUID_MAX_SOLID_SHAPES).
constexpr plUInt32 plGpuFluidMaxSolidShapes = 64;

/// Most dense multigrid levels (GPU_FLUID_MG_MAX_DENSE_LEVELS).
constexpr plUInt32 plGpuFluidMaxDenseLevels = 12;

/// Solver probe words of the stats (GPU_FLUID_STAT_PROBE_COUNT).
constexpr plUInt32 plGpuFluidStatProbeCount = 16;

/// One analytic solid of a gas tick, laid out like plGpuFluidSolidShape (GpuFluidGasConstants.h), which the solver's
/// .cpp checks. Positions in voxels of the volume's grid, velocities in m/s in the volume's space.
struct plGpuFluidSolid
{
  PL_DECLARE_POD_TYPE();

  plVec4 m_vCenterType;      ///< xyz centre, w GPU_FLUID_SHAPE_*
  plVec4 m_vAxis0;           ///< box local x / capsule axis / plane normal, w: box half extent x, capsule half height, sphere radius
  plVec4 m_vAxis1;           ///< box local y, w: box half extent y, capsule radius
  plVec4 m_vAxis2;           ///< box local z, w: box half extent z
  plVec4 m_vLinearVelocity;  ///< m/s at the centre
  plVec4 m_vAngularVelocity; ///< rad/s about the centre
  plVec4 m_vBoundsMin;       ///< AABB in voxels
  plVec4 m_vBoundsMax;
};

/// One gas source of a frame, in voxels of the volume's virtual grid (cell (i,j,k) spans [i, i+1)).
///
/// P1: the gas volume's own source. P4 adds the emitter component, which fills more of these.
struct plGpuFluidGasEmitter
{
  plGpuFluidEmitterShape::Enum m_Shape = plGpuFluidEmitterShape::None;
  plVec3 m_vCenterVoxels = plVec3::MakeZero();
  float m_fRadiusVoxels = 8.0f;
  plVec3 m_vHalfExtentsVoxels = plVec3(8.0f);
  float m_fSoftnessVoxels = 2.0f;

  float m_fDensityRate = 1.0f; ///< density added per second at full weight
  float m_fTemperature = 1.0f; ///< normalised temperature the source holds at full weight
  float m_fFuelRate = 0.0f;    ///< fuel added per second at full weight (burnt from P4 on)

  plVec3 m_vVelocity = plVec3::MakeZero(); ///< m/s, in the volume's space
  bool m_bDriveVelocity = true;            ///< blend the face velocities towards m_vVelocity (plus jitter)

  float m_fNoiseStrength = 0.5f;   ///< 0..1: puffs in the density rate and curl-noise jitter of the velocity
  float m_fNoiseFrequency = 0.08f; ///< 1/voxel
};

/// \brief Which solver the gas pressure uses (GpuFluid.Gas.PressureSolver).
struct plGpuFluidPressureSolver
{
  enum Enum : plUInt8
  {
    RedBlackSOR = 0, ///< The P1 reference: red-black SOR sweeps, m_uiPressureIterations of them (GPU_FLUID_PRESSURE_RBSOR).
    Multigrid = 1,   ///< Warm-started V-cycles (GpuFluidMG.hlsli), m_uiPressureCycles of them (GPU_FLUID_PRESSURE_MULTIGRID).
  };
};

/// Settings of the multigrid V-cycle.
struct plGpuFluidMultigridSettings
{
  plUInt32 m_uiPreSmooth = 2;       ///< red-black sweeps before the restriction, every level
  plUInt32 m_uiPostSmooth = 2;      ///< red-black sweeps after the prolongation, every level
  plUInt32 m_uiCoarsestSweeps = 4;  ///< red-black sweeps on the coarsest dense level (at most 8 cells)
  float m_fOmega = 1.0f;            ///< over-relaxation of the smoother

  /// Red-black Gauss-Seidel sweeps over the whole fine level after each V-cycle, one dispatch per colour, every
  /// neighbour current. The block red-black smoother leaves the first colour's brick faces with the residual of the
  /// second colour's last change; these sweeps spread it, at the cost of two fine-level dispatches each. Off by default.
  plUInt32 m_uiFinalSweeps = 0;

  /// Scale of every coarse-grid correction (1: as the coarse level computed it).
  float m_fProlongScale = 1.0f;

  /// The brick colour that pre-smooths the fine level first (GpuFluid.Gas.MGDownColour).
  plUInt32 m_uiDownColour = 0;

  /// Every V-cycle after a tick's first stops at M (a two-level F/M cycle, no C or dense levels): cheaper, but it only
  /// smooths what the first cycle left along the brick faces. Off by default: under a wind through open faces the
  /// smooth part of the error changes every tick, and the short second cycle left L_inf(div) at 0.0124 of its value before
  /// the projection (Gas Wind), over N2's 1e-2; two full V-cycles reach 0.006.
  bool m_bShortLaterCycles = false;

  /// Each step of Gas/Pressure in a pass of its own (Gas/Pressure Operator, Solve 1..n, Project), so each is timed
  /// (GpuFluid.Gas.ProfilePressure).
  bool m_bProfilePressure = false;

  /// Dense levels with at most this many cells run in one group (one dispatch for the whole tail of the V-cycle);
  /// larger ones get one thread per cell and a dispatch per step.
  plUInt32 m_uiDenseGroupCells = 4096;
};

/// Test hook (N3 Poisson): replaces the frame's ticks with one solve of a manufactured Poisson problem with a known
/// solution (GasPoisson.plShader), from a cold start. The stats' probe words get the residual after each V-cycle (or
/// every m_uiProbeInterval SOR iterations) when m_bProbeEachCycle, and the final residual and error.
struct plGpuFluidPoissonTest
{
  bool m_bEnabled = false;
  bool m_bProbeEachCycle = false;
  plUInt32 m_uiProbeInterval = 10; ///< SOR iterations between probes

  /// Multigrid: each stage of every V-cycle in its own render-graph pass (Gas/MG Fine Down, Mid Down, Dense, Mid Up,
  /// Fine Up), so each is timed; there is no Gas/Pressure pass then.
  bool m_bProfileStages = false;
};

/// Probe words of the stats (GPU_FLUID_STAT_PROBE + ...): residual after cycle k at k (0 = before the solve, up to
/// plGpuFluidProbeFinalResidual - 1), then the final residual and error, and the initial error.
constexpr plUInt32 plGpuFluidProbeMaxCycles = 12;
constexpr plUInt32 plGpuFluidProbeFinalResidual = 13;
constexpr plUInt32 plGpuFluidProbeFinalError = 14;
constexpr plUInt32 plGpuFluidProbeInitialError = 15;

/// Everything the gas solver needs for one frame of one volume, gathered on the update thread.
struct plGpuFluidGasSimulation
{
  /// Fixed ticks to run this frame (0 to plGpuFluidMaxGasTicksPerFrame) and their length.
  plUInt32 m_uiTicks = 0;
  float m_fTickSeconds = plGpuFluidGasTick;

  /// Simulation time at the start of the first tick, for the noise of the sources.
  float m_fTime = 0.0f;

  float m_fVoxelSize = 0.0625f; ///< metres
  plBitflags<plGpuFluidFaces> m_OpenFaces = plGpuFluidFaces::Default;

  /// Unit direction against gravity, in the volume's space.
  plVec3 m_vUp = plVec3(0.0f, 0.0f, 1.0f);

  /// Ambient wind in the volume's space, m/s: open air, inflow through open faces, the start velocity of new bricks.
  plVec3 m_vAmbientWind = plVec3::MakeZero();

  float m_fBuoyancy = 4.0f;            ///< m/s^2 per unit of (normalised) temperature above ambient
  float m_fDensityWeight = 0.2f;       ///< m/s^2 per unit of density, pulling down
  float m_fAmbientTemperature = 0.0f;  ///< normalised
  float m_fCooling = 0.8f;             ///< 1/s
  float m_fDissipation = 0.02f;        ///< density, 1/s
  float m_fVelocityDissipation = 0.0f; ///< 1/s
  float m_fFuelDissipation = 0.0f;     ///< 1/s
  float m_fDensityMax = 4.0f;          ///< density clamp, also the march's majorant

  /// Vorticity confinement epsilon (0: off), GasCurl + GasForces: the acceleration is eps dx k (N x w).
  float m_fVorticity = 0.3f;
  float m_fVorticityRate = 10.0f; ///< k, 1/s

  /// Gas fades out over this many voxels before an open face (extra dissipation up to m_fEdgeFadeRate at the face), so
  /// it leaves the box as a thinning wisp instead of being cut off along the box's edge. 0 turns it off.
  float m_fEdgeFadeVoxels = 0.0f;
  float m_fEdgeFadeRate = 6.0f; ///< 1/s

  plGpuFluidPressureSolver::Enum m_PressureSolver = plGpuFluidPressureSolver::Multigrid;
  plUInt32 m_uiPressureCycles = 2;      ///< multigrid V-cycles per tick
  plUInt32 m_uiPressureIterations = 40; ///< red-black SOR iterations per tick (each one a red and a black sweep)
  float m_fOverRelaxation = 1.7f;       ///< of the SOR solver
  plGpuFluidMultigridSettings m_Multigrid;

  /// MacCormack correction of the scalars' advection (Medium and up); plain semi-Lagrangian otherwise.
  bool m_bMacCormack = true;

  /// MacCormack correction of the velocity's advection too.
  bool m_bMacCormackVelocity = true;

  /// Each kernel of Gas/Advect and Gas/Forces in a render-graph pass of its own (Gas/Advect SL, Gas/Forces Curl, ...),
  /// so each is timed (GpuFluid.Gas.ProfileKernels).
  bool m_bProfileKernels = false;

  /// How far ahead (seconds) GasFinalize's flux marking looks for where content moves (design plan: 2 x the frame's
  /// ticks).
  float m_fAllocLookahead = 2.0f * 2.0f / 60.0f;

  /// Closed-box mass correction (GpuFluid.Gas.MassCorrection, only for volumes with every face closed and the whole
  /// box allocated): GasForces scales the density so the advection does not change the total mass.
  bool m_bMassCorrection = false;

  /// Test hook (GpuFluid.Debug.InjectNaN): the first source writes NaN into one density texel on the frame's first
  /// tick, so a test can see the NaN guards count.
  bool m_bInjectNaN = false;

  plGpuFluidPoissonTest m_PoissonTest;

  float m_fAllocThreshold = 1e-3f; ///< a brick with a smaller peak is empty for the march
  float m_fMeanFloor = 1e-4f;      ///< smallest brick mean a brick with content gets in the brick table

  plStaticArray<plGpuFluidGasEmitter, 4> m_Emitters;

  /// Solids of every tick, m_uiSolidsPerTick per tick one after the other (tick k from k * m_uiSolidsPerTick), at most
  /// plGpuFluidMaxSolidShapes per tick.
  plDynamicArray<plGpuFluidSolid> m_Solids;
  plUInt32 m_uiSolidsPerTick = 0;
};

/// \brief The gas solver of one volume: its atlases and pressure buffers, and the passes of a frame.
///
/// | Resource          | Format                                     | Per pool slot |
/// |-------------------|--------------------------------------------|---------------|
/// | Velocity[2]       | RGBA16F atlas, MAC staggered in the texel  | 10 cubed      |
/// | Scalar[2]         | RGBA16F atlas: density, temperature, fuel, burn | 10 cubed |
/// | Scratch           | RGBA16F atlas (MacCormack, curl)           | 10 cubed      |
/// | Pressure[2]       | float, Phi = p dt / rho (m^2/s), warm start; [1] is the multigrid's other side | 512 |
/// | Divergence        | float, 1/s                                 | 512           |
/// | SolidFlags        | uint, one bit per cell (GasSolids)         | 16            |
/// | FaceCodes         | uint, the fine operator (GasMGSetup)       | 512           |
/// | MGLocal           | float, x and b of M and C                  | 216           |
/// | MGCoeff           | float4, coefficients of M and C            | 144           |
/// | MGDense, MGDenseCoeff | float2 x/b and 2 float4 per cell of the dense levels (virtual brick grid and halvings) | - |
/// | SolidShapes       | the frame's solids, 2 ticks x 64           | -             |
///
/// Scalar[0] always holds the current scalars and is what the VDB march reads; Scalar[1] holds them between the
/// advection and GasForces, which writes them back. The velocity sides ping-pong.
///
/// Phases of a tick, one render-graph pass each (so each is timed under Renderer/GPU/Gas/*):
/// - Gas/Emit: GasEmit per source, in place.
/// - Gas/Advect: GasAdvect (RK2 semi-Lagrangian, hardware trilinear), GasHalo of the scalars; with MacCormack the
///   reverse trace into Scratch, the limited correction and GasHalo again; the same for the velocity.
/// - Gas/Forces: GasCurl into Scratch and its halo (vorticity), GasForces (buoyancy, confinement, cooling, dissipation,
///   clamp; counts NaN and clamped faces), GasSolids (solid flags, solid face velocities, scalars cleared), GasHalo of the
///   velocity.
/// - Gas/Pressure: GasMGSetup (the operator), the dense levels' setup, GasDivergence, the solver (multigrid V-cycles or
///   red-black SOR sweeps), GasProject, GasHalo of the velocity (keep mode).
/// Once per frame, Gas/Finalize: GasFinalize (NaN guard, brick table, stats).
///
/// GPU resources are created and destroyed on the render thread, where every Add* function runs too.
class PL_PARTICLEPLUGIN_DLL plGpuFluidGasSolver
{
public:
  plGpuFluidGasSolver();
  ~plGpuFluidGasSolver();

  plResult Create(const plVec3U32& vBrickGrid, const plVec3U32& vAtlasBricks, plUInt32 uiCapacity);
  void Destroy();

  bool IsCreated() const { return !m_hScalar[0].IsInvalidated(); }

  /// The current scalars, the atlas the VDB march reads.
  plGALTextureHandle GetScalarAtlas() const { return m_hScalar[0]; }

  /// Phase Gas/Alloc, second pass: clears every brick assigned this frame (the new list) in every atlas and buffer, then
  /// fills the new bricks' aprons in the current scalars and velocity from their neighbours. bAllocationChanged:
  /// the frame may have assigned or freed bricks, so the pressure operator is rebuilt.
  void AddClearPass(plRenderGraph& ref_graph, const plGpuFluidSparseGrid& grid, const plGpuFluidSparseGridGraph& gridGraph, const plVec3& vAmbientWind, bool bAllocationChanged,
    plBitflags<plGpuFluidFaces> openFaces);

  /// Every tick of the frame, then Finalize. hStats receives the counters (cleared by the caller).
  void AddFramePasses(plRenderGraph& ref_graph, const plGpuFluidSparseGrid& grid, const plGpuFluidSparseGridGraph& gridGraph, plRenderGraphBufferHandle hStats,
    const plGpuFluidGasSimulation& simulation);

  /// Number of dense multigrid levels of a brick grid, and the cells of level k and before it.
  static plUInt32 ComputeDenseLevels(const plVec3U32& vBrickGrid, plUInt32* pOutCells = nullptr, plUInt32* pOutTotalCells = nullptr);

private:
  /// Three sides: the current one, the semi-Lagrangian result and the MacCormack result (which becomes current).
  plGALTextureHandle m_hVelocity[3];
  plGALTextureHandle m_hScalar[2];
  plGALTextureHandle m_hScratch;
  plGALBufferHandle m_hPressure[2];
  plGALBufferHandle m_hDivergence;
  plGALBufferHandle m_hSolidFlags;
  plGALBufferHandle m_hFaceCodes;
  plGALBufferHandle m_hMGLocal;
  plGALBufferHandle m_hMGCoeff;
  plGALBufferHandle m_hMGMask;
  plGALBufferHandle m_hMGDense;
  plGALBufferHandle m_hMGDenseCoeff;
  plGALBufferHandle m_hSolidShapes;
  plGALSamplerStateHandle m_hLinearClamp;

  plVec3U32 m_vBrickGrid = plVec3U32::MakeZero();
  plUInt32 m_uiDenseLevels = 0;
  plUInt32 m_uiDenseCells[plGpuFluidMaxDenseLevels] = {};

  /// Which velocity side is current. Render thread.
  plUInt32 m_uiVelocitySide = 0;

  /// The pressure operator (face codes, multigrid coefficients) on the GPU, and what it was built from. Render thread.
  bool m_bOperatorValid = false;
  bool m_bOperatorHadSolids = false;
  bool m_bOperatorDense = false; ///< the dense levels' coefficients too
  plUInt8 m_uiOperatorFaces = 0xFF;

  plShaderResourceHandle m_hClearShader;
  plShaderResourceHandle m_hEmitShader;
  plShaderResourceHandle m_hAdvectShader;
  plShaderResourceHandle m_hHaloShader;
  plShaderResourceHandle m_hCurlShader;
  plShaderResourceHandle m_hForcesShader;
  plShaderResourceHandle m_hSolidsShader;
  plShaderResourceHandle m_hDivergenceShader;
  plShaderResourceHandle m_hPressureShader;
  plShaderResourceHandle m_hMGSetupShader;
  plShaderResourceHandle m_hMGFineShader;
  plShaderResourceHandle m_hMGMidShader;
  plShaderResourceHandle m_hMGDenseShader;
  plShaderResourceHandle m_hPoissonShader;
  plShaderResourceHandle m_hProjectShader;
  plShaderResourceHandle m_hFinalizeShader;
};
