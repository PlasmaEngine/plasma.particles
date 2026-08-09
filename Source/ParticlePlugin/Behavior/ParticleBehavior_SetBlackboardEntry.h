#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>

class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_SetBlackboardEntry final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_SetBlackboardEntry, plParticleBehaviorFactory);

public:
  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plString m_sBlackboard; ///< name of the global blackboard, created on demand
  plString m_sEntry;
  float m_fValue = 0.0f;
  bool m_bContinuous = false; ///< write every update instead of once when the system starts
};

/// Writes one value into a named global blackboard entry — the effect's outward channel to
/// gameplay, so a VFX can flip a switch or feed a number the game reads.
///
/// The value is a plain float and therefore graph-drivable through its property pin (constant per
/// effect instance; the graph folds at asset transform). Writes happen while the system has live
/// particles: once on the first update, or every update in Continuous mode.
class PL_PARTICLEPLUGIN_DLL plParticleBehavior_SetBlackboardEntry final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_SetBlackboardEntry, plParticleBehavior);

public:
  plString m_sBlackboard;
  plString m_sEntry;
  float m_fValue = 0.0f;
  bool m_bContinuous = false;

  virtual void CreateRequiredStreams() override {}

protected:
  virtual void Process(plUInt64 uiNumElements) override;

  bool m_bWritten = false;
};
