#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Behavior/ParticleConditionalCommon.h>

/// Kills particles when a chosen attribute passes a threshold.
///
/// For example, kill all particles below a certain height (PositionZ < -5)
/// or all particles that have slowed below a minimum speed (Speed < 0.1).
///
/// Retired: the editor converts it into Kill If with a Compare node when an effect opens. It stays
/// so effects built by plasma.particles 1.3 still load and run as they are.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_ConditionalKill final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_ConditionalKill, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_ConditionalKill();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleAttribute> m_Attribute;
  plEnum<plParticleConditionOp> m_Comparison;
  float m_fThreshold = 0.0f;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_ConditionalKill final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_ConditionalKill, plParticleBehavior);

public:
  plEnum<plParticleAttribute> m_Attribute;
  plEnum<plParticleConditionOp> m_Comparison;
  float m_fThreshold = 0.0f;

protected:
  virtual void CreateRequiredStreams() override;
  virtual void QueryOptionalStreams() override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
  plProcessingStream* m_pStreamLifeTime = nullptr;
};
