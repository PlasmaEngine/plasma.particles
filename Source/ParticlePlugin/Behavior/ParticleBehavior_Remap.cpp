#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Remap.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_Remap, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_Remap>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("InputAttribute", plParticleAttribute, m_InputAttribute)->AddAttributes(new plDefaultValueAttribute((plInt32)plParticleAttribute::LifeFraction)),
    PL_ENUM_MEMBER_PROPERTY("OutputAttribute", plParticleAttribute, m_OutputAttribute),
    PL_MEMBER_PROPERTY("InputMin", m_fInputMin)->AddAttributes(new plDefaultValueAttribute(0.0f)),
    PL_MEMBER_PROPERTY("InputMax", m_fInputMax)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("OutputMin", m_fOutputMin)->AddAttributes(new plDefaultValueAttribute(0.0f)),
    PL_MEMBER_PROPERTY("OutputMax", m_fOutputMax)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("ClampOutput", m_bClampOutput)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_Remap, 1, plRTTIDefaultAllocator<plParticleBehavior_Remap>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleBehaviorFactory_Remap::plParticleBehaviorFactory_Remap() = default;

const plRTTI* plParticleBehaviorFactory_Remap::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_Remap>();
}

void plParticleBehaviorFactory_Remap::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_Remap* pBehavior = static_cast<plParticleBehavior_Remap*>(pObject);

  pBehavior->m_InputAttribute = m_InputAttribute;
  pBehavior->m_OutputAttribute = m_OutputAttribute;
  pBehavior->m_fInputMin = m_fInputMin;
  pBehavior->m_fInputMax = m_fInputMax;
  pBehavior->m_fOutputMin = m_fOutputMin;
  pBehavior->m_fOutputMax = m_fOutputMax;
  pBehavior->m_bClampOutput = m_bClampOutput;
}

void plParticleBehaviorFactory_Remap::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  if (m_OutputAttribute == plParticleAttribute::Speed)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
  }
}

void plParticleBehaviorFactory_Remap::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_InputAttribute;
  inout_stream << m_OutputAttribute;
  inout_stream << m_fInputMin;
  inout_stream << m_fInputMax;
  inout_stream << m_fOutputMin;
  inout_stream << m_fOutputMax;
  inout_stream << m_bClampOutput;
}

void plParticleBehaviorFactory_Remap::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_InputAttribute;
  inout_stream >> m_OutputAttribute;
  inout_stream >> m_fInputMin;
  inout_stream >> m_fInputMax;
  inout_stream >> m_fOutputMin;
  inout_stream >> m_fOutputMax;
  inout_stream >> m_bClampOutput;
}

void plParticleBehavior_Remap::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
}

void plParticleBehavior_Remap::QueryOptionalStreams()
{
  m_pStreamSize = GetOwnerSystem()->QueryStream("Size", plProcessingStream::DataType::Half);
  m_pStreamColor = GetOwnerSystem()->QueryStream("Color", plProcessingStream::DataType::Half4);
  m_pStreamLifeTime = GetOwnerSystem()->QueryStream("LifeTime", plProcessingStream::DataType::Half2);
}

void plParticleBehavior_Remap::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Remap");

  const float fInputRange = m_fInputMax - m_fInputMin;
  if (plMath::IsZero(fInputRange, 0.0001f))
    return;

  const float fInvInputRange = 1.0f / fInputRange;
  const float fOutputRange = m_fOutputMax - m_fOutputMin;

  for (plUInt32 i = 0; i < (plUInt32)uiNumElements; ++i)
  {
    float fInput = plReadParticleAttribute(m_InputAttribute, i,
      m_pStreamPosition, m_pStreamVelocity, m_pStreamSize, m_pStreamColor, m_pStreamLifeTime);

    // Normalize input to [0..1]
    float fNormalized = (fInput - m_fInputMin) * fInvInputRange;

    if (m_bClampOutput)
      fNormalized = plMath::Clamp(fNormalized, 0.0f, 1.0f);

    // Map to output range
    const float fOutput = m_fOutputMin + fNormalized * fOutputRange;

    plWriteParticleAttribute(m_OutputAttribute, i,
      fOutput, m_pStreamPosition, m_pStreamVelocity, m_pStreamSize, m_pStreamColor);
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_Remap);
