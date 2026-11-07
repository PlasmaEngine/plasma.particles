#pragma once

#include <ParticlePlugin/Renderer/ParticleLook.h>
#include <ParticlePlugin/Type/ParticleType.h>
#include <ParticlePlugin/Type/Quad/QuadParticleRenderer.h>
#include <RendererFoundation/RendererFoundationDLL.h>

using plTexture2DResourceHandle = plTypedResourceHandle<class plTexture2DResource>;

struct PL_PARTICLEPLUGIN_DLL plQuadParticleOrientation
{
  using StorageType = plUInt8;

  enum Enum
  {
    Billboard,

    Rotating_OrthoEmitterDir,
    Rotating_EmitterDir,

    Fixed_EmitterDir,
    Fixed_WorldUp,
    Fixed_RandomDir,

    FixedAxis_EmitterDir,
    FixedAxis_ParticleDir,

    Fixed_StartAxis, ///< uses the Axis stream as written at spawn time (e.g. the mesh surface normal from the Mesh Position initializer)

    Custom_Facing, ///< faces the Axis stream every frame, as written by Set Attribute's Facing target; Rotation spins it around that

    Default = Billboard
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plQuadParticleOrientation);

/// When the orientations that follow the emitter direction read it.
struct PL_PARTICLEPLUGIN_DLL plQuadParticleEmitterDirSampling
{
  using StorageType = plUInt8;

  enum Enum
  {
    Live,    ///< every frame, so living particles turn with the emitter
    AtSpawn, ///< once per particle, when it spawns

    Default = AtSpawn
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plQuadParticleEmitterDirSampling);

class PL_PARTICLEPLUGIN_DLL plParticleTypeQuadFactory final : public plParticleTypeFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeQuadFactory, plParticleTypeFactory);

public:
  virtual const plRTTI* GetTypeType() const override;
  virtual void CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  PL_PARTICLE_LOOK_ACCESSORS

  plEnum<plQuadParticleOrientation> m_Orientation;
  plEnum<plQuadParticleEmitterDirSampling> m_EmitterDirSampling;
  float m_fMinSpeed = 0.1f;      ///< Axis: Particle Dir keeps its direction while the particle is slower than this (m/s)
  float m_fTurnSmoothing = 0.0f; ///< Axis: Particle Dir; 0 turns instantly, towards 1 turns ever more slowly
  plAngle m_MaxDeviation;
  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plString m_sTexture;
  plEnum<plParticleTextureAtlasType> m_TextureAtlasType;
  plUInt8 m_uiNumSpritesX = 1;
  plUInt8 m_uiNumSpritesY = 1;
  plString m_sTintColorParameter;
  plString m_sDistortionTexture;
  float m_fDistortionStrength = 0;
  float m_fStretch = 1;
  plEnum<plParticleLightingMode> m_LightingMode;
  float m_fNormalCurvature = 0.5f;
  float m_fLightDirectionality = 0.5f;
  // six-way response maps: A = light from +right/+up/+front with density in alpha,
  // B = the opposing three directions. The emissive map supplies the self-lit part on top.
  plString m_sSixWayMapA;
  plString m_sSixWayMapB;
  float m_fSixWayAbsorption = 1.0f;
  // per-pixel and surface lighting: perturbs the billboard normal with a tangent-space map
  plString m_sNormalMap;
  float m_fNormalMapStrength = 0.0f;
  // surface lighting: occlusion / roughness / metal in R / G / B, multiplied with the values below
  plString m_sOrmMap;
  float m_fRoughness = 0.5f;
  float m_fMetallic = 0.0f;
  float m_fSpecular = 0.5f; ///< reflectance at normal incidence, 0.5 = 4 %
  bool m_bUseCustomMaterial = false;
  plString m_sCustomMaterial;
  plParticleLookSettings m_Look;
};

class PL_PARTICLEPLUGIN_DLL plParticleTypeQuad final : public plParticleType
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeQuad, plParticleType);

public:
  plParticleTypeQuad();
  ~plParticleTypeQuad();

  virtual void CreateRequiredStreams() override;

  plEnum<plQuadParticleOrientation> m_Orientation;
  plEnum<plQuadParticleEmitterDirSampling> m_EmitterDirSampling;
  float m_fMinSpeed = 0.1f;
  float m_fTurnSmoothing = 0.0f;
  plAngle m_MaxDeviation;
  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plTexture2DResourceHandle m_hTexture;
  plEnum<plParticleTextureAtlasType> m_TextureAtlasType;
  plUInt8 m_uiNumSpritesX = 1;
  plUInt8 m_uiNumSpritesY = 1;
  plTempHashedString m_sTintColorParameter;
  plTexture2DResourceHandle m_hDistortionTexture;
  float m_fDistortionStrength = 0;
  float m_fStretch = 1;
  plEnum<plParticleLightingMode> m_LightingMode;
  float m_fNormalCurvature = 0.5f;
  float m_fLightDirectionality = 0.5f;
  plTexture2DResourceHandle m_hSixWayMapA;
  plTexture2DResourceHandle m_hSixWayMapB;
  float m_fSixWayAbsorption = 1.0f;
  plTexture2DResourceHandle m_hNormalMap;
  float m_fNormalMapStrength = 0.0f;
  plTexture2DResourceHandle m_hOrmMap;
  float m_fRoughness = 0.5f;
  float m_fMetallic = 0.0f;
  float m_fSpecular = 0.5f;
  plMaterialResourceHandle m_hCustomMaterial;
  plParticleLook m_Look;

  virtual void ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const override;

  struct sod
  {
    PL_DECLARE_POD_TYPE();

    float dist;
    plUInt32 index;
  };


  /// Whether this orientation reads the emitter direction captured per particle in the Axis stream.
  bool UsesSpawnEmitterDir() const;

  virtual bool CastsShadows() const override { return m_Look.m_bCastShadows && plParticleIsOpaqueRenderMode(m_RenderMode); }

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;
  virtual void Process(plUInt64 uiNumElements) override;
  void UpdateParticleDirFacing(plUInt64 uiNumElements);
  void AllocateParticleData(const plUInt32 numParticles, const bool bNeedsBillboardData, const bool bNeedsTangentData) const;
  void AddParticleRenderData(plMsgExtractRenderData& msg, const plTransform& instanceTransform) const;
  void CreateExtractedData(const plHybridArray<sod, 64>* pSorted) const;

  plProcessingStream* m_pStreamLifeTime = nullptr;
  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
  plProcessingStream* m_pStreamRotationSpeed = nullptr;
  plProcessingStream* m_pStreamRotationOffset = nullptr;
  plProcessingStream* m_pStreamAxis = nullptr;
  plProcessingStream* m_pStreamVariation = nullptr;
  plProcessingStream* m_pStreamLastPosition = nullptr;
  plProcessingStream* m_pStreamSizeScale = nullptr; // optional per-axis size multipliers
  plProcessingStream* m_pStreamEmissive = nullptr;  // optional per-particle emissive multiplier
  plProcessingStream* m_pStreamVelocity = nullptr;  // optional; the first facing of Axis: Particle Dir

  virtual void QueryOptionalStreams() override;

  mutable plArrayPtr<plBaseParticleShaderData> m_BaseParticleData;
  mutable plArrayPtr<plBillboardQuadParticleShaderData> m_BillboardParticleData;
  mutable plArrayPtr<plTangentQuadParticleShaderData> m_TangentParticleData;
};

/// Whether the orientation keeps a per-particle axis in the Axis stream that plParticleInitializeQuadAxis fills at spawn.
PL_PARTICLEPLUGIN_DLL bool plParticleQuadOrientationNeedsSpawnAxis(plQuadParticleOrientation::Enum orientation, plQuadParticleEmitterDirSampling::Enum sampling);

/// Fills the Axis stream of new particles for the orientations that keep a per-particle axis.
/// Shared by the quad type and the GPU type that takes its place on lowered systems.
PL_PARTICLEPLUGIN_DLL void plParticleInitializeQuadAxis(plParticleSystemInstance* pSystem, plQuadParticleOrientation::Enum orientation, plQuadParticleEmitterDirSampling::Enum sampling, plAngle maxDeviation, plRandom& ref_rng, const plVec3* pVelocity, plVec3* pAxis, plUInt64 uiStartIndex, plUInt64 uiNumElements);
