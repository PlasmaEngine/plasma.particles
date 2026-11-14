#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidPass.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidSurfaceRenderer.h>
#include <RendererCore/Lights/ContactShadowDataProvider.h>
#include <RendererCore/Pipeline/RenderPipeline.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderContext/BindGroupBuilder.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGpuFluidLiquidPass, 1, plRTTIDefaultAllocator<plGpuFluidLiquidPass>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("ResolvedDepth", m_PinResolvedDepth),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Rendering/GpuFluid"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  plGALTextureCreationDescription MakeTargetDesc(const plGALTextureCreationDescription& colorDesc, plGALResourceFormat::Enum format)
  {
    plGALTextureCreationDescription desc;
    desc.m_uiWidth = colorDesc.m_uiWidth;
    desc.m_uiHeight = colorDesc.m_uiHeight;
    desc.m_uiArraySize = plMath::Max(colorDesc.m_uiArraySize, 1u);
    desc.m_uiMipLevelCount = 1;
    desc.m_Type = plGALTextureType::Texture2DArray;
    desc.m_Format = format;
    desc.m_SampleCount = plGALMSAASampleCount::None;
    desc.m_bAllowRenderTargetView = true;
    desc.m_bAllowShaderResourceView = true;
    desc.m_bAllowUAV = false;
    desc.m_ResourceAccess.m_bImmutable = false;
    return desc;
  }
} // namespace

plGpuFluidLiquidPass::plGpuFluidLiquidPass()
  : plForwardRenderPass("GpuFluidLiquidPass")
{
  m_hDepthCopyShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Liquid/LiquidDepthCopy.plShader");
}

plGpuFluidLiquidPass::~plGpuFluidLiquidPass() = default;

plStatus plGpuFluidLiquidPass::AddRenderPasses(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs)
{
  PL_IGNORE_UNUSED(viewData);

  const plRenderGraphTextureHandle hColor = inputs[m_PinColor.m_uiInputIndex].m_TextureHandle;
  const plRenderGraphTextureHandle hDepthStencil = inputs[m_PinDepthStencil.m_uiInputIndex].m_TextureHandle;
  const plRenderGraphTextureHandle hResolvedDepth = inputs[m_PinResolvedDepth.m_uiInputIndex].m_TextureHandle;
  if (hColor.IsInvalidated())
    return plStatus(plFmt("Color: Not connected"));
  if (hDepthStencil.IsInvalidated())
    return plStatus(plFmt("DepthStencil: Not connected"));
  if (hResolvedDepth.IsInvalidated())
    return plStatus(plFmt("ResolvedDepth: Not connected"));

  outputs[m_PinColor.m_uiOutputIndex].m_TextureHandle = hColor;
  outputs[m_PinDepthStencil.m_uiOutputIndex].m_TextureHandle = hDepthStencil;

  // No liquid surface was extracted for this view (recently): no targets, no clears, no copies. The pass is injected
  // into every pipeline with a transparent pass once the package is loaded, so a scene without liquid must not pay for
  // it. The content flags are part of the graph's fingerprint, so the graph is rebuilt when liquid comes into view.
  if ((GetPipeline()->GetContentFlags() & PL_GPUFLUID_CONTENT_FLAG_LIQUID) == 0)
    return plStatus(PL_SUCCESS);

  const plGALTextureCreationDescription colorDesc = ref_graph.GetTextureDesc(hColor);
  if (ref_graph.GetTextureDesc(hDepthStencil).m_SampleCount != colorDesc.m_SampleCount)
    return plStatus(plFmt("GpuFluidLiquidPass: DepthStencil sample count must match the Color target"));

  const plUInt32 uiArraySize = plMath::Max(colorDesc.m_uiArraySize, 1u);

  plGALTextureCreationDescription sceneColorDesc = colorDesc;
  sceneColorDesc.m_Type = plGALTextureType::Texture2DArray;
  sceneColorDesc.m_uiArraySize = uiArraySize;
  sceneColorDesc.m_uiMipLevelCount = 1;
  sceneColorDesc.m_SampleCount = plGALMSAASampleCount::None;
  const plRenderGraphTextureHandle hSceneColor = ref_graph.CreateTexture(sceneColorDesc);
  const plRenderGraphTextureHandle hSceneDepth = ref_graph.CreateTexture(MakeTargetDesc(colorDesc, plGALResourceFormat::RFloat));
  const plRenderGraphTextureHandle hBackDepth = ref_graph.CreateTexture(MakeTargetDesc(colorDesc, plGALResourceFormat::RHalf));

  const plRenderData::Category category = plGpuFluidLiquidSurfaceRenderData::LiquidSurface;
  auto HasLiquid = [this, category]() { return GetPipeline()->GetRenderDataBatchesWithCategory(category).GetBatchCount() > 0; };

  // 1. The scene so far, single sampled: what the refraction looks through.
  {
    auto pass = ref_graph.AddTransferPass("GpuFluid Liquid Copy Scene Color");
    pass.ReadTexture(hColor, {}, plGALResourceState::ResolveSource);
    pass.WriteTexture(hSceneColor, {}, plGALResourceState::ResolveDestination);
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        if (!HasLiquid())
          return;
        for (plUInt32 uiSlice = 0; uiSlice < uiArraySize; ++uiSlice)
        {
          plGALTextureSubresource subresource;
          subresource.m_uiMipLevel = 0;
          subresource.m_uiArraySlice = uiSlice;
          ctx.GetCommandEncoder()->ResolveTexture(ctx.ResolveTexture(hSceneColor), subresource, ctx.ResolveTexture(hColor), subresource);
        }
      });
  }

  // 2. The opaque depth as a float texture: the liquid writes the depth buffer, and in a pipeline without MSAA the
  //    resolved depth may be that same buffer.
  {
    auto pass = ref_graph.AddGraphicsPass("GpuFluid Liquid Copy Depth");
    pass.AddColorTarget(hSceneDepth, {}, plGALRenderTargetLoadOp::Clear);
    pass.SetClearColor(0, plColor(1.0f, 1.0f, 1.0f, 1.0f));
    pass.ReadTexture(hResolvedDepth, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::PixelShader);
    pass.SetStereoscopic(camera.IsStereoscopic());
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        if (!HasLiquid())
          return;
        const plRenderViewContext& renderViewContext = *ctx.GetUserData<plRenderViewContext>();
        renderViewContext.UpdateViewport();
        plRenderContext* pContext = renderViewContext.m_pRenderContext;
        pContext->GetBindGroup(PL_GAL_BIND_GROUP_RENDER_PASS).BindTexture("GpuFluidLiqOpaqueDepth", ctx.ResolveTexture(hResolvedDepth));
        pContext->BindShader(m_hDepthCopyShader);
        pContext->BindNullMeshBuffer(plGALPrimitiveTopology::Triangles, 1);
        pContext->DrawMeshBuffer().IgnoreResult();
      });
  }

  // 3. Back faces: the farthest one per pixel, as linear depth, for the thickness.
  {
    auto pass = ref_graph.AddGraphicsPass("GpuFluid Liquid Back Faces");
    pass.AddColorTarget(hBackDepth, {}, plGALRenderTargetLoadOp::Clear);
    pass.SetClearColor(0, plColor(0.0f, 0.0f, 0.0f, 0.0f));
    pass.ReadTexture(hSceneDepth, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::PixelShader);
    pass.SetStereoscopic(camera.IsStereoscopic());
    DeclareRenderObjectDependencies(ref_graph, pass);
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        if (!HasLiquid())
          return;
        const plRenderViewContext& renderViewContext = *ctx.GetUserData<plRenderViewContext>();
        renderViewContext.UpdateViewport();
        ClearRenderPassResources(renderViewContext);
        SetupPermutationVars(renderViewContext);
        renderViewContext.m_pRenderContext->SetShaderPermutationVariable("GPU_FLUID_LIQUID_DRAW", "GPU_FLUID_LIQUID_DRAW_BACK");
        renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_RENDER_PASS).BindTexture("GpuFluidLiqSceneDepth", ctx.ResolveTexture(hSceneDepth));
        RenderObjects(renderViewContext);
      });
  }

  // 4. Front faces, shaded, into colour and depth.
  {
    auto pass = ref_graph.AddGraphicsPass("GpuFluid Liquid Surface");
    pass.AddColorTarget(hColor);
    pass.AddDepthStencilTarget(hDepthStencil);
    pass.ReadTexture(hSceneColor, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::PixelShader);
    pass.ReadTexture(hSceneDepth, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::PixelShader);
    pass.ReadTexture(hBackDepth, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::PixelShader);
    pass.SetStereoscopic(camera.IsStereoscopic());
    DeclareRenderObjectDependencies(ref_graph, pass);
    // Lighting binds the contact shadows (see plForwardRenderPass::AddRenderPasses).
    if (auto* pContactProvider = GetPipeline()->GetFrameDataProvider<plContactShadowDataProvider>())
    {
      const plGALTextureHandle hContactShadows = pContactProvider->GetTexture();
      const plGALBufferHandle hSlotTable = pContactProvider->GetSlotTable();
      if (!hContactShadows.IsInvalidated() && !hSlotTable.IsInvalidated() && plGALDevice::GetDefaultDevice()->GetTexture(hContactShadows) != nullptr)
      {
        pass.ReadTexture(ref_graph.ImportTexture(hContactShadows), {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::PixelShader);
        pass.ReadBuffer(ref_graph.ImportBuffer(hSlotTable), plGALResourceState::ShaderRead);
      }
    }
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        if (!HasLiquid())
          return;
        const plRenderViewContext& renderViewContext = *ctx.GetUserData<plRenderViewContext>();
        renderViewContext.UpdateViewport();
        ClearRenderPassResources(renderViewContext);
        SetupPermutationVars(renderViewContext);
        SetupLighting(renderViewContext);
        renderViewContext.m_pRenderContext->SetShaderPermutationVariable("GPU_FLUID_LIQUID_DRAW", "GPU_FLUID_LIQUID_DRAW_SHADE");
        plBindGroupBuilder& bindGroup = renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_RENDER_PASS);
        bindGroup.BindTexture("GpuFluidLiqSceneColor", ctx.ResolveTexture(hSceneColor));
        bindGroup.BindTexture("GpuFluidLiqSceneDepth", ctx.ResolveTexture(hSceneDepth));
        bindGroup.BindTexture("GpuFluidLiqBackDepth", ctx.ResolveTexture(hBackDepth));
        RenderObjects(renderViewContext);
      });
  }

  return plStatus(PL_SUCCESS);
}

plStatus plGpuFluidLiquidPass::AddRenderPassesInactive(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs)
{
  PL_IGNORE_UNUSED(viewData);
  PL_IGNORE_UNUSED(camera);
  PL_IGNORE_UNUSED(ref_graph);

  outputs[m_PinColor.m_uiOutputIndex] = inputs[m_PinColor.m_uiInputIndex];
  outputs[m_PinDepthStencil.m_uiOutputIndex] = inputs[m_PinDepthStencil.m_uiInputIndex];
  return plStatus(PL_SUCCESS);
}

void plGpuFluidLiquidPass::DeclareRenderObjectDependencies(plRenderGraph& ref_graph, plRenderGraphPassBuilder& ref_pass)
{
  DeclareRendererDependenciesForCategory(plGpuFluidLiquidSurfaceRenderData::LiquidSurface, ref_graph, ref_pass);
}

void plGpuFluidLiquidPass::RenderObjects(const plRenderViewContext& renderViewContext)
{
  RenderDataWithCategory(renderViewContext, plGpuFluidLiquidSurfaceRenderData::LiquidSurface);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Rendering_GpuFluidLiquidPass);
