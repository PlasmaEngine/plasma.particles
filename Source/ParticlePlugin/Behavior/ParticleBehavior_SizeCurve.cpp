#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
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
  pBehavior->m_fBaseSize = m_fBaseSize;
  pBehavior->m_fCurveScale = m_fCurveScale;
  pBehavior->m_pFactory = this;
  pBehavior->m_pProgram = &m_Program;
  pBehavior->m_pTables = GetTables();
}

namespace
{
  /// Guards the lazy bake of the shared curve tables, which several update tasks may ask for at once.
  plMutex& GetSizeCurveTablesMutex()
  {
    static plMutex s_Mutex;
    return s_Mutex;
  }
} // namespace

plSharedPtr<plParticleSizeCurveTables> plParticleBehaviorFactory_SizeCurve::GetTables() const
{
  PL_LOCK(GetSizeCurveTablesMutex());

  if (m_pTables == nullptr || m_pTables->IsOutdated())
  {
    plSharedPtr<plParticleSizeCurveTables> pTables = PL_DEFAULT_NEW(plParticleSizeCurveTables);

    // the uniform mode only reads the first curve
    const bool bPerAxis = m_Mode == plParticleSizeMode::PerAxis;
    pTables->Bake(m_hCurve, bPerAxis ? m_hCurveY : plCurve1DResourceHandle(), bPerAxis ? m_hCurveZ : plCurve1DResourceHandle());
    m_pTables = pTables;
  }

  return m_pTables;
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

  m_pTables.Clear();

  // queued now, so baking the tables rarely waits for them when the first system starts
  const plCurve1DResourceHandle* curves[] = {&m_hCurve, &m_hCurveY, &m_hCurveZ};
  for (const plCurve1DResourceHandle* pCurve : curves)
  {
    if (pCurve->IsValid())
      plResourceManager::PreloadResource(*pCurve);
  }
}

//////////////////////////////////////////////////////////////////////////

void plParticleSizeCurveTables::Bake(const plCurve1DResourceHandle& hCurveX, const plCurve1DResourceHandle& hCurveY, const plCurve1DResourceHandle& hCurveZ)
{
  const plCurve1DResourceHandle* curves[3] = {&hCurveX, &hCurveY, &hCurveZ};

  for (plUInt32 uiAxis = 0; uiAxis < 3; ++uiAxis)
  {
    m_hCurves[uiAxis] = *curves[uiAxis];
    m_bValid[uiAxis] = false;
    m_uiChangeCounters[uiAxis] = 0;

    if (!m_hCurves[uiAxis].IsValid())
      continue;

    plResourceLock<plCurve1DResource> pCurve(m_hCurves[uiAxis], plResourceAcquireMode::BlockTillLoaded);

    // the counter of the resource itself, not of a fallback that stands in for a missing one
    m_uiChangeCounters[uiAxis] = plResourceLock<plCurve1DResource>(m_hCurves[uiAxis], plResourceAcquireMode::PointerOnly)->GetCurrentResourceChangeCounter();

    if (pCurve.GetAcquireResult() == plResourceAcquireResult::MissingFallback || pCurve->GetDescriptor().m_Curves.IsEmpty())
      continue;

    plParticleProgramTables::BakeCurve(pCurve->GetDescriptor().m_Curves[0], true, plArrayPtr<float>(m_Values[uiAxis], s_uiTableSize));
    m_bValid[uiAxis] = true;
  }
}

bool plParticleSizeCurveTables::IsOutdated() const
{
  for (plUInt32 uiAxis = 0; uiAxis < 3; ++uiAxis)
  {
    if (!m_hCurves[uiAxis].IsValid())
      continue;

    plResourceLock<plCurve1DResource> pCurve(m_hCurves[uiAxis], plResourceAcquireMode::PointerOnly);
    if (pCurve->GetCurrentResourceChangeCounter() != m_uiChangeCounters[uiAxis])
      return true;
  }

  return false;
}

float plParticleSizeCurveTables::Sample(plUInt32 uiAxis, float t) const
{
  if (!m_bValid[uiAxis])
    return 0.0f;

  const float fPos = plMath::Saturate(t) * (s_uiTableSize - 1);
  const plUInt32 i0 = plMath::Min((plUInt32)fPos, s_uiTableSize - 2);
  return plMath::Lerp(m_Values[uiAxis][i0], m_Values[uiAxis][i0 + 1], fPos - (float)i0);
}

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
  // every particle while the effect is seen; an invisible one only keeps a slice of them current per step
  m_uiCurrentUpdateInterval = GetOwnerEffect()->IsVisible() ? 1 : 32;

  if (m_pFactory == nullptr)
    return;

  // a reloaded curve is baked again, once for every system that shares it
  if (m_pTables == nullptr || m_pTables->IsOutdated())
    m_pTables = m_pFactory->GetTables();

  const plParticleSizeCurveTables& tables = *m_pTables;
  if (!tables.HasCurve(0) && !tables.HasCurve(1) && !tables.HasCurve(2))
    return;

  PL_PROFILE_SCOPE("PFX: Size Curve");

  const bool bPerAxis = m_pStreamSizeScale != nullptr;
  const plVec2* pLifeTime = m_pStreamLifeTime->GetData<plVec2>();
  plFloat16* pSize = bPerAxis ? nullptr : m_pStreamSize->GetWritableData<plFloat16>();
  plFloat16Vec3* pSizeScale = bPerAxis ? m_pStreamSizeScale->GetWritableData<plFloat16Vec3>() : nullptr;

  // a per-particle wire gives every particle its own scale
  plUInt32 uiScaleComponents = 0;
  const float* pScale = m_Runner.RunProperty(GetOwnerSystem(), uiNumElements, m_TimeDiff, "CurveScale", uiScaleComponents);

  const plUInt64 uiFirst = m_uiFirstToUpdate % m_uiCurrentUpdateInterval;
  m_uiFirstToUpdate = (plUInt8)((uiFirst + 1) % m_uiCurrentUpdateInterval);

  for (plUInt64 uiParticle = uiFirst; uiParticle < uiNumElements; uiParticle += m_uiCurrentUpdateInterval)
  {
    const float fLifeTimeFraction = 1.0f - (pLifeTime[uiParticle].x * pLifeTime[uiParticle].y);
    const float fScale = pScale != nullptr ? plMath::Max(pScale[uiParticle * uiScaleComponents], 0.0f) : m_fCurveScale;

    if (bPerAxis)
    {
      const plVec3 vCurve(tables.Sample(0, fLifeTimeFraction), tables.Sample(1, fLifeTimeFraction), tables.Sample(2, fLifeTimeFraction));
      pSizeScale[uiParticle] = plVec3(m_fBaseSize) + vCurve * fScale;
    }
    else
    {
      pSize[uiParticle] = m_fBaseSize + tables.Sample(0, fLifeTimeFraction) * fScale;
    }
  }
}



PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_SizeCurve);
