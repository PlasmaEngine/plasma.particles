#pragma once

#include <Core/World/World.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGpuManager.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>

struct plMsgUpdateLocalBounds;
struct plMsgExtractRenderData;

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
/// P6 draws the particles as small spheres coloured by speed (plGpuFluidLiquidDebugRenderer); the surface comes later.
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
  plDynamicArray<plGpuFluidLiquidFillBox> m_FillBoxes;                  // [ property ]
  plDynamicArray<plGpuFluidLiquidObstacle> m_Obstacles;                 // [ property ]

  float m_fFlipBlend = 0.1f;                                            // [ property ]
  plUInt32 m_uiMaxSubsteps = 4;                                         // [ property ]
  plUInt32 m_uiPressureCycles = 3;                                      // [ property ] multigrid V-cycles
  float m_fWallFriction = 0.0f;                                         // [ property ]
  bool m_bAlwaysSimulate = true;                                        // [ property ]

  float m_fDebugPointRadius = 0.0f;                                     // [ property ] 0: from the cell size
  float m_fDebugTopSpeed = 3.0f;                                        // [ property ] m/s at the top of the colour ramp

  /// Solver tuning that is not a property yet (tests set it).
  plGpuFluidLiquidFrameInput m_Tuning;

  /// Empties the volume and seeds the fill boxes again on the next tick.
  void ResetSimulation();

  /// Cells per axis of the domain.
  plVec3U32 GetCells() const;

  /// The box the domain covers: the extents rounded up to whole bricks.
  plVec3 GetDomainSize() const;

  /// World position of the domain's minimum corner.
  plVec3 GetDomainOrigin() const;

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
