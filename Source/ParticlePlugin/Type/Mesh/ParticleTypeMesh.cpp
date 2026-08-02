#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/World/World.h>
#include <Foundation/Math/Color16f.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Type/Mesh/ParticleTypeMesh.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Textures/Texture2DResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTypeMeshFactory, 1, plRTTIDefaultAllocator<plParticleTypeMeshFactory>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Mesh", m_sMesh)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Mesh_Static")),
    PL_MEMBER_PROPERTY("Material", m_sMaterial)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material")),
    PL_MEMBER_PROPERTY("TintColorParam", m_sTintColorParameter),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTypeMesh, 1, plRTTIDefaultAllocator<plParticleTypeMesh>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleTypeMeshFactory::GetTypeType() const
{
  return plGetStaticRTTI<plParticleTypeMesh>();
}


void plParticleTypeMeshFactory::CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const
{
  plParticleTypeMesh* pType = static_cast<plParticleTypeMesh*>(pObject);

  pType->m_hMesh.Invalidate();
  pType->m_hMaterial.Invalidate();
  pType->m_sTintColorParameter = plTempHashedString(m_sTintColorParameter.GetData());

  if (!m_sMesh.IsEmpty())
    pType->m_hMesh = plResourceManager::LoadResource<plMeshResource>(m_sMesh);

  if (!m_sMaterial.IsEmpty())
    pType->m_hMaterial = plResourceManager::LoadResource<plMaterialResource>(m_sMaterial);

  pType->m_pRenderDataManager = (plRenderDataManager*)pType->GetOwnerSystem()->GetOwnerWorldModule()->GetCachedWorldModule(plGetStaticRTTI<plRenderDataManager>());
}

enum class TypeMeshVersion
{
  Version_0 = 0,
  Version_1,
  Version_2, // added material

  // insert new version numbers above
  Version_Count,
  Version_Current = Version_Count - 1
};

void plParticleTypeMeshFactory::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = (int)TypeMeshVersion::Version_Current;
  inout_stream << uiVersion;

  inout_stream << m_sMesh;
  inout_stream << m_sTintColorParameter;

  // Version 2
  inout_stream << m_sMaterial;
}

void plParticleTypeMeshFactory::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= (int)TypeMeshVersion::Version_Current, "Invalid version {0}", uiVersion);

  inout_stream >> m_sMesh;
  inout_stream >> m_sTintColorParameter;

  if (uiVersion >= 2)
  {
    inout_stream >> m_sMaterial;
  }
}

plParticleTypeMesh::plParticleTypeMesh() = default;
plParticleTypeMesh::~plParticleTypeMesh()
{
  if (m_pRenderDataManager != nullptr)
  {
    m_pRenderDataManager->DeleteInstanceData(m_InstanceDataOffset);
  }
  else
  {
    PL_ASSERT_DEBUG(m_InstanceDataOffset.IsInvalidated(), "Implementation error");
  }
}

void plParticleTypeMesh::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Size", plProcessingStream::DataType::Half, &m_pStreamSize, false);
  CreateStream("Color", plProcessingStream::DataType::Half4, &m_pStreamColor, false);
  CreateStream("RotationSpeed", plProcessingStream::DataType::Half, &m_pStreamRotationSpeed, false);
  CreateStream("RotationOffset", plProcessingStream::DataType::Half, &m_pStreamRotationOffset, false);
  CreateStream("Axis", plProcessingStream::DataType::Float3, &m_pStreamAxis, true);
}

void plParticleTypeMesh::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  plVec3* pAxis = m_pStreamAxis->GetWritableData<plVec3>();
  plRandom& rng = GetRNG();

  for (plUInt32 i = 0; i < uiNumElements; ++i)
  {
    const plUInt64 uiElementIdx = uiStartIndex + i;

    pAxis[uiElementIdx] = plVec3::MakeRandomDirection(rng);
  }
}

bool plParticleTypeMesh::QueryMeshAndMaterialInfo() const
{
  if (!m_hMesh.IsValid())
  {
    m_bRenderDataCached = true;
    m_hMaterial.Invalidate();
    return true;
  }

  plResourceLock<plMeshResource> pMesh(m_hMesh, plResourceAcquireMode::AllowLoadingFallback);
  if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
    return false;

  if (!m_hMaterial.IsValid())
  {
    m_hMaterial = pMesh->GetMaterials()[0];

    if (!m_hMaterial.IsValid())
    {
      m_bRenderDataCached = true;
      return true;
    }
  }

  plResourceLock<plMaterialResource> pMaterial(m_hMaterial, plResourceAcquireMode::AllowLoadingFallback);
  if (pMaterial.GetAcquireResult() != plResourceAcquireResult::Final)
    return false;

  m_RenderCategory = pMaterial->GetRenderDataCategory();

  m_bRenderDataCached = true;
  return true;
}

void plParticleTypeMesh::RequestRequiredWorldModulesForCache(plParticleWorldModule* pParticleModule)
{
  pParticleModule->CacheWorldModule<plRenderDataManager>();
}

void plParticleTypeMesh::ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const
{
  if (!m_bRenderDataCached)
  {
    // check if we now know how to render this thing
    if (!QueryMeshAndMaterialInfo())
      return;
  }

  if (!m_hMaterial.IsValid())
    return;

  if (m_RenderCategory.m_uiValue == 0xFFFF)
  {
    m_bRenderDataCached = false;
    return;
  }

  const plUInt32 numParticles = (plUInt32)GetOwnerSystem()->GetNumActiveParticles();

  if (numParticles == 0)
    return;

  PL_PROFILE_SCOPE("PFX: Mesh");

  const plTime tCur = GetOwnerEffect()->GetTotalEffectLifeTime();
  const plColor tintColor = GetOwnerEffect()->GetColorParameter(m_sTintColorParameter, plColor::White);

  const plVec4* pPosition = m_pStreamPosition->GetData<plVec4>();
  const plFloat16* pSize = m_pStreamSize->GetData<plFloat16>();
  const plColorLinear16f* pColor = m_pStreamColor->GetData<plColorLinear16f>();
  const plFloat16* pRotationSpeed = m_pStreamRotationSpeed->GetData<plFloat16>();
  const plFloat16* pRotationOffset = m_pStreamRotationOffset->GetData<plFloat16>();
  const plVec3* pAxis = m_pStreamAxis->GetData<plVec3>();

  const bool bIsOpaque = m_RenderCategory == plDefaultRenderDataCategories::LitOpaque ||
                         m_RenderCategory == plDefaultRenderDataCategories::LitMasked ||
                         m_RenderCategory == plDefaultRenderDataCategories::SimpleOpaque;

  {
    const bool bDynamic = true;
    plGALDynamicBufferHandle hInstanceDataBuffer;
    const plUInt32 uiMaxNumParticles = (plUInt32)GetOwnerSystem()->GetMaxParticles();
    auto instanceData = ref_msg.m_pRenderDataManager->GetOrCreateInstanceData(nullptr, bDynamic, hInstanceDataBuffer, m_InstanceDataOffset, uiMaxNumParticles);

    for (plUInt32 p = 0; p < numParticles; ++p)
    {
      const plUInt32 idx = p;

      plTransform trans;
      trans.m_qRotation = plQuat::MakeFromAxisAndAngle(pAxis[p], plAngle::MakeFromRadian((float)(tCur.GetSeconds() * pRotationSpeed[idx]) + pRotationOffset[idx]));
      trans.m_vPosition = pPosition[idx].GetAsVec3();
      trans.m_vScale.Set(pSize[idx]);

      plRenderDataManager::FillPerInstanceData(instanceData[p], nullptr, trans, plInvalidIndex, pColor[idx].ToLinearFloat() * tintColor);

      // If not rendered as opaque we need one render data per particle to allow for proper sorting
      if (!bIsOpaque)
      {
        plInstanceDataOffset perParticleOffset = m_InstanceDataOffset;
        perParticleOffset.m_uiOffset += p;

        plMeshRenderData* pRenderData = ref_msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(nullptr);
        pRenderData->m_vGlobalPosition = trans.m_vPosition;
        pRenderData->Fill(perParticleOffset, hInstanceDataBuffer, m_hMaterial, m_hMesh);

        ref_msg.AddRenderData(pRenderData, m_RenderCategory, plRenderData::Caching::Never);
      }
    }

    // If rendered as opaque we can pack everything into one render data
    if (bIsOpaque)
    {
      plMeshRenderData* pRenderData = ref_msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(nullptr);
      pRenderData->m_vGlobalPosition = GetOwnerSystem()->GetTransform().m_vPosition;
      pRenderData->Fill(m_InstanceDataOffset, hInstanceDataBuffer, m_hMaterial, m_hMesh, 0, 0, numParticles);

      ref_msg.AddRenderData(pRenderData, m_RenderCategory, plRenderData::Caching::Never);
    }
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_Mesh_ParticleTypeMesh);

