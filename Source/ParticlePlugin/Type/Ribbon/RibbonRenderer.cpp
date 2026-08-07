#include <ParticlePlugin/ParticlePluginPCH.h>

#include <ParticlePlugin/Type/Ribbon/ParticleTypeRibbon.h>
#include <ParticlePlugin/Type/Ribbon/RibbonRenderer.h>
#include <RendererCore/Pipeline/RenderDataBatch.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/Device/Device.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/ParticleSystemConstants.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleRibbonRenderData, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleRibbonRenderer, 1, plRTTIDefaultAllocator<plParticleRibbonRenderer>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

bool plParticleRibbonRenderData::CanBatch(const plRenderData& other0) const
{
  const auto& other = plStaticCast<const plParticleRibbonRenderData&>(other0);

  return m_RenderMode == other.m_RenderMode && m_hTexture == other.m_hTexture && m_hCustomMaterial == other.m_hCustomMaterial;
}

//////////////////////////////////////////////////////////////////////////

plParticleRibbonRenderer::plParticleRibbonRenderer()
{
  CreateParticleDataBuffer(m_PointDataBuffer, sizeof(plRibbonPointShaderData), s_uiMaxRibbonPoints);

  m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Particles/DefaultRibbonParticle.plShader");
}

plParticleRibbonRenderer::~plParticleRibbonRenderer()
{
  DestroyParticleDataBuffer(m_PointDataBuffer);
}

void plParticleRibbonRenderer::GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const
{
  ref_types.PushBack(plGetStaticRTTI<plParticleRibbonRenderData>());
}

void plParticleRibbonRenderer::RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const
{
  plRenderContext* pRenderContext = renderViewContext.m_pRenderContext;
  plGALCommandEncoder* pGALCommandEncoder = pRenderContext->GetCommandEncoder();

  TempSystemCB systemConstants(pRenderContext);

  bool bBindShader = true;

  plBindGroupBuilder& bindGroupMaterial = renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);

  for (auto it = batch.GetIterator<plParticleRibbonRenderData>(0, batch.GetDataCount()); it.IsValid(); ++it)
  {
    const plParticleRibbonRenderData* pRenderData = it;

    const plUInt32 uiNumPoints = plMath::Min<plUInt32>(pRenderData->m_Points.GetCount(), s_uiMaxRibbonPoints);

    if (uiNumPoints < 2)
      continue;

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
    }

    ConfigureShader(pRenderData, renderViewContext);

    const plTransform objectTransform = pRenderData->m_bApplyObjectTransform ? pRenderData->m_GlobalTransform : plTransform::MakeIdentity();

    systemConstants.SetGenericData(objectTransform, pRenderData->m_TotalEffectLifeTime, 1, 1, 1, 1, pRenderData->m_fNormalCurvature, pRenderData->m_fLightDirectionality);
    systemConstants.SetTrailData(0.0f, uiNumPoints); // NumUsedTrailPoints doubles as the ribbon point count

    plGALBufferHandle hPointDataBuffer = m_PointDataBuffer.GetNewBuffer();

    plBindGroupBuilder& bindGroupDraw = renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
    bindGroupDraw.BindBuffer("ribbonPointData", hPointDataBuffer);

    pGALCommandEncoder->UpdateBuffer(hPointDataBuffer, 0, pRenderData->m_Points.GetSubArray(0, uiNumPoints).ToByteArray(), plGALUpdateMode::AheadOfTime);

    const plUInt32 uiPrimitives = (uiNumPoints - 1) * 2;

    pRenderContext->BindNullMeshBuffer(plGALPrimitiveTopology::Triangles, uiPrimitives);
    pRenderContext->DrawMeshBuffer(uiPrimitives).IgnoreResult();
  }
}

void plParticleRibbonRenderer::ConfigureShader(const plParticleRibbonRenderData* pRenderData, const plRenderViewContext& renderViewContext) const
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
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_Ribbon_RibbonRenderer);
