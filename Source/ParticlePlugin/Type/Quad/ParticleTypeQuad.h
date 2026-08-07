#pragma once

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

    Default = Billboard
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plQuadParticleOrientation);

class PL_PARTICLEPLUGIN_DLL plParticleTypeQuadFactory final : public plParticleTypeFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeQuadFactory, plParticleTypeFactory);

public:
  virtual const plRTTI* GetTypeType() const override;
  virtual void CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  plEnum<plQuadParticleOrientation> m_Orientation;
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
  // B = the opposing three directions. The Texture slot supplies the emissive on top.
  plString m_sSixWayMapA;
  plString m_sSixWayMapB;
  float m_fSixWayAbsorption = 1.0f;
  // per-pixel lighting only: perturbs the billboard normal with a tangent-space map
  plString m_sNormalMap;
  float m_fNormalMapStrength = 0.0f;
  bool m_bUseCustomMaterial = false;
  plString m_sCustomMaterial;
};

class PL_PARTICLEPLUGIN_DLL plParticleTypeQuad final : public plParticleType
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeQuad, plParticleType);

public:
  plParticleTypeQuad();
  ~plParticleTypeQuad();

  virtual void CreateRequiredStreams() override;

  plEnum<plQuadParticleOrientation> m_Orientation;
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
  plMaterialResourceHandle m_hCustomMaterial;

  virtual void ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const override;

  struct sod
  {
    PL_DECLARE_POD_TYPE();

    float dist;
    plUInt32 index;
  };


protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;
  virtual void Process(plUInt64 uiNumElements) override {}
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
  plProcessingStream* m_pStreamSize2 = nullptr; // optional per-axis size multipliers

  virtual void QueryOptionalStreams() override;

  mutable plArrayPtr<plBaseParticleShaderData> m_BaseParticleData;
  mutable plArrayPtr<plBillboardQuadParticleShaderData> m_BillboardParticleData;
  mutable plArrayPtr<plTangentQuadParticleShaderData> m_TangentParticleData;
};
