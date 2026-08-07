#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior_Raycast.h> // plParticleRaycastHitReaction
#include <ParticlePlugin/Type/ParticleType.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h> // plQuadParticleOrientation
#include <RendererFoundation/RendererFoundationDLL.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/GPUParticleData.h>

using plTexture2DResourceHandle = plTypedResourceHandle<class plTexture2DResource>;
using plTexture3DResourceHandle = plTypedResourceHandle<class plTexture3DResource>;

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
  virtual void QueryOptionalStreams() override;
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

  // ---- lowered-module state, configured by the CPU->GPU lowering (SimulationTarget GPU/Auto) ----
  // GPU_PARTICLE_FEATURE_* bits selecting which lowered modules run in the simulate shader
  plUInt32 m_uiFeatureFlags = 0;
  bool m_bLowered = false;          // when set, gravity comes from the physics module * factor
  float m_fGravityFactor = 1.0f;
  float m_fRiseSpeed = 0.0f;
  float m_fFriction = 0.0f;
  float m_fLinearDrag = 0.0f;
  float m_fFadeStartAlpha = 1.0f;
  float m_fFadeExponent = 1.0f;
  float m_fColorMaxSpeed = 1.0f;
  float m_fSizeBase = 0.0f;
  float m_fSizeScale = 1.0f;
  plVec3 m_vTurbScrollSpeed = plVec3::MakeZero();
  float m_fTurbStrength = 0.0f;
  float m_fTurbFrequency = 1.0f;
  plUInt8 m_uiTurbOctaves = 1;
  plVec3 m_vVectorFieldSize = plVec3(4.0f);
  plVec3 m_vVectorFieldOffset = plVec3::MakeZero();
  float m_fVectorFieldStrength = 1.0f;
  plVec3 m_vBoundsOffset = plVec3::MakeZero();
  plVec3 m_vBoundsExtents = plVec3(2.0f);
  // attractor: the target and axis are effect-relative when the attractor targets the effect origin
  bool m_bAttractorAtEffectOrigin = false;
  plVec3 m_vAttractorPos = plVec3::MakeZero();
  plVec3 m_vAttractorAxis = plVec3(0, 0, 1);
  float m_fAttractorForce = 0.0f;
  float m_fAttractorMaxDist = 10.0f;
  float m_fAttractorMinDist = 0.1f;
  plUInt8 m_uiAttractorShape = 0;
  float m_fPullAlongStrength = 0.0f;
  plVec3 m_vSphereOffset = plVec3::MakeZero();
  float m_fSphereRadius = 3.0f;
  bool m_bSceneForces = false;
  float m_fSceneForceInfluence = 1.0f;
  plTempHashedString m_sSceneForceParameter;
  bool m_bFlies = false;
  float m_fFliesSpeed = 0.2f;
  float m_fFliesPathLength = 0.2f;
  float m_fFliesMaxEmitterDistance = 0.5f;
  plAngle m_FliesMaxSteeringAngle;
  plTexture2DResourceHandle m_hGradientLUT;
  plTexture2DResourceHandle m_hSizeCurveLUT;
  plTexture3DResourceHandle m_hTurbulenceNoise;
  plTexture3DResourceHandle m_hVectorFieldTexture;

  /// Which CPU renderer draws this lowered system from structs the simulate dispatch writes.
  /// Legacy means the authored GPU type's own render path instead.
  enum class CPURenderPath : plUInt8
  {
    Legacy,
    Quad,
    Point,
    Trail,
  };

  CPURenderPath m_CPURenderPath = CPURenderPath::Legacy;

  // ---- quad rendering: a lowered quad system is drawn by the ordinary quad renderer, from
  // per-particle structs the simulate dispatch writes, so it keeps the full quad feature set ----
  bool m_bRenderAsQuad = false;
  plEnum<plQuadParticleOrientation> m_QuadOrientation;
  plEnum<plParticleTextureAtlasType> m_TextureAtlasType;
  plUInt8 m_uiNumSpritesX = 1;
  plUInt8 m_uiNumSpritesY = 1;
  plTempHashedString m_sTintColorParameter;
  plTexture2DResourceHandle m_hDistortionTexture;
  plTexture2DResourceHandle m_hSixWayMapA;
  plTexture2DResourceHandle m_hSixWayMapB;
  float m_fSixWayAbsorption = 1.0f;
  float m_fDistortionStrength = 0;
  float m_fStretch = 1;
  plEnum<plParticleLightingMode> m_LightingMode;
  float m_fNormalCurvature = 0.5f;
  float m_fLightDirectionality = 0.5f;
  plMaterialResourceHandle m_hCustomMaterial;

  virtual void RequestRequiredWorldModulesForCache(plParticleWorldModule* pParticleModule) override;

protected:
  friend class plParticleTypeGPUFactory;

  virtual void Process(plUInt64 uiNumElements) override {}
  virtual void StepParticleSystem(const plTime& tDiff, plUInt32 uiNumNewParticles) override;

  // pull-along state, tracked exactly like the CPU behavior: this frame's emitter movement
  bool m_bPullAlongFirstStep = true;
  plVec3 m_vLastEmitterPosition = plVec3::MakeZero();
  plVec3 m_vApplyPull = plVec3::MakeZero();

  // flies picks a new heading only when its path-length timer elapses, same as the CPU behavior
  plTime m_FliesTimeToChangeDir;
  bool m_bFliesChangeDirThisFrame = false;
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  void EnsureGPUBuffers() const;
  void DestroyGPUBuffers();
  void AddQuadRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const;
  void AddPointRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const;
  void AddTrailRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const;
  void AddGPURenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform, plUInt32 uiActiveSlotCount, plArrayPtr<plGPUParticle> newParticles) const;
  void QueueSimulation(const plTransform& instanceTransform, plUInt32 uiActiveSlotCount, plArrayPtr<plGPUParticle> newParticles) const;
  plArrayPtr<plGPUParticleForceField> GatherForceFields(const plTransform& instanceTransform) const;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamLifeTime = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
  plProcessingStream* m_pStreamRotationSpeed = nullptr;
  plProcessingStream* m_pStreamRotationOffset = nullptr;
  plProcessingStream* m_pStreamAxis = nullptr;      // optional, drives the fixed quad orientations
  plProcessingStream* m_pStreamVariation = nullptr; // optional, texture atlas variation

  mutable plGALBufferHandle m_hParticleBuffer;
  mutable plGALBufferHandle m_hCounterBuffer;
  mutable plGALBufferHandle m_hAliveListBuffer;
  mutable plGALBufferHandle m_hDrawArgsBuffer;
  mutable plGALBufferHandle m_hQuadBaseBuffer;
  mutable plGALBufferHandle m_hQuadBillboardBuffer;
  mutable plGALBufferHandle m_hQuadTangentBuffer;
  mutable plGALBufferHandle m_hTrailDataBuffer;
  mutable plGALBufferHandle m_hTrailPointsPackedBuffer;
  mutable plGALBufferHandle m_hSortKeyBuffer;
  mutable plGALBufferHandle m_hTrailPositionBuffer;
  mutable bool m_bGPUBuffersCreated = false;
  mutable plUInt32 m_uiTrailWriteIndex = 0;

  mutable plUInt64 m_uiNewParticleStartIndex = 0;
  mutable plUInt32 m_uiNumNewParticles = 0;
  mutable plUInt32 m_uiActiveSlotCount = 0;
};