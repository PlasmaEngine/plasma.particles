#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Messages/UpdateLocalBoundsMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Configuration/CVar.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasVolumeComponent.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/RenderDataManager.h>

plCVarFloat cvar_GpuFluidDebugStepTime("GpuFluid.Debug.StepTime", 0.0f, plCVarFlags::Default, "Overrides the gas solver's fixed tick, in seconds (0: 1/60 s, 1/30 s on Low).");
plCVarBool cvar_GpuFluidDebugInjectNaN("GpuFluid.Debug.InjectNaN", false, plCVarFlags::Default, "Test hook: the next simulated gas tick writes one NaN into the density at its source's centre, then this turns itself off.");
plCVarBool cvar_GpuFluidDebugSemiLagrangian("GpuFluid.Debug.SemiLagrangian", false, plCVarFlags::Default, "Advects the gas scalars semi-Lagrangian only, without the MacCormack correction, whatever the quality.");

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plGpuFluidGasVolumeComponent, 2, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(4.0f)), new plClampValueAttribute(plVec3(0.1f), plVec3(1024.0f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("VoxelSize", m_fVoxelSize)->AddAttributes(new plDefaultValueAttribute(0.0625f), new plClampValueAttribute(0.005f, 16.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("MaxBricks", m_uiMaxBricks)->AddAttributes(new plDefaultValueAttribute(1536), new plClampValueAttribute(1, 204 * 204 * 204)),
    PL_ENUM_MEMBER_PROPERTY("Quality", plGpuFluidQuality, m_Quality),
    PL_BITFLAGS_MEMBER_PROPERTY("OpenFaces", plGpuFluidFaces, m_OpenFaces),
    PL_MEMBER_PROPERTY("Buoyancy", m_fBuoyancy)->AddAttributes(new plGroupAttribute("Simulation"), new plDefaultValueAttribute(6.0f), new plClampValueAttribute(-100.0f, 100.0f), new plSuffixAttribute(" m/s²")),
    PL_MEMBER_PROPERTY("DensityWeight", m_fDensityWeight)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(-100.0f, 100.0f), new plSuffixAttribute(" m/s²")),
    PL_MEMBER_PROPERTY("Cooling", m_fCooling)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" 1/s")),
    PL_MEMBER_PROPERTY("Dissipation", m_fDissipation)->AddAttributes(new plDefaultValueAttribute(0.03f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" 1/s")),
    PL_MEMBER_PROPERTY("VelocityDissipation", m_fVelocityDissipation)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" 1/s")),
    PL_MEMBER_PROPERTY("DensityMax", m_fDensityMax)->AddAttributes(new plDefaultValueAttribute(4.0f), new plClampValueAttribute(0.01f, 64.0f)),
    PL_MEMBER_PROPERTY("PressureIterations", m_uiPressureIterations)->AddAttributes(new plDefaultValueAttribute(40), new plClampValueAttribute(0, 1000)),
    PL_MEMBER_PROPERTY("EdgeFade", m_fEdgeFade)->AddAttributes(new plDefaultValueAttribute(0.75f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" m")),
    PL_ENUM_MEMBER_PROPERTY("EmitterShape", plGpuFluidEmitterShape, m_EmitterShape)->AddAttributes(new plGroupAttribute("Source")),
    PL_MEMBER_PROPERTY("EmitterOffset", m_vEmitterOffset)->AddAttributes(new plDefaultValueAttribute(plVec3(0.0f, 0.0f, -1.4f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("EmitterRadius", m_fEmitterRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("EmitterHalfExtents", m_vEmitterHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.4f, 0.4f, 0.2f)), new plClampValueAttribute(plVec3(0.0f), plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("EmitterSoftness", m_fEmitterSoftness)->AddAttributes(new plDefaultValueAttribute(0.2f), new plClampValueAttribute(0.001f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("EmissionRate", m_fEmissionRate)->AddAttributes(new plDefaultValueAttribute(5.0f), new plClampValueAttribute(0.0f, 1000.0f), new plSuffixAttribute(" 1/s")),
    PL_MEMBER_PROPERTY("EmitterTemperature", m_fEmitterTemperature)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("EmitterVelocity", m_vEmitterVelocity)->AddAttributes(new plDefaultValueAttribute(plVec3(0.0f, 0.0f, 2.0f)), new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("EmitterNoise", m_fEmitterNoise)->AddAttributes(new plDefaultValueAttribute(0.9f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("EmitterNoiseScale", m_fEmitterNoiseScale)->AddAttributes(new plDefaultValueAttribute(0.3f), new plClampValueAttribute(0.01f, plVariant()), new plSuffixAttribute(" m")),
    PL_ENUM_MEMBER_PROPERTY("DebugSource", plGpuFluidDebugSource, m_DebugSource)->AddAttributes(new plGroupAttribute("Debug Source")),
    PL_MEMBER_PROPERTY("SphereRadius", m_fSphereRadius)->AddAttributes(new plDefaultValueAttribute(1.4f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("SphereSoftness", m_fSphereSoftness)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.001f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("PulseAmplitude", m_fPulseAmplitude)->AddAttributes(new plDefaultValueAttribute(0.15f), new plClampValueAttribute(0.0f, 0.9f)),
    PL_MEMBER_PROPERTY("PulsePeriod", m_fPulsePeriod)->AddAttributes(new plDefaultValueAttribute(2.5f), new plClampValueAttribute(0.01f, plVariant()), new plSuffixAttribute(" s")),
    PL_MEMBER_PROPERTY("PeakDensity", m_fPeakDensity)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 64.0f)),
    PL_MEMBER_PROPERTY("PeakTemperature", m_fPeakTemperature)->AddAttributes(new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("AllocThreshold", m_fAllocThreshold)->AddAttributes(new plDefaultValueAttribute(1e-3f), new plClampValueAttribute(0.0f, plVariant())),
    PL_MEMBER_PROPERTY("DensityScale", m_fDensityScale)->AddAttributes(new plGroupAttribute("Rendering"), new plDefaultValueAttribute(12.0f), new plClampValueAttribute(0.0f, plVariant())),
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

  // Version 2
  s << m_OpenFaces;
  s << m_fBuoyancy;
  s << m_fDensityWeight;
  s << m_fCooling;
  s << m_fDissipation;
  s << m_fVelocityDissipation;
  s << m_fDensityMax;
  s << m_uiPressureIterations;
  s << m_EmitterShape;
  s << m_vEmitterOffset;
  s << m_fEmitterRadius;
  s << m_vEmitterHalfExtents;
  s << m_fEmitterSoftness;
  s << m_fEmissionRate;
  s << m_fEmitterTemperature;
  s << m_vEmitterVelocity;
  s << m_fEmitterNoise;
  s << m_fEmitterNoiseScale;
  s << m_fEdgeFade;
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

  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());
  if (uiVersion < 2)
  {
    // Version 1 had no solver: its volumes showed the analytic sphere, whatever the source said, so keep that.
    m_DebugSource = plGpuFluidDebugSource::AnalyticSphere;
    return;
  }

  s >> m_OpenFaces;
  s >> m_fBuoyancy;
  s >> m_fDensityWeight;
  s >> m_fCooling;
  s >> m_fDissipation;
  s >> m_fVelocityDissipation;
  s >> m_fDensityMax;
  s >> m_uiPressureIterations;
  s >> m_EmitterShape;
  s >> m_vEmitterOffset;
  s >> m_fEmitterRadius;
  s >> m_vEmitterHalfExtents;
  s >> m_fEmitterSoftness;
  s >> m_fEmissionRate;
  s >> m_fEmitterTemperature;
  s >> m_vEmitterVelocity;
  s >> m_fEmitterNoise;
  s >> m_fEmitterNoiseScale;
  s >> m_fEdgeFade;
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
  settings.m_bSimulate = m_DebugSource == plGpuFluidDebugSource::None;

  // P1 allocates the whole box, so the pool needs no more slots than the box has bricks.
  if (settings.m_bSimulate)
    settings.m_uiMaxBricks = plMath::Min(m_uiMaxBricks, settings.m_vBrickGrid.x * settings.m_vBrickGrid.y * settings.m_vBrickGrid.z);

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

  // Extents and VoxelSize are plain properties: a change (with or without a recreate) only reaches culling here.
  if (m_VolumeId != 0 && GetGridExtents() != m_vBoundsExtents)
    GetOwner()->UpdateLocalBounds();

  const plTime timeDiff = GetWorld()->GetClock().GetTimeDiff();
  m_PulseTime += timeDiff;

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

  if (m_DebugSource == plGpuFluidDebugSource::None)
    FillSimulation(timeDiff, input.m_Simulation);

  m_pManager->SetGasFrameInput(m_VolumeId, input);
}

void plGpuFluidGasVolumeComponent::FillSimulation(plTime timeDiff, plGpuFluidGasSimulation& out_simulation)
{
  // Fixed ticks: 60 Hz (30 Hz on Low), at most two a frame; time beyond that is dropped, so a slow frame runs the gas
  // in slow motion rather than spiralling. The harness's fixed 1/30 s step gives exactly two ticks every frame.
  const float fOverride = cvar_GpuFluidDebugStepTime.GetValue();
  const plTime tick = plTime::MakeFromSeconds(fOverride > 0.0f ? fOverride : (m_Quality == plGpuFluidQuality::Low ? plGpuFluidGasTickLow : plGpuFluidGasTick));

  m_SimAccumulator += timeDiff;

  // A hair of slack, so 1/30 s is two ticks of 1/60 s despite rounding.
  plUInt32 uiTicks = static_cast<plUInt32>(plMath::Floor(m_SimAccumulator.GetSeconds() / tick.GetSeconds() + 1e-6));
  if (uiTicks > plGpuFluidMaxGasTicksPerFrame)
  {
    uiTicks = plGpuFluidMaxGasTicksPerFrame;
    m_SimAccumulator = plTime::MakeZero();
  }
  else
  {
    m_SimAccumulator = plMath::Max(m_SimAccumulator - tick * static_cast<double>(uiTicks), plTime::MakeZero());
  }

  const float fVoxel = plMath::Max(m_fVoxelSize, 1e-4f);
  const plVec3U32 vGrid = m_ActiveSettings.m_vBrickGrid;
  const plVec3 vGridVoxels = plVec3(static_cast<float>(vGrid.x), static_cast<float>(vGrid.y), static_cast<float>(vGrid.z)) * static_cast<float>(plGpuFluidBrickSize);

  plGpuFluidGasSimulation& sim = out_simulation;
  sim.m_uiTicks = uiTicks;
  sim.m_fTickSeconds = static_cast<float>(tick.GetSeconds());
  sim.m_fTime = static_cast<float>(m_SimTime.GetSeconds());
  sim.m_fVoxelSize = fVoxel;
  sim.m_OpenFaces = m_OpenFaces;

  m_SimTime += tick * static_cast<double>(uiTicks);
  m_uiTicksRun += uiTicks;

  // Buoyancy pushes against gravity: the physics world's when there is one, else -Z. In the volume's space.
  plVec3 vGravity(0.0f, 0.0f, -9.81f);
  if (const plPhysicsWorldModuleInterface* pPhysics = GetWorld()->GetModuleReadOnly<plPhysicsWorldModuleInterface>())
  {
    const plVec3 vWorldGravity = pPhysics->GetGravity();
    if (!vWorldGravity.IsZero(1e-4f))
      vGravity = vWorldGravity;
  }
  const plVec3 vUpLocal = -(GetOwner()->GetGlobalRotation().GetInverse() * vGravity);
  sim.m_vUp = vUpLocal.GetNormalized();

  sim.m_fBuoyancy = m_fBuoyancy;
  sim.m_fDensityWeight = m_fDensityWeight;
  sim.m_fAmbientTemperature = 0.0f;
  sim.m_fCooling = m_fCooling;
  sim.m_fDissipation = m_fDissipation;
  sim.m_fVelocityDissipation = m_fVelocityDissipation;
  sim.m_fDensityMax = m_fDensityMax;
  sim.m_fEdgeFadeVoxels = m_fEdgeFade / fVoxel;
  sim.m_uiPressureIterations = m_uiPressureIterations;
  sim.m_bMacCormack = m_Quality != plGpuFluidQuality::Low && !cvar_GpuFluidDebugSemiLagrangian.GetValue();
  sim.m_fAllocThreshold = m_fAllocThreshold;
  sim.m_fMeanFloor = 1e-4f;

  sim.m_Emitters.Clear();
  if (m_EmitterShape != plGpuFluidEmitterShape::None)
  {
    // The box is centred on the object; voxel (0,0,0) sits at its -X -Y -Z corner.
    plGpuFluidGasEmitter& emitter = sim.m_Emitters.ExpandAndGetRef();
    emitter.m_Shape = m_EmitterShape;
    emitter.m_vCenterVoxels = vGridVoxels * 0.5f + m_vEmitterOffset / fVoxel;
    emitter.m_fRadiusVoxels = m_fEmitterRadius / fVoxel;
    emitter.m_vHalfExtentsVoxels = m_vEmitterHalfExtents / fVoxel;
    emitter.m_fSoftnessVoxels = plMath::Max(m_fEmitterSoftness / fVoxel, 1e-3f);
    emitter.m_fDensityRate = m_fEmissionRate;
    emitter.m_fTemperature = m_fEmitterTemperature;
    emitter.m_fFuelRate = 0.0f;
    emitter.m_vVelocity = m_vEmitterVelocity;
    emitter.m_bDriveVelocity = true;
    emitter.m_fNoiseStrength = m_fEmitterNoise;
    emitter.m_fNoiseFrequency = fVoxel / plMath::Max(m_fEmitterNoiseScale, 0.01f);

    // One shot, on a frame that runs a tick.
    if (uiTicks > 0 && cvar_GpuFluidDebugInjectNaN.GetValue())
    {
      sim.m_bInjectNaN = true;
      cvar_GpuFluidDebugInjectNaN = false;
    }
  }
}

void plGpuFluidGasVolumeComponent::OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg)
{
  if (m_VolumeId == 0)
    return;

  m_vBoundsExtents = GetGridExtents();
  const plBoundingBox box = plBoundingBox::MakeFromCenterAndHalfExtents(plVec3::MakeZero(), m_vBoundsExtents * 0.5f);
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
  // The march's majorant: the solver clamps density to DensityMax, the analytic sphere peaks at PeakDensity.
  pRenderData->m_fMaxDensity = m_DebugSource == plGpuFluidDebugSource::None ? plMath::Max(m_fDensityMax, 1e-3f) : plMath::Max(m_fPeakDensity, 1e-3f);
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
