#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_Attribute, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_Attribute>)
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

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_Attribute, 1, plRTTIDefaultAllocator<plParticleBehavior_Attribute>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_Attribute::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_Attribute>();
}

void plParticleBehaviorFactory_Attribute::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_Attribute* pBehavior = static_cast<plParticleBehavior_Attribute*>(pObject);
  pBehavior->m_Sink.SetSettings(&m_Settings);
}

void plParticleBehaviorFactory_Attribute::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  // a written velocity has to move the particles
  if (m_Settings.m_Target == plParticleSinkTarget::Velocity)
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
}

void plParticleBehaviorFactory_Attribute::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;
  m_Settings.Save(inout_stream);
}

void plParticleBehaviorFactory_Attribute::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);
  m_Settings.Load(inout_stream);
}

//////////////////////////////////////////////////////////////////////////

void plParticleBehavior_Attribute::CreateRequiredStreams()
{
  m_Sink.CreateRequiredStreams([this](const char* szName, plProcessingStream::DataType type, plProcessingStream** ppStream)
    { CreateStream(szName, type, ppStream, false); });
}

void plParticleBehavior_Attribute::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Set Attribute");

  m_Sink.Apply(GetOwnerSystem(), 0, uiNumElements, m_TimeDiff, (float)m_TimeDiff.GetSeconds());
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_Attribute);
