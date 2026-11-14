#pragma once

#include <Core/World/World.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

/// \brief Shape of a GpuFluid collider, in the owner's space.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidColliderShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    Sphere,  ///< Radius.
    Box,     ///< Half extents.
    Capsule, ///< Radius and half height of the segment along local +Z.
    Plane,   ///< Solid below the plane through the owner, against local +Z.

    Default = Box
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidColliderShape);

struct plMsgExtractGeometry;

using plGpuFluidColliderComponentManager = plComponentManager<class plGpuFluidColliderComponent, plBlockStorageType::Compact>;

/// \brief An analytic solid for GpuFluid volumes: a box, sphere, capsule or plane the gas and the liquid flow around.
///
/// Every gas volume whose box it overlaps rasterises it each tick (GasSolids): cells inside are solid, faces touching it
/// take its velocity (no flow through a static collider, a moving one pushes the gas), the pressure treats it as a wall,
/// and gas inside is removed. The velocity of a moving collider comes from its transform's change since the last frame,
/// linear and angular; a static one is always at rest. Scale is applied per axis to the box, as the largest factor to
/// the sphere and the capsule's radius.
///
/// A static collider is also a collision mesh (plMsgExtractGeometry), so liquid volumes bake it into their static
/// solids with the rest of the level, rotation and all; a liquid volume bakes again when one changes. Moving
/// colliders do not affect liquid yet.
class PL_PARTICLEPLUGIN_DLL plGpuFluidColliderComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidColliderComponent, plComponent, plGpuFluidColliderComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  //////////////////////////////////////////////////////////////////////////
  // plGpuFluidColliderComponent

public:
  plGpuFluidColliderComponent();
  ~plGpuFluidColliderComponent();

  plEnum<plGpuFluidColliderShape> m_Shape;                           // [ property ]
  plVec3 m_vHalfExtents = plVec3(0.5f);                              // [ property ] metres (box)
  float m_fRadius = 0.5f;                                            // [ property ] metres (sphere, capsule)
  float m_fHalfHeight = 0.5f;                                        // [ property ] metres, half the capsule's segment
  bool m_bStatic = false;                                            // [ property ] never moves: always at rest for the gas, baked for the liquid

  /// Hash of what a liquid bake takes from this collider (shape, sizes, transform); 0 when it is not static.
  plUInt64 GetBakeHash() const;

protected:
  void OnMsgExtractGeometry(plMsgExtractGeometry& ref_msg) const;
};
