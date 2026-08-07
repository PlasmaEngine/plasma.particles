#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>

class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_Drag final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_Drag, plParticleBehaviorFactory);

public:
  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  float m_fDrag = 1.0f;         // exponential decay coefficient per second (1 = ~63% speed loss per second)
  bool m_bSizeRelative = false; // larger particles brake harder (drag scales with particle size)
  plString m_sDragParameter;
};

/// Slows particles down over time, like air resistance.
///
/// Unlike the friction folded into the Velocity behavior, this is a standalone, frame-rate
/// independent exponential damping that can also scale with the particle size.
class PL_PARTICLEPLUGIN_DLL plParticleBehavior_Drag final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_Drag, plParticleBehavior);

public:
  float m_fDrag = 1.0f;
  bool m_bSizeRelative = false;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void QueryOptionalStreams() override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
};
