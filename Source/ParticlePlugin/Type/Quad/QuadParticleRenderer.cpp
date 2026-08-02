#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Types/ScopeExit.h>
#include <ParticlePlugin/Type/Quad/QuadParticleRenderer.h>
#include <RendererCore/Pipeline/RenderDataBatch.h>
#include <RendererCore/RenderContext/RenderContext.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleQuadRenderData, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleQuadRenderer, 1, plRTTIDefaultAllocator<plParticleQuadRenderer>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

bool plParticleQuadRenderData::CanBatch(const plRenderData& other0) const
{
  const auto& other = plStaticCast<const plParticleQuadRenderData&>(other0);

  return m_RenderMode == other.m_RenderMode && m_hTexture == other.m_hTexture && m_hCustomMaterial == other.m_hCustomMaterial && m_hDistortionTexture == other.m_hDistortionTexture;
}

//////////////////////////////////////////////////////////////////////////

plParticleQuadRenderer::plParticleQuadRenderer()
{
  CreateParticleDataBuffer(m_BaseDataBuffer, sizeof(plBaseParticleShaderData), s_uiParticlesPerBatch);
  CreateParticleDataBuffer(m_BillboardDataBuffer, sizeof(plBillboardQuadParticleShaderData), s_uiParticlesPerBatch);
  CreateParticleDataBuffer(m_TangentDataBuffer, sizeof(plTangentQuadParticleShaderData), s_uiParticlesPerBatch);

  m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Particles/DefaultQuadParticle.plShader");
}

plParticleQuadRenderer::~plParticleQuadRenderer()
{
  DestroyParticleDataBuffer(m_BaseDataBuffer);
  DestroyParticleDataBuffer(m_BillboardDataBuffer);
  DestroyParticleDataBuffer(m_TangentDataBuffer);
}

void plParticleQuadRenderer::GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const
{
  ref_types.PushBack(plGetStaticRTTI<plParticleQuadRenderData>());
}

void plParticleQuadRenderer::RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const
{
  plRenderContext* pRenderContext = renderViewContext.m_pRenderContext;
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  plGALCommandEncoder* pGALCommandEncoder = pRenderContext->GetCommandEncoder();

  TempSystemCB systemConstants(pRenderContext);

  bool bBindShader = true;

  // Bind mesh buffer
  {
    pRenderContext->BindNullMeshBuffer(plGALPrimitiveTopology::Triangles, s_uiParticlesPerBatch * 2);
  }

  plBindGroupBuilder& bindGroupMaterial = renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);

  // now render all particle effects of type Quad
  for (auto it = batch.GetIterator<plParticleQuadRenderData>(0, batch.GetDataCount()); it.IsValid(); ++it)
  {
    const plParticleQuadRenderData* pRenderData = it;

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

    const plBaseParticleShaderData* pParticleBaseData = pRenderData->m_BaseParticleData.GetPtr();
    const plBillboardQuadParticleShaderData* pParticleBillboardData = pRenderData->m_BillboardParticleData.GetPtr();
    const plTangentQuadParticleShaderData* pParticleTangentData = pRenderData->m_TangentParticleData.GetPtr();

    plUInt32 uiNumParticles = pRenderData->m_BaseParticleData.GetCount();

    ConfigureRenderMode(pRenderData, pRenderContext);

    systemConstants.SetGenericData(pRenderData->m_GlobalTransform, pRenderData->m_TotalEffectLifeTime, pRenderData->m_uiNumVariationsX, pRenderData->m_uiNumVariationsY, pRenderData->m_uiNumFlipbookAnimationsX, pRenderData->m_uiNumFlipbookAnimationsY, pRenderData->m_fNormalCurvature, pRenderData->m_fLightDirectionality);

    pRenderContext->SetShaderPermutationVariable("PARTICLE_QUAD_MODE", pRenderData->m_QuadModePermutation);

    while (uiNumParticles > 0)
    {
      // Request new buffers and bind them
      plGALBufferHandle hBaseDataBuffer = m_BaseDataBuffer.GetNewBuffer();
      plGALBufferHandle hBillboardDataBuffer = m_BillboardDataBuffer.GetNewBuffer();
      plGALBufferHandle hTangentDataBuffer = m_TangentDataBuffer.GetNewBuffer();

      plBindGroupBuilder& bindGroupDraw = renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
      bindGroupDraw.BindBuffer("particleBaseData", hBaseDataBuffer);
      bindGroupDraw.BindBuffer("particleBillboardQuadData", hBillboardDataBuffer);
      bindGroupDraw.BindBuffer("particleTangentQuadData", hTangentDataBuffer);

      // upload this batch of particle data
      const plUInt32 uiNumParticlesInBatch = plMath::Min<plUInt32>(uiNumParticles, s_uiParticlesPerBatch);
      uiNumParticles -= uiNumParticlesInBatch;

      pGALCommandEncoder->UpdateBuffer(hBaseDataBuffer, 0, plMakeArrayPtr(pParticleBaseData, uiNumParticlesInBatch).ToByteArray(), plGALUpdateMode::AheadOfTime);
      pParticleBaseData += uiNumParticlesInBatch;

      if (pParticleBillboardData != nullptr)
      {
        pGALCommandEncoder->UpdateBuffer(hBillboardDataBuffer, 0, plMakeArrayPtr(pParticleBillboardData, uiNumParticlesInBatch).ToByteArray(), plGALUpdateMode::AheadOfTime);
        pParticleBillboardData += uiNumParticlesInBatch;
      }

      if (pParticleTangentData != nullptr)
      {
        pGALCommandEncoder->UpdateBuffer(hTangentDataBuffer, 0, plMakeArrayPtr(pParticleTangentData, uiNumParticlesInBatch).ToByteArray(), plGALUpdateMode::AheadOfTime);
        pParticleTangentData += uiNumParticlesInBatch;
      }

      // do one drawcall
      renderViewContext.m_pRenderContext->DrawMeshBuffer(uiNumParticlesInBatch * 2).IgnoreResult();
    }
  }
}

void plParticleQuadRenderer::ConfigureRenderMode(const plParticleQuadRenderData* pRenderData, plRenderContext* pRenderContext) const
{
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


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_Quad_QuadParticleRenderer);
