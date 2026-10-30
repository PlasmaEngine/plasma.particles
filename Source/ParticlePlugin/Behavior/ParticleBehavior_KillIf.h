#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/System/ParticleProgram.h>

/// Removes every particle whose Condition is true, at its place among the behaviors.
///
/// The condition is meant to be wired per particle (Position Z < 0, Speed < 0.1, ...); the authored
/// bool applies to all particles at once.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_KillIf final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_KillIf, plParticleBehaviorFactory);

public:
  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual plParticleProgramData* GetParticleProgram() override { return &m_Program; }

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  bool m_bCondition = false;

  /// The compiled per-particle wire into "Condition".
  plParticleProgramData m_Program;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_KillIf final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_KillIf, plParticleBehavior);

public:
  virtual void CreateRequiredStreams() override;

protected:
  friend class plParticleBehaviorFactory_KillIf;

  virtual void Process(plUInt64 uiNumElements) override;

  bool m_bCondition = false;
  const plParticleProgramData* m_pProgram = nullptr;
  plParticleProgramRunner m_Runner;
};
