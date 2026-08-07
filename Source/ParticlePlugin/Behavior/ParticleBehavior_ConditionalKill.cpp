#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ConditionalKill.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleKillAttribute, 1)
  PL_ENUM_CONSTANT(plParticleKillAttribute::PositionX),
  PL_ENUM_CONSTANT(plParticleKillAttribute::PositionY),
  PL_ENUM_CONSTANT(plParticleKillAttribute::PositionZ),
  PL_ENUM_CONSTANT(plParticleKillAttribute::Speed),
  PL_ENUM_CONSTANT(plParticleKillAttribute::Size),
  PL_ENUM_CONSTANT(plParticleKillAttribute::ColorAlpha),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleComparisonOp, 1)
  PL_ENUM_CONSTANT(plParticleComparisonOp::Less),
  PL_ENUM_CONSTANT(plParticleComparisonOp::LessEqual),
  PL_ENUM_CONSTANT(plParticleComparisonOp::Greater),
  PL_ENUM_CONSTANT(plParticleComparisonOp::GreaterEqual),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_ConditionalKill, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_ConditionalKill>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Attribute", plParticleKillAttribute, m_Attribute),
    PL_ENUM_MEMBER_PROPERTY("Comparison", plParticleComparisonOp, m_Comparison),
    PL_MEMBER_PROPERTY("Threshold", m_fThreshold),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_ConditionalKill, 1, plRTTIDefaultAllocator<plParticleBehavior_ConditionalKill>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleBehaviorFactory_ConditionalKill::plParticleBehaviorFactory_ConditionalKill() = default;

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
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_Attribute;
  inout_stream << m_Comparison;
  inout_stream << m_fThreshold;
}

void plParticleBehaviorFactory_ConditionalKill::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_Attribute;
  inout_stream >> m_Comparison;
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
}

static float GetAttributeValue(
  plParticleKillAttribute::Enum attr,
  const plSimdVec4f& position,
  const plVec3* pVelocity,
  const plFloat16* pSize,
  const plFloat16Vec4* pColor)
{
  switch (attr)
  {
    case plParticleKillAttribute::PositionX:
      return position.GetComponent<0>();
    case plParticleKillAttribute::PositionY:
      return position.GetComponent<1>();
    case plParticleKillAttribute::PositionZ:
      return position.GetComponent<2>();
    case plParticleKillAttribute::Speed:
    {
      if (pVelocity)
        return pVelocity->GetLength();
      return 0.0f;
    }
    case plParticleKillAttribute::Size:
    {
      if (pSize)
        return static_cast<float>(*pSize);
      return 0.0f;
    }
    case plParticleKillAttribute::ColorAlpha:
    {
      if (pColor)
        return static_cast<float>(pColor->w);
      return 1.0f;
    }
    default:
      return 0.0f;
  }
}

static bool CompareValue(plParticleComparisonOp::Enum op, float fValue, float fThreshold)
{
  switch (op)
  {
    case plParticleComparisonOp::Less:
      return fValue < fThreshold;
    case plParticleComparisonOp::LessEqual:
      return fValue <= fThreshold;
    case plParticleComparisonOp::Greater:
      return fValue > fThreshold;
    case plParticleComparisonOp::GreaterEqual:
      return fValue >= fThreshold;
    default:
      return false;
  }
}

void plParticleBehavior_ConditionalKill::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: ConditionalKill");

  plProcessingStreamIterator<plSimdVec4f> itPosition(m_pStreamPosition, uiNumElements, 0);

  const plVec3* pVelocityData = m_pStreamVelocity ? m_pStreamVelocity->GetData<plVec3>() : nullptr;
  const plFloat16* pSizeData = m_pStreamSize ? m_pStreamSize->GetData<plFloat16>() : nullptr;
  const plFloat16Vec4* pColorData = m_pStreamColor ? m_pStreamColor->GetData<plFloat16Vec4>() : nullptr;

  plUInt32 idx = 0;

  while (!itPosition.HasReachedEnd())
  {
    const plSimdVec4f pos = itPosition.Current();

    const float fValue = GetAttributeValue(
      m_Attribute,
      pos,
      pVelocityData ? &pVelocityData[idx] : nullptr,
      pSizeData ? &pSizeData[idx] : nullptr,
      pColorData ? &pColorData[idx] : nullptr);

    if (CompareValue(m_Comparison, fValue, m_fThreshold))
    {
      m_pStreamGroup->RemoveElement(idx);
    }

    ++idx;
    itPosition.Advance();
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_ConditionalKill);
