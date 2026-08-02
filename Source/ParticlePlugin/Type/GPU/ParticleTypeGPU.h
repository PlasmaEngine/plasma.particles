#pragma once

#include <ParticlePlugin/Type/ParticleType.h>
#include <RendererFoundation/RendererFoundationDLL.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/GPUParticleData.h>

using plTexture2DResourceHandle = plTypedResourceHandle<class plTexture2DResource>;

class PL_PARTICLEPLUGIN_DLL plParticleTypeGPUFactory final : public plParticleTypeFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeGPUFactory, plParticleTypeFactory);

public:
  virtual const plRTTI* GetTypeType() const override;
  virtual void CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plUInt32 m_uiMaxParticles = 10000;
  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plString m_sTexture;

  float m_fGravity = 9.81f;
  float m_fDragCoefficient = 0.0f;
  float m_fWindStrength = 0.0f;

  bool m_bEnableDepthCollision = false;
  bool m_bEnableSDFCollision = false;
  plEnum<plParticleRaycastHitReaction> m_CollisionReaction;
  float m_fCollisionBounceFactor = 0.5f;
  float m_fCollisionSlideFactor = 0.5f;
  float m_fCollisionThickness = 0.5f;

  plColor m_ColorStart = plColor::White;
  plColor m_ColorEnd = plColor(1, 1, 1, 0);

  plEnum<plGPUParticleRenderType> m_GPURenderType;
  plUInt32 m_uiMaxTrailPoints = 16;
  float m_fVelocityStretch = 1.0f;
};

class PL_PARTICLEPLUGIN_DLL plParticleTypeGPU final : public plParticleType
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeGPU, plParticleType);

public:
  plParticleTypeGPU();
  ~plParticleTypeGPU();

  virtual void CreateRequiredStreams() override;
  virtual bool HasActiveGPUElements() const override { return m_uiActiveSlotCount > 0 || m_bGPUBuffersCreated; }
  virtual void ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const override;

  plUInt32 m_uiMaxGPUParticles = 10000;
  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plTexture2DResourceHandle m_hTexture;

  float m_fGravity = 9.81f;
  float m_fDragCoefficient = 0.0f;
  float m_fWindStrength = 0.0f;

  bool m_bEnableDepthCollision = false;
  bool m_bEnableSDFCollision = false;
  plEnum<plParticleRaycastHitReaction> m_CollisionReaction;
  float m_fCollisionBounceFactor = 0.5f;
  float m_fCollisionSlideFactor = 0.5f;
  float m_fCollisionThickness = 0.5f;

  plColor m_ColorStart = plColor::White;
  plColor m_ColorEnd = plColor(1, 1, 1, 0);

  plEnum<plGPUParticleRenderType> m_GPURenderType;
  plUInt32 m_uiMaxTrailPoints = 16;
  float m_fVelocityStretch = 1.0f;

protected:
  friend class plParticleTypeGPUFactory;

  virtual void Process(plUInt64 uiNumElements) override {}
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  void EnsureGPUBuffers() const;
  void DestroyGPUBuffers();

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamLifeTime = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
  plProcessingStream* m_pStreamRotationSpeed = nullptr;
  plProcessingStream* m_pStreamRotationOffset = nullptr;

  mutable plGALBufferHandle m_hParticleBuffer;
  mutable plGALBufferHandle m_hCounterBuffer;
  mutable plGALBufferHandle m_hTrailPositionBuffer;
  mutable bool m_bGPUBuffersCreated = false;
  mutable plUInt32 m_uiGPUEmitIndex = 0;
  mutable plUInt32 m_uiTrailWriteIndex = 0;

  mutable plUInt64 m_uiNewParticleStartIndex = 0;
  mutable plUInt32 m_uiNumNewParticles = 0;
  mutable plUInt32 m_uiActiveSlotCount = 0;
};