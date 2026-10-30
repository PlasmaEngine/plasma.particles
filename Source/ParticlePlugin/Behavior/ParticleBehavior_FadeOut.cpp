#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Math/Color16f.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_FadeOut.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_FadeOut, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_FadeOut>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("StartAlpha", m_fStartAlpha)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("Exponent", m_fExponent)->AddAttributes(new plDefaultValueAttribute(1.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_FadeOut, 1, plRTTIDefaultAllocator<plParticleBehavior_FadeOut>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_FadeOut::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_FadeOut>();
}

void plParticleBehaviorFactory_FadeOut::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_FadeOut* pBehavior = static_cast<plParticleBehavior_FadeOut*>(pObject);

  pBehavior->m_fStartAlpha = m_fStartAlpha;
  pBehavior->m_fExponent = m_fExponent;
}

void plParticleBehaviorFactory_FadeOut::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_fStartAlpha;
  inout_stream << m_fExponent;
}

void plParticleBehaviorFactory_FadeOut::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  inout_stream >> m_fStartAlpha;
  inout_stream >> m_fExponent;
}

void plParticleBehavior_FadeOut::CreateRequiredStreams()
{
  CreateStream("LifeTime", plProcessingStream::DataType::Float2, &m_pStreamLifeTime, false);
  CreateStream("Color", plProcessingStream::DataType::Half4, &m_pStreamColor, false);
}

void plParticleBehavior_FadeOut::Process(plUInt64 uiNumElements)
{
  if (!GetOwnerEffect()->IsVisible())
  {
    // set the update interval such that once the effect becomes visible,
    // all particles get fully updated
    m_uiCurrentUpdateInterval = 1;
    m_uiFirstToUpdate = 0;
    return;
  }

  PL_PROFILE_SCOPE("PFX: Fade Out");

  plProcessingStreamIterator<plVec2> itLifeTime(m_pStreamLifeTime, uiNumElements, 0);
  plProcessingStreamIterator<plColorLinear16f> itColor(m_pStreamColor, uiNumElements, 0);

  // skip the first n particles
  itLifeTime.Advance(m_uiFirstToUpdate);
  itColor.Advance(m_uiFirstToUpdate);

  if (m_fStartAlpha <= 1.0f)
  {
    while (!itLifeTime.HasReachedEnd())
    {
      const float fLifeTimeFraction = itLifeTime.Current().x * itLifeTime.Current().y;
      itColor.Current().a = m_fStartAlpha * plMath::Pow(fLifeTimeFraction, m_fExponent);

      itLifeTime.Advance(m_uiCurrentUpdateInterval);
      itColor.Advance(m_uiCurrentUpdateInterval);
    }
  }
  else
  {
    // this case has to clamp alpha to 1
    while (!itLifeTime.HasReachedEnd())
    {
      const float fLifeTimeFraction = itLifeTime.Current().x * itLifeTime.Current().y;
      itColor.Current().a = plMath::Min(1.0f, m_fStartAlpha * plMath::Pow(fLifeTimeFraction, m_fExponent));

      itLifeTime.Advance(m_uiCurrentUpdateInterval);
      itColor.Advance(m_uiCurrentUpdateInterval);
    }
  }

  // the next frame updates the next slice, at the interval the effect's camera distance asks for
  m_uiCurrentUpdateInterval = GetOwnerEffect()->GetLodUpdateInterval();

  ++m_uiFirstToUpdate;
  if (m_uiFirstToUpdate >= m_uiCurrentUpdateInterval)
    m_uiFirstToUpdate = 0;
}



PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_FadeOut);
