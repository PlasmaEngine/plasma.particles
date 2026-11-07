#pragma once

#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererCore/Pipeline/Passes/ForwardRenderPass.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/RenderPipelineExtension.h>
#include <RendererCore/Shader/ShaderResource.h>

/// Render data categories the particle plugin adds.
struct PL_PARTICLEPLUGIN_DLL plParticleRenderDataCategories
{
  /// Distortion particles: drawn by plParticleDistortionPass as screen offsets, not by the transparent pass.
  static plRenderData::Category Distortion;
};

/// Reported by particle types that extracted distortion render data, so the pass only builds its resources then.
constexpr plUInt32 PL_PARTICLE_CONTENT_FLAG_DISTORTION = PL_BIT(24);

/// \brief Bends everything drawn before it, including transparent particles, by the offsets distortion particles write.
///
/// Distortion particles add pixel offsets into an RG16F target. One full-screen pass then reads a copy of the colour
/// target at the offset position. Placed after plTransparentForwardRenderPass by plParticleDistortionPipelineExtension.
class PL_PARTICLEPLUGIN_DLL plParticleDistortionPass : public plForwardRenderPass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleDistortionPass, plForwardRenderPass);

public:
  plParticleDistortionPass();
  ~plParticleDistortionPass();

  virtual plStatus AddRenderPasses(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs) override;

protected:
  virtual void DeclareRenderObjectDependencies(plRenderGraph& ref_graph, plRenderGraphPassBuilder& ref_pass) override;
  virtual void RenderObjects(const plRenderViewContext& renderViewContext) override;

  plRenderPipelineNodeInputPin m_PinResolvedDepth;
  plShaderResourceHandle m_hApplyShader;
};

/// \brief Inserts a plParticleDistortionPass behind every plTransparentForwardRenderPass of a pipeline.
///
/// The pass takes over the transparent pass's colour consumers and reads the same resolved depth. Pipeline assets never
/// contain it, so they still load without the particle plugin.
class PL_PARTICLEPLUGIN_DLL plParticleDistortionPipelineExtension : public plRenderPipelineExtension
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleDistortionPipelineExtension, plRenderPipelineExtension);

public:
  virtual plResult Apply(plRenderPipeline& ref_pipeline) const override;
};
