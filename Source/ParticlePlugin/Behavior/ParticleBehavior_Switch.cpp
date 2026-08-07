#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Switch.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_Switch, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_Switch>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("InputAttribute", plParticleAttribute, m_InputAttribute)->AddAttributes(new plDefaultValueAttribute((plInt32)plParticleAttribute::LifeFraction)),
    PL_ENUM_MEMBER_PROPERTY("OutputAttribute", plParticleAttribute, m_OutputAttribute),
    PL_MEMBER_PROPERTY("Threshold1", m_fThreshold1)->AddAttributes(new plDefaultValueAttribute(0.25f)),
    PL_MEMBER_PROPERTY("Threshold2", m_fThreshold2)->AddAttributes(new plDefaultValueAttribute(0.5f)),
    PL_MEMBER_PROPERTY("Threshold3", m_fThreshold3)->AddAttributes(new plDefaultValueAttribute(0.75f)),
    PL_MEMBER_PROPERTY("Value0", m_fValue0)->AddAttributes(new plDefaultValueAttribute(0.0f)),
    PL_MEMBER_PROPERTY("Value1", m_fValue1)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("Value2", m_fValue2)->AddAttributes(new plDefaultValueAttribute(2.0f)),
    PL_MEMBER_PROPERTY("Value3", m_fValue3)->AddAttributes(new plDefaultValueAttribute(3.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_Switch, 1, plRTTIDefaultAllocator<plParticleBehavior_Switch>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleBehaviorFactory_Switch::plParticleBehaviorFactory_Switch() = default;

const plRTTI* plParticleBehaviorFactory_Switch::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_Switch>();
}

void plParticleBehaviorFactory_Switch::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_Switch* pBehavior = static_cast<plParticleBehavior_Switch*>(pObject);

  pBehavior->m_InputAttribute = m_InputAttribute;
  pBehavior->m_OutputAttribute = m_OutputAttribute;
  pBehavior->m_fThreshold1 = m_fThreshold1;
  pBehavior->m_fThreshold2 = m_fThreshold2;
  pBehavior->m_fThreshold3 = m_fThreshold3;
  pBehavior->m_fValue0 = m_fValue0;
  pBehavior->m_fValue1 = m_fValue1;
  pBehavior->m_fValue2 = m_fValue2;
  pBehavior->m_fValue3 = m_fValue3;
}

void plParticleBehaviorFactory_Switch::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  if (m_OutputAttribute == plParticleAttribute::Speed)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
  }
}

void plParticleBehaviorFactory_Switch::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_InputAttribute;
  inout_stream << m_OutputAttribute;
  inout_stream << m_fThreshold1;
  inout_stream << m_fThreshold2;
  inout_stream << m_fThreshold3;
  inout_stream << m_fValue0;
  inout_stream << m_fValue1;
  inout_stream << m_fValue2;
  inout_stream << m_fValue3;
}

void plParticleBehaviorFactory_Switch::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_InputAttribute;
  inout_stream >> m_OutputAttribute;
  inout_stream >> m_fThreshold1;
  inout_stream >> m_fThreshold2;
  inout_stream >> m_fThreshold3;
  inout_stream >> m_fValue0;
  inout_stream >> m_fValue1;
  inout_stream >> m_fValue2;
  inout_stream >> m_fValue3;
}

void plParticleBehavior_Switch::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
}

void plParticleBehavior_Switch::QueryOptionalStreams()
{
  m_pStreamSize = GetOwnerSystem()->QueryStream("Size", plProcessingStream::DataType::Half);
  m_pStreamColor = GetOwnerSystem()->QueryStream("Color", plProcessingStream::DataType::Half4);
  m_pStreamLifeTime = GetOwnerSystem()->QueryStream("LifeTime", plProcessingStream::DataType::Half2);
}

void plParticleBehavior_Switch::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Switch");

  for (plUInt32 i = 0; i < (plUInt32)uiNumElements; ++i)
  {
    const float fInput = plReadParticleAttribute(m_InputAttribute, i,
      m_pStreamPosition, m_pStreamVelocity, m_pStreamSize, m_pStreamColor, m_pStreamLifeTime);

    float fOutput;
    if (fInput < m_fThreshold1)
      fOutput = m_fValue0;
    else if (fInput < m_fThreshold2)
      fOutput = m_fValue1;
    else if (fInput < m_fThreshold3)
      fOutput = m_fValue2;
    else
      fOutput = m_fValue3;

    plWriteParticleAttribute(m_OutputAttribute, i,
      fOutput, m_pStreamPosition, m_pStreamVelocity, m_pStreamSize, m_pStreamColor);
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_Switch);
