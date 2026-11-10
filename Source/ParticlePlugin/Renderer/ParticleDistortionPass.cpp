#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <ParticlePlugin/Renderer/ParticleDistortionPass.h>
#include <RendererCore/Pipeline/Passes/TransparentForwardRenderPass.h>
#include <RendererCore/Pipeline/RenderPipeline.h>
#include <RendererCore/Pipeline/SortingFunctions.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>

plRenderData::Category plParticleRenderDataCategories::Distortion = plRenderData::RegisterCategory("ParticleDistortion", &plRenderSortingFunctions::BackToFrontThenByRenderData);

plCVarBool cvar_ParticlesShowDistortion("Particles.ShowDistortion", false, plCVarFlags::Default, "Draws the screen offsets of distortion particles instead of applying them (red = X, green = Y).");

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleDistortionPass, 1, plRTTIDefaultAllocator<plParticleDistortionPass>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("ResolvedDepth", m_PinResolvedDepth),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleDistortionPass::plParticleDistortionPass()
  : plForwardRenderPass("ParticleDistortionPass")
{
  m_hApplyShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Particles/ParticleDistortionApply.plShader");
  m_hShowShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Particles/ParticleDistortionShow.plShader");
}

plParticleDistortionPass::~plParticleDistortionPass() = default;

plStatus plParticleDistortionPass::AddRenderPasses(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs)
{
  PL_IGNORE_UNUSED(viewData);

  plRenderGraphTextureHandle hColor = inputs[m_PinColor.m_uiInputIndex].m_TextureHandle;
  if (hColor.IsInvalidated())
    return plStatus(plFmt("Color: Not connected"));

  outputs[m_PinColor.m_uiOutputIndex].m_TextureHandle = hColor;

  // without distortion particles (recently) the pass is a plain pass-through
  if ((GetPipeline()->GetContentFlags() & PL_PARTICLE_CONTENT_FLAG_DISTORTION) == 0)
    return plStatus(PL_SUCCESS);

  const plRenderGraphTextureHandle hResolvedDepth = inputs[m_PinResolvedDepth.m_uiInputIndex].m_TextureHandle;
  const plGALTextureCreationDescription colorDesc = ref_graph.GetTextureDesc(hColor);

  plGALTextureCreationDescription offsetDesc;
  offsetDesc.SetAsRenderTarget(colorDesc.m_uiWidth, colorDesc.m_uiHeight, plGALResourceFormat::RGHalf);
  offsetDesc.m_Type = plGALTextureType::Texture2DArray;
  offsetDesc.m_uiArraySize = colorDesc.m_uiArraySize;
  const plRenderGraphTextureHandle hOffsets = ref_graph.CreateTexture(offsetDesc);

  // single-sampled copy of the colour, resolved when the target is multisampled
  plGALTextureCreationDescription copyDesc;
  copyDesc.SetAsRenderTarget(colorDesc.m_uiWidth, colorDesc.m_uiHeight, colorDesc.m_Format);
  copyDesc.m_Type = plGALTextureType::Texture2DArray;
  copyDesc.m_uiArraySize = colorDesc.m_uiArraySize;
  copyDesc.m_uiMipLevelCount = 1;
  const plRenderGraphTextureHandle hColorCopy = ref_graph.CreateTexture(copyDesc);

  // the particles add their offsets; depth is tested in the shader against the resolved scene depth
  {
    auto pass = ref_graph.AddGraphicsPass("Particle Distortion Offsets");
    pass.AddColorTarget(hOffsets);
    pass.SetClearColor(0, plColor(0, 0, 0, 0));
    if (!hResolvedDepth.IsInvalidated())
      pass.ReadTexture(hResolvedDepth, {}, plGALResourceState::ShaderRead);
    pass.SetStereoscopic(camera.IsStereoscopic());
    DeclareRenderObjectDependencies(ref_graph, pass);
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
      const plRenderViewContext& renderViewContext = *ctx.GetUserData<plRenderViewContext>();
      renderViewContext.UpdateViewport();
      ClearRenderPassResources(renderViewContext);
      SetupPermutationVars(renderViewContext);
      SetupLighting(renderViewContext);

      if (!hResolvedDepth.IsInvalidated())
      {
        plBindGroupBuilder& bindGroupRenderPass = renderViewContext.m_pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_RENDER_PASS);
        bindGroupRenderPass.BindTexture("SceneDepth", ctx.ResolveTexture(hResolvedDepth));
      }

      RenderObjects(renderViewContext); });
  }

  {
    const bool bMsaa = colorDesc.m_SampleCount != plGALMSAASampleCount::None;
    auto transferPass = ref_graph.AddTransferPass("Particle Distortion Copy");
    transferPass.ReadTexture(hColor, {}, bMsaa ? plGALResourceState::ResolveSource : plGALResourceState::CopySource);
    transferPass.WriteTexture(hColorCopy, {}, bMsaa ? plGALResourceState::ResolveDestination : plGALResourceState::CopyDestination);
    const plUInt32 uiNumSlices = plMath::Max<plUInt32>(colorDesc.m_uiArraySize, 1);
    transferPass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
      // every eye of a stereo target
      for (plUInt32 uiSlice = 0; uiSlice < uiNumSlices; ++uiSlice)
      {
        plGALTextureSubresource subresource;
        subresource.m_uiMipLevel = 0;
        subresource.m_uiArraySlice = uiSlice;
        ctx.GetCommandEncoder()->ResolveTexture(ctx.ResolveTexture(hColorCopy), subresource, ctx.ResolveTexture(hColor), subresource);
      } });
  }

  {
    // a cvar change rebuilds the render graph, so the choice can be made here
    const plShaderResourceHandle hShader = cvar_ParticlesShowDistortion ? m_hShowShader : m_hApplyShader;

    auto pass = ref_graph.AddGraphicsPass("Particle Distortion Apply");
    pass.AddColorTarget(hColor);
    pass.ReadTexture(hColorCopy, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::PixelShader);
    pass.ReadTexture(hOffsets, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::PixelShader);
    pass.SetStereoscopic(camera.IsStereoscopic());
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
      const plRenderViewContext& renderViewContext = *ctx.GetUserData<plRenderViewContext>();
      renderViewContext.UpdateViewport();

      renderViewContext.m_pRenderContext->BindShader(hShader);
      renderViewContext.m_pRenderContext->BindNullMeshBuffer(plGALPrimitiveTopology::Triangles, 1);

      plBindGroupBuilder& bindGroup = renderViewContext.m_pRenderContext->GetBindGroup();
      bindGroup.BindTexture("DistortionSceneColor", ctx.ResolveTexture(hColorCopy));
      bindGroup.BindTexture("DistortionOffsets", ctx.ResolveTexture(hOffsets));

      renderViewContext.m_pRenderContext->DrawMeshBuffer().IgnoreResult(); });
  }

  return plStatus(PL_SUCCESS);
}

void plParticleDistortionPass::DeclareRenderObjectDependencies(plRenderGraph& ref_graph, plRenderGraphPassBuilder& ref_pass)
{
  DeclareRendererDependenciesForCategory(plParticleRenderDataCategories::Distortion, ref_graph, ref_pass);
}

void plParticleDistortionPass::RenderObjects(const plRenderViewContext& renderViewContext)
{
  RenderDataWithCategory(renderViewContext, plParticleRenderDataCategories::Distortion);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Renderer_ParticleDistortionPass);
