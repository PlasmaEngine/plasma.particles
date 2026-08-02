#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Behavior/ParticleConditionalCommon.h>

/// Per-particle Remap: reads an input attribute and linearly maps it from
/// one range to another, writing the result to an output attribute.
///
/// Example: Map LifeFraction [0..1] to Size [2.0..0.1] to make particles
/// shrink over their lifetime.
/// Values outside InputMin..InputMax are clamped before mapping.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_Remap final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_Remap, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_Remap();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleAttribute> m_InputAttribute;
  plEnum<plParticleAttribute> m_OutputAttribute;
  float m_fInputMin = 0.0f;
  float m_fInputMax = 1.0f;
  float m_fOutputMin = 0.0f;
  float m_fOutputMax = 1.0f;
  bool m_bClampOutput = true;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_Remap final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_Remap, plParticleBehavior);

public:
  plEnum<plParticleAttribute> m_InputAttribute;
  plEnum<plParticleAttribute> m_OutputAttribute;
  float m_fInputMin = 0.0f;
  float m_fInputMax = 1.0f;
  float m_fOutputMin = 0.0f;
  float m_fOutputMax = 1.0f;
  bool m_bClampOutput = true;

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
