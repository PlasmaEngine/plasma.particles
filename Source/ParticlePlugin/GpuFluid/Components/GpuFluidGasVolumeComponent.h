#pragma once

#include <Core/World/World.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGpuManager.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>
#include <RendererCore/Volumes/VDBVolumeComponent.h>

struct plMsgUpdateLocalBounds;
struct plMsgExtractRenderData;

using plGpuFluidGasVolumeComponentManager = plComponentManagerSimple<class plGpuFluidGasVolumeComponent, plComponentUpdateType::Always>;

/// \brief A box of simulated gas, rendered by the VDB march straight from the GPU.
///
/// The volume is a sparse grid of 8 cubed bricks fixed in the object's space: Extents over VoxelSize, rounded up to
/// whole bricks. Its atlases and brick table live on the GPU and are handed to the march every frame as
/// plVDBVolumeGpuRenderData, with no resource and no copy in between.
///
/// The gas solver (plGpuFluidGasSolver) steps at a fixed tick of 1/60 s, at most two ticks a frame, with buoyancy
/// against the physics world's gravity (or -Z without physics). P1 allocates every brick of the box (up to MaxBricks);
/// the volume's own source (Emitter*) feeds it. The analytic debug source replaces the simulation with a known sphere,
/// which checks the hand-off against a baked volume.
class PL_PARTICLEPLUGIN_DLL plGpuFluidGasVolumeComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidGasVolumeComponent, plComponent, plGpuFluidGasVolumeComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plGpuFluidGasVolumeComponent

public:
  plGpuFluidGasVolumeComponent();
  ~plGpuFluidGasVolumeComponent();

  /// Authored size of the box, in metres. The grid rounds it up to whole bricks.
  plVec3 m_vExtents = plVec3(4.0f);                                   // [ property ]
  float m_fVoxelSize = 0.0625f;                                       // [ property ] metres
  plUInt32 m_uiMaxBricks = 0;                                         // [ property ] atlas pool, 0: the quality's cap (GetTierBrickCap)
  bool m_bAllocateAll = false;                                        // [ property ] closed-box mode: the whole box stays allocated (up to the pool)
  plEnum<plGpuFluidQuality> m_Quality;                                // [ property ]
  plBitflags<plGpuFluidFaces> m_OpenFaces;                            // [ property ] the rest are walls

  // Simulation
  float m_fBuoyancy = 6.0f;                                           // [ property ] m/s^2 per unit temperature
  float m_fDensityWeight = 0.1f;                                      // [ property ] m/s^2 per unit density
  float m_fCooling = 0.5f;                                            // [ property ] 1/s
  float m_fDissipation = 0.03f;                                       // [ property ] density, 1/s
  float m_fVelocityDissipation = 0.0f;                                // [ property ] 1/s
  float m_fDensityMax = 4.0f;                                         // [ property ] clamp and march majorant
  plUInt32 m_uiPressureIterations = 40;                               // [ property ] red-black SOR iterations per tick (GpuFluid.Gas.PressureSolver 0)
  plUInt32 m_uiPressureCycles = 0;                                    // [ property ] multigrid V-cycles per tick, 0: the quality's default
  float m_fEdgeFade = 0.75f;                                          // [ property ] metres before an open face over which gas fades
  float m_fVorticity = 0.3f;                                          // [ property ] vorticity confinement epsilon (Medium and up)
  float m_fWindInfluence = 1.0f;                                      // [ property ] how much of the world's wind the gas takes as its ambient velocity
  bool m_bPhysicsBodies = true;                                       // [ property ] dynamic physics bodies in the box act as moving solids (their bounds)

  // The volume's own source.
  plEnum<plGpuFluidEmitterShape> m_EmitterShape;                      // [ property ]
  plVec3 m_vEmitterOffset = plVec3(0.0f, 0.0f, -1.4f);                // [ property ] metres, from the box centre
  float m_fEmitterRadius = 0.5f;                                      // [ property ] metres (sphere)
  plVec3 m_vEmitterHalfExtents = plVec3(0.4f, 0.4f, 0.2f);            // [ property ] metres (box)
  float m_fEmitterSoftness = 0.2f;                                    // [ property ] metres from full strength to none
  float m_fEmissionRate = 5.0f;                                       // [ property ] density per second
  float m_fEmitterTemperature = 1.0f;                                 // [ property ] 0..1
  plVec3 m_vEmitterVelocity = plVec3(0.0f, 0.0f, 2.0f);               // [ property ] m/s, volume space
  float m_fEmitterNoise = 0.9f;                                       // [ property ] 0..1
  float m_fEmitterNoiseScale = 0.3f;                                  // [ property ] metres, size of the puffs

  plEnum<plGpuFluidDebugSource> m_DebugSource;                        // [ property ]
  float m_fSphereRadius = 1.4f;                                       // [ property ] metres
  float m_fSphereSoftness = 0.5f;                                     // [ property ] metres from full density to none
  float m_fPulseAmplitude = 0.15f;                                    // [ property ] fraction of the radius
  float m_fPulsePeriod = 2.5f;                                        // [ property ] seconds
  float m_fPeakDensity = 1.0f;                                        // [ property ] atlas units, before DensityScale
  float m_fPeakTemperature = 0.8f;                                    // [ property ] 0..1
  float m_fAllocThreshold = 1e-3f;                                    // [ property ] peak density a brick needs to be kept

  float m_fDensityScale = 12.0f;                                       // [ property ]
  plColor m_Albedo = plColor::White;                                  // [ property ]
  plColor m_EmissionColor = plColor::Black;                           // [ property ]
  float m_fEmissionScale = 0.0f;                                      // [ property ]
  float m_fAnisotropy = 0.2f;                                         // [ property ]
  plEnum<plVDBVolumeRenderMode> m_RenderMode;                         // [ property ]

  /// Sets the pulse clock and the simulation clock, so a scene looks the same whatever ran before it.
  void ResetClock(plTime time = plTime::MakeZero())
  {
    m_PulseTime = time;
    m_SimAccumulator = plTime::MakeZero();
    m_SimTime = plTime::MakeZero();
    m_uiTicksRun = 0;
  }

  /// Solver ticks requested since the volume was created or the clock reset.
  plUInt64 GetTicksRun() const { return m_uiTicksRun; }

  /// Bricks per axis of the virtual grid.
  plVec3U32 GetBrickGrid() const;

  /// The box the grid actually covers: the extents rounded up to whole bricks.
  plVec3 GetGridExtents() const;

  /// What the GPU counted, two to three frames late. False until the first readback.
  bool GetStats(plGpuFluidGasStats& out_stats) const;

  /// Whether the volume was handed to the march in the last extraction.
  bool WasPublished() const { return m_bPublished; }

  /// Test hook (N3): solve a manufactured Poisson problem every frame instead of simulating.
  plGpuFluidPoissonTest m_DebugPoissonTest;

  /// Solids handed to the solver in the last update, per tick (for tests and debugging).
  plUInt32 GetLastSolidCount() const { return m_uiLastSolids; }

  /// Ambient wind of the last update, m/s in the volume's space.
  const plVec3& GetLastAmbientWind() const { return m_vLastWind; }

  /// Multigrid V-cycles a tick runs at this quality when PressureCycles is 0.
  static plUInt32 GetDefaultPressureCycles(plGpuFluidQuality::Enum quality);

  /// Pool slots of a quality tier (design plan section 9): Low 512, Medium 1536, High 3072, Ultra 6144. The pool is
  /// created at this size when the volume activates and never resized.
  static plUInt32 GetTierBrickCap(plGpuFluidQuality::Enum quality);

  /// Pool slots the volume has (0 before its GPU volume exists).
  plUInt32 GetPoolCapacity() const;

  /// Test hook (N1): copy the allocator's state to the CPU every frame, with the per virtual brick part.
  bool m_bDebugGridReadback = false;

  /// The newest copy of the allocator's state (m_bDebugGridReadback or GpuFluid.Debug.Bricks), false until one arrived.
  bool GetGridDebug(plGpuFluidGridDebug& out_debug) const;

protected:
  void Update();
  void OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg);
  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;

private:
  plGpuFluidGasSettings ComputeSettings() const;
  void EnsureVolume();

  /// Advances the fixed-tick clock by the frame's time and describes the frame's solver work.
  void FillSimulation(plTime timeDiff, plGpuFluidGasSimulation& out_simulation);

  /// The colliders (and dynamic physics bodies) overlapping the box, as shapes in voxels for every tick of the frame.
  void GatherSolids(plTime timeDiff, plGpuFluidGasSimulation& inout_simulation);

  /// GpuFluid.Debug.Bricks: every allocated brick as a line box, coloured by the bucket that keeps it.
  void DrawBrickDebug() const;

  void ReleaseVolume();

  plGpuFluidGpuManager* m_pManager = nullptr;
  plGpuFluidVolumeId m_VolumeId = 0;
  plGpuFluidGasSettings m_ActiveSettings;
  plTime m_PulseTime;

  /// Fixed-tick clock of the solver: unspent time, and the time the next tick starts at.
  plTime m_SimAccumulator;
  plTime m_SimTime;
  plUInt64 m_uiTicksRun = 0;

  /// The grid extents the owner's local bounds were last computed from; a change to Extents or VoxelSize refreshes them.
  plVec3 m_vBoundsExtents = plVec3::MakeZero();
  mutable bool m_bPublished = false;

  /// Last frame's global transform of every collider and physics body seen, for their velocities.
  plHashTable<plUInt64, plTransform> m_SolidTransforms;
  plUInt32 m_uiLastSolids = 0;
  plVec3 m_vLastWind = plVec3::MakeZero();
};
