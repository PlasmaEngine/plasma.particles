#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SetAttribute.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleSetAttributeMode, 1)
  PL_ENUM_CONSTANT(plParticleSetAttributeMode::Replace),
  PL_ENUM_CONSTANT(plParticleSetAttributeMode::Add),
  PL_ENUM_CONSTANT(plParticleSetAttributeMode::Multiply),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_SetAttribute, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_SetAttribute>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Attribute", plParticleAttribute, m_Attribute),
    PL_ENUM_MEMBER_PROPERTY("Mode", plParticleSetAttributeMode, m_Mode),
    PL_MEMBER_PROPERTY("Value", m_fValue)->AddAttributes(new plDefaultValueAttribute(1.0f)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plTitleAttribute("{Attribute} = {Value}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_SetAttribute, 1, plRTTIDefaultAllocator<plParticleBehavior_SetAttribute>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_SetAttribute::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_SetAttribute>();
}

void plParticleBehaviorFactory_SetAttribute::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_SetAttribute* pBehavior = static_cast<plParticleBehavior_SetAttribute*>(pObject);

  pBehavior->m_Attribute = m_Attribute;
  pBehavior->m_Mode = m_Mode;
  pBehavior->m_fValue = m_fValue;
}

void plParticleBehaviorFactory_SetAttribute::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_Attribute;
  inout_stream << m_Mode;
  inout_stream << m_fValue;
}

void plParticleBehaviorFactory_SetAttribute::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_Attribute;
  inout_stream >> m_Mode;
  inout_stream >> m_fValue;
}

//////////////////////////////////////////////////////////////////////////

void plParticleBehavior_SetAttribute::CreateRequiredStreams()
{
  // Only the stream this attribute writes is required; the rest are picked up if they exist.
  // bWillInitialize is false so an initializer's values survive until this block runs.
  switch (m_Attribute)
  {
    case plParticleAttribute::PositionX:
    case plParticleAttribute::PositionY:
    case plParticleAttribute::PositionZ:
      CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
      break;

    case plParticleAttribute::Speed:
      CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
      break;

    case plParticleAttribute::Size:
      CreateStream("Size", plProcessingStream::DataType::Half, &m_pStreamSize, false);
      break;

    case plParticleAttribute::ColorR:
    case plParticleAttribute::ColorG:
    case plParticleAttribute::ColorB:
    case plParticleAttribute::ColorA:
      CreateStream("Color", plProcessingStream::DataType::Half4, &m_pStreamColor, false);
      break;

    default:
      // LifeFraction is read-only; the write helper ignores it
      break;
  }
}

void plParticleBehavior_SetAttribute::QueryOptionalStreams()
{
  // Add and Multiply read the current value first, and the read helper needs whichever streams
  // exist. All queried lazily so this block never forces streams the system does not have.
  m_pStreamPosition = GetOwnerSystem()->QueryStream("Position", plProcessingStream::DataType::Float4);
  m_pStreamVelocity = GetOwnerSystem()->QueryStream("Velocity", plProcessingStream::DataType::Float3);
  m_pStreamSize = GetOwnerSystem()->QueryStream("Size", plProcessingStream::DataType::Half);
  m_pStreamColor = GetOwnerSystem()->QueryStream("Color", plProcessingStream::DataType::Half4);
  m_pStreamLifeTime = GetOwnerSystem()->QueryStream("LifeTime", plProcessingStream::DataType::Half2);
}

void plParticleBehavior_SetAttribute::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Set Attribute");

  if (m_Attribute == plParticleAttribute::LifeFraction)
    return; // read-only

  for (plUInt32 i = 0; i < (plUInt32)uiNumElements; ++i)
  {
    float fValue = m_fValue;

    if (m_Mode != plParticleSetAttributeMode::Replace)
    {
      const float fCurrent = plReadParticleAttribute(
        m_Attribute, i, m_pStreamPosition, m_pStreamVelocity, m_pStreamSize, m_pStreamColor, m_pStreamLifeTime);

      fValue = (m_Mode == plParticleSetAttributeMode::Add) ? (fCurrent + fValue) : (fCurrent * fValue);
    }

    plWriteParticleAttribute(m_Attribute, i, fValue, m_pStreamPosition, m_pStreamVelocity, m_pStreamSize, m_pStreamColor);
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_SetAttribute);
