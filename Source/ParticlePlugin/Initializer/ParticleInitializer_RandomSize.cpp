#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Curves/Curve1DResource.h>
#include <Foundation/DataProcessing/Stream/ProcessingStreamGroup.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Serialization/GraphPatch.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_RandomSize.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializerFactory_RandomSize, 2, plRTTIDefaultAllocator<plParticleInitializerFactory_RandomSize>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Mode", plParticleSizeMode, m_Mode),
    PL_MEMBER_PROPERTY("Size", m_Size)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("Aspect", m_Aspect)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.01f, 100.0f)),
    PL_MEMBER_PROPERTY("AxisScale", m_vSizeAxes)->AddAttributes(new plDefaultValueAttribute(plVec3(1.0f)), new plClampValueAttribute(plVec3(0.0f), plVariant())),
    PL_MEMBER_PROPERTY("SameRandomForAllAxes", m_bSameRandomForAllAxes),
    PL_RESOURCE_MEMBER_PROPERTY("SizeCurve", m_hCurve)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Data_Curve")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializer_RandomSize, 1, plRTTIDefaultAllocator<plParticleInitializer_RandomSize>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleInitializerFactory_RandomSize::GetInitializerType() const
{
  return plGetStaticRTTI<plParticleInitializer_RandomSize>();
}

void plParticleInitializerFactory_RandomSize::CopyInitializerProperties(plParticleInitializer* pInitializer0, bool bFirstTime) const
{
  plParticleInitializer_RandomSize* pInitializer = static_cast<plParticleInitializer_RandomSize*>(pInitializer0);

  pInitializer->m_hCurve = m_hCurve;
  pInitializer->m_Mode = m_Mode;
  pInitializer->m_Size = m_Size;
  pInitializer->m_Aspect = m_Aspect;
  pInitializer->m_vSizeAxes = m_vSizeAxes;
  pInitializer->m_bSameRandomForAllAxes = m_bSameRandomForAllAxes;
}

void plParticleInitializerFactory_RandomSize::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 4;
  inout_stream << uiVersion;

  inout_stream << m_hCurve;
  inout_stream << m_Size.m_Value;
  inout_stream << m_Size.m_fVariance;

  // version 3
  inout_stream << m_Aspect.m_Value;
  inout_stream << m_Aspect.m_fVariance;

  // version 4
  inout_stream << m_Mode;
  inout_stream << m_vSizeAxes;
  inout_stream << m_bSameRandomForAllAxes;
}

void plParticleInitializerFactory_RandomSize::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  inout_stream >> m_hCurve;
  inout_stream >> m_Size.m_Value;
  inout_stream >> m_Size.m_fVariance;

  if (uiVersion >= 3)
  {
    inout_stream >> m_Aspect.m_Value;
    inout_stream >> m_Aspect.m_fVariance;
  }

  if (uiVersion >= 4)
  {
    inout_stream >> m_Mode;
    inout_stream >> m_vSizeAxes;
    inout_stream >> m_bSameRandomForAllAxes;
  }
}


void plParticleInitializer_RandomSize::CreateRequiredStreams()
{
  CreateStream("Size", plProcessingStream::DataType::Half, &m_pStreamSize, true);

  m_pStreamSizeScale = nullptr;

  const bool bAspect = m_Aspect.m_Value != 1.0f || m_Aspect.m_fVariance > 0.0f;
  if (m_Mode == plParticleSizeMode::PerAxis || bAspect)
  {
    CreateStream("SizeScale", plProcessingStream::DataType::Half3, &m_pStreamSizeScale, true);
  }
}

void plParticleInitializer_RandomSize::ComputeCurveFactors(plUInt64 uiStartIndex, plUInt64 uiNumElements, plFloat16* pOut)
{
  auto FillOne = [&]()
  {
    for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
      pOut[i] = 1.0f;
  };

  if (!m_hCurve.IsValid())
  {
    FillOne();
    return;
  }

  plResourceLock<plCurve1DResource> pResource(m_hCurve, plResourceAcquireMode::BlockTillLoaded);
  if (pResource->GetDescriptor().m_Curves.IsEmpty())
  {
    FillOne();
    return;
  }

  const plCurve1D* pCurve = &pResource->GetDescriptor().m_Curves[0];
  plRandom& rng = GetRNG();

  double fMinX, fMaxX;
  pCurve->QueryExtents(fMinX, fMaxX);

  for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
  {
    const double f = rng.DoubleMinMax(fMinX, fMaxX);
    pOut[i] = (float)pCurve->NormalizeValue(pCurve->Evaluate(f));
  }
}

void plParticleInitializer_RandomSize::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Random Size");

  plFloat16* pSize = m_pStreamSize->GetWritableData<plFloat16>();
  plFloat16Vec3* pScale = m_pStreamSizeScale != nullptr ? m_pStreamSizeScale->GetWritableData<plFloat16Vec3>() : nullptr;

  plRandom& rng = GetRNG();

  // Size starts as the curve factor and is then multiplied by the random size
  ComputeCurveFactors(uiStartIndex, uiNumElements, pSize);

  if (m_Mode == plParticleSizeMode::PerAxis)
  {
    // Size times the X/Y/Z multipliers is the particle's size; the variance either varies Size (one random value
    // for all axes) or every multiplier on its own
    for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
    {
      if (m_bSameRandomForAllAxes)
      {
        pSize[i] = (float)pSize[i] * (float)rng.DoubleVariance(m_Size.m_Value, m_Size.m_fVariance);
        pScale[i] = m_vSizeAxes;
      }
      else
      {
        const plVec3 vRandom((float)rng.DoubleVariance(1.0, m_Size.m_fVariance), (float)rng.DoubleVariance(1.0, m_Size.m_fVariance), (float)rng.DoubleVariance(1.0, m_Size.m_fVariance));
        pSize[i] = (float)pSize[i] * m_Size.m_Value;
        pScale[i] = m_vSizeAxes.CompMul(vRandom);
      }
    }

    return;
  }

  for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
  {
    pSize[i] = (float)pSize[i] * (float)rng.DoubleVariance(m_Size.m_Value, m_Size.m_fVariance);
  }

  if (pScale != nullptr)
  {
    for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
    {
      pScale[i] = plVec3(plMath::Max((float)rng.DoubleVariance(m_Aspect.m_Value, m_Aspect.m_fVariance), 0.01f), 1.0f, 1.0f);
    }
  }
}

//////////////////////////////////////////////////////////////////////////

class plParticleInitializerFactory_RandomSize_1_2 : public plGraphPatch
{
public:
  plParticleInitializerFactory_RandomSize_1_2()
    : plGraphPatch("plParticleInitializerFactory_RandomSize", 2)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    pNode->InlineProperty("Size").IgnoreResult();
  }
};

plParticleInitializerFactory_RandomSize_1_2 g_plParticleInitializerFactory_RandomSize_1_2;

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Initializer_ParticleInitializer_RandomSize);
