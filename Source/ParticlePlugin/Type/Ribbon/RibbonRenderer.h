#pragma once

#include <ParticlePlugin/Declarations.h>
#include <ParticlePlugin/Renderer/ParticleRenderer.h>
#include <RendererCore/Pipeline/Declarations.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererFoundation/Resources/BufferPool.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/RibbonShaderData.h>

class PL_PARTICLEPLUGIN_DLL plParticleRibbonRenderData final : public plRenderData
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRibbonRenderData, plRenderData);

public:
  virtual bool CanBatch(const plRenderData& other) const override;

  plTexture2DResourceHandle m_hTexture;
  plMaterialResourceHandle m_hCustomMaterial;
  plArrayPtr<plRibbonPointShaderData> m_Points;
  plTime m_TotalEffectLifeTime;
  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plEnum<plParticleLightingMode> m_LightingMode;
  float m_fNormalCurvature = 0.5f;
  float m_fLightDirectionality = 0.5f;
  bool m_bApplyObjectTransform = true;
  plTransform m_GlobalTransform;
};

/// \brief Renders a particle system as one continuous camera-facing band through all particles in spawn order.
class PL_PARTICLEPLUGIN_DLL plParticleRibbonRenderer final : public plParticleRenderer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRibbonRenderer, plParticleRenderer);
  PL_DISALLOW_COPY_AND_ASSIGN(plParticleRibbonRenderer);

public:
  plParticleRibbonRenderer();
  ~plParticleRibbonRenderer();

  virtual void GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const override;

  virtual void RenderBatch(const plRenderViewContext& renderContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const override;

  static constexpr plUInt32 s_uiMaxRibbonPoints = 2048;

protected:
  void ConfigureShader(const plParticleRibbonRenderData* pRenderData, const plRenderViewContext& renderViewContext) const;

  mutable plGALBufferPool m_PointDataBuffer;
};
