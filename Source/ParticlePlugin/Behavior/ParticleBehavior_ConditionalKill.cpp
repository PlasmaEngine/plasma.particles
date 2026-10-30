#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ConditionalKill.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_ConditionalKill, 2, plRTTIDefaultAllocator<plParticleBehaviorFactory_ConditionalKill>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Attribute", plParticleAttribute, m_Attribute)->AddAttributes(new plDefaultValueAttribute((plInt32)plParticleAttribute::PositionZ)),
    PL_ENUM_MEMBER_PROPERTY("Comparison", plParticleConditionOp, m_Comparison),
    PL_MEMBER_PROPERTY("Threshold", m_fThreshold),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    // converted into Kill If by the editor; still reflected so existing effects load
    new plHiddenAttribute(),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_ConditionalKill, 1, plRTTIDefaultAllocator<plParticleBehavior_ConditionalKill>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// Version 1 stored its own attribute list: PositionX, PositionY, PositionZ, Speed, Size, ColorAlpha.
  plParticleAttribute::Enum FromVersion1Attribute(plUInt8 uiValue)
  {
    return uiValue == 5 ? plParticleAttribute::ColorA : (plParticleAttribute::Enum)plMath::Min<plUInt8>(uiValue, 4);
  }
} // namespace

plParticleBehaviorFactory_ConditionalKill::plParticleBehaviorFactory_ConditionalKill()
{
  m_Attribute = plParticleAttribute::PositionZ;
}

const plRTTI* plParticleBehaviorFactory_ConditionalKill::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_ConditionalKill>();
}

void plParticleBehaviorFactory_ConditionalKill::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_ConditionalKill* pBehavior = static_cast<plParticleBehavior_ConditionalKill*>(pObject);

  pBehavior->m_Attribute = m_Attribute;
  pBehavior->m_Comparison = m_Comparison;
  pBehavior->m_fThreshold = m_fThreshold;
}

void plParticleBehaviorFactory_ConditionalKill::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 2;
  inout_stream << uiVersion;

  inout_stream << m_Attribute;
  inout_stream << m_Comparison;
  inout_stream << m_fThreshold;
}

void plParticleBehaviorFactory_ConditionalKill::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 2, "Invalid version {0}", uiVersion);

  if (uiVersion >= 2)
  {
    inout_stream >> m_Attribute;
    inout_stream >> m_Comparison;
  }
  else
  {
    // the comparison list of version 1 is the start of plParticleConditionOp
    plUInt8 uiAttribute = 0, uiComparison = 0;
    inout_stream >> uiAttribute;
    inout_stream >> uiComparison;
    m_Attribute = FromVersion1Attribute(uiAttribute);
    m_Comparison = (plParticleConditionOp::Enum)uiComparison;
  }

  inout_stream >> m_fThreshold;
}

void plParticleBehavior_ConditionalKill::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
}

void plParticleBehavior_ConditionalKill::QueryOptionalStreams()
{
  m_pStreamVelocity = GetOwnerSystem()->QueryStream("Velocity", plProcessingStream::DataType::Float3);
  m_pStreamSize = GetOwnerSystem()->QueryStream("Size", plProcessingStream::DataType::Half);
  m_pStreamColor = GetOwnerSystem()->QueryStream("Color", plProcessingStream::DataType::Half4);
  m_pStreamLifeTime = GetOwnerSystem()->QueryStream("LifeTime", plProcessingStream::DataType::Float2);
}

void plParticleBehavior_ConditionalKill::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: ConditionalKill");

  for (plUInt32 i = 0; i < (plUInt32)uiNumElements; ++i)
  {
    const float fValue = plReadParticleAttribute(m_Attribute, i, m_pStreamPosition, m_pStreamVelocity, m_pStreamSize, m_pStreamColor, m_pStreamLifeTime);

    if (plEvaluateParticleCondition(m_Comparison, fValue, m_fThreshold))
    {
      m_pStreamGroup->RemoveElement(i);
    }
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_ConditionalKill);
