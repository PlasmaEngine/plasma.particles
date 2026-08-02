#pragma once

#include <ParticlePlugin/Declarations.h>
#include <ParticlePlugin/Renderer/ParticleRenderer.h>
#include <RendererCore/Pipeline/Declarations.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererFoundation/Resources/BufferPool.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/BillboardQuadParticleShaderData.h>
#include <RendererCore/../../../Data/Base/Shaders/Particles/TangentQuadParticleShaderData.h>

class PL_PARTICLEPLUGIN_DLL plParticleQuadRenderData final : public plRenderData
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleQuadRenderData, plRenderData);

public:
  virtual bool CanBatch(const plRenderData& other) const override;

  plTexture2DResourceHandle m_hTexture;
  plArrayPtr<plBaseParticleShaderData> m_BaseParticleData;
  plArrayPtr<plBillboardQuadParticleShaderData> m_BillboardParticleData;
  plArrayPtr<plTangentQuadParticleShaderData> m_TangentParticleData;
  plTransform m_GlobalTransform;
  plTime m_TotalEffectLifeTime;
  plUInt8 m_uiNumVariationsX = 1;
  plUInt8 m_uiNumVariationsY = 1;
  plUInt8 m_uiNumFlipbookAnimationsX = 1;
  plUInt8 m_uiNumFlipbookAnimationsY = 1;

  float m_fDistortionStrength = 0;
  plTexture2DResourceHandle m_hDistortionTexture;
  plTempHashedString m_QuadModePermutation;

  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plEnum<plParticleLightingMode> m_LightingMode;
  float m_fNormalCurvature = 0.5f;
  float m_fLightDirectionality = 0.5f;
  plMaterialResourceHandle m_hCustomMaterial;
};

/// \brief Implements rendering of particle systems
class PL_PARTICLEPLUGIN_DLL plParticleQuadRenderer final : public plParticleRenderer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleQuadRenderer, plParticleRenderer);
  PL_DISALLOW_COPY_AND_ASSIGN(plParticleQuadRenderer);

public:
  plParticleQuadRenderer();
  ~plParticleQuadRenderer();

  virtual void GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const override;
  virtual void RenderBatch(const plRenderViewContext& renderContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const override;


protected:
  void ConfigureRenderMode(const plParticleQuadRenderData* pRenderData, plRenderContext* pRenderContext) const;

  static const plUInt32 s_uiParticlesPerBatch = 1024;
  plGALBufferPool m_BaseDataBuffer;
  plGALBufferPool m_BillboardDataBuffer;
  plGALBufferPool m_TangentDataBuffer;
};
