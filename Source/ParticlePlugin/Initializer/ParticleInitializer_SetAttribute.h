#pragma once

#include <ParticlePlugin/Behavior/ParticleAttributeSink.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>

/// Sets one attribute of every new particle, from a value or a per-particle wire.
///
/// Runs at its place among the initializers, so it sees what the ones before it wrote. Add adds the
/// value once.
class PL_PARTICLEPLUGIN_DLL plParticleInitializerFactory_SetAttribute final : public plParticleInitializerFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializerFactory_SetAttribute, plParticleInitializerFactory);

public:
  virtual const plRTTI* GetInitializerType() const override;
  virtual void CopyInitializerProperties(plParticleInitializer* pInitializer, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  PL_PARTICLE_ATTRIBUTE_SINK_ACCESSORS

  plParticleAttributeSinkSettings m_Settings;
};

class PL_PARTICLEPLUGIN_DLL plParticleInitializer_SetAttribute final : public plParticleInitializer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializer_SetAttribute, plParticleInitializer);

public:
  virtual void CreateRequiredStreams() override;

protected:
  friend class plParticleInitializerFactory_SetAttribute;

  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  plParticleAttributeSink m_Sink;
};
