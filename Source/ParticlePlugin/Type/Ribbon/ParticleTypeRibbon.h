#pragma once

#include <ParticlePlugin/Type/ParticleType.h>
#include <ParticlePlugin/Type/Ribbon/RibbonRenderer.h>
#include <RendererFoundation/RendererFoundationDLL.h>

using plTexture2DResourceHandle = plTypedResourceHandle<class plTexture2DResource>;

/// How the texture is mapped along the ribbon
struct PL_PARTICLEPLUGIN_DLL plParticleRibbonUvMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Stretch, ///< the texture covers the whole ribbon once
    Tiled,   ///< the texture repeats every TileLength meters

    Default = Stretch
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleRibbonUvMode);

class PL_PARTICLEPLUGIN_DLL plParticleTypeRibbonFactory final : public plParticleTypeFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeRibbonFactory, plParticleTypeFactory);

public:
  virtual const plRTTI* GetTypeType() const override;
  virtual void CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plEnum<plParticleRibbonUvMode> m_UvMode;
  float m_fTileLength = 1.0f;
  plString m_sTexture;
  plString m_sTintColorParameter;
  plEnum<plParticleLightingMode> m_LightingMode;
  float m_fNormalCurvature = 0.5f;
  float m_fLightDirectionality = 0.5f;
  bool m_bUseCustomMaterial = false;
  plString m_sCustomMaterial;
};

/// Connects all particles of the system in spawn order into one continuous camera-facing band.
///
/// This is the go-to type for beams, lightning bolts and sword swipes. The band width per
/// particle comes from the Size stream, so a Size Curve behavior shapes the ribbon over
/// each particle's lifetime.
class PL_PARTICLEPLUGIN_DLL plParticleTypeRibbon final : public plParticleType
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeRibbon, plParticleType);

public:
  plParticleTypeRibbon();
  ~plParticleTypeRibbon();

  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plEnum<plParticleRibbonUvMode> m_UvMode;
  float m_fTileLength = 1.0f;
  plTexture2DResourceHandle m_hTexture;
  plTempHashedString m_sTintColorParameter;
  plEnum<plParticleLightingMode> m_LightingMode;
  float m_fNormalCurvature = 0.5f;
  float m_fLightDirectionality = 0.5f;
  plMaterialResourceHandle m_hCustomMaterial;

  virtual void CreateRequiredStreams() override;
  virtual void ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const override;

protected:
  friend class plParticleTypeRibbonFactory;

  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;
  virtual void Process(plUInt64 uiNumElements) override {}

  plProcessingStream* m_pStreamLifeTime = nullptr;
  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
  plProcessingStream* m_pStreamRibbonOrder = nullptr;

  // strictly increasing spawn counter; deliberately not reset on live re-configuration,
  // so already-alive particles keep sorting before newly spawned ones
  plUInt32 m_uiNextOrderIndex = 0;

  mutable plArrayPtr<plRibbonPointShaderData> m_PointData;
};
