#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Math/BoundingBox.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/SharedPtr.h>
#include <Foundation/Types/UniquePtr.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGasSolver.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidLiquidSolver.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/RendererFoundationDLL.h>

class plRenderGraph;
struct plRenderWorldRenderEvent;
class plGpuFluidReadbackRing;

/// Identifies a volume inside its world's manager. 0 is never a valid volume.
using plGpuFluidVolumeId = plUInt32;

/// What a gas volume's analytic debug source writes this frame. Positions are in voxels of the virtual grid.
struct plGpuFluidAnalyticSphere
{
  plVec3 m_vCenterVoxels = plVec3::MakeZero();
  float m_fRadiusVoxels = 0.0f;
  float m_fSoftnessVoxels = 1.0f;
  float m_fPeakDensity = 1.0f;
  float m_fPeakTemperature = 0.0f;
};

/// Everything a gas volume's GPU work needs for one frame, gathered on the update thread.
struct plGpuFluidGasFrameInput
{
  /// None: the solver runs from m_Simulation. Otherwise the analytic source replaces it.
  plGpuFluidDebugSource::Enum m_Source = plGpuFluidDebugSource::None;
  plGpuFluidAnalyticSphere m_Sphere;

  /// A brick is kept when its peak density is above this.
  float m_fAllocThreshold = 1e-3f;

  /// Smallest brick mean the brick table stores for a kept brick, in atlas density units. Zero would mark it empty.
  float m_fMeanFloor = 1e-4f;

  /// The closed-box mode of the allocator: every brick of the grid stays allocated (up to the pool), in virtual order.
  /// Otherwise the grid is sparse: bricks are allocated where the sources, the content and its motion need them.
  bool m_bAllocateAll = false;

  /// Copy the allocator's state to the CPU this frame (GetGasGridDebug), with the per virtual brick part when
  /// m_bDebugGridVirtual. For N1 and the brick debug view.
  bool m_bDebugGrid = false;
  bool m_bDebugGridVirtual = false;

  /// N11: hash the gas state in virtual brick order after the frame (plGpuFluidGasStats::m_uiStateHash).
  bool m_bHashState = false;

  plGpuFluidGasSimulation m_Simulation;
};

/// \brief Numbers the GPU counts for a gas volume, two to three frames late.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidGasStats
{
  /// Render frame (plRenderWorld::GetFrameCounter) the numbers were recorded in.
  plUInt64 m_uiFrame = 0;

  /// Solver ticks run in that frame (0 for the analytic source).
  plUInt32 m_uiTicks = 0;

  plUInt32 m_uiRequestedBricks = 0; ///< bricks whose content asked for a slot (analytic source)
  plUInt32 m_uiAllocatedBricks = 0; ///< bricks with content: kept by the analytic source, above the threshold for the solver
  plUInt32 m_uiActiveBricks = 0;    ///< bricks the solver ran on
  plUInt32 m_uiDeniedBricks = 0;    ///< the ones that found the pool full
  plUInt32 m_uiNaNCount = 0;        ///< interior values that came out NaN or infinite, counted where they were zeroed
  plUInt32 m_uiOverflowCount = 0;   ///< fixed-point stat contributions that were clamped, or sums that wrapped
  plUInt32 m_uiSpeedClamped = 0;    ///< face velocities the solver clamped to 32 m/s, over the frame's ticks

  /// Bricks with content span these, inclusive. Min > max on an axis when there are none.
  plVec3U32 m_vBoundsMinBrick = plVec3U32::MakeZero();
  plVec3U32 m_vBoundsMaxBrick = plVec3U32::MakeZero();

  /// Sum of the interior density, density times voxels.
  float m_fMass = 0.0f;

  /// Density-weighted centre of the content, as a fraction of the grid (0..1 per axis). Zero without mass.
  plVec3 m_vCentroid = plVec3::MakeZero();

  float m_fMaxDensity = 0.0f;
  float m_fMaxTemperature = 0.0f; ///< normalised
  float m_fMaxSpeed = 0.0f;       ///< largest face velocity, m/s

  /// |div u| of the frame's last tick before and after its projection, 1/s: the largest, and the mean over the active
  /// cells.
  float m_fMaxDivergenceBefore = 0.0f;
  float m_fMaxDivergenceAfter = 0.0f;
  float m_fMeanDivergenceBefore = 0.0f;
  float m_fMeanDivergenceAfter = 0.0f;

  /// The frame's allocation (Gas/Alloc): slots assigned and freed, free slots before the assignment, requests for
  /// bricks without a slot, denied requests per priority bucket (0 sources, 1 alive, 2 flux, 3 proximity), and the
  /// active bricks after it.
  plUInt32 m_uiNewBricks = 0;
  plUInt32 m_uiFreedBricks = 0;
  plUInt32 m_uiFreeSlots = 0;
  plUInt32 m_uiAllocRequests = 0;
  plUInt32 m_uiDeniedPerBucket[4] = {};
  plUInt32 m_uiRequestsPerBucket[4] = {}; ///< requests for bricks without a slot, each counted in the first bucket that asked
  plUInt32 m_uiAllocActive = 0;

  /// The state hash (plGpuFluidGasFrameInput::m_bHashState) of the atlases in virtual brick order, of frame
  /// m_uiHashFrame (0 until one arrived).
  plUInt64 m_uiStateHash = 0;
  plUInt64 m_uiHashFrame = 0;

  float m_fMassCorrection = 1.0f;    ///< the closed-box mass correction's factor of the frame's last tick (1 without one)
  float m_fMassAdvect[4] = {};       ///< with the mass correction: interior density before and after the advection of the frame's ticks 0 and 1

  plUInt32 m_uiSolidCells = 0;       ///< interior cells inside a solid on the frame's last tick
  float m_fSolidContactDensity = 0.0f; ///< largest density in a fluid cell next to a solid on that tick
  float m_fSolidDensity = 0.0f;      ///< largest density left in a solid cell at the end of the frame (must be 0)
  float m_fSolidCleared = 0.0f;      ///< density taken out of solid cells over the frame's ticks (gas lost to the solids)
  float m_fEmitted = 0.0f;           ///< density the sources added over the frame's ticks

  /// Solver probes of the N3 Poisson test (plGpuFluidProbe*): max |residual| or |error|.
  float m_fProbes[plGpuFluidStatProbeCount] = {};

  bool HasBounds() const { return m_uiAllocatedBricks > 0 && m_vBoundsMinBrick.x <= m_vBoundsMaxBrick.x; }
};

/// What the extractor needs to hand a gas volume to the VDB march this frame.
struct plGpuFluidGasPublish
{
  plGALTextureHandle m_hAtlas;
  plGALTextureHandle m_hIndirection;
  plVec3U32 m_vBrickGrid = plVec3U32::MakeZero();
  plVec3U32 m_vAtlasBricks = plVec3U32::MakeZero();
  plBoundingBox m_BoundsUVW = plBoundingBox::MakeFromMinMax(plVec3::MakeZero(), plVec3(1.0f));
};

/// \brief Owns the GPU side of every GpuFluid volume of one render world and records their work.
///
/// One manager per world. Each frame, from the BeginRender event, it builds one PreRender render graph that holds all
/// of the world's GPU work, so results are ready before the views render in the same frame, barriers are tracked and
/// each pass is timed.
///
/// Threading: volumes are created and destroyed on the update thread, inputs are set there too (double buffered by
/// the render world's data index), and the extractor asks what to publish. GPU resources are created and destroyed
/// on the render thread only.
///
/// Rules:
/// - Retire, never free: a destroyed volume's textures live on for a few frames, since render data naming them can
///   still be in flight.
/// - Nothing is published on a frame whose resources are fresh: a volume is only handed to the march once its first
///   GPU frame has been recorded.
class PL_PARTICLEPLUGIN_DLL plGpuFluidGpuManager
{
public:
  plGpuFluidGpuManager(plUInt32 uiWorldIndex);
  ~plGpuFluidGpuManager();

  /// The manager of a world, created on first use.
  static plGpuFluidGpuManager* GetForWorld(plUInt32 uiWorldIndex);

  /// Plugin lifetime: hooks the render event, and on shutdown frees every resource and graph.
  static void Startup();
  static void Shutdown();

  /// Registers a gas volume. Its GPU resources are created on the render thread at the next BeginRender.
  plGpuFluidVolumeId CreateGasVolume(const plGpuFluidGasSettings& settings);

  /// Unregisters a volume. Its resources are retired, not freed.
  void DestroyVolume(plGpuFluidVolumeId id);

  /// The work of the frame being updated. Update thread.
  void SetGasFrameInput(plGpuFluidVolumeId id, const plGpuFluidGasFrameInput& input);

  /// False while the volume has nothing on the GPU yet: it must not be published then.
  bool GetGasPublish(plGpuFluidVolumeId id, plGpuFluidGasPublish& out_publish) const;

  /// False until the first readback has arrived.
  bool GetGasStats(plGpuFluidVolumeId id, plGpuFluidGasStats& out_stats) const;

  /// The newest grid debug readback of a gas volume (plGpuFluidGasFrameInput::m_bDebugGrid), false until one arrived.
  bool GetGasGridDebug(plGpuFluidVolumeId id, plGpuFluidGridDebug& out_debug) const;

  /// Pool slots of a gas volume (its tier cap or MaxBricks, never more than its grid has bricks).
  plUInt32 GetGasCapacity(plGpuFluidVolumeId id) const;

  /// Registers a liquid volume. Its GPU resources are created on the render thread at the next BeginRender.
  plGpuFluidVolumeId CreateLiquidVolume(const plGpuFluidLiquidSettings& settings);

  /// The work of the frame being updated. Update thread.
  void SetLiquidFrameInput(plGpuFluidVolumeId id, const plGpuFluidLiquidFrameInput& input);

  /// False while the volume has nothing on the GPU yet: its points must not be drawn then.
  bool GetLiquidPublish(plGpuFluidVolumeId id, plGpuFluidLiquidPublish& out_publish) const;

  /// False until the first readback of the volume's current simulation has arrived.
  bool GetLiquidStats(plGpuFluidVolumeId id, plGpuFluidLiquidStats& out_stats) const;

  plUInt32 GetNumVolumes() const;

  /// Frames a retired volume's resources outlive it.
  static constexpr plUInt64 s_uiRetireFrames = 4;

private:
  struct GasVolume;
  struct LiquidVolume;

  void RecordLiquidVolumes(plRenderGraph& ref_graph, plUInt64 uiFrame, bool& inout_bAnyPass);
  void ReadLiquidStats(LiquidVolume& ref_volume);

  static void OnRenderEvent(const plRenderWorldRenderEvent& e);
  void BeginRender(plUInt64 uiFrame);
  void CreateResources(GasVolume& ref_volume);
  void DestroyResources(GasVolume& ref_volume);
  void DestroyAll();
  void ReadStats(GasVolume& ref_volume);

  plUInt32 m_uiWorldIndex = 0;

  mutable plMutex m_Mutex;
  plGpuFluidVolumeId m_NextId = 1;
  plHashTable<plGpuFluidVolumeId, plUniquePtr<GasVolume>> m_GasVolumes;

  struct Retired
  {
    plUniquePtr<GasVolume> m_pVolume;
    plUInt64 m_uiDestroyAtFrame = 0;
  };
  plDynamicArray<Retired> m_Retired;

  plHashTable<plGpuFluidVolumeId, plUniquePtr<LiquidVolume>> m_LiquidVolumes;

  struct RetiredLiquid
  {
    plUniquePtr<LiquidVolume> m_pVolume;
    plUInt64 m_uiDestroyAtFrame = 0;
  };
  plDynamicArray<RetiredLiquid> m_RetiredLiquid;
  plGpuFluidLiquidShaders m_LiquidShaders;

  plSharedPtr<plRenderGraph> m_pGraph;
  plShaderResourceHandle m_hAnalyticSphereShader;
  plShaderResourceHandle m_hStatsClearShader;
};
