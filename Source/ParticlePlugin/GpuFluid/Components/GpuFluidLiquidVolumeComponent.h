#pragma once

#include <Core/World/World.h>
#include <Foundation/Threading/TaskSystem.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGpuManager.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>

struct plMsgUpdateLocalBounds;
struct plMsgExtractRenderData;
class plDebugRendererContext;

using plGpuFluidLiquidVolumeComponentManager = plComponentManagerSimple<class plGpuFluidLiquidVolumeComponent, plComponentUpdateType::Always>;

/// \brief Sides of a liquid domain. A closed side is a wall (the default: a tank); an open side lets the liquid out.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidDomainFaces
{
  using StorageType = plUInt8;

  enum Enum
  {
    None = 0,
    NegX = PL_BIT(0),
    PosX = PL_BIT(1),
    NegY = PL_BIT(2),
    PosY = PL_BIT(3),
    NegZ = PL_BIT(4),
    PosZ = PL_BIT(5),

    Default = None
  };

  struct Bits
  {
    StorageType NegX : 1;
    StorageType PosX : 1;
    StorageType NegY : 1;
    StorageType PosY : 1;
    StorageType NegZ : 1;
    StorageType PosZ : 1;
  };
};

PL_DECLARE_FLAGS_OPERATORS(plGpuFluidDomainFaces);
PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidDomainFaces);

/// \brief A box of liquid seeded when the volume starts or is reset. In the volume's space (metres from its centre).
struct PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidFillBox
{
  plVec3 m_vCenter = plVec3::MakeZero();
  plVec3 m_vHalfExtents = plVec3(0.5f);
  plVec3 m_vVelocity = plVec3::MakeZero();
  float m_fJitter = 1.0f;
  bool m_bStratified = true; ///< exactly ParticlesPerCell in each cell, one per sub-cell; false: anywhere in the box
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidLiquidFillBox);

struct PL_PARTICLEPLUGIN_DLL plGpuFluidObstacleShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    Box,
    Sphere,

    Default = Box
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidObstacleShape);

/// \brief An inflow of liquid: a disc (a nozzle) across its velocity, in the volume's space. A minimal source; the
/// full inflow and drain component comes with the flooding work.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidInflow
{
  plVec3 m_vCenter = plVec3::MakeZero();
  plVec3 m_vVelocity = plVec3(0.0f, 0.0f, -1.0f); ///< m/s
  float m_fRadius = 0.02f;                        ///< m
  float m_fFlowRate = 0.0f;                       ///< litres per second; 0: the disc's area times the speed
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidLiquidInflow);

/// \brief A drain: a box in the volume's space that, while open, removes the liquid reaching it (counted as drained).
/// The cells inside are air to the pressure solve, so the liquid around flows in under its own weight (a floor drain
/// is a thin box on the floor; a hole in a wall a box through it).
struct PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidDrain
{
  plVec3 m_vCenter = plVec3::MakeZero();
  plVec3 m_vHalfExtents = plVec3(0.1f, 0.1f, 0.05f); ///< m
  bool m_bOpen = true;
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidLiquidDrain);

/// \brief How a liquid volume is drawn.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidRenderMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Surface,           ///< the extracted surface, refracting and reflecting (plGpuFluidLiquidPass)
    Points,            ///< the particles as small spheres coloured by speed (debug)
    SurfaceAndPoints,  ///< both

    Default = Surface
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidLiquidRenderMode);

/// \brief A static analytic solid inside the domain, in the volume's space.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidObstacle
{
  plEnum<plGpuFluidObstacleShape> m_Shape;
  plVec3 m_vCenter = plVec3::MakeZero();
  plVec3 m_vHalfExtents = plVec3(0.25f); ///< box
  float m_fRadius = 0.25f;               ///< sphere
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidLiquidObstacle);

/// \brief A box of simulated liquid: APIC/FLIP particles on a dense MAC grid, simulated on the GPU.
///
/// The domain is axis aligned in world space and centred on the owner (its rotation and scale are not used, so
/// gravity always points the same way through it). It spans Extents rounded up to whole bricks of 8 cells of
/// CellSize. The fill boxes are seeded when the component activates and on ResetSimulation().
///
/// The liquid comes and goes through the modifier components in the domain (plGpuFluidLiquidSourceComponent,
/// plGpuFluidLiquidDrainComponent, plGpuFluidLiquidFillComponent); static solids are the world's collision meshes, static
/// plGpuFluidColliderComponent objects among them. The arrays below do the same from code (the tests use them).
///
/// The surface is extracted on the GPU every frame (surface nets on a smoothed level set) and drawn by
/// plGpuFluidLiquidPass in front of the transparent pass. The viewport's Fluid render modes (or the cvars
/// GpuFluid.Liquid.RenderMode, ShowSleep and ShowSolids) draw the particles as small spheres coloured by speed or by
/// sleep (plGpuFluidLiquidDebugRenderer), or the triangles the static solids bake caught.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidVolumeComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidLiquidVolumeComponent, plComponent, plGpuFluidLiquidVolumeComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plGpuFluidLiquidVolumeComponent

public:
  plGpuFluidLiquidVolumeComponent();
  ~plGpuFluidLiquidVolumeComponent();

  plVec3 m_vExtents = plVec3(4.0f, 3.0f, 2.0f);                        // [ property ] metres
  float m_fCellSize = 0.0625f;                                          // [ property ] metres
  plUInt32 m_uiParticlesPerCell = 4;                                    // [ property ] 4 or 8
  plUInt32 m_uiMaxParticles = 1024 * 1024;                              // [ property ]
  plEnum<plGpuFluidQuality> m_Quality;                                  // [ property ]
  plBitflags<plGpuFluidDomainFaces> m_OpenFaces;                        // [ property ] closed by default

  /// Modifiers set from code, in the volume's space, on top of the modifier components in the domain.
  plDynamicArray<plGpuFluidLiquidFillBox> m_FillBoxes;
  plDynamicArray<plGpuFluidLiquidObstacle> m_Obstacles;
  plDynamicArray<plGpuFluidLiquidInflow> m_Inflows;
  plDynamicArray<plGpuFluidLiquidDrain> m_Drains;

  /// Static solids from the world's collision meshes (plWorldGeoExtractionUtil, CollisionMesh: Jolt static actors, or
  /// any component answering plMsgExtractGeometry), baked into a distance field on activation, when the domain
  /// changes, on RebakeStaticSolids() and when the cvar GpuFluid.Rebake changes.
  bool m_bBakeStaticSolids = true;                                      // [ property ]
  /// Bricks calm for 60 substeps freeze their particles, and a volume with nothing awake and nothing to wake it stops
  /// simulating (only its surface is drawn) until an inflow, drain, solid or gravity changes.
  bool m_bAllowSleep = true;                                            // [ property ]

  float m_fFlipBlend = 0.1f;                                            // [ property ]
  plUInt32 m_uiMaxSubsteps = 0;                                         // [ property ] 0: the quality tier's (GetTierMaxSubsteps)
  plUInt32 m_uiPressureCycles = 0;                                      // [ property ] MGPCG iterations per substep, one V-cycle each; 0: the tier's
  float m_fWallFriction = 0.0f;                                         // [ property ]
  bool m_bAlwaysSimulate = true;                                        // [ property ]

  plVec3 m_vAbsorption = plVec3(0.45f, 0.07f, 0.04f);                   // [ property ] sigma_a per colour, 1/m (clear water)
  plColor m_ScatterColor = plColor(0.03f, 0.09f, 0.11f);                // [ property ] the water's own glow from its depth
  float m_fSurfaceRoughness = 0.04f;                                    // [ property ]
  float m_fRefractionStrength = 1.0f;                                 // [ property ] how far the bent ray is followed, per m of thickness

  /// Debug look set from code (the tests): the views and cvars (GpuFluid.Liquid.RenderMode, DebugPointRadius,
  /// DebugTopSpeed) take over when set.
  plEnum<plGpuFluidLiquidRenderMode> m_RenderMode;
  float m_fDebugPointRadius = 0.0f;                                     // 0: from the cell size
  float m_fDebugTopSpeed = 3.0f;                                        // m/s at the top of the colour ramp

  /// Solver tuning that is not a property yet (tests set it).
  plGpuFluidLiquidFrameInput m_Tuning;

  /// Empties the volume and seeds the fill boxes again on the next tick.
  void ResetSimulation();

  /// Gathers the static solids again (on the next update) and bakes them; the simulation goes on.
  void RebakeStaticSolids() { m_bSolidsDirty = true; }

  /// The bake the simulation uses (null: none yet, or BakeStaticSolids off), and how many have been built.
  const plGpuFluidLiquidSolidBake* GetStaticSolids() const { return m_pSolidBake.Borrow(); }
  plUInt32 GetStaticSolidsBuilt() const { return m_uiSolidBakesBuilt; }
  /// Whether a bake is being built on a task.
  bool IsBakingStaticSolids() const { return m_BakeTaskGroup.IsValid(); }

  /// Cells per axis of the domain.
  plVec3U32 GetCells() const;

  /// The box the domain covers: the extents rounded up to whole bricks.
  plVec3 GetDomainSize() const;

  /// World position of the domain's minimum corner.
  plVec3 GetDomainOrigin() const;

  /// Substep cap of a quality tier, when MaxSubsteps is 0.
  static plUInt32 GetTierMaxSubsteps(plGpuFluidQuality::Enum quality);
  /// MGPCG iterations per substep of a quality tier (one multigrid V-cycle each), when PressureCycles is 0.
  static plUInt32 GetTierPressureIterations(plGpuFluidQuality::Enum quality);

  /// What the GPU counted, two to three frames late. False until the first readback of the current simulation.
  bool GetStats(plGpuFluidLiquidStats& out_stats) const;

  /// Ticks handed to the GPU since the last reset.
  plUInt64 GetTicksSinceReset() const { return m_uiTicksSinceReset; }

  /// Whether the particles were drawn in the last extraction.
  bool WasPublished() const { return m_bPublished; }

protected:
  void Update();
  void OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg);
  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;

private:
  plGpuFluidLiquidSettings ComputeSettings() const;
  void EnsureVolume();
  void ReleaseVolume();
  void UpdateStaticSolids();

  /// The modifier components in the domain, into the frame's input (domain-relative), with the code-set arrays.
  void GatherModifiers(plGpuFluidLiquidFrameInput& inout_input, plDynamicArray<plGpuFluidLiquidFillBox>& out_fills);

  /// The bake's triangles over the scene (the LiquidStaticSolids view and GpuFluid.Liquid.ShowSolids).
  void DrawStaticSolids(const plDebugRendererContext& context) const;

  // What restarts the simulation (the fill components) and what bakes the solids again (the static colliders).
  plUInt64 m_uiFillHash = 0;
  plUInt64 m_uiColliderHash = 0;

  // Static solids: the bake in use, the one a task is building (with the meshes it reads), and what asks for a new one.
  plSharedPtr<plGpuFluidLiquidSolidBake> m_pSolidBake;
  plSharedPtr<plGpuFluidLiquidSolidBakeJob> m_pBakeJob;
  plTaskGroupID m_BakeTaskGroup;
  bool m_bSolidsDirty = true;
  plInt32 m_iLastRebakeCVar = 0;
  plUInt32 m_uiSolidBakesBuilt = 0;
  plVec3 m_vBakedDomainOrigin = plVec3::MakeZero();
  plVec3U32 m_vBakedCells = plVec3U32::MakeZero();
  float m_fBakedCellSize = 0.0f;

  plGpuFluidGpuManager* m_pManager = nullptr;
  plGpuFluidVolumeId m_VolumeId = 0;
  plGpuFluidLiquidSettings m_ActiveSettings;
  plUInt32 m_uiResetCounter = 0;
  plUInt64 m_uiTick = 0;
  plUInt64 m_uiTicksSinceReset = 0;
  plVec3 m_vBoundsSize = plVec3::MakeZero();
  mutable bool m_bPublished = false;
  mutable plUInt64 m_uiLastExtractedFrame = 0;
};
