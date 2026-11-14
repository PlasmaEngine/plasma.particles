#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Interfaces/WindWorldModule.h>
#include <Core/Messages/UpdateLocalBoundsMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Configuration/CVar.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidColliderComponent.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasEmitterComponent.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasVolumeComponent.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <Shaders/GpuFluid/GpuFluidGasConstants.h>

plCVarFloat cvar_GpuFluidDebugStepTime("GpuFluid.Debug.StepTime", 0.0f, plCVarFlags::Default, "Overrides the gas solver's fixed tick, in seconds (0: the volume's TickRate, by default its quality's: 1/30 s on Low and Medium, 1/60 s from High up).");
plCVarBool cvar_GpuFluidDebugInjectNaN("GpuFluid.Debug.InjectNaN", false, plCVarFlags::Default, "Test hook: the next simulated gas tick writes one NaN into the density at its source's centre, then this turns itself off.");
plCVarBool cvar_GpuFluidDebugSemiLagrangian("GpuFluid.Debug.SemiLagrangian", false, plCVarFlags::Default, "Advects the gas scalars and velocity semi-Lagrangian only, without the MacCormack correction, whatever the quality.");
plCVarInt cvar_GpuFluidGasPressureSolver("GpuFluid.Gas.PressureSolver", 1, plCVarFlags::Default, "Gas pressure solver: 0 red-black SOR (the volume's PressureIterations sweeps), 1 multigrid V-cycles.");
plCVarInt cvar_GpuFluidGasPressureCycles("GpuFluid.Gas.PressureCycles", 0, plCVarFlags::Default, "Overrides the multigrid V-cycles per gas tick (0: the volume's PressureCycles, or its quality's default).");
plCVarInt cvar_GpuFluidGasMGPreSmooth("GpuFluid.Gas.MGPreSmooth", 2, plCVarFlags::Default, "Multigrid: red-black sweeps before each restriction.");
plCVarInt cvar_GpuFluidGasMGPostSmooth("GpuFluid.Gas.MGPostSmooth", 2, plCVarFlags::Default, "Multigrid: red-black sweeps after each prolongation.");
plCVarInt cvar_GpuFluidGasMGCoarseSweeps("GpuFluid.Gas.MGCoarseSweeps", 4, plCVarFlags::Default, "Multigrid: red-black sweeps on the coarsest level.");
plCVarInt cvar_GpuFluidGasMGFinalSweeps("GpuFluid.Gas.MGFinalSweeps", 0, plCVarFlags::Default, "Multigrid: red-black sweeps over the whole fine level after each V-cycle, every neighbour current.");
plCVarFloat cvar_GpuFluidGasMGProlongScale("GpuFluid.Gas.MGProlongScale", 1.0f, plCVarFlags::Default, "Multigrid: scale of every coarse-grid correction.");
plCVarInt cvar_GpuFluidGasMGDownColour("GpuFluid.Gas.MGDownColour", 0, plCVarFlags::Default, "Multigrid: the brick colour that pre-smooths the fine level first (0 or 1).");
plCVarBool cvar_GpuFluidGasMGShortCycles("GpuFluid.Gas.MGShortCycles", false, plCVarFlags::Default, "Multigrid: every V-cycle after a tick's first stops at the 4 cubed level (cheaper; misses N2's 1e-2 under wind).");
plCVarBool cvar_GpuFluidGasProfilePressure("GpuFluid.Gas.ProfilePressure", false, plCVarFlags::Default, "Each step of the gas pressure phase in a render-graph pass of its own, so each is timed.");
plCVarBool cvar_GpuFluidGasExactSampling("GpuFluid.Gas.ExactSampling", false, plCVarFlags::Default, "Gas advection samples with eight loads and weights from the position in the brick, so runs with different pool sizes give the same bits (N11); otherwise hardware filtering.");
plCVarBool cvar_GpuFluidGasProfileKernels("GpuFluid.Gas.ProfileKernels", false, plCVarFlags::Default, "Each kernel of the gas advection and forces phases in a render-graph pass of its own, so each is timed.");
plCVarFloat cvar_GpuFluidGasMGOmega("GpuFluid.Gas.MGOmega", 1.0f, plCVarFlags::Default, "Multigrid: over-relaxation of the smoother.");
plCVarInt cvar_GpuFluidGasMGDenseGroupCells("GpuFluid.Gas.MGDenseGroupCells", 4096, plCVarFlags::Default, "Multigrid: dense levels up to this many cells run in one dispatch of one group; larger ones one thread per cell.");
plCVarInt cvar_GpuFluidGasVelocityMacCormack("GpuFluid.Gas.VelocityMacCormack", -1, plCVarFlags::Default, "MacCormack correction of the gas velocity's advection: -1 the quality's default (High and up), 0 off, 1 on (from Medium up).");
plCVarBool cvar_GpuFluidGasMassCorrection("GpuFluid.Gas.MassCorrection", true, plCVarFlags::Default, "Closed gas volumes (every face closed, AllocateAll) scale their density each tick so the advection keeps the total mass.");
plCVarFloat cvar_GpuFluidGasVorticity("GpuFluid.Gas.Vorticity", -1.0f, plCVarFlags::Default, "Overrides the gas volumes' vorticity confinement epsilon (negative: each volume's own).");
plCVarBool cvar_GpuFluidDebugBricks("GpuFluid.Debug.Bricks", false, plCVarFlags::Default, "Draws the edges of every allocated gas brick into the gas, glowing in the colour of the bucket that keeps it: orange a source's, green with content, cyan flux, blue proximity, grey idle; the gas itself is shown without its glow.");
plCVarBool cvar_GpuFluidDebugBrickLines("GpuFluid.Debug.BrickLines", false, plCVarFlags::Default, "Draws every allocated gas brick as a debug line box (orange a source's, green with content, cyan flux, blue proximity, grey idle). On Metal the VDB march then draws no gas in that view.");
plCVarBool cvar_GpuFluidDebugBrickStats("GpuFluid.Debug.BrickStats", false, plCVarFlags::Default, "Shows the gas volumes' active bricks and pool size on screen.");

namespace
{
  /// The brick view: optical depth of one crossing of a brick edge (about a voxel of edge density). A faint edge hardly
  /// dims the gas behind the lattice.
  constexpr float s_fBrickViewEdgeDepth = 0.01f;
  /// Optical depth per voxel of gas above which the edges inside it fade (GasFinalize): thick smoke keeps its look.
  constexpr float s_fBrickViewGasDepth = 0.02f;
} // namespace

plCVarFloat cvar_GpuFluidDebugBrickGlow("GpuFluid.Debug.BrickGlow", 0.04f, plCVarFlags::Default, "The brick view: how much of its colour one crossing of a brick edge puts on screen. A ray through a plume crosses the edges of many bricks, each adding its glow, so above about 0.05 the lattice tints the dark parts of the gas until they read as transparent (the P2 review's vanishing smoke; the gas data itself is unchanged).");

plCVarFloat cvar_GpuFluidGasVorticityRate("GpuFluid.Gas.VorticityRate", 10.0f, plCVarFlags::Default, "Vorticity confinement: the rate k (1/s) of the acceleration eps dx k (N x w).");

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plGpuFluidGasVolumeComponent, 4, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(4.0f)), new plClampValueAttribute(plVec3(0.1f), plVec3(1024.0f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("VoxelSize", m_fVoxelSize)->AddAttributes(new plDefaultValueAttribute(0.0625f), new plClampValueAttribute(0.005f, 16.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("MaxBricks", m_uiMaxBricks)->AddAttributes(new plDefaultValueAttribute(0), new plClampValueAttribute(0, 204 * 204 * 204)),
    PL_MEMBER_PROPERTY("AllocateAll", m_bAllocateAll)->AddAttributes(new plDefaultValueAttribute(false)),
    PL_ENUM_MEMBER_PROPERTY("Quality", plGpuFluidQuality, m_Quality),
    PL_ENUM_MEMBER_PROPERTY("TickRate", plGpuFluidGasTickRate, m_TickRate),
    PL_BITFLAGS_MEMBER_PROPERTY("OpenFaces", plGpuFluidFaces, m_OpenFaces),
    PL_MEMBER_PROPERTY("Buoyancy", m_fBuoyancy)->AddAttributes(new plGroupAttribute("Simulation"), new plDefaultValueAttribute(6.0f), new plClampValueAttribute(-100.0f, 100.0f), new plSuffixAttribute(" m/s²")),
    PL_MEMBER_PROPERTY("DensityWeight", m_fDensityWeight)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(-100.0f, 100.0f), new plSuffixAttribute(" m/s²")),
    PL_MEMBER_PROPERTY("Cooling", m_fCooling)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" 1/s")),
    PL_MEMBER_PROPERTY("Dissipation", m_fDissipation)->AddAttributes(new plDefaultValueAttribute(0.03f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" 1/s")),
    PL_MEMBER_PROPERTY("VelocityDissipation", m_fVelocityDissipation)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" 1/s")),
    PL_MEMBER_PROPERTY("DensityMax", m_fDensityMax)->AddAttributes(new plDefaultValueAttribute(4.0f), new plClampValueAttribute(0.01f, 64.0f)),
    PL_MEMBER_PROPERTY("PressureIterations", m_uiPressureIterations)->AddAttributes(new plDefaultValueAttribute(40), new plClampValueAttribute(0, 1000)),
    PL_MEMBER_PROPERTY("PressureCycles", m_uiPressureCycles)->AddAttributes(new plDefaultValueAttribute(0), new plClampValueAttribute(0, 16)),
    PL_MEMBER_PROPERTY("EdgeFade", m_fEdgeFade)->AddAttributes(new plDefaultValueAttribute(0.75f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Vorticity", m_fVorticity)->AddAttributes(new plDefaultValueAttribute(0.3f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("WindInfluence", m_fWindInfluence)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("PhysicsBodies", m_bPhysicsBodies)->AddAttributes(new plDefaultValueAttribute(true)),
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

  // Version 3
  s << m_uiPressureCycles;
  s << m_fVorticity;
  s << m_fWindInfluence;
  s << m_bPhysicsBodies;

  // Version 4
  s << m_bAllocateAll;
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

  // Up to version 3 the whole box was allocated.
  m_bAllocateAll = uiVersion < 4;

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

  if (uiVersion < 3)
    return;

  s >> m_uiPressureCycles;
  s >> m_fVorticity;
  s >> m_fWindInfluence;
  s >> m_bPhysicsBodies;

  if (uiVersion < 4)
    return;

  s >> m_bAllocateAll;
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

bool plGpuFluidGasVolumeComponent::ContainsPoint(const plVec3& vGlobalPosition) const
{
  const plVec3 vLocal = GetOwner()->GetGlobalTransform().GetInverse().TransformPosition(vGlobalPosition);
  const plVec3 vHalf = GetGridExtents() * 0.5f;
  return plMath::Abs(vLocal.x) <= vHalf.x && plMath::Abs(vLocal.y) <= vHalf.y && plMath::Abs(vLocal.z) <= vHalf.z;
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

  // The pool: MaxBricks, or the tier's cap. It never needs more slots than the box has bricks.
  if (settings.m_bSimulate)
  {
    const plUInt32 uiCap = m_uiMaxBricks > 0 ? m_uiMaxBricks : GetTierBrickCap(m_Quality);
    settings.m_uiMaxBricks = plMath::Min(uiCap, settings.m_vBrickGrid.x * settings.m_vBrickGrid.y * settings.m_vBrickGrid.z);
  }
  else if (m_uiMaxBricks == 0)
  {
    settings.m_uiMaxBricks = GetTierBrickCap(m_Quality);
  }

  return settings;
}

plUInt32 plGpuFluidGasVolumeComponent::GetTierBrickCap(plGpuFluidQuality::Enum quality)
{
  return plGpuFluidGetGasTier(quality).m_uiBrickCap;
}

plUInt32 plGpuFluidGasVolumeComponent::GetPoolCapacity() const
{
  return m_pManager != nullptr && m_VolumeId != 0 ? m_pManager->GetGasCapacity(m_VolumeId) : 0;
}

bool plGpuFluidGasVolumeComponent::GetGridDebug(plGpuFluidGridDebug& out_debug) const
{
  return m_pManager != nullptr && m_VolumeId != 0 && m_pManager->GetGasGridDebug(m_VolumeId, out_debug);
}

void plGpuFluidGasVolumeComponent::DrawBrickDebug() const
{
  plGpuFluidGridDebug debug;
  if (!GetGridDebug(debug))
    return;

  const plVec3 vGridExtents = GetGridExtents();
  const float fBrick = plMath::Max(m_fVoxelSize, 1e-4f) * plGpuFluidBrickSize;
  const plTransform transform = GetOwner()->GetGlobalTransform();

  // The bucket that keeps a brick, lowest first; an idle brick (no request this frame) is grey.
  const plColor colours[5] = {plColor(1.0f, 0.55f, 0.1f), plColor(0.2f, 0.9f, 0.3f), plColor(0.1f, 0.8f, 1.0f), plColor(0.25f, 0.35f, 1.0f), plColor(0.5f, 0.5f, 0.5f)};

  plDynamicArray<plDebugRenderer::Line> lines;
  lines.Reserve(debug.m_ActiveList.GetCount() * 12);
  for (plUInt32 uiSlot : debug.m_ActiveList)
  {
    if (uiSlot >= debug.m_SlotBrick.GetCount() || debug.m_SlotBrick[uiSlot] == 0xFFFFFFFFu)
      continue;

    const plUInt32 uiPacked = debug.m_SlotBrick[uiSlot];
    const plVec3 vBrick(static_cast<float>(uiPacked & 1023u), static_cast<float>((uiPacked >> 10) & 1023u), static_cast<float>(uiPacked >> 20));
    const plUInt32 uiBuckets = debug.m_SlotBuckets[uiSlot];
    const plUInt32 uiColour = uiBuckets == 0 ? 4u : plMath::FirstBitLow(uiBuckets);

    // Inset a little, so neighbouring boxes do not draw over each other.
    const plVec3 vMin = vBrick * fBrick - vGridExtents * 0.5f + plVec3(fBrick * 0.04f);
    const plVec3 vMax = vMin + plVec3(fBrick * 0.92f);
    const plVec3 c[8] = {plVec3(vMin.x, vMin.y, vMin.z), plVec3(vMax.x, vMin.y, vMin.z), plVec3(vMin.x, vMax.y, vMin.z), plVec3(vMax.x, vMax.y, vMin.z),
      plVec3(vMin.x, vMin.y, vMax.z), plVec3(vMax.x, vMin.y, vMax.z), plVec3(vMin.x, vMax.y, vMax.z), plVec3(vMax.x, vMax.y, vMax.z)};
    const plUInt8 edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges)
      lines.PushBack(plDebugRenderer::Line(c[e[0]], c[e[1]], colours[uiColour]));
  }

  // On Metal the VDB march draws no gas in a view that also draws world-space debug lines (depth tested or not), so
  // GpuFluid.Debug.Bricks draws the bricks into the gas instead (the brick view); these lines are for the editor.
  plDebugRenderer::DrawLines(GetWorld(), lines, plColor::White, transform);
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

  input.m_bAllocateAll = m_bAllocateAll;
  input.m_bDebugGrid = m_bDebugGridReadback || cvar_GpuFluidDebugBrickLines.GetValue();
  // The brick view: everywhere with the cvar, else while a view in the GasBricks render mode draws the volume.
  const bool bBrickViewWanted = m_uiBrickViewFrame != 0 && m_uiBrickViewFrame + 3 >= plRenderWorld::GetFrameCounter();
  input.m_Simulation.m_bBrickView = cvar_GpuFluidDebugBricks.GetValue() || bBrickViewWanted;
  input.m_bDebugGridVirtual = m_bDebugGridReadback;
  input.m_bHashState = m_bDebugHashState;
  input.m_bReverseSlots = m_bDebugReverseSlots;

  m_pManager->SetGasFrameInput(m_VolumeId, input);

  if (cvar_GpuFluidDebugBrickLines.GetValue())
    DrawBrickDebug();

  if (cvar_GpuFluidDebugBrickStats.GetValue())
  {
    plGpuFluidGasStats stats;
    if (GetStats(stats))
    {
      plDebugRenderer::DrawInfoText(GetWorld(), plDebugTextPlacement::TopLeft, "GpuFluid", plFmt("Gas bricks: {} active of {} in the pool, {} with content, {} new, {} freed, {} evicted, {} denied", stats.m_uiAllocActive,
                                                                                              GetPoolCapacity(), stats.m_uiAllocatedBricks, stats.m_uiNewBricks, stats.m_uiFreedBricks, stats.m_uiEvictedBricks, stats.m_uiDeniedBricks));
    }
  }
}

void plGpuFluidGasVolumeComponent::FillSimulation(plTime timeDiff, plGpuFluidGasSimulation& out_simulation)
{
  // Fixed ticks at the tier's rate (30 Hz on Low and Medium, 60 Hz from High up) or the volume's TickRate, at most two
  // a frame; time beyond that is dropped, so a slow frame runs the gas in slow motion rather than spiralling. The
  // harness's fixed 1/30 s step gives one 30 Hz or two 60 Hz ticks every frame.
  const plGpuFluidGasTier& tier = plGpuFluidGetGasTier(m_Quality);
  float fTickSeconds = tier.m_fTickSeconds;
  if (m_TickRate == plGpuFluidGasTickRate::Hz30)
    fTickSeconds = 1.0f / 30.0f;
  else if (m_TickRate == plGpuFluidGasTickRate::Hz60)
    fTickSeconds = 1.0f / 60.0f;
  const float fOverride = cvar_GpuFluidDebugStepTime.GetValue();
  const plTime tick = plTime::MakeFromSeconds(fOverride > 0.0f ? fOverride : fTickSeconds);

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
  // MacCormack scalars from Medium up; MacCormack velocity from High up (Medium advects the velocity semi-Lagrangian:
  // it costs about 0.7 ms a tick at 1000 bricks and gains gas in a closed box, see N4), unless the CVar says otherwise.
  sim.m_bMacCormack = tier.m_bMacCormack && !cvar_GpuFluidDebugSemiLagrangian.GetValue();
  const plInt32 iVelocityMacCormack = cvar_GpuFluidGasVelocityMacCormack.GetValue();
  sim.m_bMacCormackVelocity = sim.m_bMacCormack && (iVelocityMacCormack < 0 ? tier.m_bMacCormackVelocity : iVelocityMacCormack > 0);

  // The closed-box mass correction: only where nothing may leave.
  sim.m_bMassCorrection = cvar_GpuFluidGasMassCorrection.GetValue() && m_bAllocateAll && m_OpenFaces.GetValue() == 0;
  sim.m_fAllocThreshold = m_fAllocThreshold;
  sim.m_fMeanFloor = 1e-4f;
  sim.m_fBrickViewEdgeDensity = s_fBrickViewEdgeDepth / (plMath::Max(m_fDensityScale, 1e-3f) * fVoxel);
  sim.m_fBrickViewGasDensity = s_fBrickViewGasDepth / (plMath::Max(m_fDensityScale, 1e-3f) * fVoxel);
  // Twice the frame's ticks, so the next frame's ticks never carry content past what was allocated for it.
  sim.m_fAllocLookahead = 2.0f * static_cast<float>(plMath::Max(uiTicks, 1u)) * sim.m_fTickSeconds;

  // Vorticity confinement from Medium up.
  const float fVorticity = cvar_GpuFluidGasVorticity.GetValue() >= 0.0f ? cvar_GpuFluidGasVorticity.GetValue() : m_fVorticity;
  sim.m_fVorticity = tier.m_bVorticity ? fVorticity : 0.0f;
  sim.m_fVorticityRate = plMath::Max(cvar_GpuFluidGasVorticityRate.GetValue(), 0.0f);

  // Pressure: the multigrid unless the reference solver is asked for.
  sim.m_PressureSolver = cvar_GpuFluidGasPressureSolver.GetValue() == 0 ? plGpuFluidPressureSolver::RedBlackSOR : plGpuFluidPressureSolver::Multigrid;
  sim.m_uiPressureCycles = cvar_GpuFluidGasPressureCycles.GetValue() > 0 ? static_cast<plUInt32>(cvar_GpuFluidGasPressureCycles.GetValue())
                                                                          : (m_uiPressureCycles > 0 ? m_uiPressureCycles : GetDefaultPressureCycles(m_Quality));
  sim.m_Multigrid.m_uiPreSmooth = static_cast<plUInt32>(plMath::Clamp(cvar_GpuFluidGasMGPreSmooth.GetValue(), 0, 16));
  sim.m_Multigrid.m_uiPostSmooth = static_cast<plUInt32>(plMath::Clamp(cvar_GpuFluidGasMGPostSmooth.GetValue(), 0, 16));
  sim.m_Multigrid.m_uiCoarsestSweeps = static_cast<plUInt32>(plMath::Clamp(cvar_GpuFluidGasMGCoarseSweeps.GetValue(), 1, 256));
  sim.m_Multigrid.m_fOmega = plMath::Clamp(cvar_GpuFluidGasMGOmega.GetValue(), 0.1f, 1.95f);
  sim.m_Multigrid.m_uiFinalSweeps = static_cast<plUInt32>(plMath::Clamp(cvar_GpuFluidGasMGFinalSweeps.GetValue(), 0, 16));
  sim.m_Multigrid.m_fProlongScale = plMath::Clamp(cvar_GpuFluidGasMGProlongScale.GetValue(), 0.0f, 4.0f);
  sim.m_Multigrid.m_uiDownColour = cvar_GpuFluidGasMGDownColour.GetValue() != 0 ? 1u : 0u;
  sim.m_Multigrid.m_bShortLaterCycles = cvar_GpuFluidGasMGShortCycles.GetValue();
  sim.m_Multigrid.m_bProfilePressure = cvar_GpuFluidGasProfilePressure.GetValue();
  sim.m_bProfileKernels = cvar_GpuFluidGasProfileKernels.GetValue();
  sim.m_bExactSampling = cvar_GpuFluidGasExactSampling.GetValue();
  sim.m_Multigrid.m_uiDenseGroupCells = static_cast<plUInt32>(plMath::Max(cvar_GpuFluidGasMGDenseGroupCells.GetValue(), 0));
  sim.m_PoissonTest = m_DebugPoissonTest;

  // Wind: the world's wind at the box's centre, into the volume's space.
  sim.m_vAmbientWind.SetZero();
  if (const plWindWorldModuleInterface* pWind = GetWorld()->GetModuleReadOnly<plWindWorldModuleInterface>())
  {
    const plVec3 vWorldWind = pWind->GetWindAt(GetOwner()->GetGlobalPosition()) * m_fWindInfluence;
    const float fMaxSpeed = static_cast<float>(GPU_FLUID_MAX_SPEED);
    sim.m_vAmbientWind = (GetOwner()->GetGlobalRotation().GetInverse() * vWorldWind).CompClamp(plVec3(-fMaxSpeed), plVec3(fMaxSpeed));
  }
  m_vLastWind = sim.m_vAmbientWind;

  GatherSolids(timeDiff, sim);

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

  // The emitter components inside the box, in the volume's voxels. Their rotation turns only their velocity: the
  // shapes are axis aligned in the volume's space.
  if (const plGpuFluidGasEmitterComponentManager* pEmitters = GetWorld()->GetComponentManager<plGpuFluidGasEmitterComponentManager>())
  {
    const plTransform toLocal = GetOwner()->GetGlobalTransform().GetInverse();
    const plQuat qToLocal = GetOwner()->GetGlobalRotation().GetInverse();
    for (auto it = pEmitters->GetComponents(); it.IsValid(); ++it)
    {
      const plGpuFluidGasEmitterComponent& source = *it;
      if (!source.IsActiveAndInitialized() || !source.m_bEmitting || source.m_Shape == plGpuFluidEmitterShape::None || !ContainsPoint(source.GetOwner()->GetGlobalPosition()))
        continue;
      if (sim.m_Emitters.GetCount() >= sim.m_Emitters.GetCapacity())
        break;

      const plVec3 vScale = source.GetOwner()->GetGlobalScaling();
      plGpuFluidGasEmitter& emitter = sim.m_Emitters.ExpandAndGetRef();
      emitter.m_Shape = source.m_Shape;
      emitter.m_vCenterVoxels = vGridVoxels * 0.5f + toLocal.TransformPosition(source.GetOwner()->GetGlobalPosition()) / fVoxel;
      emitter.m_fRadiusVoxels = source.m_fRadius * plMath::Max(vScale.x, vScale.y, vScale.z) / fVoxel;
      emitter.m_vHalfExtentsVoxels = source.m_vHalfExtents.CompMul(vScale.Abs()) / fVoxel;
      emitter.m_fSoftnessVoxels = plMath::Max(source.m_fSoftness / fVoxel, 1e-3f);
      emitter.m_fDensityRate = source.m_fRate;
      emitter.m_fTemperature = source.m_fTemperature;
      emitter.m_fFuelRate = 0.0f;
      emitter.m_vVelocity = qToLocal * (source.GetOwner()->GetGlobalRotation() * source.m_vVelocity);
      emitter.m_bDriveVelocity = true;
      emitter.m_fNoiseStrength = source.m_fNoise;
      emitter.m_fNoiseFrequency = fVoxel / plMath::Max(source.m_fNoiseScale, 0.01f);
    }
  }

  for (const ExtraSource& extra : m_DebugExtraSources)
  {
    if (sim.m_Emitters.GetCount() >= sim.m_Emitters.GetCapacity())
      break;

    plGpuFluidGasEmitter& emitter = sim.m_Emitters.ExpandAndGetRef();
    emitter.m_Shape = plGpuFluidEmitterShape::Sphere;
    emitter.m_vCenterVoxels = vGridVoxels * 0.5f + extra.m_vOffset / fVoxel;
    emitter.m_fRadiusVoxels = extra.m_fRadius / fVoxel;
    emitter.m_fSoftnessVoxels = plMath::Max(m_fEmitterSoftness / fVoxel, 1e-3f);
    emitter.m_fDensityRate = extra.m_fRate;
    emitter.m_fTemperature = extra.m_fTemperature;
    emitter.m_vVelocity = extra.m_vVelocity;
    emitter.m_bDriveVelocity = true;
    emitter.m_fNoiseStrength = m_fEmitterNoise;
    emitter.m_fNoiseFrequency = fVoxel / plMath::Max(m_fEmitterNoiseScale, 0.01f);
  }
}

plUInt32 plGpuFluidGasVolumeComponent::GetDefaultPressureCycles(plGpuFluidQuality::Enum quality)
{
  return plGpuFluidGetGasTier(quality).m_uiPressureCycles;
}

namespace
{
  /// A solid in the world (centre, rotation, size in metres) as a shape in the volume's voxels.
  struct SolidPose
  {
    plGpuFluidColliderShape::Enum m_Shape = plGpuFluidColliderShape::Box;
    plVec3 m_vCenter = plVec3::MakeZero();
    plQuat m_qRotation = plQuat::MakeIdentity();
    plVec3 m_vHalfExtents = plVec3(0.5f); ///< box
    float m_fRadius = 0.5f;               ///< sphere, capsule
    float m_fHalfHeight = 0.5f;           ///< capsule
  };

  /// Linear and angular velocity (world space) of a transform that went from prev to cur in fSeconds.
  void TransformVelocity(const plTransform& prev, const plTransform& cur, float fSeconds, plVec3& out_vLinear, plVec3& out_vAngular)
  {
    out_vLinear.SetZero();
    out_vAngular.SetZero();
    if (fSeconds <= 1e-6f)
      return;

    out_vLinear = (cur.m_vPosition - prev.m_vPosition) / fSeconds;

    plQuat qDelta = cur.m_qRotation * prev.m_qRotation.GetInverse();
    if (qDelta.w < 0.0f)
      qDelta = plQuat(-qDelta.x, -qDelta.y, -qDelta.z, -qDelta.w); // the short way round
    plVec3 vAxis;
    plAngle angle;
    qDelta.GetRotationAxisAndAngle(vAxis, angle, 1e-6f);
    if (angle.GetRadian() > 1e-6f)
      out_vAngular = vAxis * (angle.GetRadian() / fSeconds);
  }

  plGpuFluidSolidShape MakeShape(const SolidPose& pose, const plVec3& vLinear, const plVec3& vAngular, const plTransform& volume, float fVoxel, const plVec3& vGridVoxels)
  {
    const plQuat qInv = volume.m_qRotation.GetInverse();
    const plVec3 vCenter = (qInv * (pose.m_vCenter - volume.m_vPosition)) / fVoxel + vGridVoxels * 0.5f;
    const plQuat qRel = qInv * pose.m_qRotation;
    const plVec3 a0 = qRel * plVec3(1.0f, 0.0f, 0.0f);
    const plVec3 a1 = qRel * plVec3(0.0f, 1.0f, 0.0f);
    const plVec3 a2 = qRel * plVec3(0.0f, 0.0f, 1.0f);
    const plVec3 vLin = qInv * vLinear;
    const plVec3 vAng = qInv * vAngular;

    plGpuFluidSolidShape shape;
    plMemoryUtils::ZeroFill(&shape, 1);
    shape.LinearVelocity = plVec4(vLin.x, vLin.y, vLin.z, 0.0f);
    shape.AngularVelocity = plVec4(vAng.x, vAng.y, vAng.z, 0.0f);

    plVec3 vReach = plVec3::MakeZero();
    float fType = static_cast<float>(GPU_FLUID_SHAPE_BOX);
    switch (pose.m_Shape)
    {
      case plGpuFluidColliderShape::Sphere:
      {
        fType = static_cast<float>(GPU_FLUID_SHAPE_SPHERE);
        const float r = pose.m_fRadius / fVoxel;
        shape.Axis0 = plVec4(a0.x, a0.y, a0.z, r);
        vReach = plVec3(r);
        break;
      }
      case plGpuFluidColliderShape::Capsule:
      {
        fType = static_cast<float>(GPU_FLUID_SHAPE_CAPSULE);
        const float hh = pose.m_fHalfHeight / fVoxel;
        const float r = pose.m_fRadius / fVoxel;
        shape.Axis0 = plVec4(a2.x, a2.y, a2.z, hh);
        shape.Axis1 = plVec4(a0.x, a0.y, a0.z, r);
        vReach = a2.Abs() * hh + plVec3(r);
        break;
      }
      case plGpuFluidColliderShape::Plane:
      {
        fType = static_cast<float>(GPU_FLUID_SHAPE_PLANE);
        shape.Axis0 = plVec4(a2.x, a2.y, a2.z, 0.0f);
        vReach = plVec3(1e7f);
        break;
      }
      default:
      {
        const plVec3 h = pose.m_vHalfExtents / fVoxel;
        shape.Axis0 = plVec4(a0.x, a0.y, a0.z, h.x);
        shape.Axis1 = plVec4(a1.x, a1.y, a1.z, h.y);
        shape.Axis2 = plVec4(a2.x, a2.y, a2.z, h.z);
        vReach = a0.Abs() * h.x + a1.Abs() * h.y + a2.Abs() * h.z;
        break;
      }
    }

    shape.CenterType = plVec4(vCenter.x, vCenter.y, vCenter.z, fType);
    shape.BoundsMin = plVec4(vCenter.x - vReach.x, vCenter.y - vReach.y, vCenter.z - vReach.z, 0.0f);
    shape.BoundsMax = plVec4(vCenter.x + vReach.x, vCenter.y + vReach.y, vCenter.z + vReach.z, 0.0f);
    return shape;
  }
} // namespace

void plGpuFluidGasVolumeComponent::GatherSolids(plTime timeDiff, plGpuFluidGasSimulation& inout_simulation)
{
  plGpuFluidGasSimulation& sim = inout_simulation;
  sim.m_Solids.Clear();
  sim.m_uiSolidsPerTick = 0;

  const float fVoxel = sim.m_fVoxelSize;
  const plVec3U32 vGrid = m_ActiveSettings.m_vBrickGrid;
  const plVec3 vGridVoxels = plVec3(static_cast<float>(vGrid.x), static_cast<float>(vGrid.y), static_cast<float>(vGrid.z)) * static_cast<float>(plGpuFluidBrickSize);
  const float fSeconds = static_cast<float>(timeDiff.GetSeconds());

  plTransform volume = GetOwner()->GetGlobalTransform();
  volume.m_vScale.Set(1.0f);

  // Each solid's pose last frame and now, and its velocity over the frame.
  struct Moving
  {
    SolidPose m_Prev;
    SolidPose m_Cur;
    plVec3 m_vLinear;
    plVec3 m_vAngular;
  };
  plHybridArray<Moving, GPU_FLUID_MAX_SOLID_SHAPES> solids;

  plHashTable<plUInt64, plTransform> seen;

  // bWorldAligned: the pose is a box of world bounds (a physics body), not a shape attached to the transform.
  auto Track = [&](plUInt64 uiKey, const plTransform& cur, bool bStatic, bool bWorldAligned, SolidPose pose)
  {
    plTransform prev = cur;
    if (!bStatic)
      m_SolidTransforms.TryGetValue(uiKey, prev);
    seen.Insert(uiKey, cur);

    if (solids.GetCount() >= GPU_FLUID_MAX_SOLID_SHAPES)
      return;

    Moving& m = solids.ExpandAndGetRef();
    m.m_Cur = pose;
    m.m_Prev = pose;
    TransformVelocity(prev, cur, fSeconds, m.m_vLinear, m.m_vAngular);

    if (bWorldAligned)
    {
      // The box does not turn with the body: it stays world aligned on every tick and only its centre moves. Its faces
      // still move with the body's material, v + w x (p - origin) about the body's origin, not about the box centre:
      // ShapeVelocity takes r from the centre, so the centre's own velocity goes into the linear part.
      m.m_vLinear += m.m_vAngular.CrossRH(pose.m_vCenter - cur.m_vPosition);
      m.m_Prev.m_vCenter = pose.m_vCenter - m.m_vLinear * fSeconds;
    }
    else
    {
      m.m_Prev.m_vCenter = pose.m_vCenter + (prev.m_vPosition - cur.m_vPosition);
      m.m_Prev.m_qRotation = (prev.m_qRotation * cur.m_qRotation.GetInverse()) * pose.m_qRotation;
    }
  };

  // Authored colliders.
  if (const plGpuFluidColliderComponentManager* pManager = GetWorld()->GetComponentManager<plGpuFluidColliderComponentManager>())
  {
    for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
    {
      const plGpuFluidColliderComponent& collider = *it;
      if (!collider.IsActiveAndInitialized())
        continue;

      const plTransform cur = collider.GetOwner()->GetGlobalTransform();
      const plVec3 vScale = cur.m_vScale.Abs();

      SolidPose pose;
      pose.m_Shape = collider.m_Shape;
      pose.m_vCenter = cur.m_vPosition;
      pose.m_qRotation = cur.m_qRotation;
      pose.m_vHalfExtents = collider.m_vHalfExtents.CompMul(vScale);
      pose.m_fRadius = collider.m_fRadius * (collider.m_Shape == plGpuFluidColliderShape::Capsule ? plMath::Max(vScale.x, vScale.y) : plMath::Max(vScale.x, vScale.y, vScale.z));
      pose.m_fHalfHeight = collider.m_fHalfHeight * vScale.z;
      Track(collider.GetHandle().GetInternalID().m_Data, cur, collider.m_bStatic, false, pose);
    }
  }

  // Dynamic physics bodies in the box, as boxes of their world bounds (P9 replaces these with their real shapes).
  const plPhysicsWorldModuleInterface* pPhysics = m_bPhysicsBodies ? GetWorld()->GetModuleReadOnly<plPhysicsWorldModuleInterface>() : nullptr;
  if (pPhysics != nullptr)
  {
    plPhysicsOverlapResultArray results;
    plPhysicsQueryParameters params(0, plPhysicsShapeType::Dynamic | plPhysicsShapeType::Character);
    pPhysics->QueryShapesInBox(results, GetGridExtents(), volume, params);

    plHybridArray<plGameObjectHandle, 16> actors;
    for (const plPhysicsOverlapResult& result : results.m_Results)
    {
      const plGameObjectHandle hActor = result.m_hActorObject.IsInvalidated() ? result.m_hShapeObject : result.m_hActorObject;
      if (hActor.IsInvalidated() || actors.Contains(hActor))
        continue;
      actors.PushBack(hActor);

      plGameObject* pActor = nullptr;
      if (!GetWorld()->TryGetObject(hActor, pActor))
        continue;

      // Bodies that carry their own collider are already in.
      plGpuFluidColliderComponent* pCollider = nullptr;
      if (pActor->TryGetComponentOfBaseType(pCollider))
        continue;

      const plBoundingBoxSphere bounds = pPhysics->GetWorldSpaceBounds(pActor, 0, plPhysicsShapeType::Dynamic | plPhysicsShapeType::Character, true);
      if (!bounds.IsValid())
        continue;

      const plBoundingBox box = bounds.GetBox();
      plTransform cur = pActor->GetGlobalTransform();
      cur.m_vScale.Set(1.0f);

      SolidPose pose;
      pose.m_Shape = plGpuFluidColliderShape::Box;
      pose.m_vCenter = box.GetCenter();
      pose.m_qRotation = plQuat::MakeIdentity(); // world axis aligned bounds
      pose.m_vHalfExtents = box.GetHalfExtents();
      Track(hActor.GetInternalID().m_Data | (plUInt64(1) << 63), cur, false, true, pose);
    }
  }

  m_SolidTransforms = std::move(seen);
  m_uiLastSolids = solids.GetCount();

  const plUInt32 uiTicks = plMath::Max(sim.m_uiTicks, 1u);
  if (solids.IsEmpty() || sim.m_uiTicks == 0)
    return;

  // Each tick sees the solids where they are at its end, moved along the frame's motion. A solid is kept for every tick
  // when it reaches the box on any of them, so all ticks list the same solids in the same order.
  const plVec3 vGridMax = vGridVoxels + plVec3(1.0f);
  auto Reaches = [&](const plGpuFluidSolidShape& shape)
  {
    return shape.BoundsMax.x >= -1.0f && shape.BoundsMax.y >= -1.0f && shape.BoundsMax.z >= -1.0f && shape.BoundsMin.x <= vGridMax.x && shape.BoundsMin.y <= vGridMax.y &&
           shape.BoundsMin.z <= vGridMax.z;
  };

  plHybridArray<plGpuFluidSolidShape, GPU_FLUID_MAX_SOLID_SHAPES * plGpuFluidMaxGasTicksPerFrame> shapes; // solid-major
  plUInt32 uiKept = 0;
  for (const Moving& m : solids)
  {
    plGpuFluidSolidShape perTick[plGpuFluidMaxGasTicksPerFrame];
    bool bReaches = false;
    for (plUInt32 uiTick = 0; uiTick < uiTicks && uiTick < plGpuFluidMaxGasTicksPerFrame; ++uiTick)
    {
      const float t = static_cast<float>(uiTick + 1) / static_cast<float>(uiTicks);
      SolidPose pose = m.m_Cur;
      pose.m_vCenter = plMath::Lerp(m.m_Prev.m_vCenter, m.m_Cur.m_vCenter, t);
      pose.m_qRotation = plQuat::MakeSlerp(m.m_Prev.m_qRotation, m.m_Cur.m_qRotation, t);
      perTick[uiTick] = MakeShape(pose, m.m_vLinear, m.m_vAngular, volume, fVoxel, vGridVoxels);
      bReaches = bReaches || Reaches(perTick[uiTick]);
    }

    if (!bReaches)
      continue;
    for (plUInt32 uiTick = 0; uiTick < uiTicks && uiTick < plGpuFluidMaxGasTicksPerFrame; ++uiTick)
      shapes.PushBack(perTick[uiTick]);
    ++uiKept;
  }

  const plUInt32 uiTickCount = plMath::Min(uiTicks, plGpuFluidMaxGasTicksPerFrame);
  m_uiLastSolids = uiKept;
  sim.m_uiSolidsPerTick = uiKept;
  sim.m_Solids.SetCount(uiKept * uiTickCount);
  for (plUInt32 uiTick = 0; uiTick < uiTickCount; ++uiTick)
  {
    for (plUInt32 i = 0; i < uiKept; ++i)
      plMemoryUtils::RawByteCopy(&sim.m_Solids[uiTick * uiKept + i], &shapes[i * uiTickCount + uiTick], sizeof(plGpuFluidSolid)); // same layout, checked in GpuFluidGasSolver.cpp
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

  // The brick view is shown with the cvar, or in a view in the GasBricks render mode; the other views draw the gas
  // without it.
  const bool bBrickViewMode = msg.m_pView != nullptr && msg.m_pView->GetViewRenderMode() == plViewRenderMode::GasBricks;
  if (bBrickViewMode)
    m_uiBrickViewFrame = plRenderWorld::GetFrameCounter();
  if (publish.m_bBrickView && !bBrickViewMode && !cvar_GpuFluidDebugBricks.GetValue())
  {
    publish.m_bBrickView = false;
    publish.m_hAtlas = publish.m_hScalarAtlas;
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
  if (publish.m_bBrickView)
  {
    // The brick view's edges glow in their bucket's colour, which GasFinalize writes into the emission channel (gba).
    // An edge crossing has an optical depth of s_fBrickViewEdgeDepth, so this puts about GpuFluid.Debug.BrickGlow of its
    // colour on screen.
    pRenderData->m_EmissionColor = plColor::White;
    pRenderData->m_fEmissionScale = plMath::Max(cvar_GpuFluidDebugBrickGlow.GetValue(), 0.0f) / s_fBrickViewEdgeDepth;
  }
  pRenderData->m_fAnisotropy = m_fAnisotropy;
  // The march's majorant: the solver clamps density to DensityMax, the analytic sphere peaks at PeakDensity.
  pRenderData->m_fMaxDensity = m_DebugSource == plGpuFluidDebugSource::None ? plMath::Max(m_fDensityMax, 1e-3f) : plMath::Max(m_fPeakDensity, 1e-3f);
  pRenderData->m_uiFrame = 0;
  pRenderData->m_Channels = publish.m_bBrickView ? (plVDBVolumeChannel::Density | plVDBVolumeChannel::Emission) : (plVDBVolumeChannel::Density | plVDBVolumeChannel::Temperature);
  // The coarse taps lose the temperature (design plan F4): the brick view needs the full march.
  pRenderData->m_RenderMode = m_RenderMode;
  if (publish.m_bBrickView)
    pRenderData->m_RenderMode = plVDBVolumeRenderMode::RayMarch;

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
