#pragma once

#include <Foundation/Math/Color.h>
#include <Foundation/Strings/HashedString.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererCore/Textures/Texture2DResource.h>

class plParticleEffectInstance;
class plStreamWriter;
class plStreamReader;

/// Emissive and depth-blending settings of a render type as the renderer gets them.
struct plParticleLookData
{
  plTexture2DResourceHandle m_hEmissiveMap;
  plColor m_Emissive = plColor::White; ///< linear colour times intensity
  float m_fSoftEdgeDistance = 0.1f;    ///< metres over which a particle fades into geometry, 0 = off
  float m_fCameraFadeDistance = 0.5f;  ///< metres in front of the camera over which it fades, 0 = off
  bool m_bPerParticleEmissive = false; ///< the base data carries an Emissive multiplier per particle
  bool m_bDistortionTexture = false;   ///< a distortion texture is bound; without one the shader bends like a lens
  bool m_bRoundMask = false;           ///< no base texture: the shader masks the quad with a soft disc
  float m_fDistortionStrength = 0.0f;  ///< screen offset in pixels at full distortion

  float m_fAlphaCutoff = 0.5f;         ///< Masked render mode only

  // Surface lighting only
  bool m_bOrmMap = false;
  float m_fSurfaceRoughness = 0.5f;
  float m_fSurfaceMetallic = 0.0f;
  float m_fSurfaceSpecular = 0.5f;
};

/// The authored emissive and depth-blending settings, shared by the quad, trail and ribbon factories.
struct PL_PARTICLEPLUGIN_DLL plParticleLookSettings
{
  plString m_sEmissiveMap;
  plColor m_EmissiveColor = plColor::White;
  float m_fEmissiveIntensity = 1.0f;
  plString m_sEmissiveColorParameter;
  float m_fSoftEdgeDistance = 0.1f;
  float m_fCameraFadeDistance = 0.5f;
  float m_fAlphaCutoff = 0.5f;  ///< Masked render mode only
  bool m_bCastShadows = false; ///< Masked and Opaque render modes only

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// The runtime half, held by a render type.
struct PL_PARTICLEPLUGIN_DLL plParticleLook
{
  plTexture2DResourceHandle m_hEmissiveMap;
  plColor m_EmissiveColor = plColor::White;
  float m_fEmissiveIntensity = 1.0f;
  plTempHashedString m_sEmissiveColorParameter;
  float m_fSoftEdgeDistance = 0.1f;
  float m_fCameraFadeDistance = 0.5f;
  float m_fAlphaCutoff = 0.5f;
  bool m_bCastShadows = false;

  void Configure(const plParticleLookSettings& settings);

  /// Resolves the effect's emissive colour parameter for this frame.
  plParticleLookData GetRenderData(const plParticleEffectInstance* pEffect, bool bPerParticleEmissive) const;
};

/// Accessors for a factory holding the settings as m_Look. Reflection cannot point at a member of a member.
#define PL_PARTICLE_LOOK_ACCESSORS                                                                               \
  const char* GetEmissiveMap() const { return m_Look.m_sEmissiveMap; }                                          \
  void SetEmissiveMap(const char* szValue) { m_Look.m_sEmissiveMap = szValue; }                                 \
  const plColor& GetEmissiveColor() const { return m_Look.m_EmissiveColor; }                                    \
  void SetEmissiveColor(const plColor& value) { m_Look.m_EmissiveColor = value; }                               \
  float GetEmissiveIntensity() const { return m_Look.m_fEmissiveIntensity; }                                    \
  void SetEmissiveIntensity(float fValue) { m_Look.m_fEmissiveIntensity = fValue; }                             \
  const char* GetEmissiveColorParameter() const { return m_Look.m_sEmissiveColorParameter; }                    \
  void SetEmissiveColorParameter(const char* szValue) { m_Look.m_sEmissiveColorParameter = szValue; }           \
  float GetSoftEdgeDistance() const { return m_Look.m_fSoftEdgeDistance; }                                      \
  void SetSoftEdgeDistance(float fValue) { m_Look.m_fSoftEdgeDistance = fValue; }                               \
  float GetCameraFadeDistance() const { return m_Look.m_fCameraFadeDistance; }                                  \
  void SetCameraFadeDistance(float fValue) { m_Look.m_fCameraFadeDistance = fValue; }                           \
  float GetAlphaCutoff() const { return m_Look.m_fAlphaCutoff; }                                                \
  void SetAlphaCutoff(float fValue) { m_Look.m_fAlphaCutoff = fValue; }                                         \
  bool GetCastShadows() const { return m_Look.m_bCastShadows; }                                                 \
  void SetCastShadows(bool bValue) { m_Look.m_bCastShadows = bValue; }

/// Reflection of the settings, for a factory that uses PL_PARTICLE_LOOK_ACCESSORS.
#define PL_PARTICLE_LOOK_PROPERTIES                                                                                                        \
  PL_ACCESSOR_PROPERTY("EmissiveMap", GetEmissiveMap, SetEmissiveMap)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D")), \
    PL_ACCESSOR_PROPERTY("EmissiveColor", GetEmissiveColor, SetEmissiveColor)->AddAttributes(new plDefaultValueAttribute(plColor::White)),    \
    PL_ACCESSOR_PROPERTY("EmissiveIntensity", GetEmissiveIntensity, SetEmissiveIntensity)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 64.0f)), \
    PL_ACCESSOR_PROPERTY("EmissiveColorParam", GetEmissiveColorParameter, SetEmissiveColorParameter),                                    \
    PL_ACCESSOR_PROPERTY("SoftEdgeDistance", GetSoftEdgeDistance, SetSoftEdgeDistance)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 10.0f), new plSuffixAttribute(" m")), \
    PL_ACCESSOR_PROPERTY("CameraFadeDistance", GetCameraFadeDistance, SetCameraFadeDistance)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 10.0f), new plSuffixAttribute(" m")), \
    PL_ACCESSOR_PROPERTY("AlphaCutoff", GetAlphaCutoff, SetAlphaCutoff)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.01f, 0.99f)),                 \
    PL_ACCESSOR_PROPERTY("CastShadows", GetCastShadows, SetCastShadows)
