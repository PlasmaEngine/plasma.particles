#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>

class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_SceneForces final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_SceneForces, plParticleBehaviorFactory);

public:
  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  float m_fInfluence = 1.0f;
  plString m_sInfluenceParameter;
};

/// Makes the system react to plParticleForceFieldComponent volumes placed in the scene
/// (directional push, point repulsion/attraction, vortices).
///
/// This is the opt-in per system, so effects that should ignore scene forces simply don't
/// add this behavior. Global wind is separate: the Velocity behavior's wind influence.
class PL_PARTICLEPLUGIN_DLL plParticleBehavior_SceneForces final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_SceneForces, plParticleBehavior);

public:
  float m_fInfluence = 1.0f;
  plTempHashedString m_sInfluenceParameter;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
};
