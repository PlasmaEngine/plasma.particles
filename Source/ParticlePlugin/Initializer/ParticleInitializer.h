#pragma once

#include <Foundation/DataProcessing/Stream/ProcessingStream.h>
#include <Foundation/Reflection/Reflection.h>
#include <ParticlePlugin/Declarations.h>
#include <ParticlePlugin/Module/ParticleModule.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

class plParticleSystemInstance;
class plProcessingStream;
class plParticleInitializer;
class plParticleEffectInstance;
struct plParticleProgramData;

/// \brief Base class for all particle emitters
class PL_PARTICLEPLUGIN_DLL plParticleInitializerFactory : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializerFactory, plReflectedClass);

public:
  virtual const plRTTI* GetInitializerType() const = 0;
  virtual void CopyInitializerProperties(plParticleInitializer* pInitializer, bool bFirstTime) const = 0;
  virtual float GetSpawnCountMultiplier(const plParticleEffectInstance* pEffect) const;

  plParticleInitializer* CreateInitializer(plParticleSystemInstance* pOwner) const;

  virtual void Save(plStreamWriter& inout_stream) const = 0;
  virtual void Load(plStreamReader& inout_stream) = 0;

  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const {}

  /// The compiled per-particle wires into the properties marked plParticlePerParticleAttribute, or
  /// null for blocks that have none of those. They run at spawn, over the new particles.
  virtual plParticleProgramData* GetParticleProgram() { return nullptr; }
  const plParticleProgramData* GetParticleProgram() const { return const_cast<plParticleInitializerFactory*>(this)->GetParticleProgram(); }

  /// Authoring only: a disabled block is kept in the document but left out of the built
  /// descriptor, so the runtime never sees it and nothing needs to serialize the flag.
  bool m_bEnabled = true;

  /// Authoring only: names this block in the graph's wire pins (see plParticlePropertyPin). Empty
  /// means the block's guid; a copy fills it in so pasted wires still find their block.
  plString m_sPinId;
};

/// \brief Base class for stream spawners that are used by plParticleEmitter's
class PL_PARTICLEPLUGIN_DLL plParticleInitializer : public plParticleModule
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializer, plParticleModule);

  friend class plParticleSystemInstance;
  friend class plParticleInitializerFactory;

protected:
  plParticleInitializer();

  virtual void Process(plUInt64 uiNumElements) final override {}
};
