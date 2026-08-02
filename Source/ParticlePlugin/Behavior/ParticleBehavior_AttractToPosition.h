#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>

/// Where the attractor pulls particles toward
struct PL_PARTICLEPLUGIN_DLL plParticleAttractorTarget
{
  using StorageType = plUInt8;

  enum Enum
  {
    EffectOrigin, ///< Attract toward the effect's world position
    CustomPosition, ///< Attract toward a specified world-space point

    Default = EffectOrigin
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleAttractorTarget);

/// Pulls particles toward a point with distance-based falloff.
///
/// Can modify velocity (physically correct acceleration) or position directly (snapping).
/// MinDistance prevents singularity at the attractor center.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_AttractToPosition final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_AttractToPosition, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_AttractToPosition();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleAttractorTarget> m_Target;
  plVec3 m_vCustomPosition = plVec3::MakeZero();
  float m_fForce = 5.0f;
  float m_fMaxDistance = 10.0f;
  float m_fMinDistance = 0.1f;
  bool m_bAffectVelocity = true;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_AttractToPosition final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_AttractToPosition, plParticleBehavior);

public:
  plEnum<plParticleAttractorTarget> m_Target;
  plVec3 m_vCustomPosition = plVec3::MakeZero();
  float m_fForce = 5.0f;
  float m_fMaxDistance = 10.0f;
  float m_fMinDistance = 0.1f;
  bool m_bAffectVelocity = true;

protected:
  virtual void CreateRequiredStreams() override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
};
