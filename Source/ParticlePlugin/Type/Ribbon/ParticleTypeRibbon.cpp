#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Math/Color16f.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Type/Ribbon/ParticleTypeRibbon.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Textures/Texture2DResource.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleRibbonUvMode, 1)
  PL_ENUM_CONSTANTS(plParticleRibbonUvMode::Stretch, plParticleRibbonUvMode::Tiled)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTypeRibbonFactory, 1, plRTTIDefaultAllocator<plParticleTypeRibbonFactory>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("RenderMode", plParticleTypeRenderMode, m_RenderMode),
    PL_ENUM_MEMBER_PROPERTY("LightingMode", plParticleLightingMode, m_LightingMode),
    PL_MEMBER_PROPERTY("NormalCurvature", m_fNormalCurvature)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0, 1)),
    PL_MEMBER_PROPERTY("LightDirectionality", m_fLightDirectionality)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0, 1)),
    PL_ENUM_MEMBER_PROPERTY("UvMode", plParticleRibbonUvMode, m_UvMode),
    PL_MEMBER_PROPERTY("TileLength", m_fTileLength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.01f, 100.0f)),
    PL_MEMBER_PROPERTY("UseCustomMaterial", m_bUseCustomMaterial),
    PL_MEMBER_PROPERTY("CustomMaterial", m_sCustomMaterial)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material", "QuadParticle")),
    PL_MEMBER_PROPERTY("Texture", m_sTexture)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D"), new plDefaultValueAttribute(plStringView("{ e00262e8-58f5-42f5-880d-569257047201 }"))), // wrap in plStringView to prevent a memory leak report
    PL_MEMBER_PROPERTY("TintColorParam", m_sTintColorParameter),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTypeRibbon, 1, plRTTIDefaultAllocator<plParticleTypeRibbon>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleTypeRibbonFactory::GetTypeType() const
{
  return plGetStaticRTTI<plParticleTypeRibbon>();
}

void plParticleTypeRibbonFactory::CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const
{
  plParticleTypeRibbon* pType = static_cast<plParticleTypeRibbon*>(pObject);

  pType->m_RenderMode = m_RenderMode;
  pType->m_UvMode = m_UvMode;
  pType->m_fTileLength = plMath::Max(m_fTileLength, 0.01f);
  pType->m_hTexture.Invalidate();
  pType->m_sTintColorParameter = plTempHashedString(m_sTintColorParameter.GetData());
  pType->m_LightingMode = m_LightingMode;
  pType->m_fNormalCurvature = m_fNormalCurvature;
  pType->m_fLightDirectionality = m_fLightDirectionality;
  pType->m_hCustomMaterial.Invalidate();

  if (m_bUseCustomMaterial)
  {
    pType->m_hCustomMaterial = plResourceManager::LoadResource<plMaterialResource>(m_sCustomMaterial);
  }

  if (!m_sTexture.IsEmpty())
  {
    pType->m_hTexture = plResourceManager::LoadResource<plTexture2DResource>(m_sTexture);
  }
}

enum class TypeRibbonVersion
{
  Version_0 = 0,
  Version_1,

  // insert new version numbers above
  Version_Count,
  Version_Current = Version_Count - 1
};

void plParticleTypeRibbonFactory::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = (int)TypeRibbonVersion::Version_Current;
  inout_stream << uiVersion;

  inout_stream << m_RenderMode;
  inout_stream << m_UvMode;
  inout_stream << m_fTileLength;
  inout_stream << m_sTexture;
  inout_stream << m_sTintColorParameter;
  inout_stream << m_LightingMode;
  inout_stream << m_fNormalCurvature;
  inout_stream << m_fLightDirectionality;
  inout_stream << m_bUseCustomMaterial;
  inout_stream << m_sCustomMaterial;
}

void plParticleTypeRibbonFactory::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= (int)TypeRibbonVersion::Version_Current, "Invalid version {0}", uiVersion);

  inout_stream >> m_RenderMode;
  inout_stream >> m_UvMode;
  inout_stream >> m_fTileLength;
  inout_stream >> m_sTexture;
  inout_stream >> m_sTintColorParameter;
  inout_stream >> m_LightingMode;
  inout_stream >> m_fNormalCurvature;
  inout_stream >> m_fLightDirectionality;
  inout_stream >> m_bUseCustomMaterial;
  inout_stream >> m_sCustomMaterial;
}

//////////////////////////////////////////////////////////////////////////

plParticleTypeRibbon::plParticleTypeRibbon() = default;
plParticleTypeRibbon::~plParticleTypeRibbon() = default;

void plParticleTypeRibbon::CreateRequiredStreams()
{
  CreateStream("LifeTime", plProcessingStream::DataType::Half2, &m_pStreamLifeTime, false);
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Size", plProcessingStream::DataType::Half, &m_pStreamSize, false);
  CreateStream("Color", plProcessingStream::DataType::Half4, &m_pStreamColor, false);

  // spawn-order index; survives the swap-erase compaction of the streams and lets
  // extraction restore the spawn order to connect the band correctly
  CreateStream("RibbonOrder", plProcessingStream::DataType::Int, &m_pStreamRibbonOrder, true);
}

void plParticleTypeRibbon::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Init Ribbon Order");

  plUInt32* pOrder = m_pStreamRibbonOrder->GetWritableData<plUInt32>();

  for (plUInt64 i = 0; i < uiNumElements; ++i)
  {
    pOrder[uiStartIndex + i] = m_uiNextOrderIndex++;
  }
}

void plParticleTypeRibbon::ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const
{
  PL_PROFILE_SCOPE("PFX: Ribbon");

  if (!m_hTexture.IsValid() && !m_hCustomMaterial.IsValid())
    return;

  const plUInt32 numParticles = (plUInt32)GetOwnerSystem()->GetNumActiveParticles();
  if (numParticles < 2)
    return;

  // the band and its UVs are view-independent, so build the point list only once per frame,
  // no matter how many views or shared instances extract this effect
  if (m_uiLastExtractedFrame != plRenderWorld::GetFrameCounter())
  {
    m_uiLastExtractedFrame = plRenderWorld::GetFrameCounter();

    const plVec4* pPosition = m_pStreamPosition->GetData<plVec4>();
    const plFloat16* pSize = m_pStreamSize->GetData<plFloat16>();
    const plColorLinear16f* pColor = m_pStreamColor->GetData<plColorLinear16f>();
    const plFloat16Vec2* pLifeTime = m_pStreamLifeTime->GetData<plFloat16Vec2>();
    const plUInt32* pOrder = m_pStreamRibbonOrder->GetData<plUInt32>();

    // restore spawn order (stream compaction shuffles the particle order)
    struct SortEntry
    {
      PL_DECLARE_POD_TYPE();

      plUInt32 m_uiOrder;
      plUInt32 m_uiIndex;
    };

    struct SortEntryComparer
    {
      PL_ALWAYS_INLINE bool Less(const SortEntry& a, const SortEntry& b) const { return a.m_uiOrder < b.m_uiOrder; }
      PL_ALWAYS_INLINE bool Equal(const SortEntry& a, const SortEntry& b) const { return a.m_uiOrder == b.m_uiOrder; }
    };

    plHybridArray<SortEntry, 64> sorted(plFrameAllocator::GetCurrentAllocator());
    sorted.SetCountUninitialized(numParticles);

    for (plUInt32 p = 0; p < numParticles; ++p)
    {
      sorted[p].m_uiOrder = pOrder[p];
      sorted[p].m_uiIndex = p;
    }

    sorted.Sort(SortEntryComparer());

    const plColor tintColor = GetOwnerEffect()->GetColorParameter(m_sTintColorParameter, plColor::White);

    m_PointData = PL_NEW_ARRAY(plFrameAllocator::GetCurrentAllocator(), plRibbonPointShaderData, numParticles);

    // accumulate the length along the band in full float precision, U is only converted to half at the end
    plHybridArray<float, 64> lengths(plFrameAllocator::GetCurrentAllocator());
    lengths.SetCountUninitialized(numParticles);

    float fTotalLength = 0.0f;

    for (plUInt32 p = 0; p < numParticles; ++p)
    {
      const plUInt32 uiSrcIdx = sorted[p].m_uiIndex;

      if (p > 0)
      {
        fTotalLength += (pPosition[uiSrcIdx].GetAsVec3() - pPosition[sorted[p - 1].m_uiIndex].GetAsVec3()).GetLength();
      }

      lengths[p] = fTotalLength;

      auto& point = m_PointData[p];
      point.Position = pPosition[uiSrcIdx].GetAsVec3();
      point.Width = pSize[uiSrcIdx];
      point.Color = pColor[uiSrcIdx].ToLinearFloat() * tintColor;
      point.Life = pLifeTime[uiSrcIdx].x * pLifeTime[uiSrcIdx].y;
      point.Dummy = 0;
    }

    const float fUvScale = (m_UvMode == plParticleRibbonUvMode::Stretch)
                             ? (fTotalLength > 0.0001f ? 1.0f / fTotalLength : 0.0f)
                             : 1.0f / m_fTileLength;

    for (plUInt32 p = 0; p < numParticles; ++p)
    {
      m_PointData[p].U = lengths[p] * fUvScale;
    }
  }

  auto pRenderData = ref_msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plParticleRibbonRenderData>(nullptr);

  if (m_hCustomMaterial.IsValid())
  {
    pRenderData->m_uiSortingKey = ComputeSortingKey(m_RenderMode, m_hCustomMaterial.GetResourceIDHash(), 0);
  }
  else
  {
    pRenderData->m_uiSortingKey = ComputeSortingKey(m_RenderMode, m_hTexture.GetResourceIDHash(), 0);
  }

  pRenderData->m_vGlobalPosition = instanceTransform.m_vPosition;
  pRenderData->m_GlobalTransform = GetOwnerEffect()->NeedsToApplyTransform() ? instanceTransform : plTransform::MakeIdentity();
  pRenderData->m_bApplyObjectTransform = true;
  pRenderData->m_TotalEffectLifeTime = GetOwnerEffect()->GetTotalEffectLifeTime();
  pRenderData->m_RenderMode = m_RenderMode;
  pRenderData->m_LightingMode = m_LightingMode;
  pRenderData->m_fNormalCurvature = m_fNormalCurvature;
  pRenderData->m_fLightDirectionality = m_fLightDirectionality;
  pRenderData->m_hTexture = m_hTexture;
  pRenderData->m_hCustomMaterial = m_hCustomMaterial;
  pRenderData->m_Points = m_PointData;

  ref_msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::LitTransparent, plRenderData::Caching::Never);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_Ribbon_ParticleTypeRibbon);
