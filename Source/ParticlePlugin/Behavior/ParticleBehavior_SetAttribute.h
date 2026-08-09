#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Behavior/ParticleConditionalCommon.h>

/// How Set Attribute combines its value with what the attribute already holds.
struct PL_PARTICLEPLUGIN_DLL plParticleSetAttributeMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Replace,
    Add,
    Multiply,

    Default = Replace
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleSetAttributeMode);

class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_SetAttribute final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_SetAttribute, plParticleBehaviorFactory);

public:
  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleAttribute> m_Attribute;
  plEnum<plParticleSetAttributeMode> m_Mode;
  float m_fValue = 1.0f;
};

/// Writes one value into one particle attribute, every update, at this block's position in the
/// Update order.
///
/// This is the graph's numeric sink: the value is a plain float, so it can be driven by the
/// operator graph through its property pin. The value is constant per effect instance (the graph
/// folds at asset transform); per-particle sources arrive with the per-particle tier.
class PL_PARTICLEPLUGIN_DLL plParticleBehavior_SetAttribute final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_SetAttribute, plParticleBehavior);

public:
  plEnum<plParticleAttribute> m_Attribute;
  plEnum<plParticleSetAttributeMode> m_Mode;
  float m_fValue = 1.0f;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void QueryOptionalStreams() override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
  plProcessingStream* m_pStreamLifeTime = nullptr;
};
