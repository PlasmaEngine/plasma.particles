#pragma once

#include <ParticlePlugin/Declarations.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <ParticlePlugin/Renderer/ParticleRenderer.h>
#include <RendererCore/Pipeline/Declarations.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererFoundation/Resources/BufferPool.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/TrailShaderData.h>

class PL_PARTICLEPLUGIN_DLL plParticleTrailRenderData final : public plRenderData
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTrailRenderData, plRenderData);

public:
  virtual bool CanBatch(const plRenderData& other) const override;

  plTexture2DResourceHandle m_hTexture;
  plMaterialResourceHandle m_hCustomMaterial;
  plUInt16 m_uiMaxTrailPoints;
  float m_fSnapshotFraction;
  plArrayPtr<plBaseParticleShaderData> m_BaseParticleData;
  plArrayPtr<plTrailParticleShaderData> m_TrailParticleData;
  plArrayPtr<plVec4> m_TrailPointsShared;
  plTransform m_GlobalTransform;
  plTime m_TotalEffectLifeTime;
  plUInt8 m_uiNumVariationsX = 1;
  plUInt8 m_uiNumVariationsY = 1;
  plUInt8 m_uiNumFlipbookAnimationsX = 1;
  plUInt8 m_uiNumFlipbookAnimationsY = 1;

  float m_fDistortionStrength = 0;
  plTexture2DResourceHandle m_hDistortionTexture;

  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plEnum<plParticleLightingMode> m_LightingMode;
  float m_fNormalCurvature = 0.5f;
  float m_fLightDirectionality = 0.5f;
};

/// \brief Implements rendering of a trail particle systems
class PL_PARTICLEPLUGIN_DLL plParticleTrailRenderer final : public plParticleRenderer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTrailRenderer, plParticleRenderer);
  PL_DISALLOW_COPY_AND_ASSIGN(plParticleTrailRenderer);

public:
  plParticleTrailRenderer();
  ~plParticleTrailRenderer();

  virtual void GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const override;
  virtual void RenderBatch(
    const plRenderViewContext& renderContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const override;

protected:
  bool ConfigureShader(const plParticleTrailRenderData* pRenderData, const plRenderViewContext& renderViewContext) const;

  static const plUInt32 s_uiParticlesPerBatch = 512;
  plGALBufferPool m_BaseDataBuffer;
  plGALBufferPool m_TrailDataBuffer;
  plGALBufferPool m_TrailPointsDataBuffer8;
  plGALBufferPool m_TrailPointsDataBuffer16;
  plGALBufferPool m_TrailPointsDataBuffer32;
  plGALBufferPool m_TrailPointsDataBuffer64;

  mutable const plGALBufferPool* m_pActiveTrailPointsDataBuffer = nullptr;
};
