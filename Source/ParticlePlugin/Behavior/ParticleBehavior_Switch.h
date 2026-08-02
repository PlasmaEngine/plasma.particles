#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Behavior/ParticleConditionalCommon.h>

/// Per-particle Switch: divides an input attribute's range into up to 4 buckets
/// and writes a different value to the output for each bucket.
///
/// Example: Based on LifeFraction, set Size to different values at different
/// stages of the particle's life:
///   [0.0 - 0.25] -> 0.5 (small at birth)
///   [0.25 - 0.5] -> 2.0 (grows)
///   [0.5 - 0.75] -> 2.0 (stays large)
///   [0.75 - 1.0] -> 0.1 (shrinks before death)
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_Switch final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_Switch, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_Switch();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleAttribute> m_InputAttribute;
  plEnum<plParticleAttribute> m_OutputAttribute;

  /// Thresholds dividing the input range into buckets.
  /// Bucket 0: input < Threshold1
  /// Bucket 1: Threshold1 <= input < Threshold2
  /// Bucket 2: Threshold2 <= input < Threshold3
  /// Bucket 3: input >= Threshold3
  float m_fThreshold1 = 0.25f;
  float m_fThreshold2 = 0.5f;
  float m_fThreshold3 = 0.75f;

  float m_fValue0 = 0.0f;
  float m_fValue1 = 1.0f;
  float m_fValue2 = 2.0f;
  float m_fValue3 = 3.0f;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_Switch final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_Switch, plParticleBehavior);

public:
  plEnum<plParticleAttribute> m_InputAttribute;
  plEnum<plParticleAttribute> m_OutputAttribute;
  float m_fThreshold1 = 0.25f;
  float m_fThreshold2 = 0.5f;
  float m_fThreshold3 = 0.75f;
  float m_fValue0 = 0.0f;
  float m_fValue1 = 1.0f;
  float m_fValue2 = 2.0f;
  float m_fValue3 = 3.0f;

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
