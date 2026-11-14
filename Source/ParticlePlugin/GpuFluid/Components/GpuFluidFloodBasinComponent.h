#pragma once

#include <Core/World/World.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidBasinColumns.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGpuManager.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>

struct plMsgUpdateLocalBounds;
struct plMsgExtractRenderData;
class plGpuFluidGpuManager;

using plGpuFluidFloodBasinComponentManager = plComponentManagerSimple<class plGpuFluidFloodBasinComponent, plComponentUpdateType::Always>;

/// \brief A room that fills with water: a water level tracked from the volume poured in, drawn as a water surface with
/// simulated waves on it. For floods over large floors, where particles everywhere would cost too much.
///
/// The basin is a box (Extents, axis aligned, centred on the owner) whose bottom is the room's floor. Its water is one
/// number, the volume: the level is the height that volume reaches over the free floor, with the room's collision
/// geometry (furniture, pillars, a raised floor; the same meshes the liquid bakes) taking up its share. What changes the
/// volume:
/// - GpuFluid Liquid Source components in the box pour into it (those inside a GpuFluid Liquid Volume pour particles
///   instead, which drain into the basin when they reach its water);
/// - open GpuFluid Liquid Drain components below the level take water out (an orifice: Cd * area * sqrt(2 g head));
/// - GpuFluid Flood Opening components between two basins let the higher water flow into the lower (a weir).
///
/// The surface is a grid of columns (ColumnSize) running the shallow-water wave equation: jets landing in the water,
/// moving GpuFluid colliders crossing it and the flow toward drains make waves, which reflect off walls and furniture.
/// It is drawn by the liquid surface pass, with the same look settings as a liquid volume.
///
/// Not simulated: the water spreading across a dry floor (the whole basin rises at once), breaking waves, water flowing
/// on top of water (a GpuFluid Liquid Volume there does that), and an underwater view.
class PL_PARTICLEPLUGIN_DLL plGpuFluidFloodBasinComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidFloodBasinComponent, plComponent, plGpuFluidFloodBasinComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plGpuFluidFloodBasinComponent

public:
  plGpuFluidFloodBasinComponent();
  ~plGpuFluidFloodBasinComponent();

  plVec3 m_vExtents = plVec3(15.0f, 15.0f, 2.0f);       // [ property ] metres; the bottom is the floor
  float m_fColumnSize = 0.05f;                          // [ property ] metres, the wave grid
  float m_fInitialLevel = 0.0f;                         // [ property ] metres above the floor at the start
  float m_fDrainCoefficient = 0.6f;                     // [ property ] orifice discharge coefficient of drains and openings
  float m_fWaveDamping = 0.003f;                        // [ property ] share of the wave motion lost per step
  float m_fMaxWaveHeight = 0.12f;                       // [ property ] metres
  float m_fWaveStrength = 1.0f;                         // [ property ] scales every wave source

  plVec3 m_vAbsorption = plVec3(2.5f, 1.0f, 0.7f);      // [ property ] sigma_a per colour, 1/m (flood water)
  plColor m_ScatterColor = plColor(0.035f, 0.075f, 0.075f); // [ property ]
  float m_fSurfaceRoughness = 0.05f;                    // [ property ]
  float m_fRefractionStrength = 1.0f;                   // [ property ]

  /// The water's level above the floor (m) and in world space, and its volume (m^3).
  float GetLevel() const { return m_fLevel; }
  float GetWorldLevel() const;
  double GetVolume() const { return m_fVolume; }

  /// Adds (or with a negative value removes) water, in m^3. Used by liquid volumes draining into the basin and by
  /// openings.
  void AddVolume(double fCubicMetres);

  /// Free water area at the current level (m^2): a cubic metre raises the level by 1 / this.
  float GetFreeArea() const;

  /// A wave at a world position: radius (m) and height (m, negative pushes the water down). Collected for the next
  /// frame; at most 16 a frame.
  void AddWave(const plVec3& vWorldPosition, float fRadius, float fHeight);

  /// Empties the basin to its initial level and stills its waves.
  void ResetWater();

  /// Bakes the solid profile again on the next update (furniture moved).
  void RebakeSolids() { m_bColumnsDirty = true; }

  /// The world box of the basin.
  plBoundingBox GetWorldBox() const;

  /// The basin whose box holds a world position, or null.
  static plGpuFluidFloodBasinComponent* FindBasinAt(plWorld& ref_world, const plVec3& vWorldPosition);

protected:
  void Update();
  void OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg);
  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;

private:
  plVec2U32 GetColumns() const;
  void UpdateColumns();
  void UpdateFlows(float fDt);
  void GatherWaves(float fDt);

  plGpuFluidGpuManager* m_pManager = nullptr;
  plGpuFluidVolumeId m_BasinId = 0;
  plVec2U32 m_vActiveColumns = plVec2U32::MakeZero();
  float m_fActiveColumnSize = 0.0f;

  plSharedPtr<plGpuFluidBasinColumns> m_pColumns;
  bool m_bColumnsDirty = true;
  plInt32 m_iLastRebakeCVar = 0;
  plUInt64 m_uiColliderHash = 0;
  plUInt32 m_uiBakedObjects = 0;    ///< collision mesh objects of the last bake
  plUInt32 m_uiBakeCheckFrames = 0; ///< frames left in which a change in that count bakes again

  double m_fVolume = 0.0;
  float m_fLevel = 0.0f;
  bool m_bVolumeFromInitialLevel = true;
  plSharedPtr<plGpuFluidBasinDepths> m_pDepths;
  float m_fDepthsLevel = -1.0f;

  plHybridArray<plVec4, 16> m_PendingWaves; // xy basin-relative, z radius, w height
  plUInt64 m_uiTick = 0;
  plUInt64 m_uiRandom = 0x9E3779B97F4A7C15ull;
  plHashTable<plUInt64, plVec3> m_ColliderPositions;

  plVec3 m_vBoundsSize = plVec3::MakeZero();
};

//////////////////////////////////////////////////////////////////////////

using plGpuFluidFloodOpeningComponentManager = plComponentManagerSimple<class plGpuFluidFloodOpeningComponent, plComponentUpdateType::Always>;

/// \brief An opening between two flood basins (a doorway, a hole in a wall): the box (HalfExtents, its world bounds)
/// overlaps both basins, its bottom is the sill. The higher water flows through it into the lower basin as over a weir,
/// Cd * width * 2/3 * sqrt(2 g) * (h_high^1.5 - h_low^1.5), never past the point where both stand level. Open switches it
/// (plMsgSetPlaying too: a door closing).
class PL_PARTICLEPLUGIN_DLL plGpuFluidFloodOpeningComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidFloodOpeningComponent, plComponent, plGpuFluidFloodOpeningComponentManager);

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  plGpuFluidFloodOpeningComponent();
  ~plGpuFluidFloodOpeningComponent();

  plVec3 m_vHalfExtents = plVec3(0.3f, 0.6f, 1.0f); // [ property ] metres
  bool m_bOpen = true;                              // [ property ]

  /// m^3/s through the opening last frame, positive from the first basin to the second it found.
  float GetLastFlow() const { return m_fLastFlow; }

protected:
  void Update();
  void OnMsgSetPlaying(struct plMsgSetPlaying& ref_msg);

private:
  float m_fLastFlow = 0.0f;
};
