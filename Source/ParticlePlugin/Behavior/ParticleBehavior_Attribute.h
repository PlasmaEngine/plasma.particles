#pragma once

#include <ParticlePlugin/Behavior/ParticleAttributeSink.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>

/// Sets one attribute of every particle each step, from a value or a per-particle wire.
///
/// Runs at its place among the behaviors. Add is a rate here: the value is scaled by the step
/// length, so 2 adds 2 per second whatever the frame rate.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_Attribute final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_Attribute, plParticleBehaviorFactory);

public:
  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;
  virtual plParticleProgramData* GetParticleProgram() override { return &m_Settings.m_Program; }

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  PL_PARTICLE_ATTRIBUTE_SINK_ACCESSORS

  plParticleAttributeSinkSettings m_Settings;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_Attribute final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_Attribute, plParticleBehavior);

public:
  virtual void CreateRequiredStreams() override;

protected:
  friend class plParticleBehaviorFactory_Attribute;

  virtual void Process(plUInt64 uiNumElements) override;

  plParticleAttributeSink m_Sink;
};
