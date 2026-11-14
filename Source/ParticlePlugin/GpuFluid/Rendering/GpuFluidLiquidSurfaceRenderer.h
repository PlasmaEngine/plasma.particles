#pragma once

#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/Renderer.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/RendererFoundationDLL.h>

/// \brief A liquid volume's surface mesh, as the solver extracted it on the GPU this frame (surface nets).
///
/// Drawn by plGpuFluidLiquidPass (category LiquidSurface) in two draws: the back faces into a linear-depth target for
/// the thickness, then the front faces into colour and depth. Nothing is read back: the draw arguments are on the GPU.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidSurfaceRenderData final : public plRenderData
{
  PL_ADD_DYNAMIC_REFLECTION(plGpuFluidLiquidSurfaceRenderData, plRenderData);

public:
  static plRenderData::Category LiquidSurface;

  plGALBufferHandle m_hVertices; ///< per vertex float4 (domain-relative position, 1), float4 (normal, 0)
  plGALBufferHandle m_hQuads;    ///< per quad 4 vertex indices
  plGALBufferHandle m_hDrawArgs; ///< {6 * quads, 1, 0, 0}
  plUInt32 m_uiMaxQuads = 0;
  plVec3 m_vDomainOrigin = plVec3::MakeZero();

  float m_fIndexOfRefraction = 1.333f;
  plVec3 m_vAbsorption = plVec3(0.45f, 0.07f, 0.04f); ///< sigma_a, 1/m
  plVec3 m_vScatterColor = plVec3(0.03f, 0.09f, 0.11f);
  float m_fRoughness = 0.04f;
  float m_fRefractionStrength = 1.0f; ///< share of the thickness the bent ray is followed (1: to the back face)
  float m_fMaxThickness = 4.0f;
  plUInt32 m_uiDebugView = 0;
};

/// \brief Draws plGpuFluidLiquidSurfaceRenderData for plGpuFluidLiquidPass: vertex pulling from the solver's quads,
/// indirect, no index buffer. The pass picks the draw (back faces or the shaded front) with the permutation variable
/// GPU_FLUID_LIQUID_DRAW.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidSurfaceRenderer final : public plRenderer
{
  PL_ADD_DYNAMIC_REFLECTION(plGpuFluidLiquidSurfaceRenderer, plRenderer);
  PL_DISALLOW_COPY_AND_ASSIGN(plGpuFluidLiquidSurfaceRenderer);

public:
  plGpuFluidLiquidSurfaceRenderer();
  ~plGpuFluidLiquidSurfaceRenderer();

  virtual void GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const override;
  virtual void GetSupportedRenderDataCategories(plHybridArray<plRenderData::Category, 8>& ref_categories) const override;
  virtual void RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const override;

private:
  plShaderResourceHandle m_hShader;
};
