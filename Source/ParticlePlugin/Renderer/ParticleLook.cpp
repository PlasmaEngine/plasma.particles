#include <ParticlePlugin/ParticlePluginPCH.h>

#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Renderer/ParticleLook.h>

void plParticleLookSettings::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_sEmissiveMap;
  inout_stream << m_EmissiveColor;
  inout_stream << m_fEmissiveIntensity;
  inout_stream << m_sEmissiveColorParameter;
  inout_stream << m_fSoftEdgeDistance;
  inout_stream << m_fCameraFadeDistance;
}

void plParticleLookSettings::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  PL_ASSERT_DEV(uiVersion == 1, "Invalid particle look version {0}", uiVersion);

  inout_stream >> m_sEmissiveMap;
  inout_stream >> m_EmissiveColor;
  inout_stream >> m_fEmissiveIntensity;
  inout_stream >> m_sEmissiveColorParameter;
  inout_stream >> m_fSoftEdgeDistance;
  inout_stream >> m_fCameraFadeDistance;
}

void plParticleLook::Configure(const plParticleLookSettings& settings)
{
  m_hEmissiveMap.Invalidate();
  if (!settings.m_sEmissiveMap.IsEmpty())
    m_hEmissiveMap = plResourceManager::LoadResource<plTexture2DResource>(settings.m_sEmissiveMap);

  m_EmissiveColor = settings.m_EmissiveColor;
  m_fEmissiveIntensity = settings.m_fEmissiveIntensity;
  m_sEmissiveColorParameter = plTempHashedString(settings.m_sEmissiveColorParameter.GetData());
  m_fSoftEdgeDistance = settings.m_fSoftEdgeDistance;
  m_fCameraFadeDistance = settings.m_fCameraFadeDistance;
}

plParticleLookData plParticleLook::GetRenderData(const plParticleEffectInstance* pEffect, bool bPerParticleEmissive) const
{
  plParticleLookData data;
  data.m_hEmissiveMap = m_hEmissiveMap;
  data.m_fSoftEdgeDistance = m_fSoftEdgeDistance;
  data.m_fCameraFadeDistance = m_fCameraFadeDistance;
  data.m_bPerParticleEmissive = bPerParticleEmissive && m_hEmissiveMap.IsValid();

  if (m_hEmissiveMap.IsValid())
  {
    const plColor param = pEffect != nullptr ? pEffect->GetColorParameter(m_sEmissiveColorParameter, plColor::White) : plColor::White;
    data.m_Emissive = m_EmissiveColor * param * m_fEmissiveIntensity;
  }
  else
  {
    data.m_Emissive = plColor::Black;
  }

  return data;
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Renderer_ParticleLook);
