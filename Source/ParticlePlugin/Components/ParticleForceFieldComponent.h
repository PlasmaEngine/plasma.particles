#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

/// What kind of force the field applies
struct PL_PARTICLEPLUGIN_DLL plParticleForceFieldType
{
  using StorageType = plUInt8;

  enum Enum
  {
    Directional, ///< pushes along the component's +Z axis (rotate the object to aim)
    Point,       ///< pushes radially away from the center; negative strength pulls inward
    Vortex,      ///< swirls around the component's +Z axis

    Default = Directional
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleForceFieldType);

/// The volume the force applies within
struct PL_PARTICLEPLUGIN_DLL plParticleForceFieldShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    Sphere,
    Box,

    Default = Sphere
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleForceFieldShape);

using plParticleForceFieldComponentManager = plComponentManager<class plParticleForceFieldComponent, plBlockStorageType::Compact>;

/// A scene-placed force volume that pushes, pulls or swirls particles inside it.
///
/// Only particle systems with a 'Scene Forces' behavior react to it, so effects opt in
/// per system - global wind is separate and reaches particles through the Velocity
/// behavior's wind influence.
class PL_PARTICLEPLUGIN_DLL plParticleForceFieldComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plParticleForceFieldComponent, plComponent, plParticleForceFieldComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plParticleForceFieldComponent

public:
  plParticleForceFieldComponent();
  ~plParticleForceFieldComponent();

  plEnum<plParticleForceFieldType> m_Type;    // [ property ]
  plEnum<plParticleForceFieldShape> m_Shape;  // [ property ]
  float m_fRadius = 5.0f;                     // [ property ] sphere volume
  plVec3 m_vExtents = plVec3(10.0f);          // [ property ] box volume (full extents)
  float m_fStrength = 5.0f;                   // [ property ] acceleration in m/s^2; negative reverses the push/swirl
  float m_fFalloffStart = 0.5f;               // [ property ] normalized distance where the force starts fading towards the border
};
