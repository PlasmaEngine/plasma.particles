#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>

/// What happens when a particle is outside the sphere
struct PL_PARTICLEPLUGIN_DLL plParticleSphereOutOfBoundsMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Kill,      ///< Remove particle immediately
    Constrain, ///< Clamp particle to sphere surface

    Default = Kill
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleSphereOutOfBoundsMode);

/// Constrains particles to a spherical volume.
///
/// Particles outside the radius are either killed or pushed back to the sphere surface.
/// The center is relative to the effect's world position.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_BoundsSphere final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_BoundsSphere, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_BoundsSphere();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plVec3 m_vCenterOffset = plVec3::MakeZero();
  float m_fRadius = 3.0f;
  plEnum<plParticleSphereOutOfBoundsMode> m_OutOfBoundsMode;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_BoundsSphere final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_BoundsSphere, plParticleBehavior);

public:
  plVec3 m_vCenterOffset = plVec3::MakeZero();
  float m_fRadius = 3.0f;
  plEnum<plParticleSphereOutOfBoundsMode> m_OutOfBoundsMode;

protected:
  virtual void CreateRequiredStreams() override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
};