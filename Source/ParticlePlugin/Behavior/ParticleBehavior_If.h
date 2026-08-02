#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Behavior/ParticleConditionalCommon.h>

/// Per-particle IF: compares an input attribute against a threshold
/// and writes one of two values to an output attribute.
///
/// Example: If Speed > 5, set Size to 2.0, else set Size to 0.5.
/// This allows particles to change appearance or behavior based on their state.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_If final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_If, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_If();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  /// The attribute to test the condition against
  plEnum<plParticleAttribute> m_ConditionInput;
  plEnum<plParticleConditionOp> m_Comparison;
  float m_fThreshold = 0.0f;

  /// The attribute to write the result to
  plEnum<plParticleAttribute> m_OutputAttribute;
  float m_fTrueValue = 1.0f;
  float m_fFalseValue = 0.0f;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_If final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_If, plParticleBehavior);

public:
  plEnum<plParticleAttribute> m_ConditionInput;
  plEnum<plParticleConditionOp> m_Comparison;
  float m_fThreshold = 0.0f;

  plEnum<plParticleAttribute> m_OutputAttribute;
  float m_fTrueValue = 1.0f;
  float m_fFalseValue = 0.0f;

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
