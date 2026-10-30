#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SizeCurve.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_SizeCurve, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_SizeCurve>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Mode", plParticleSizeMode, m_Mode),
    PL_RESOURCE_MEMBER_PROPERTY("SizeCurve", m_hCurve)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Data_Curve")),
    PL_RESOURCE_MEMBER_PROPERTY("SizeCurveY", m_hCurveY)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Data_Curve")),
    PL_RESOURCE_MEMBER_PROPERTY("SizeCurveZ", m_hCurveZ)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Data_Curve")),
    PL_MEMBER_PROPERTY("BaseSize", m_fBaseSize)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("CurveScale", m_fCurveScale)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, plVariant()), new plParticlePerParticleAttribute()),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_SizeCurve, 1, plRTTIDefaultAllocator<plParticleBehavior_SizeCurve>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_SizeCurve::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_SizeCurve>();
}

void plParticleBehaviorFactory_SizeCurve::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_SizeCurve* pBehavior = static_cast<plParticleBehavior_SizeCurve*>(pObject);

  pBehavior->m_Mode = m_Mode;
  pBehavior->m_hCurve = m_hCurve;
  pBehavior->m_hCurveY = m_hCurveY;
  pBehavior->m_hCurveZ = m_hCurveZ;
  pBehavior->m_fBaseSize = m_fBaseSize;
  pBehavior->m_fCurveScale = m_fCurveScale;
  pBehavior->m_pProgram = &m_Program;
}

void plParticleBehaviorFactory_SizeCurve::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 3;
  inout_stream << uiVersion;

  inout_stream << m_hCurve;
  inout_stream << m_fBaseSize;
  inout_stream << m_fCurveScale;
  m_Program.Save(inout_stream);

  // version 3
  inout_stream << m_Mode;
  inout_stream << m_hCurveY;
  inout_stream << m_hCurveZ;
}

void plParticleBehaviorFactory_SizeCurve::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 3, "Invalid version {0}", uiVersion);

  inout_stream >> m_hCurve;
  inout_stream >> m_fBaseSize;
  inout_stream >> m_fCurveScale;

  if (uiVersion >= 2)
    m_Program.Load(inout_stream);

  if (uiVersion >= 3)
  {
    inout_stream >> m_Mode;
    inout_stream >> m_hCurveY;
    inout_stream >> m_hCurveZ;
  }
}

namespace
{
  /// Holds one curve acquired for the duration of a Process call; an invalid or empty curve samples 0.
  class plSizeCurveSampler
  {
  public:
    explicit plSizeCurveSampler(const plCurve1DResourceHandle& hCurve)
    {
      if (!hCurve.IsValid())
        return;

      plResourceAcquireResult result = plResourceAcquireResult::None;
      m_pResource = plResourceManager::BeginAcquireResource(hCurve, plResourceAcquireMode::BlockTillLoaded, plCurve1DResourceHandle(), &result);

      if (result != plResourceAcquireResult::MissingFallback && !m_pResource->GetDescriptor().m_Curves.IsEmpty())
        m_pCurve = &m_pResource->GetDescriptor().m_Curves[0];
    }

    ~plSizeCurveSampler()
    {
      if (m_pResource != nullptr)
        plResourceManager::EndAcquireResource(m_pResource);
    }

    bool IsValid() const { return m_pCurve != nullptr; }

    float Sample(float fLifeTimeFraction) const
    {
      if (m_pCurve == nullptr)
        return 0.0f;

      return (float)m_pCurve->NormalizeValue(m_pCurve->Evaluate(m_pCurve->ConvertNormalizedPos(fLifeTimeFraction)));
    }

  private:
    plCurve1DResource* m_pResource = nullptr;
    const plCurve1D* m_pCurve = nullptr;
  };
} // namespace

void plParticleBehavior_SizeCurve::CreateRequiredStreams()
{
  CreateStream("LifeTime", plProcessingStream::DataType::Float2, &m_pStreamLifeTime, false);

  m_pStreamSize = nullptr;
  m_pStreamSizeScale = nullptr;

  if (m_Mode == plParticleSizeMode::PerAxis)
    CreateStream("SizeScale", plProcessingStream::DataType::Half3, &m_pStreamSizeScale, false);
  else
    CreateStream("Size", plProcessingStream::DataType::Half, &m_pStreamSize, false);

  m_Runner.Configure(m_pProgram);
  m_Runner.CreateRequiredStreams([this](const char* szName, plProcessingStream::DataType type, plProcessingStream** ppStream)
    { CreateStream(szName, type, ppStream, false); });
}


void plParticleBehavior_SizeCurve::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  if (m_pStreamSizeScale != nullptr)
  {
    plProcessingStreamIterator<plFloat16Vec3> itScale(m_pStreamSizeScale, uiNumElements, uiStartIndex);
    while (!itScale.HasReachedEnd())
    {
      itScale.Current() = plVec3(m_fBaseSize);
      itScale.Advance();
    }

    return;
  }

  plProcessingStreamIterator<plFloat16> itSize(m_pStreamSize, uiNumElements, uiStartIndex);
  while (!itSize.HasReachedEnd())
  {
    itSize.Current() = m_fBaseSize;
    itSize.Advance();
  }
}

void plParticleBehavior_SizeCurve::Process(plUInt64 uiNumElements)
{
  if (!GetOwnerEffect()->IsVisible())
  {
    // reduce the update interval when the effect is not visible
    m_uiCurrentUpdateInterval = 32;
  }
  else
  {
    m_uiCurrentUpdateInterval = 2;
  }

  const bool bPerAxis = m_pStreamSizeScale != nullptr;

  if (!m_hCurve.IsValid() && (!bPerAxis || (!m_hCurveY.IsValid() && !m_hCurveZ.IsValid())))
    return;

  PL_PROFILE_SCOPE("PFX: Size Curve");

  const plSizeCurveSampler curveX(m_hCurve);
  const plSizeCurveSampler curveY(bPerAxis ? m_hCurveY : plCurve1DResourceHandle());
  const plSizeCurveSampler curveZ(bPerAxis ? m_hCurveZ : plCurve1DResourceHandle());

  if (!curveX.IsValid() && !curveY.IsValid() && !curveZ.IsValid())
    return;

  const plVec2* pLifeTime = m_pStreamLifeTime->GetData<plVec2>();
  plFloat16* pSize = bPerAxis ? nullptr : m_pStreamSize->GetWritableData<plFloat16>();
  plFloat16Vec3* pSizeScale = bPerAxis ? m_pStreamSizeScale->GetWritableData<plFloat16Vec3>() : nullptr;

  // a per-particle wire gives every particle its own scale
  plUInt32 uiScaleComponents = 0;
  const float* pScale = m_Runner.RunProperty(GetOwnerSystem(), uiNumElements, m_TimeDiff, "CurveScale", uiScaleComponents);

  // only every n-th particle is evaluated per step, since sampling the curve is expensive
  const plUInt64 uiFirst = m_uiFirstToUpdate;
  ++m_uiFirstToUpdate;
  if (m_uiFirstToUpdate >= m_uiCurrentUpdateInterval)
    m_uiFirstToUpdate = 0;

  for (plUInt64 uiParticle = uiFirst; uiParticle < uiNumElements; uiParticle += m_uiCurrentUpdateInterval)
  {
    const float fLifeTimeFraction = 1.0f - (pLifeTime[uiParticle].x * pLifeTime[uiParticle].y);
    const float fScale = pScale != nullptr ? plMath::Max(pScale[uiParticle * uiScaleComponents], 0.0f) : m_fCurveScale;

    if (bPerAxis)
    {
      const plVec3 vCurve(curveX.Sample(fLifeTimeFraction), curveY.Sample(fLifeTimeFraction), curveZ.Sample(fLifeTimeFraction));
      pSizeScale[uiParticle] = plVec3(m_fBaseSize) + vCurve * fScale;
    }
    else
    {
      pSize[uiParticle] = m_fBaseSize + curveX.Sample(fLifeTimeFraction) * fScale;
    }
  }
}



PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_SizeCurve);
