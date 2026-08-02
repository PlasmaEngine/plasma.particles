#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>

/// Which particle attribute to evaluate for the condition
struct PL_PARTICLEPLUGIN_DLL plParticleKillAttribute
{
  using StorageType = plUInt8;

  enum Enum
  {
    PositionX,
    PositionY,
    PositionZ,
    Speed,
    Size,
    ColorAlpha,

    Default = PositionZ
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleKillAttribute);

/// Comparison operator for the conditional kill
struct PL_PARTICLEPLUGIN_DLL plParticleComparisonOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    Default = Less
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleComparisonOp);

/// Kills particles when a chosen attribute passes a threshold.
///
/// For example, kill all particles below a certain height (PositionZ < -5)
/// or all particles that have slowed below a minimum speed (Speed < 0.1).
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_ConditionalKill final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_ConditionalKill, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_ConditionalKill();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleKillAttribute> m_Attribute;
  plEnum<plParticleComparisonOp> m_Comparison;
  float m_fThreshold = 0.0f;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_ConditionalKill final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_ConditionalKill, plParticleBehavior);

public:
  plEnum<plParticleKillAttribute> m_Attribute;
  plEnum<plParticleComparisonOp> m_Comparison;
  float m_fThreshold = 0.0f;

protected:
  virtual void CreateRequiredStreams() override;
  virtual void QueryOptionalStreams() override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
};
