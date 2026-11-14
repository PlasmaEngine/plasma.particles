#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidSurfaceRenderer.h>
#include <RendererCore/Pipeline/RenderDataBatch.h>
#include <RendererCore/Pipeline/SortingFunctions.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidDrawConstants.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGpuFluidLiquidSurfaceRenderData, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGpuFluidLiquidSurfaceRenderer, 1, plRTTIDefaultAllocator<plGpuFluidLiquidSurfaceRenderer>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plRenderData::Category plGpuFluidLiquidSurfaceRenderData::LiquidSurface = plRenderData::RegisterCategory("GpuFluidLiquidSurface", &plRenderSortingFunctions::BackToFrontThenByRenderData);

plGpuFluidLiquidSurfaceRenderer::plGpuFluidLiquidSurfaceRenderer()
{
  m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Liquid/LiquidSurface.plShader");
}

plGpuFluidLiquidSurfaceRenderer::~plGpuFluidLiquidSurfaceRenderer() = default;

void plGpuFluidLiquidSurfaceRenderer::GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const
{
  ref_types.PushBack(plGetStaticRTTI<plGpuFluidLiquidSurfaceRenderData>());
}

void plGpuFluidLiquidSurfaceRenderer::GetSupportedRenderDataCategories(plHybridArray<plRenderData::Category, 8>& ref_categories) const
{
  ref_categories.PushBack(plGpuFluidLiquidSurfaceRenderData::LiquidSurface);
}

void plGpuFluidLiquidSurfaceRenderer::RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const
{
  PL_IGNORE_UNUSED(pPass);
  plRenderContext* pContext = renderViewContext.m_pRenderContext;

  for (auto it = batch.GetIterator<plGpuFluidLiquidSurfaceRenderData>(); it.IsValid(); ++it)
  {
    const plGpuFluidLiquidSurfaceRenderData* pRenderData = it;
    if (pRenderData->m_hVertices.IsInvalidated() || pRenderData->m_hQuads.IsInvalidated() || pRenderData->m_hDrawArgs.IsInvalidated() || pRenderData->m_uiMaxQuads == 0)
      continue;

    pContext->BindShader(m_hShader);

    plGpuFluidLiquidDrawConstants constants = {};
    constants.GpuFluidDrawOrigin = plVec4(pRenderData->m_vDomainOrigin.x, pRenderData->m_vDomainOrigin.y, pRenderData->m_vDomainOrigin.z, pRenderData->m_fIndexOfRefraction);
    constants.GpuFluidDrawAbsorption = plVec4(pRenderData->m_vAbsorption.x, pRenderData->m_vAbsorption.y, pRenderData->m_vAbsorption.z, pRenderData->m_fRefractionStrength);
    constants.GpuFluidDrawScatter = plVec4(pRenderData->m_vScatterColor.x, pRenderData->m_vScatterColor.y, pRenderData->m_vScatterColor.z, pRenderData->m_fRoughness);
    constants.GpuFluidDrawParams = plVec4(pRenderData->m_fMaxThickness, static_cast<float>(pRenderData->m_uiDebugView), 0.0f, 0.0f);
    pContext->SetPushConstants("plGpuFluidLiquidDrawConstants", constants);

    plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
    bindGroup.BindBuffer("GpuFluidLiqSurfVertices", pRenderData->m_hVertices);
    bindGroup.BindBuffer("GpuFluidLiqSurfQuads", pRenderData->m_hQuads);

    // Six vertices per quad; the indirect arguments carry the quad count, the primitive count only sizes the null mesh.
    pContext->BindNullMeshBuffer(plGALPrimitiveTopology::Triangles, pRenderData->m_uiMaxQuads * 2);
    pContext->DrawMeshBufferIndirect(pRenderData->m_hDrawArgs, 0).IgnoreResult();
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Rendering_GpuFluidLiquidSurfaceRenderer);
