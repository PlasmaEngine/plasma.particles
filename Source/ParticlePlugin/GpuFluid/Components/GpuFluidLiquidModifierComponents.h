#pragma once

#include <Core/World/World.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

struct plMsgSetPlaying;
class plGpuFluidLiquidVolumeComponent;

/// \brief Placeable modifiers of GpuFluid liquid volumes: a source pouring liquid in, a drain taking it out, and a box
/// of liquid seeded when the simulation starts.
///
/// Each acts on every liquid volume whose domain it is in (its centre for a source, any overlap for a drain or a fill),
/// so nothing is wired up. While the world is not simulating (editing in the editor), a modifier outside every liquid
/// volume draws itself red, and a drain draws whether it is open.
namespace plGpuFluidLiquidModifiers
{
  /// The liquid volume whose domain holds vPoint, or null. The first one found when domains overlap.
  PL_PARTICLEPLUGIN_DLL const plGpuFluidLiquidVolumeComponent* FindVolumeAt(const plWorld& world, const plVec3& vPoint);

  /// Whether any liquid volume's domain overlaps the box.
  PL_PARTICLEPLUGIN_DLL bool OverlapsAnyVolume(const plWorld& world, const plBoundingBox& box);

  /// World-space bounds of a box of half extents vHalfExtents in the transform's space (rotation and scale applied).
  PL_PARTICLEPLUGIN_DLL plBoundingBox GetWorldBox(const plTransform& transform, const plVec3& vHalfExtents);
} // namespace plGpuFluidLiquidModifiers

//////////////////////////////////////////////////////////////////////////

using plGpuFluidLiquidSourceComponentManager = plComponentManagerSimple<class plGpuFluidLiquidSourceComponent, plComponentUpdateType::Always>;

/// \brief Pours liquid into the liquid volume it is in: a disc of Radius across the object's +X, the liquid leaving it
/// at Speed along +X.
///
/// FlowRate sets the litres per second; 0 takes the disc's area times the speed. plMsgSetPlaying starts and stops it.
/// A volume takes up to four sources.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidSourceComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidLiquidSourceComponent, plComponent, plGpuFluidLiquidSourceComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  //////////////////////////////////////////////////////////////////////////
  // plGpuFluidLiquidSourceComponent

public:
  plGpuFluidLiquidSourceComponent();
  ~plGpuFluidLiquidSourceComponent();

  float m_fRadius = 0.05f;   // [ property ] metres
  float m_fSpeed = 2.0f;     // [ property ] m/s along +X
  float m_fFlowRate = 0.0f;  // [ property ] litres per second; 0: area times speed
  bool m_bFlowing = true;    // [ property ]

  /// World position of the disc's centre and the velocity of the liquid leaving it.
  plVec3 GetWorldCenter() const;
  plVec3 GetWorldVelocity() const;
  /// The disc's radius with the object's scale.
  float GetWorldRadius() const;

protected:
  void Update();
  void OnMsgSetPlaying(plMsgSetPlaying& ref_msg);
};

//////////////////////////////////////////////////////////////////////////

using plGpuFluidLiquidDrainComponentManager = plComponentManagerSimple<class plGpuFluidLiquidDrainComponent, plComponentUpdateType::Always>;

/// \brief Takes liquid out of the liquid volumes it overlaps while Open: the liquid reaching the box is removed.
///
/// The box is HalfExtents in the object's space; rotated, its world-axis bounds are used (the liquid grid is axis
/// aligned). A floor drain is a thin box on the floor, a hole in a wall a box through it. plMsgSetPlaying opens and
/// closes it.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidDrainComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidLiquidDrainComponent, plComponent, plGpuFluidLiquidDrainComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  //////////////////////////////////////////////////////////////////////////
  // plGpuFluidLiquidDrainComponent

public:
  plGpuFluidLiquidDrainComponent();
  ~plGpuFluidLiquidDrainComponent();

  plVec3 m_vHalfExtents = plVec3(0.2f, 0.2f, 0.05f); // [ property ] metres
  bool m_bOpen = true;                                // [ property ]

  plBoundingBox GetWorldBox() const;

protected:
  void Update();
  void OnMsgSetPlaying(plMsgSetPlaying& ref_msg);
};

//////////////////////////////////////////////////////////////////////////

using plGpuFluidLiquidFillComponentManager = plComponentManagerSimple<class plGpuFluidLiquidFillComponent, plComponentUpdateType::Always>;

/// \brief A box of liquid the liquid volumes it overlaps are filled with when they start or reset.
///
/// The box is HalfExtents in the object's space; rotated, its world-axis bounds are filled. Velocity is the liquid's
/// starting velocity in the object's space. Moving, resizing or changing a fill restarts the volumes it overlaps.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidFillComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidLiquidFillComponent, plComponent, plGpuFluidLiquidFillComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  //////////////////////////////////////////////////////////////////////////
  // plGpuFluidLiquidFillComponent

public:
  plGpuFluidLiquidFillComponent();
  ~plGpuFluidLiquidFillComponent();

  plVec3 m_vHalfExtents = plVec3(0.5f);      // [ property ] metres
  plVec3 m_vVelocity = plVec3::MakeZero();   // [ property ] m/s, object space
  float m_fJitter = 1.0f;                    // [ property ] 0: particles on a regular lattice, 1: random in their sub-cell
  bool m_bStratified = true;                 // [ property ] one particle per sub-cell; false: anywhere in the cell

  plBoundingBox GetWorldBox() const;
  plVec3 GetWorldVelocity() const;

protected:
  void Update();
};
