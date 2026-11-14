#pragma once

#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererCore/Pipeline/Passes/ForwardRenderPass.h>
#include <RendererCore/Shader/ShaderResource.h>

/// \brief Reported by every liquid volume that extracts a surface (plRenderPipeline::GetContentFlags), so
/// plGpuFluidLiquidPass only creates and clears its targets while a view has liquid in it. plasma.water uses bit 25,
/// the particle distortion pass bit 24.
constexpr plUInt32 PL_GPUFLUID_CONTENT_FLAG_LIQUID = PL_BIT(26);

/// \brief Draws the GpuFluid liquid surfaces in front of the transparent pass.
///
/// Injected at BeforeTransparent behind plasma.water (Startup.cpp). Per frame with a liquid surface in view:
/// 1. copies the scene colour (pre-exposed) and the opaque depth (into a single-sample float target, so the depth
///    buffer can be written while it is read);
/// 2. draws the back faces into an R16F linear-depth target, keeping the farthest (blend max, clamped to the opaque
///    depth): the thickness of the liquid along each pixel's ray is that minus the front face's depth;
/// 3. draws the front faces into colour with depth test and write: Fresnel (IoR 1.333), the sky and probe reflection,
///    the sun's specular and its shadow, refraction by offsetting the scene-colour lookup along the bent ray (with a
///    foreground reject), Beer-Lambert absorption over the path plus a small in-scatter.
///
/// Because the liquid writes depth before the transparent pass, fog, the VDB march and OIT composite at its depth.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidPass : public plForwardRenderPass
{
  PL_ADD_DYNAMIC_REFLECTION(plGpuFluidLiquidPass, plForwardRenderPass);

public:
  plGpuFluidLiquidPass();
  ~plGpuFluidLiquidPass();

  virtual plStatus AddRenderPasses(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs) override;
  virtual plStatus AddRenderPassesInactive(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs) override;

protected:
  virtual void DeclareRenderObjectDependencies(plRenderGraph& ref_graph, plRenderGraphPassBuilder& ref_pass) override;
  virtual void RenderObjects(const plRenderViewContext& renderViewContext) override;

private:
  plRenderPipelineNodeInputPin m_PinResolvedDepth;

  plShaderResourceHandle m_hDepthCopyShader;
};
