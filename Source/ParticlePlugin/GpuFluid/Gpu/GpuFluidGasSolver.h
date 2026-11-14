#pragma once

#include <Foundation/Containers/StaticArray.h>
#include <Foundation/Math/Vec3.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidSparseGrid.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>

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

  float m_fBuoyancy = 4.0f;            ///< m/s^2 per unit of (normalised) temperature above ambient
  float m_fDensityWeight = 0.2f;       ///< m/s^2 per unit of density, pulling down
  float m_fAmbientTemperature = 0.0f;  ///< normalised
  float m_fCooling = 0.8f;             ///< 1/s
  float m_fDissipation = 0.02f;        ///< density, 1/s
  float m_fVelocityDissipation = 0.0f; ///< 1/s
  float m_fFuelDissipation = 0.0f;     ///< 1/s
  float m_fDensityMax = 4.0f;          ///< density clamp, also the march's majorant

  /// Gas fades out over this many voxels before an open face (extra dissipation up to m_fEdgeFadeRate at the face), so
  /// it leaves the box as a thinning wisp instead of being cut off along the box's edge. 0 turns it off.
  float m_fEdgeFadeVoxels = 0.0f;
  float m_fEdgeFadeRate = 6.0f; ///< 1/s

  plUInt32 m_uiPressureIterations = 40; ///< red-black Gauss-Seidel iterations per tick (each one a red and a black sweep)
  float m_fOverRelaxation = 1.7f;

  /// MacCormack correction of the scalars' advection (Medium and up); plain semi-Lagrangian otherwise. The velocity is
  /// always semi-Lagrangian in P1.
  bool m_bMacCormack = true;

  float m_fAllocThreshold = 1e-3f; ///< a brick with a smaller peak is empty for the march
  float m_fMeanFloor = 1e-4f;      ///< smallest brick mean a brick with content gets in the brick table

  plStaticArray<plGpuFluidGasEmitter, 4> m_Emitters;
};

/// \brief The gas solver of one volume: its atlases and pressure buffers, and the passes of a frame.
///
/// | Resource          | Format                                     | Per pool slot |
/// |-------------------|--------------------------------------------|---------------|
/// | Velocity[2]       | RGBA16F atlas, MAC staggered in the texel  | 10 cubed      |
/// | Scalar[2]         | RGBA16F atlas: density, temperature, fuel, burn | 10 cubed |
/// | Scratch           | RGBA16F atlas (MacCormack and curl, P3)    | 10 cubed      |
/// | Pressure          | float, Phi = p dt / rho (m^2/s), warm start | 512          |
/// | Divergence        | float, 1/s                                 | 512           |
///
/// Scalar[0] always holds the current scalars and is what the VDB march reads; Scalar[1] holds them between the
/// advection and GasForces, which writes them back. The velocity sides ping-pong.
///
/// Phases of a tick, one render-graph pass each (so each is timed under Renderer/GPU/Gas/*):
/// - Gas/Emit: GasEmit per source, in place.
/// - Gas/Advect: GasAdvect (RK2 semi-Lagrangian, hardware trilinear), GasHalo of the scalars; with MacCormack the
///   reverse trace into Scratch, the limited correction and GasHalo again.
/// - Gas/Forces: GasForces (buoyancy, cooling, dissipation, clamp), GasHalo of the velocity.
/// - Gas/Pressure: GasDivergence, red-black SOR sweeps, GasProject, GasHalo of the velocity (keep mode).
/// Once per frame, Gas/Finalize: GasFinalize (NaN guard, brick table, stats).
///
/// GPU resources are created and destroyed on the render thread, where every Add* function runs too.
class PL_PARTICLEPLUGIN_DLL plGpuFluidGasSolver
{
public:
  plGpuFluidGasSolver();
  ~plGpuFluidGasSolver();

  plResult Create(const plVec3U32& vAtlasBricks, plUInt32 uiCapacity);
  void Destroy();

  bool IsCreated() const { return !m_hScalar[0].IsInvalidated(); }

  /// The current scalars, the atlas the VDB march reads.
  plGALTextureHandle GetScalarAtlas() const { return m_hScalar[0]; }

  /// Phase Gas/Alloc, second pass: clears every new brick (P1: all of them) in every atlas and buffer.
  void AddClearPass(plRenderGraph& ref_graph, const plGpuFluidSparseGrid& grid, const plGpuFluidSparseGridGraph& gridGraph);

  /// Every tick of the frame, then Finalize. hStats receives the counters (cleared by the caller).
  void AddFramePasses(plRenderGraph& ref_graph, const plGpuFluidSparseGrid& grid, const plGpuFluidSparseGridGraph& gridGraph, plRenderGraphBufferHandle hStats,
    const plGpuFluidGasSimulation& simulation);

private:
  plGALTextureHandle m_hVelocity[2];
  plGALTextureHandle m_hScalar[2];
  plGALTextureHandle m_hScratch;
  plGALBufferHandle m_hPressure;
  plGALBufferHandle m_hDivergence;
  plGALSamplerStateHandle m_hLinearClamp;

  /// Which velocity side is current. Render thread.
  plUInt32 m_uiVelocitySide = 0;

  plShaderResourceHandle m_hClearShader;
  plShaderResourceHandle m_hEmitShader;
  plShaderResourceHandle m_hAdvectShader;
  plShaderResourceHandle m_hHaloShader;
  plShaderResourceHandle m_hForcesShader;
  plShaderResourceHandle m_hDivergenceShader;
  plShaderResourceHandle m_hPressureShader;
  plShaderResourceHandle m_hProjectShader;
  plShaderResourceHandle m_hFinalizeShader;
};
