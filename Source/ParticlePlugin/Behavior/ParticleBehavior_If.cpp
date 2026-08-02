#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_If.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_If, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_If>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("ConditionInput", plParticleAttribute, m_ConditionInput),
    PL_ENUM_MEMBER_PROPERTY("Comparison", plParticleConditionOp, m_Comparison),
    PL_MEMBER_PROPERTY("Threshold", m_fThreshold),
    PL_ENUM_MEMBER_PROPERTY("OutputAttribute", plParticleAttribute, m_OutputAttribute),
    PL_MEMBER_PROPERTY("TrueValue", m_fTrueValue)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("FalseValue", m_fFalseValue)->AddAttributes(new plDefaultValueAttribute(0.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_If, 1, plRTTIDefaultAllocator<plParticleBehavior_If>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleBehaviorFactory_If::plParticleBehaviorFactory_If() = default;

const plRTTI* plParticleBehaviorFactory_If::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_If>();
}

void plParticleBehaviorFactory_If::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_If* pBehavior = static_cast<plParticleBehavior_If*>(pObject);

  pBehavior->m_ConditionInput = m_ConditionInput;
  pBehavior->m_Comparison = m_Comparison;
  pBehavior->m_fThreshold = m_fThreshold;
  pBehavior->m_OutputAttribute = m_OutputAttribute;
  pBehavior->m_fTrueValue = m_fTrueValue;
  pBehavior->m_fFalseValue = m_fFalseValue;
}

void plParticleBehaviorFactory_If::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  if (m_OutputAttribute == plParticleAttribute::Speed)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
  }
}

void plParticleBehaviorFactory_If::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_ConditionInput;
  inout_stream << m_Comparison;
  inout_stream << m_fThreshold;
  inout_stream << m_OutputAttribute;
  inout_stream << m_fTrueValue;
  inout_stream << m_fFalseValue;
}

void plParticleBehaviorFactory_If::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_ConditionInput;
  inout_stream >> m_Comparison;
  inout_stream >> m_fThreshold;
  inout_stream >> m_OutputAttribute;
  inout_stream >> m_fTrueValue;
  inout_stream >> m_fFalseValue;
}

void plParticleBehavior_If::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Half4, &m_pStreamVelocity, false);
}

void plParticleBehavior_If::QueryOptionalStreams()
{
  m_pStreamSize = GetOwnerSystem()->QueryStream("Size", plProcessingStream::DataType::Half);
  m_pStreamColor = GetOwnerSystem()->QueryStream("Color", plProcessingStream::DataType::Half4);
  m_pStreamLifeTime = GetOwnerSystem()->QueryStream("LifeTime", plProcessingStream::DataType::Float2);
}

void plParticleBehavior_If::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: If");

  for (plUInt32 i = 0; i < (plUInt32)uiNumElements; ++i)
  {
    const float fInput = plReadParticleAttribute(m_ConditionInput, i,
      m_pStreamPosition, m_pStreamVelocity, m_pStreamSize, m_pStreamColor, m_pStreamLifeTime);

    const bool bResult = plEvaluateParticleCondition(m_Comparison, fInput, m_fThreshold);
    const float fOutput = bResult ? m_fTrueValue : m_fFalseValue;

    plWriteParticleAttribute(m_OutputAttribute, i,
      fOutput, m_pStreamPosition, m_pStreamVelocity, m_pStreamSize, m_pStreamColor);
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_If);
