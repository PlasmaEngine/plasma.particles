#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidDebugRenderer.h>
#include <RendererCore/Pipeline/RenderDataBatch.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidDebugConstants.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGpuFluidLiquidDebugRenderData, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGpuFluidLiquidDebugRenderer, 1, plRTTIDefaultAllocator<plGpuFluidLiquidDebugRenderer>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plGpuFluidLiquidDebugRenderer::plGpuFluidLiquidDebugRenderer()
{
  m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Liquid/LiquidDebugPoints.plShader");
}

plGpuFluidLiquidDebugRenderer::~plGpuFluidLiquidDebugRenderer() = default;

void plGpuFluidLiquidDebugRenderer::GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const
{
  ref_types.PushBack(plGetStaticRTTI<plGpuFluidLiquidDebugRenderData>());
}

void plGpuFluidLiquidDebugRenderer::GetSupportedRenderDataCategories(plHybridArray<plRenderData::Category, 8>& ref_categories) const
{
  ref_categories.PushBack(plDefaultRenderDataCategories::SimpleOpaque);
}

void plGpuFluidLiquidDebugRenderer::RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const
{
  plRenderContext* pContext = renderViewContext.m_pRenderContext;

  for (auto it = batch.GetIterator<plGpuFluidLiquidDebugRenderData>(); it.IsValid(); ++it)
  {
    const plGpuFluidLiquidDebugRenderData* pRenderData = it;
    if (pRenderData->m_hPoints.IsInvalidated() || pRenderData->m_hDrawArgs.IsInvalidated() || pRenderData->m_uiMaxParticles == 0)
      continue;

    pContext->BindShader(m_hShader);

    plGpuFluidLiquidDebugConstants constants = {};
    constants.GpuFluidDebugParams = plVec4(pRenderData->m_fRadius, pRenderData->m_fTopSpeed, 0.0f, 0.0f);
    pContext->SetPushConstants("plGpuFluidLiquidDebugConstants", constants);

    plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
    bindGroup.BindBuffer("GpuFluidLiqPoints", pRenderData->m_hPoints);

    // The indirect arguments carry the alive count; the primitive count only sizes the null mesh.
    pContext->BindNullMeshBuffer(plGALPrimitiveTopology::Triangles, pRenderData->m_uiMaxParticles * 2);
    pContext->DrawMeshBufferIndirect(pRenderData->m_hDrawArgs, 0).IgnoreResult();
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Rendering_GpuFluidLiquidDebugRenderer);
