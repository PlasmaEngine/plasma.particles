#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Types/ScopeExit.h>
#include <ParticlePlugin/Type/Point/PointRenderer.h>
#include <RendererCore/Pipeline/RenderDataBatch.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/Device/Device.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/ParticleSystemConstants.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticlePointRenderData, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticlePointRenderer, 1, plRTTIDefaultAllocator<plParticlePointRenderer>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticlePointRenderer::plParticlePointRenderer()
{
  CreateParticleDataBuffer(m_BaseDataBuffer, sizeof(plBaseParticleShaderData), s_uiParticlesPerBatch);
  CreateParticleDataBuffer(m_BillboardDataBuffer, sizeof(plBillboardQuadParticleShaderData), s_uiParticlesPerBatch);

  m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Particles/Point.plShader");
}


plParticlePointRenderer::~plParticlePointRenderer()
{
  DestroyParticleDataBuffer(m_BaseDataBuffer);
  DestroyParticleDataBuffer(m_BillboardDataBuffer);
}

void plParticlePointRenderer::GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const
{
  ref_types.PushBack(plGetStaticRTTI<plParticlePointRenderData>());
}

void plParticlePointRenderer::RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const
{
  plRenderContext* pRenderContext = renderViewContext.m_pRenderContext;
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  plGALCommandEncoder* pGALCommandEncoder = pRenderContext->GetCommandEncoder();

  TempSystemCB systemConstants(pRenderContext);

  pRenderContext->BindShader(m_hShader);

  // Bind mesh buffer
  {
    pRenderContext->BindNullMeshBuffer(plGALPrimitiveTopology::Points, s_uiParticlesPerBatch);
  }

  // now render all particle effects of type Point
  for (auto it = batch.GetIterator<plParticlePointRenderData>(0, batch.GetDataCount()); it.IsValid(); ++it)
  {
    const plParticlePointRenderData* pRenderData = it;

    const plBaseParticleShaderData* pParticleBaseData = pRenderData->m_BaseParticleData.GetPtr();
    const plBillboardQuadParticleShaderData* pParticleBillboardData = pRenderData->m_BillboardParticleData.GetPtr();

    plUInt32 uiNumParticles = pRenderData->m_BaseParticleData.GetCount();

    systemConstants.SetGenericData(pRenderData->m_GlobalTransform, pRenderData->m_TotalEffectLifeTime, 1, 1, 1, 1);

    while (uiNumParticles > 0)
    {
      // Request new buffers and bind them
      plGALBufferHandle hBaseDataBuffer = m_BaseDataBuffer.GetNewBuffer();
      plGALBufferHandle hBillboardDataBuffer = m_BillboardDataBuffer.GetNewBuffer();
      plBindGroupBuilder& bindGroupDraw = renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
      bindGroupDraw.BindBuffer("particleBaseData", hBaseDataBuffer);
      bindGroupDraw.BindBuffer("particleBillboardQuadData", hBillboardDataBuffer);

      // upload this batch of particle data
      const plUInt32 uiNumParticlesInBatch = plMath::Min<plUInt32>(uiNumParticles, s_uiParticlesPerBatch);
      uiNumParticles -= uiNumParticlesInBatch;

      pGALCommandEncoder->UpdateBuffer(hBaseDataBuffer, 0, plMakeArrayPtr(pParticleBaseData, uiNumParticlesInBatch).ToByteArray(), plGALUpdateMode::AheadOfTime);
      pParticleBaseData += uiNumParticlesInBatch;

      pGALCommandEncoder->UpdateBuffer(hBillboardDataBuffer, 0, plMakeArrayPtr(pParticleBillboardData, uiNumParticlesInBatch).ToByteArray(), plGALUpdateMode::AheadOfTime);
      pParticleBillboardData += uiNumParticlesInBatch;

      // do one drawcall
      pRenderContext->DrawMeshBuffer(uiNumParticlesInBatch).IgnoreResult();
    }
  }
}



PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_Point_PointRenderer);
