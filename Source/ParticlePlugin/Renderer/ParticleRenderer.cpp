#include <ParticlePlugin/ParticlePluginPCH.h>

#include <ParticlePlugin/Renderer/ParticleDistortionPass.h>
#include <ParticlePlugin/Renderer/ParticleRenderer.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/BufferPool.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleRenderer, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleRenderer::TempSystemCB::TempSystemCB(plRenderContext* pRenderContext)
{
  // Create/Delete per batch is cheap: the storage comes from plRenderContext's size-keyed
  // free list (s_FreeConstantBufferStorage), so no GPU resource is created per call.
  m_pRenderContext = pRenderContext;
  m_hConstantBuffer = plRenderContext::CreateConstantBufferStorage(m_pConstants);
  m_hLookConstantBuffer = plRenderContext::CreateConstantBufferStorage(m_pLookConstants);

  // Must bind into the caller's (per-slot) render context, not the default instance: under multithreaded command
  // recording the particle draw records into pRenderContext, so binding the system constants to the default instance
  // leaves them unbound on the recording context and the particles render with no transform (invisible).
  plBindGroupBuilder& bindGroupMaterial = pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
  bindGroupMaterial.BindBuffer("plParticleSystemConstants", m_hConstantBuffer);
  bindGroupMaterial.BindBuffer("plParticleLookConstants", m_hLookConstantBuffer);

  // defaults for a renderer that never calls SetLookData: today's fades, no emissive
  SetLookData(plParticleLookData());
}

plParticleRenderer::TempSystemCB::~TempSystemCB()
{
  plRenderContext::DeleteConstantBufferStorage(m_hConstantBuffer);
  plRenderContext::DeleteConstantBufferStorage(m_hLookConstantBuffer);
}

void plParticleRenderer::TempSystemCB::SetLookData(const plParticleLookData& look)
{
  const bool bEmissive = look.m_hEmissiveMap.IsValid();

  plParticleLookConstants& cb = m_pLookConstants->GetDataForWriting();
  cb.EmissiveColor = bEmissive ? look.m_Emissive : plColor::Black;
  cb.SoftEdgeDistance = look.m_fSoftEdgeDistance;
  cb.CameraFadeDistance = look.m_fCameraFadeDistance;
  cb.LookFlags = (bEmissive ? PARTICLE_LOOK_FLAG_EMISSIVE_MAP : 0u) | ((bEmissive && look.m_bPerParticleEmissive) ? PARTICLE_LOOK_FLAG_PER_PARTICLE_EMISSIVE : 0u) |
                 (look.m_bDistortionTexture ? PARTICLE_LOOK_FLAG_DISTORTION_TEXTURE : 0u) | (look.m_bRoundMask ? PARTICLE_LOOK_FLAG_ROUND_MASK : 0u) |
                 (look.m_bOrmMap ? PARTICLE_LOOK_FLAG_ORM_MAP : 0u);
  cb.AlphaCutoff = look.m_fAlphaCutoff;
  cb.SurfaceRoughness = look.m_fSurfaceRoughness;
  cb.SurfaceMetallic = look.m_fSurfaceMetallic;
  cb.SurfaceSpecular = look.m_fSurfaceSpecular;
  cb.LookPadding0 = 0.0f;

  // the distortion strength lives in the system constants, which every particle shader reads as well
  m_pConstants->GetDataForWriting().DistortionStrength = look.m_fDistortionStrength;

  // the slot is declared by every default particle shader, so it is always bound; black samples to nothing
  plBindGroupBuilder& bindGroupDraw = m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
  bindGroupDraw.BindTexture("ParticleEmissiveMap", bEmissive ? look.m_hEmissiveMap : plTexture2DResourceHandle());
}

void plParticleRenderer::TempSystemCB::SetGenericData(const plTransform& objectTransform, plTime effectLifeTime, plUInt8 uiNumVariationsX, plUInt8 uiNumVariationsY, plUInt8 uiNumFlipbookAnimsX, plUInt8 uiNumFlipbookAnimsY, float fNormalCurvature, float fLightDirectionality, float fSixWayAbsorption, float fNormalMapStrength)
{
  plParticleSystemConstants& cb = m_pConstants->GetDataForWriting();
  cb.ObjectToWorldMatrix = objectTransform.GetAsMat4();
  cb.TextureAtlasVariationFramesX = uiNumVariationsX;
  cb.TextureAtlasVariationFramesY = uiNumVariationsY;
  cb.TextureAtlasFlipbookFramesX = uiNumFlipbookAnimsX;
  cb.TextureAtlasFlipbookFramesY = uiNumFlipbookAnimsY;
  cb.TotalEffectLifeTime = effectLifeTime.AsFloatInSeconds();
  cb.NormalCurvature = fNormalCurvature;
  cb.LightDirectionality = fLightDirectionality;
  cb.SixWayAbsorption = fSixWayAbsorption;
  cb.NormalMapStrength = fNormalMapStrength;
}


void plParticleRenderer::TempSystemCB::SetTrailData(float fSnapshotFraction, plInt32 iNumUsedTrailPoints)
{
  plParticleSystemConstants& cb = m_pConstants->GetDataForWriting();
  cb.SnapshotFraction = fSnapshotFraction;
  cb.NumUsedTrailPoints = iNumUsedTrailPoints;
}

plRenderData::Category plParticleGetRenderDataCategory(plParticleTypeRenderMode::Enum renderMode)
{
  if (renderMode == plParticleTypeRenderMode::Distortion)
    return plParticleRenderDataCategories::Distortion;

  // forward-lit: a deferred pipeline shades it after its lighting, the depth passes draw it in every view
  if (plParticleIsOpaqueRenderMode(renderMode))
    return plDefaultRenderDataCategories::LitForwardOpaqueDynamic;

  return plDefaultRenderDataCategories::LitTransparent;
}

void plParticleAddRenderData(plMsgExtractRenderData& ref_msg, plRenderData* pRenderData, plParticleTypeRenderMode::Enum renderMode)
{
  ref_msg.AddRenderData(pRenderData, plParticleGetRenderDataCategory(renderMode), plRenderData::Caching::Never);

  if (renderMode == plParticleTypeRenderMode::Distortion)
    ref_msg.AddContentFlags(PL_PARTICLE_CONTENT_FLAG_DISTORTION);
}

bool plParticleSkipView(const plMsgExtractRenderData& msg, bool bCastsShadows)
{
  return !bCastsShadows && msg.m_pView->GetCameraUsageHint() == plCameraUsageHint::Shadow;
}

plParticleRenderer::plParticleRenderer() = default;
plParticleRenderer::~plParticleRenderer() = default;

void plParticleRenderer::GetSupportedRenderDataCategories(plHybridArray<plRenderData::Category, 8>& ref_categories) const
{
  ref_categories.PushBack(plDefaultRenderDataCategories::LitTransparent);
  ref_categories.PushBack(plDefaultRenderDataCategories::LitForwardOpaqueDynamic);
  ref_categories.PushBack(plParticleRenderDataCategories::Distortion);
}

void plParticleRenderer::CreateParticleDataBuffer(plGALBufferPool& inout_Buffer, plUInt32 uiDataTypeSize, plUInt32 uiNumParticlesPerBatch)
{
  if (!inout_Buffer.IsInitialized())
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = uiDataTypeSize;
    desc.m_uiTotalSize = uiNumParticlesPerBatch * desc.m_uiStructSize;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::Transient;
    desc.m_ResourceAccess.m_bImmutable = false;

    inout_Buffer.Initialize(desc, "ParticleRenderer - StructuredBuffer");
  }
}


void plParticleRenderer::DestroyParticleDataBuffer(plGALBufferPool& inout_Buffer)
{
  if (inout_Buffer.IsInitialized())
  {
    inout_Buffer.Deinitialize();
  }
}

void plParticleRenderer::BindParticleShader(plRenderContext* pRenderContext, const char* szShader) const
{
  if (!m_hShader.IsValid())
  {
    // m_hShader = plResourceManager::LoadResource<plShaderResource>(szShader);
  }

  pRenderContext->BindShader(m_hShader);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Renderer_ParticleRenderer);
