#pragma once

#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/Renderer.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/RendererFoundationDLL.h>

/// \brief A liquid volume's particles, drawn as small lit sphere impostors coloured by speed.
///
/// The debug view of the liquid until the surface exists (P7). The points buffer is written by the solver on the GPU
/// every tick (world position and speed per particle) and drawn with the GPU's alive count, so nothing is read back.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidDebugRenderData final : public plRenderData
{
  PL_ADD_DYNAMIC_REFLECTION(plGpuFluidLiquidDebugRenderData, plRenderData);

public:
  plGALBufferHandle m_hPoints;   ///< float4 per particle: world position, speed
  plGALBufferHandle m_hDrawArgs; ///< {6 * alive, 1, 0, 0}
  plUInt32 m_uiMaxParticles = 0;
  float m_fRadius = 0.01f;
  float m_fTopSpeed = 3.0f; ///< speed shown at the top of the colour ramp
};

/// \brief Draws plGpuFluidLiquidDebugRenderData in the simple opaque category: depth tested and written, unlit by the
/// scene (the shading is part of the impostor), after tone mapping.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidDebugRenderer final : public plRenderer
{
  PL_ADD_DYNAMIC_REFLECTION(plGpuFluidLiquidDebugRenderer, plRenderer);
  PL_DISALLOW_COPY_AND_ASSIGN(plGpuFluidLiquidDebugRenderer);

public:
  plGpuFluidLiquidDebugRenderer();
  ~plGpuFluidLiquidDebugRenderer();

  virtual void GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const override;
  virtual void GetSupportedRenderDataCategories(plHybridArray<plRenderData::Category, 8>& ref_categories) const override;
  virtual void RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const override;

private:
  plShaderResourceHandle m_hShader;
};
