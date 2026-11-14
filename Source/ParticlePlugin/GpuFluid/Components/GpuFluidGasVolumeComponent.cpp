#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Messages/UpdateLocalBoundsMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasVolumeComponent.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/RenderDataManager.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plGpuFluidGasVolumeComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(4.0f)), new plClampValueAttribute(plVec3(0.1f), plVec3(1024.0f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("VoxelSize", m_fVoxelSize)->AddAttributes(new plDefaultValueAttribute(0.0625f), new plClampValueAttribute(0.005f, 16.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("MaxBricks", m_uiMaxBricks)->AddAttributes(new plDefaultValueAttribute(512), new plClampValueAttribute(1, 204 * 204 * 204)),
    PL_ENUM_MEMBER_PROPERTY("Quality", plGpuFluidQuality, m_Quality),
    PL_ENUM_MEMBER_PROPERTY("DebugSource", plGpuFluidDebugSource, m_DebugSource)->AddAttributes(new plGroupAttribute("Debug Source")),
    PL_MEMBER_PROPERTY("SphereRadius", m_fSphereRadius)->AddAttributes(new plDefaultValueAttribute(1.4f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("SphereSoftness", m_fSphereSoftness)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.001f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("PulseAmplitude", m_fPulseAmplitude)->AddAttributes(new plDefaultValueAttribute(0.15f), new plClampValueAttribute(0.0f, 0.9f)),
    PL_MEMBER_PROPERTY("PulsePeriod", m_fPulsePeriod)->AddAttributes(new plDefaultValueAttribute(2.5f), new plClampValueAttribute(0.01f, plVariant()), new plSuffixAttribute(" s")),
    PL_MEMBER_PROPERTY("PeakDensity", m_fPeakDensity)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 64.0f)),
    PL_MEMBER_PROPERTY("PeakTemperature", m_fPeakTemperature)->AddAttributes(new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("AllocThreshold", m_fAllocThreshold)->AddAttributes(new plDefaultValueAttribute(1e-3f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("DensityScale", m_fDensityScale)->AddAttributes(new plGroupAttribute("Rendering"), new plDefaultValueAttribute(4.0f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("Albedo", m_Albedo)->AddAttributes(new plDefaultValueAttribute(plColor::White)),
    PL_MEMBER_PROPERTY("EmissionColor", m_EmissionColor)->AddAttributes(new plDefaultValueAttribute(plColor::Black)),
    PL_MEMBER_PROPERTY("EmissionScale", m_fEmissionScale)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("Anisotropy", m_fAnisotropy)->AddAttributes(new plDefaultValueAttribute(0.2f), new plClampValueAttribute(-0.99f, 0.99f)),
    PL_ENUM_MEMBER_PROPERTY("RenderMode", plVDBVolumeRenderMode, m_RenderMode),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgUpdateLocalBounds, OnUpdateLocalBounds),
    PL_MESSAGE_HANDLER(plMsgExtractRenderData, OnMsgExtractRenderData),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/GpuFluid"),
    new plBoxVisualizerAttribute("Extents", 1.0f, plColorScheme::LightUI(plColorScheme::Cyan)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

plGpuFluidGasVolumeComponent::plGpuFluidGasVolumeComponent() = default;
plGpuFluidGasVolumeComponent::~plGpuFluidGasVolumeComponent() = default;

void plGpuFluidGasVolumeComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_vExtents;
  s << m_fVoxelSize;
  s << m_uiMaxBricks;
  s << m_Quality;
  s << m_DebugSource;
  s << m_fSphereRadius;
  s << m_fSphereSoftness;
  s << m_fPulseAmplitude;
  s << m_fPulsePeriod;
  s << m_fPeakDensity;
  s << m_fPeakTemperature;
  s << m_fAllocThreshold;
  s << m_fDensityScale;
  s << m_Albedo;
  s << m_EmissionColor;
  s << m_fEmissionScale;
  s << m_fAnisotropy;
  s << m_RenderMode;
}

void plGpuFluidGasVolumeComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();

  s >> m_vExtents;
  s >> m_fVoxelSize;
  s >> m_uiMaxBricks;
  s >> m_Quality;
  s >> m_DebugSource;
  s >> m_fSphereRadius;
  s >> m_fSphereSoftness;
  s >> m_fPulseAmplitude;
  s >> m_fPulsePeriod;
  s >> m_fPeakDensity;
  s >> m_fPeakTemperature;
  s >> m_fAllocThreshold;
  s >> m_fDensityScale;
  s >> m_Albedo;
  s >> m_EmissionColor;
  s >> m_fEmissionScale;
  s >> m_fAnisotropy;
  s >> m_RenderMode;
}

void plGpuFluidGasVolumeComponent::OnActivated()
{
  SUPER::OnActivated();

  m_pManager = plGpuFluidGpuManager::GetForWorld(GetWorld()->GetIndex());
  EnsureVolume();
  GetOwner()->UpdateLocalBounds();
}

void plGpuFluidGasVolumeComponent::OnDeactivated()
{
  ReleaseVolume();
  GetOwner()->UpdateLocalBounds();

  SUPER::OnDeactivated();
}

plVec3U32 plGpuFluidGasVolumeComponent::GetBrickGrid() const
{
  const float fBrick = plMath::Max(m_fVoxelSize, 1e-4f) * plGpuFluidBrickSize;
  auto Axis = [&](float fExtent) { return plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Ceil(fExtent / fBrick - 1e-4f)), 1u, 127u); };
  return plVec3U32(Axis(m_vExtents.x), Axis(m_vExtents.y), Axis(m_vExtents.z));
}

plVec3 plGpuFluidGasVolumeComponent::GetGridExtents() const
{
  const plVec3U32 vGrid = GetBrickGrid();
  const float fBrick = plMath::Max(m_fVoxelSize, 1e-4f) * plGpuFluidBrickSize;
  return plVec3(static_cast<float>(vGrid.x), static_cast<float>(vGrid.y), static_cast<float>(vGrid.z)) * fBrick;
}

bool plGpuFluidGasVolumeComponent::GetStats(plGpuFluidGasStats& out_stats) const
{
  return m_pManager != nullptr && m_VolumeId != 0 && m_pManager->GetGasStats(m_VolumeId, out_stats);
}

plGpuFluidGasSettings plGpuFluidGasVolumeComponent::ComputeSettings() const
{
  plGpuFluidGasSettings settings;
  settings.m_vBrickGrid = GetBrickGrid();
  settings.m_uiMaxBricks = m_uiMaxBricks;
  settings.m_Quality = m_Quality;
  return settings;
}

void plGpuFluidGasVolumeComponent::EnsureVolume()
{
  if (m_pManager == nullptr)
    return;

  // Anything that sizes the GPU resources recreates the volume, which then publishes nothing for a frame.
  const plGpuFluidGasSettings settings = ComputeSettings();
  if (m_VolumeId != 0 && settings == m_ActiveSettings)
    return;

  ReleaseVolume();
  m_VolumeId = m_pManager->CreateGasVolume(settings);
  m_ActiveSettings = settings;
}

void plGpuFluidGasVolumeComponent::ReleaseVolume()
{
  if (m_pManager != nullptr && m_VolumeId != 0)
    m_pManager->DestroyVolume(m_VolumeId);

  m_VolumeId = 0;
  m_bPublished = false;
}

void plGpuFluidGasVolumeComponent::Update()
{
  if (m_pManager == nullptr)
    return;

  EnsureVolume();

  m_PulseTime += GetWorld()->GetClock().GetTimeDiff();

  const float fVoxel = plMath::Max(m_fVoxelSize, 1e-4f);
  const plVec3U32 vGrid = m_ActiveSettings.m_vBrickGrid;
  const plVec3 vGridVoxels = plVec3(static_cast<float>(vGrid.x), static_cast<float>(vGrid.y), static_cast<float>(vGrid.z)) * static_cast<float>(plGpuFluidBrickSize);

  const float fPhase = static_cast<float>(m_PulseTime.GetSeconds() / plMath::Max(m_fPulsePeriod, 0.01f));
  const float fPulse = 1.0f + m_fPulseAmplitude * plMath::Sin(plAngle::MakeFromRadian(fPhase * 2.0f * plMath::Pi<float>()));

  plGpuFluidGasFrameInput input;
  input.m_Source = m_DebugSource;
  input.m_Sphere.m_vCenterVoxels = vGridVoxels * 0.5f;
  input.m_Sphere.m_fRadiusVoxels = m_fSphereRadius * fPulse / fVoxel;
  input.m_Sphere.m_fSoftnessVoxels = plMath::Max(m_fSphereSoftness / fVoxel, 1e-3f);
  input.m_Sphere.m_fPeakDensity = m_fPeakDensity;
  input.m_Sphere.m_fPeakTemperature = m_fPeakTemperature;
  input.m_fAllocThreshold = m_fAllocThreshold;

  // Same units as the atlas density, so a faint brick never reads as empty on the coarse taps.
  input.m_fMeanFloor = 1e-4f * plMath::Max(m_fPeakDensity, 1e-3f);

  m_pManager->SetGasFrameInput(m_VolumeId, input);
}

void plGpuFluidGasVolumeComponent::OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg)
{
  if (m_VolumeId == 0)
    return;

  const plBoundingBox box = plBoundingBox::MakeFromCenterAndHalfExtents(plVec3::MakeZero(), GetGridExtents() * 0.5f);
  msg.AddBounds(plBoundingBoxSphere::MakeFromBox(box),
    GetOwner()->IsDynamic() ? plDefaultSpatialDataCategories::RenderDynamic : plDefaultSpatialDataCategories::RenderStatic);
}

void plGpuFluidGasVolumeComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (msg.m_OverrideCategory != plInvalidRenderDataCategory)
    return;

  plGpuFluidGasPublish publish;
  if (m_pManager == nullptr || m_VolumeId == 0 || !m_pManager->GetGasPublish(m_VolumeId, publish))
  {
    // A fresh volume has nothing on the GPU yet.
    m_bPublished = false;
    return;
  }

  auto pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plVDBVolumeGpuRenderData>(GetOwner());

  const plTransform globalTransform = GetOwner()->GetGlobalTransform();
  pRenderData->m_vGlobalPosition = globalTransform.m_vPosition;
  pRenderData->m_GlobalRotation = globalTransform.m_qRotation;
  pRenderData->m_vGlobalScale = globalTransform.m_vScale;

  // The grid is centred on the object, and spans whole bricks.
  pRenderData->m_vExtents = GetGridExtents();
  pRenderData->m_vLocalOffset = plVec3::MakeZero();

  pRenderData->m_fDensityScale = m_fDensityScale;
  pRenderData->m_Albedo = m_Albedo;
  pRenderData->m_EmissionColor = m_EmissionColor;
  pRenderData->m_fEmissionScale = m_fEmissionScale;
  pRenderData->m_fAnisotropy = m_fAnisotropy;
  pRenderData->m_fMaxDensity = plMath::Max(m_fPeakDensity, 1e-3f);
  pRenderData->m_uiFrame = 0;
  pRenderData->m_Channels = plVDBVolumeChannel::Density | plVDBVolumeChannel::Temperature;
  pRenderData->m_RenderMode = m_RenderMode;

  // No procedural detail on simulated gas: the sim carries its own.
  pRenderData->m_fDetailStrength = 0.0f;
  pRenderData->m_fDetailSize = 0.15f;
  pRenderData->m_fDetailEdgeBand = 0.5f;
  pRenderData->m_DetailMotion = plVDBDetailMotion::Static;
  pRenderData->m_vDetailDrift = plVec3::MakeZero();
  pRenderData->m_bLightDetail = false;

  pRenderData->m_hTexture = publish.m_hAtlas;
  pRenderData->m_hIndirection = publish.m_hIndirection;
  pRenderData->m_vBrickGrid = publish.m_vBrickGrid;
  pRenderData->m_vAtlasBricks = publish.m_vAtlasBricks;
  pRenderData->m_BoundsUVW = publish.m_BoundsUVW;
  pRenderData->m_fVoxelSize = plMath::Max(m_fVoxelSize, 1e-4f);
  pRenderData->m_vResolution = publish.m_vBrickGrid * plGpuFluidBrickSize;

  m_bPublished = true;

  // Rewritten on the GPU every frame, so never cached.
  msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::Light, plRenderData::Caching::Never);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Components_GpuFluidGasVolumeComponent);
