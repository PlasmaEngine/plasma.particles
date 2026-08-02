#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Types/ScopeExit.h>
#include <ParticlePlugin/Type/Trail/ParticleTypeTrail.h>
#include <ParticlePlugin/Type/Trail/TrailRenderer.h>
#include <RendererCore/Pipeline/RenderDataBatch.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/Device/Device.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/ParticleSystemConstants.h>

//clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTrailRenderData, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTrailRenderer, 1, plRTTIDefaultAllocator<plParticleTrailRenderer>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

bool plParticleTrailRenderData::CanBatch(const plRenderData& other0) const
{
  const auto& other = plStaticCast<const plParticleTrailRenderData&>(other0);

  return m_RenderMode == other.m_RenderMode && m_hTexture == other.m_hTexture && m_uiMaxTrailPoints == other.m_uiMaxTrailPoints && m_hCustomMaterial == other.m_hCustomMaterial && m_hDistortionTexture == other.m_hDistortionTexture;
}

//////////////////////////////////////////////////////////////////////////

plParticleTrailRenderer::plParticleTrailRenderer()
{
  CreateParticleDataBuffer(m_BaseDataBuffer, sizeof(plBaseParticleShaderData), s_uiParticlesPerBatch);
  CreateParticleDataBuffer(m_TrailDataBuffer, sizeof(plTrailParticleShaderData), s_uiParticlesPerBatch);

  // this is kinda stupid, apparently due to stride enforcement I cannot reuse the same buffer for different sizes
  // and instead have to create one buffer with every size ...

  CreateParticleDataBuffer(m_TrailPointsDataBuffer8, sizeof(plTrailParticlePointsData8), s_uiParticlesPerBatch);
  CreateParticleDataBuffer(m_TrailPointsDataBuffer16, sizeof(plTrailParticlePointsData16), s_uiParticlesPerBatch);
  CreateParticleDataBuffer(m_TrailPointsDataBuffer32, sizeof(plTrailParticlePointsData32), s_uiParticlesPerBatch);
  CreateParticleDataBuffer(m_TrailPointsDataBuffer64, sizeof(plTrailParticlePointsData64), s_uiParticlesPerBatch);

  m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Particles/DefaultTrailParticle.plShader");
}

plParticleTrailRenderer::~plParticleTrailRenderer()
{
  DestroyParticleDataBuffer(m_BaseDataBuffer);
  DestroyParticleDataBuffer(m_TrailDataBuffer);
  DestroyParticleDataBuffer(m_TrailPointsDataBuffer8);
  DestroyParticleDataBuffer(m_TrailPointsDataBuffer16);
  DestroyParticleDataBuffer(m_TrailPointsDataBuffer32);
  DestroyParticleDataBuffer(m_TrailPointsDataBuffer64);
}

void plParticleTrailRenderer::GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const
{
  ref_types.PushBack(plGetStaticRTTI<plParticleTrailRenderData>());
}

void plParticleTrailRenderer::RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const
{
  plRenderContext* pRenderContext = renderViewContext.m_pRenderContext;
  plGALCommandEncoder* pGALCommandEncoder = pRenderContext->GetCommandEncoder();

  TempSystemCB systemConstants(pRenderContext);

  bool bBindShader = true;

  plBindGroupBuilder& bindGroupMaterial = renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);

  // now render all particle effects of type Trail
  for (auto it = batch.GetIterator<plParticleTrailRenderData>(0, batch.GetDataCount()); it.IsValid(); ++it)
  {
    const plParticleTrailRenderData* pRenderData = it;

    if (pRenderData->m_hCustomMaterial.IsValid())
    {
      pRenderContext->BindMaterial(pRenderData->m_hCustomMaterial);
    }
    else
    {
      if (bBindShader)
      {
        bBindShader = false;
        pRenderContext->BindShader(m_hShader);
      }

      bindGroupMaterial.BindTexture("ParticleTexture", pRenderData->m_hTexture);

      if (pRenderData->m_RenderMode == plParticleTypeRenderMode::Distortion)
      {
        bindGroupMaterial.BindTexture("ParticleDistortionTexture", pRenderData->m_hDistortionTexture);
      }
    }


    if (!ConfigureShader(pRenderData, renderViewContext))
      continue;

    const plUInt32 uiBucketSize = plParticleTypeTrail::ComputeTrailPointBucketSize(pRenderData->m_uiMaxTrailPoints);
    const plUInt32 uiMaxTrailSegments = uiBucketSize - 1;
    const plUInt32 uiPrimFactor = 2;
    const plUInt32 uiMaxPrimitivesToRender = s_uiParticlesPerBatch * uiMaxTrailSegments * uiPrimFactor;


    pRenderContext->BindNullMeshBuffer(plGALPrimitiveTopology::Triangles, uiMaxPrimitivesToRender);

    const plBaseParticleShaderData* pParticleBaseData = pRenderData->m_BaseParticleData.GetPtr();
    const plTrailParticleShaderData* pParticleTrailData = pRenderData->m_TrailParticleData.GetPtr();


    const plVec4* pParticlePointsData = pRenderData->m_TrailPointsShared.GetPtr();

    bindGroupMaterial.BindTexture("ParticleTexture", pRenderData->m_hTexture);

    systemConstants.SetGenericData(pRenderData->m_GlobalTransform, pRenderData->m_TotalEffectLifeTime, pRenderData->m_uiNumVariationsX, pRenderData->m_uiNumVariationsY, pRenderData->m_uiNumFlipbookAnimationsX, pRenderData->m_uiNumFlipbookAnimationsY, pRenderData->m_fNormalCurvature, pRenderData->m_fLightDirectionality);
    systemConstants.SetTrailData(pRenderData->m_fSnapshotFraction, pRenderData->m_uiMaxTrailPoints);

    plUInt32 uiNumParticles = pRenderData->m_BaseParticleData.GetCount();
    while (uiNumParticles > 0)
    {
      // Request and bind new buffers for this batch
      plGALBufferHandle hBaseDataBuffer = m_BaseDataBuffer.GetNewBuffer();
      plGALBufferHandle hTrailDataBuffer = m_TrailDataBuffer.GetNewBuffer();
      plGALBufferHandle hActiveTrailPointsDataBuffer = m_pActiveTrailPointsDataBuffer->GetNewBuffer();

      plBindGroupBuilder& bindGroupDraw = renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
      bindGroupDraw.BindBuffer("particleBaseData", hBaseDataBuffer);
      bindGroupDraw.BindBuffer("particleTrailData", hTrailDataBuffer);
      bindGroupDraw.BindBuffer("particlePointsData", hActiveTrailPointsDataBuffer);

      // upload this batch of particle data
      const plUInt32 uiNumParticlesInBatch = plMath::Min<plUInt32>(uiNumParticles, s_uiParticlesPerBatch);
      uiNumParticles -= uiNumParticlesInBatch;

      pGALCommandEncoder->UpdateBuffer(hBaseDataBuffer, 0, plMakeArrayPtr(pParticleBaseData, uiNumParticlesInBatch).ToByteArray(), plGALUpdateMode::AheadOfTime);
      pParticleBaseData += uiNumParticlesInBatch;

      pGALCommandEncoder->UpdateBuffer(hTrailDataBuffer, 0, plMakeArrayPtr(pParticleTrailData, uiNumParticlesInBatch).ToByteArray(), plGALUpdateMode::AheadOfTime);
      pParticleTrailData += uiNumParticlesInBatch;

      pGALCommandEncoder->UpdateBuffer(hActiveTrailPointsDataBuffer, 0, plMakeArrayPtr(pParticlePointsData, uiNumParticlesInBatch * uiBucketSize).ToByteArray(), plGALUpdateMode::AheadOfTime);
      pParticlePointsData += uiNumParticlesInBatch * uiBucketSize;

      // do one drawcall
      pRenderContext->DrawMeshBuffer(uiNumParticlesInBatch * uiMaxTrailSegments * uiPrimFactor).IgnoreResult();
    }
  }
}

bool plParticleTrailRenderer::ConfigureShader(const plParticleTrailRenderData* pRenderData, const plRenderViewContext& renderViewContext) const
{
  auto pRenderContext = renderViewContext.m_pRenderContext;

  switch (pRenderData->m_RenderMode)
  {
    case plParticleTypeRenderMode::Additive:
      pRenderContext->SetShaderPermutationVariable("PARTICLE_RENDER_MODE", "PARTICLE_RENDER_MODE_ADDITIVE");
      break;
    case plParticleTypeRenderMode::Blended:
    case plParticleTypeRenderMode::BlendedForeground:
    case plParticleTypeRenderMode::BlendedBackground:
      pRenderContext->SetShaderPermutationVariable("PARTICLE_RENDER_MODE", "PARTICLE_RENDER_MODE_BLENDED");
      break;
    case plParticleTypeRenderMode::Opaque:
      pRenderContext->SetShaderPermutationVariable("PARTICLE_RENDER_MODE", "PARTICLE_RENDER_MODE_OPAQUE");
      break;
    case plParticleTypeRenderMode::Distortion:
      pRenderContext->SetShaderPermutationVariable("PARTICLE_RENDER_MODE", "PARTICLE_RENDER_MODE_DISTORTION");
      break;
    case plParticleTypeRenderMode::BlendAdd:
      pRenderContext->SetShaderPermutationVariable("PARTICLE_RENDER_MODE", "PARTICLE_RENDER_MODE_BLENDADD");
      break;
  }

  switch (pRenderData->m_LightingMode)
  {
    case plParticleLightingMode::Fullbright:
      pRenderContext->SetShaderPermutationVariable("PARTICLE_LIGHTING_MODE", "PARTICLE_LIGHTING_MODE_FULLBRIGHT");
      break;
    case plParticleLightingMode::VertexLit:
      pRenderContext->SetShaderPermutationVariable("PARTICLE_LIGHTING_MODE", "PARTICLE_LIGHTING_MODE_VERTEX_LIT");
      break;
  }

  switch (plParticleTypeTrail::ComputeTrailPointBucketSize(pRenderData->m_uiMaxTrailPoints))
  {
    case 8:
      renderViewContext.m_pRenderContext->SetShaderPermutationVariable("PARTICLE_TRAIL_POINTS", "PARTICLE_TRAIL_POINTS_COUNT8");
      m_pActiveTrailPointsDataBuffer = &m_TrailPointsDataBuffer8;
      break;
    case 16:
      renderViewContext.m_pRenderContext->SetShaderPermutationVariable("PARTICLE_TRAIL_POINTS", "PARTICLE_TRAIL_POINTS_COUNT16");
      m_pActiveTrailPointsDataBuffer = &m_TrailPointsDataBuffer16;
      break;
    case 32:
      renderViewContext.m_pRenderContext->SetShaderPermutationVariable("PARTICLE_TRAIL_POINTS", "PARTICLE_TRAIL_POINTS_COUNT32");
      m_pActiveTrailPointsDataBuffer = &m_TrailPointsDataBuffer32;
      break;
    case 64:
      renderViewContext.m_pRenderContext->SetShaderPermutationVariable("PARTICLE_TRAIL_POINTS", "PARTICLE_TRAIL_POINTS_COUNT64");
      m_pActiveTrailPointsDataBuffer = &m_TrailPointsDataBuffer64;
      break;

    default:
      return false;
  }

  return true;
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_Trail_TrailRenderer);
