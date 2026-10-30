#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SetAttribute.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializerFactory_SetAttribute, 1, plRTTIDefaultAllocator<plParticleInitializerFactory_SetAttribute>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_PARTICLE_ATTRIBUTE_SINK_PROPERTIES,
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plTitleAttribute("Set Attribute"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializer_SetAttribute, 1, plRTTIDefaultAllocator<plParticleInitializer_SetAttribute>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleInitializerFactory_SetAttribute::GetInitializerType() const
{
  return plGetStaticRTTI<plParticleInitializer_SetAttribute>();
}

void plParticleInitializerFactory_SetAttribute::CopyInitializerProperties(plParticleInitializer* pInitializer0, bool bFirstTime) const
{
  plParticleInitializer_SetAttribute* pInitializer = static_cast<plParticleInitializer_SetAttribute*>(pInitializer0);
  pInitializer->m_Sink.SetSettings(&m_Settings);
}

void plParticleInitializerFactory_SetAttribute::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  // a written velocity has to move the particles
  if (m_Settings.m_Target == plParticleSinkTarget::Velocity)
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
}

void plParticleInitializerFactory_SetAttribute::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;
  m_Settings.Save(inout_stream);
}

void plParticleInitializerFactory_SetAttribute::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);
  m_Settings.Load(inout_stream);
}

//////////////////////////////////////////////////////////////////////////

void plParticleInitializer_SetAttribute::CreateRequiredStreams()
{
  m_Sink.CreateRequiredStreams([this](const char* szName, plProcessingStream::DataType type, plProcessingStream** ppStream)
    { CreateStream(szName, type, ppStream, false); });
}

void plParticleInitializer_SetAttribute::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Set Attribute (Initialize)");

  // at spawn Add adds the value once, so it is not scaled by a step length
  m_Sink.Apply(GetOwnerSystem(), uiStartIndex, uiNumElements, plTime::MakeZero(), 1.0f);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Initializer_ParticleInitializer_SetAttribute);
