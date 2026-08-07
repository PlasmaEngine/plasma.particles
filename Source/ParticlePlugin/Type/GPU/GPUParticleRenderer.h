#pragma once

#include <ParticlePlugin/Declarations.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <ParticlePlugin/Renderer/ParticleRenderer.h>
#include <RendererFoundation/RendererFoundationDLL.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/GPUParticleData.h>

class PL_PARTICLEPLUGIN_DLL plGPUParticleRenderData final : public plRenderData
{
  PL_ADD_DYNAMIC_REFLECTION(plGPUParticleRenderData, plRenderData);

public:
  virtual bool CanBatch(const plRenderData& other) const override;

  plTexture2DResourceHandle m_hTexture;
  plTransform m_GlobalTransform;
  plTime m_TotalEffectLifeTime;
  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plUInt32 m_uiMaxParticles = 0;
  plUInt32 m_uiActiveSlotCount = 0;

  // GPU buffer handles
  plGALBufferHandle m_hParticleBuffer;
  plGALBufferHandle m_hCounterBuffer;
  plGALBufferHandle m_hAliveListBuffer;
  plGALBufferHandle m_hDrawArgsBuffer;

  // Emission data (kept alive for the compute pass via the frame allocator)
  plArrayPtr<plGPUParticle> m_NewParticles;

  bool m_bEnableDepthCollision = false;
  bool m_bEnableSDFCollision = false;
  plUInt8 m_uiCollisionReaction = 0;

  // Multi-type rendering
  plUInt8 m_uiGPURenderType = 0;
  plUInt32 m_uiMaxTrailPoints = 16;
  plUInt32 m_uiTrailWriteIndex = 0;
  plGALBufferHandle m_hTrailPositionBuffer;
  float m_fVelocityStretch = 1.0f;
};

class PL_PARTICLEPLUGIN_DLL plGPUParticleRenderer final : public plParticleRenderer
{
  PL_ADD_DYNAMIC_REFLECTION(plGPUParticleRenderer, plParticleRenderer);
  PL_DISALLOW_COPY_AND_ASSIGN(plGPUParticleRenderer);

public:
  plGPUParticleRenderer();
  ~plGPUParticleRenderer();

  virtual void GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& out_types) const override;
  virtual void RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const override;

protected:
  void ConfigureRenderMode(const plGPUParticleRenderData* pRenderData, plRenderContext* pRenderContext) const;

  // the render VS reads GPUPartVelocityStretch / trail fields; the renderer must bind its own
  // constants rather than relying on whatever the compute pass left bound (stale-value bug)
  plConstantBufferStorageHandle m_hGPUConstantBuffer;
};