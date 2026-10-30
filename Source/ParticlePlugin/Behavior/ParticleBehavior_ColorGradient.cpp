#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Math/Color16f.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ColorGradient.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_ColorGradient, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_ColorGradient>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_RESOURCE_MEMBER_PROPERTY("Gradient", m_hGradient)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Data_Gradient")),
    PL_MEMBER_PROPERTY("TintColor", m_TintColor)->AddAttributes(new plExposeColorAlphaAttribute(), new plParticlePerParticleAttribute()),
    PL_ENUM_MEMBER_PROPERTY("ColorGradientMode", plParticleColorGradientMode, m_GradientMode),
    PL_MEMBER_PROPERTY("GradientMaxSpeed", m_fMaxSpeed)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 100.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_ColorGradient, 1, plRTTIDefaultAllocator<plParticleBehavior_ColorGradient>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_ColorGradient::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_ColorGradient>();
}

void plParticleBehaviorFactory_ColorGradient::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_ColorGradient* pBehavior = static_cast<plParticleBehavior_ColorGradient*>(pObject);

  pBehavior->m_hGradient = m_hGradient;
  pBehavior->m_GradientMode = m_GradientMode;
  pBehavior->m_fMaxSpeed = m_fMaxSpeed;
  pBehavior->m_TintColor = m_TintColor;
  pBehavior->m_pProgram = &m_Program;

  // the gradient resource may not be specified yet, so defer evaluation until an element is created
  pBehavior->m_InitColor = plColor::RebeccaPurple;
}

void plParticleBehaviorFactory_ColorGradient::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 5;
  inout_stream << uiVersion;

  inout_stream << m_hGradient;

  // version 3
  inout_stream << m_GradientMode;
  inout_stream << m_fMaxSpeed;

  // Version 4
  inout_stream << m_TintColor;

  // version 5
  m_Program.Save(inout_stream);
}

void plParticleBehaviorFactory_ColorGradient::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  inout_stream >> m_hGradient;

  if (uiVersion >= 3)
  {
    inout_stream >> m_GradientMode;
    inout_stream >> m_fMaxSpeed;
  }

  if (uiVersion >= 4)
  {
    inout_stream >> m_TintColor;
  }

  if (uiVersion >= 5)
  {
    m_Program.Load(inout_stream);
  }
}

void plParticleBehavior_ColorGradient::CreateRequiredStreams()
{
  m_pStreamColor = nullptr;
  m_pStreamVelocity = nullptr;

  CreateStream("Color", plProcessingStream::DataType::Half4, &m_pStreamColor, false);

  if (m_GradientMode == plParticleColorGradientMode::Age)
  {
    CreateStream("LifeTime", plProcessingStream::DataType::Float2, &m_pStreamLifeTime, false);
  }
  else if (m_GradientMode == plParticleColorGradientMode::Speed)
  {
    CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
  }

  m_Runner.Configure(m_pProgram);
  m_Runner.CreateRequiredStreams([this](const char* szName, plProcessingStream::DataType type, plProcessingStream** ppStream)
    { CreateStream(szName, type, ppStream, false); });
}

void plParticleBehavior_ColorGradient::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  if (!m_hGradient.IsValid())
    return;

  PL_PROFILE_SCOPE("PFX: Color Gradient Init");

  // query the init color from the gradient
  if (m_InitColor == plColor::RebeccaPurple)
  {
    m_InitColor = m_TintColor;

    plResourceLock<plColorGradientResource> pGradient(m_hGradient, plResourceAcquireMode::BlockTillLoaded);

    if (pGradient.GetAcquireResult() != plResourceAcquireResult::MissingFallback)
    {
      const plColorGradient& gradient = pGradient->GetDescriptor().m_Gradient;

      plColor rgba;
      plUInt8 alpha;
      gradient.EvaluateColor(0, rgba);
      gradient.EvaluateAlpha(0, alpha);
      rgba.a = plMath::ColorByteToFloat(alpha);

      m_InitColor = rgba;
    }
  }

  const plColorLinear16f initCol16 = m_InitColor;

  plProcessingStreamIterator<plColorLinear16f> itColor(m_pStreamColor, uiNumElements, uiStartIndex);
  while (!itColor.HasReachedEnd())
  {
    itColor.Current() = initCol16;
    itColor.Advance();
  }
}

void plParticleBehavior_ColorGradient::Process(plUInt64 uiNumElements)
{
  if (!GetOwnerEffect()->IsVisible())
  {
    // set the update interval such that once the effect becomes visible,
    // all particles get fully updated
    m_uiCurrentUpdateInterval = 1;
    m_uiFirstToUpdate = 0;
    return;
  }

  if (!m_hGradient.IsValid())
    return;

  PL_PROFILE_SCOPE("PFX: Color Gradient");

  plResourceLock<plColorGradientResource> pGradient(m_hGradient, plResourceAcquireMode::BlockTillLoaded);

  if (pGradient.GetAcquireResult() == plResourceAcquireResult::MissingFallback)
    return;

  const plColorGradient& gradient = pGradient->GetDescriptor().m_Gradient;

  // a per-particle wire gives every particle its own tint; uiParticle follows the iterators
  plUInt32 uiTintComponents = 0;
  const float* pTint = m_Runner.RunProperty(GetOwnerSystem(), uiNumElements, m_TimeDiff, "TintColor", uiTintComponents);
  plUInt64 uiParticle = m_uiFirstToUpdate;

  auto getTint = [&]() -> plColor
  {
    if (pTint == nullptr || uiTintComponents < 4)
      return m_TintColor;

    const float* pValue = pTint + uiParticle * uiTintComponents;
    return plColor(pValue[0], pValue[1], pValue[2], pValue[3]);
  };

  plProcessingStreamIterator<plColorLinear16f> itColor(m_pStreamColor, uiNumElements, 0);

  // skip the first n particles
  itColor.Advance(m_uiFirstToUpdate);

  if (m_GradientMode == plParticleColorGradientMode::Age)
  {
    plProcessingStreamIterator<plVec2> itLifeTime(m_pStreamLifeTime, uiNumElements, 0);

    // skip the first n particles
    itLifeTime.Advance(m_uiFirstToUpdate);

    while (!itLifeTime.HasReachedEnd())
    {
      // if (itLifeTime.Current().y > 0)
      {
        const float fLifeTimeFraction = itLifeTime.Current().x * itLifeTime.Current().y;
        const float posx = 1.0f - fLifeTimeFraction;

        plColor rgba;
        plUInt8 alpha;
        gradient.EvaluateColor(posx, rgba);
        gradient.EvaluateAlpha(posx, alpha);
        rgba.a = plMath::ColorByteToFloat(alpha);

        itColor.Current() = rgba * getTint();
      }

      // skip the next n items
      // this is to reduce the number of particles that need to be fully evaluated,
      // since sampling the color gradient is pretty expensive
      uiParticle += m_uiCurrentUpdateInterval;
      itLifeTime.Advance(m_uiCurrentUpdateInterval);
      itColor.Advance(m_uiCurrentUpdateInterval);
    }
  }
  else if (m_GradientMode == plParticleColorGradientMode::Speed)
  {
    plProcessingStreamIterator<plVec3> itVelocity(m_pStreamVelocity, uiNumElements, 0);

    // skip the first n particles
    itVelocity.Advance(m_uiFirstToUpdate);

    while (!itVelocity.HasReachedEnd())
    {
      // if (itLifeTime.Current().y > 0)
      {
        const float fSpeed = itVelocity.Current().GetLength();
        const float posx = fSpeed / m_fMaxSpeed; // no need to clamp the range, the color lookup will already do that

        plColor rgba;
        plUInt8 alpha;
        gradient.EvaluateColor(posx, rgba);
        gradient.EvaluateAlpha(posx, alpha);
        rgba.a = plMath::ColorByteToFloat(alpha);

        itColor.Current() = rgba * getTint();
      }

      // skip the next n items
      // this is to reduce the number of particles that need to be fully evaluated,
      // since sampling the color gradient is pretty expensive
      uiParticle += m_uiCurrentUpdateInterval;
      itVelocity.Advance(m_uiCurrentUpdateInterval);
      itColor.Advance(m_uiCurrentUpdateInterval);
    }
  }

  // the next frame updates the next slice, at the interval the effect's camera distance asks for
  m_uiCurrentUpdateInterval = GetOwnerEffect()->GetLodUpdateInterval();

  ++m_uiFirstToUpdate;
  if (m_uiFirstToUpdate >= m_uiCurrentUpdateInterval)
    m_uiFirstToUpdate = 0;
}



PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_ColorGradient);
