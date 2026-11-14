#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Messages/UpdateLocalBoundsMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidLiquidVolumeComponent.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Configuration/CVar.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidColliderComponent.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidLiquidModifierComponents.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <RendererCore/Pipeline/View.h>
#include <Foundation/Threading/DelegateTask.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidDebugRenderer.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidPass.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidSurfaceRenderer.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_BITFLAGS(plGpuFluidDomainFaces, 1)
  PL_BITFLAGS_CONSTANTS(plGpuFluidDomainFaces::NegX, plGpuFluidDomainFaces::PosX, plGpuFluidDomainFaces::NegY, plGpuFluidDomainFaces::PosY, plGpuFluidDomainFaces::NegZ, plGpuFluidDomainFaces::PosZ)
PL_END_STATIC_REFLECTED_BITFLAGS;

PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidObstacleShape, 1)
  PL_ENUM_CONSTANT(plGpuFluidObstacleShape::Box),
  PL_ENUM_CONSTANT(plGpuFluidObstacleShape::Sphere),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_TYPE(plGpuFluidLiquidFillBox, plNoBase, 1, plRTTIDefaultAllocator<plGpuFluidLiquidFillBox>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Center", m_vCenter),
    PL_MEMBER_PROPERTY("HalfExtents", m_vHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.5f)), new plClampValueAttribute(plVec3(0.0f), plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Velocity", m_vVelocity)->AddAttributes(new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("Jitter", m_fJitter)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Stratified", m_bStratified)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE

PL_BEGIN_STATIC_REFLECTED_TYPE(plGpuFluidLiquidObstacle, plNoBase, 1, plRTTIDefaultAllocator<plGpuFluidLiquidObstacle>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Shape", plGpuFluidObstacleShape, m_Shape),
    PL_MEMBER_PROPERTY("Center", m_vCenter),
    PL_MEMBER_PROPERTY("HalfExtents", m_vHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.25f)), new plClampValueAttribute(plVec3(0.0f), plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.25f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE

PL_BEGIN_STATIC_REFLECTED_TYPE(plGpuFluidLiquidInflow, plNoBase, 1, plRTTIDefaultAllocator<plGpuFluidLiquidInflow>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Center", m_vCenter),
    PL_MEMBER_PROPERTY("Velocity", m_vVelocity)->AddAttributes(new plDefaultValueAttribute(plVec3(0.0f, 0.0f, -1.0f)), new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.02f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("FlowRate", m_fFlowRate)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" l/s")),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE

PL_BEGIN_STATIC_REFLECTED_TYPE(plGpuFluidLiquidDrain, plNoBase, 1, plRTTIDefaultAllocator<plGpuFluidLiquidDrain>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Center", m_vCenter),
    PL_MEMBER_PROPERTY("HalfExtents", m_vHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.1f, 0.1f, 0.05f)), new plClampValueAttribute(plVec3(0.0f), plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Open", m_bOpen)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE

PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidLiquidRenderMode, 1)
  PL_ENUM_CONSTANT(plGpuFluidLiquidRenderMode::Surface),
  PL_ENUM_CONSTANT(plGpuFluidLiquidRenderMode::Points),
  PL_ENUM_CONSTANT(plGpuFluidLiquidRenderMode::SurfaceAndPoints),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_COMPONENT_TYPE(plGpuFluidLiquidVolumeComponent, 4, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(4.0f, 3.0f, 2.0f)), new plClampValueAttribute(plVec3(0.1f), plVec3(1024.0f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("CellSize", m_fCellSize)->AddAttributes(new plDefaultValueAttribute(0.0625f), new plClampValueAttribute(0.005f, 4.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("ParticlesPerCell", m_uiParticlesPerCell)->AddAttributes(new plDefaultValueAttribute(4), new plClampValueAttribute(1, 8)),
    PL_MEMBER_PROPERTY("MaxParticles", m_uiMaxParticles)->AddAttributes(new plDefaultValueAttribute(1024 * 1024), new plClampValueAttribute(256, 16 * 1024 * 1024)),
    PL_ENUM_MEMBER_PROPERTY("Quality", plGpuFluidQuality, m_Quality),
    PL_BITFLAGS_MEMBER_PROPERTY("OpenFaces", plGpuFluidDomainFaces, m_OpenFaces),
    PL_MEMBER_PROPERTY("BakeStaticSolids", m_bBakeStaticSolids)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("AllowSleep", m_bAllowSleep)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("Absorption", m_vAbsorption)->AddAttributes(new plGroupAttribute("Look"), new plDefaultValueAttribute(plVec3(0.45f, 0.07f, 0.04f)), new plClampValueAttribute(plVec3(0.0f), plVariant()), new plSuffixAttribute(" 1/m")),
    PL_MEMBER_PROPERTY("ScatterColor", m_ScatterColor)->AddAttributes(new plDefaultValueAttribute(plColor(0.03f, 0.09f, 0.11f))),
    PL_MEMBER_PROPERTY("SurfaceRoughness", m_fSurfaceRoughness)->AddAttributes(new plDefaultValueAttribute(0.04f), new plClampValueAttribute(0.02f, 1.0f)),
    PL_MEMBER_PROPERTY("RefractionStrength", m_fRefractionStrength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 2.0f)),
    PL_MEMBER_PROPERTY("FlipBlend", m_fFlipBlend)->AddAttributes(new plGroupAttribute("Solver"), new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("MaxSubsteps", m_uiMaxSubsteps)->AddAttributes(new plDefaultValueAttribute(0), new plClampValueAttribute(0, 16)),
    PL_MEMBER_PROPERTY("PressureCycles", m_uiPressureCycles)->AddAttributes(new plDefaultValueAttribute(0), new plClampValueAttribute(0, 32)),
    PL_MEMBER_PROPERTY("WallFriction", m_fWallFriction)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("AlwaysSimulate", m_bAlwaysSimulate)->AddAttributes(new plDefaultValueAttribute(true)),
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
    new plBoxVisualizerAttribute("Extents", 1.0f, plColorScheme::LightUI(plColorScheme::Blue)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

plCVarInt cvar_GpuFluidLiquidRenderMode("GpuFluid.Liquid.RenderMode", 0, plCVarFlags::Default, "How every liquid volume is drawn: 0 the surface, 1 the particles coloured by speed instead (the surface is not built), 2 both. The viewport's Fluid render modes do the same for one view.");
plCVarBool cvar_GpuFluidLiquidShowSleep("GpuFluid.Liquid.ShowSleep", false, plCVarFlags::Default, "Draws every liquid volume's particles coloured by brick state: orange awake, blue asleep (the LiquidSleep view does it for one view).");
plCVarBool cvar_GpuFluidLiquidShowSolids("GpuFluid.Liquid.ShowSolids", false, plCVarFlags::Default, "Draws the triangles each liquid volume's static solids bake caught (the LiquidStaticSolids view does it for one view).");
plCVarFloat cvar_GpuFluidLiquidDebugTopSpeed("GpuFluid.Liquid.DebugTopSpeed", 0.0f, plCVarFlags::Default, "Speed (m/s) at the top of the particles' colour ramp; 0: 3 m/s (or what the volume was given from code).");
plCVarFloat cvar_GpuFluidLiquidDebugPointRadius("GpuFluid.Liquid.DebugPointRadius", 0.0f, plCVarFlags::Default, "Radius (m) the particles are drawn with; 0: from the cell size and particles per cell.");
plCVarInt cvar_GpuFluidLiquidSurfaceDebug("GpuFluid.Liquid.SurfaceDebug", 0, plCVarFlags::Default, "Liquid surface debug view: 0 off, 1 normals, 2 path length through the liquid.");
plCVarInt cvar_GpuFluidRebake("GpuFluid.Rebake", 0, plCVarFlags::Default, "Change the value to bake the static solids of every liquid volume again from the world's collision meshes.");

plGpuFluidLiquidVolumeComponent::plGpuFluidLiquidVolumeComponent() = default;
plGpuFluidLiquidVolumeComponent::~plGpuFluidLiquidVolumeComponent() = default;

void plGpuFluidLiquidVolumeComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_vExtents;
  s << m_fCellSize;
  s << m_uiParticlesPerCell;
  s << m_uiMaxParticles;
  s << m_Quality;
  s << m_OpenFaces;

  s << m_FillBoxes.GetCount();
  for (const plGpuFluidLiquidFillBox& box : m_FillBoxes)
  {
    s << box.m_vCenter;
    s << box.m_vHalfExtents;
    s << box.m_vVelocity;
    s << box.m_fJitter;
    s << box.m_bStratified;
  }

  s << m_Obstacles.GetCount();
  for (const plGpuFluidLiquidObstacle& obstacle : m_Obstacles)
  {
    s << obstacle.m_Shape;
    s << obstacle.m_vCenter;
    s << obstacle.m_vHalfExtents;
    s << obstacle.m_fRadius;
  }

  s << m_fFlipBlend;
  s << m_uiMaxSubsteps;
  s << m_uiPressureCycles;
  s << m_fWallFriction;
  s << m_bAlwaysSimulate;
  s << m_fDebugPointRadius;
  s << m_fDebugTopSpeed;

  // Version 3.
  s << m_Inflows.GetCount();
  for (const plGpuFluidLiquidInflow& inflow : m_Inflows)
  {
    s << inflow.m_vCenter;
    s << inflow.m_vVelocity;
    s << inflow.m_fRadius;
    s << inflow.m_fFlowRate;
  }
  s << m_RenderMode;
  s << m_vAbsorption;
  s << m_ScatterColor;
  s << m_fSurfaceRoughness;
  s << m_fRefractionStrength;

  // Version 4.
  s << m_Drains.GetCount();
  for (const plGpuFluidLiquidDrain& drain : m_Drains)
  {
    s << drain.m_vCenter;
    s << drain.m_vHalfExtents;
    s << drain.m_bOpen;
  }
  s << m_bBakeStaticSolids;
  s << m_bAllowSleep;
}

void plGpuFluidLiquidVolumeComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());
  plStreamReader& s = inout_stream.GetStream();

  s >> m_vExtents;
  s >> m_fCellSize;
  s >> m_uiParticlesPerCell;
  s >> m_uiMaxParticles;
  s >> m_Quality;
  s >> m_OpenFaces;

  plUInt32 uiCount = 0;
  s >> uiCount;
  m_FillBoxes.SetCount(uiCount);
  for (plGpuFluidLiquidFillBox& box : m_FillBoxes)
  {
    s >> box.m_vCenter;
    s >> box.m_vHalfExtents;
    s >> box.m_vVelocity;
    s >> box.m_fJitter;
    if (uiVersion >= 2)
      s >> box.m_bStratified;
  }

  s >> uiCount;
  m_Obstacles.SetCount(uiCount);
  for (plGpuFluidLiquidObstacle& obstacle : m_Obstacles)
  {
    s >> obstacle.m_Shape;
    s >> obstacle.m_vCenter;
    s >> obstacle.m_vHalfExtents;
    s >> obstacle.m_fRadius;
  }

  s >> m_fFlipBlend;
  s >> m_uiMaxSubsteps;
  s >> m_uiPressureCycles;
  s >> m_fWallFriction;
  s >> m_bAlwaysSimulate;
  s >> m_fDebugPointRadius;
  s >> m_fDebugTopSpeed;

  if (uiVersion >= 3)
  {
    s >> uiCount;
    m_Inflows.SetCount(uiCount);
    for (plGpuFluidLiquidInflow& inflow : m_Inflows)
    {
      s >> inflow.m_vCenter;
      s >> inflow.m_vVelocity;
      s >> inflow.m_fRadius;
      s >> inflow.m_fFlowRate;
    }
    s >> m_RenderMode;
    s >> m_vAbsorption;
    s >> m_ScatterColor;
    s >> m_fSurfaceRoughness;
    s >> m_fRefractionStrength;
  }

  if (uiVersion >= 4)
  {
    s >> uiCount;
    m_Drains.SetCount(uiCount);
    for (plGpuFluidLiquidDrain& drain : m_Drains)
    {
      s >> drain.m_vCenter;
      s >> drain.m_vHalfExtents;
      s >> drain.m_bOpen;
    }
    s >> m_bBakeStaticSolids;
    s >> m_bAllowSleep;
  }
}

void plGpuFluidLiquidVolumeComponent::OnActivated()
{
  SUPER::OnActivated();

  m_pManager = plGpuFluidGpuManager::GetForWorld(GetWorld()->GetIndex());
  EnsureVolume();
  ResetSimulation();
  GetOwner()->UpdateLocalBounds();

  // Gathered on the first update: the solids around may be created after this component.
  m_bSolidsDirty = true;
  m_iLastRebakeCVar = cvar_GpuFluidRebake.GetValue();
}

void plGpuFluidLiquidVolumeComponent::OnDeactivated()
{
  if (m_BakeTaskGroup.IsValid())
    plTaskSystem::WaitForGroup(m_BakeTaskGroup);
  m_BakeTaskGroup.Invalidate();
  m_pBakeJob = nullptr;
  m_pSolidBake = nullptr;

  ReleaseVolume();
  GetOwner()->UpdateLocalBounds();

  SUPER::OnDeactivated();
}

void plGpuFluidLiquidVolumeComponent::ResetSimulation()
{
  ++m_uiResetCounter;
  m_uiTicksSinceReset = 0;
}

plVec3U32 plGpuFluidLiquidVolumeComponent::GetCells() const
{
  const float fBrick = plMath::Max(m_fCellSize, 1e-3f) * 8.0f;
  auto Axis = [&](float fExtent) { return 8u * plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Ceil(fExtent / fBrick - 1e-4f)), 1u, 256u); };
  return plVec3U32(Axis(m_vExtents.x), Axis(m_vExtents.y), Axis(m_vExtents.z));
}

plVec3 plGpuFluidLiquidVolumeComponent::GetDomainSize() const
{
  const plVec3U32 vCells = GetCells();
  return plVec3(static_cast<float>(vCells.x), static_cast<float>(vCells.y), static_cast<float>(vCells.z)) * plMath::Max(m_fCellSize, 1e-3f);
}

plVec3 plGpuFluidLiquidVolumeComponent::GetDomainOrigin() const
{
  return GetOwner()->GetGlobalPosition() - GetDomainSize() * 0.5f;
}

bool plGpuFluidLiquidVolumeComponent::GetStats(plGpuFluidLiquidStats& out_stats) const
{
  return m_pManager != nullptr && m_VolumeId != 0 && m_pManager->GetLiquidStats(m_VolumeId, out_stats);
}

plUInt32 plGpuFluidLiquidVolumeComponent::GetTierMaxSubsteps(plGpuFluidQuality::Enum quality)
{
  // Plan amendment A2: Medium at most 3 substeps; beyond that the tick simulates less time (slow motion) instead of
  // breaking the CFL number. Low keeps the plan's single substep, High and Ultra the plan's 3.
  switch (quality)
  {
    case plGpuFluidQuality::Low:
      return 1;
    default:
      return 3;
  }
}

plUInt32 plGpuFluidLiquidVolumeComponent::GetTierPressureIterations(plGpuFluidQuality::Enum quality)
{
  // MGPCG iterations per substep, one multigrid V-cycle each (plan amendment A2: a small fixed count, measured in P8;
  // see plGpuFluidLiquidFrameInput::m_bPcg). Medium 4: median relative residual 2e-4 at the Medium room, 95th
  // percentile 7e-4. High 6: the first count that kept every tick below 1e-3 in both the room and the dam break.
  // Low 3 and Ultra 8 are not measured on their own tiers.
  switch (quality)
  {
    case plGpuFluidQuality::Low:
      return 3;
    case plGpuFluidQuality::High:
      return 6;
    case plGpuFluidQuality::Ultra:
      return 8;
    default:
      return 4;
  }
}

plGpuFluidLiquidSettings plGpuFluidLiquidVolumeComponent::ComputeSettings() const
{
  plGpuFluidLiquidSettings settings;
  settings.m_vCells = GetCells();
  settings.m_uiMaxParticles = m_uiMaxParticles;
  settings.m_Quality = m_Quality;
  settings.m_fCellSize = plMath::Max(m_fCellSize, 1e-3f);
  settings.m_uiParticlesPerCell = plMath::Clamp<plUInt32>(m_uiParticlesPerCell, 1, 8);
  return settings;
}

void plGpuFluidLiquidVolumeComponent::EnsureVolume()
{
  if (m_pManager == nullptr)
    return;

  // Anything that sizes the GPU resources recreates the volume, which then starts over from its fill boxes.
  const plGpuFluidLiquidSettings settings = ComputeSettings();
  if (m_VolumeId != 0 && settings == m_ActiveSettings)
    return;

  ReleaseVolume();
  m_VolumeId = m_pManager->CreateLiquidVolume(settings);
  m_ActiveSettings = settings;
  ResetSimulation();
}

void plGpuFluidLiquidVolumeComponent::ReleaseVolume()
{
  if (m_pManager != nullptr && m_VolumeId != 0)
    m_pManager->DestroyVolume(m_VolumeId);

  m_VolumeId = 0;
  m_bPublished = false;
}

void plGpuFluidLiquidVolumeComponent::UpdateStaticSolids()
{
  if (!m_bBakeStaticSolids)
  {
    m_pSolidBake = nullptr;
    return;
  }

  // A finished task hands over its bake. One built for another domain (changed while it ran) is dropped.
  if (m_BakeTaskGroup.IsValid() && plTaskSystem::IsTaskGroupFinished(m_BakeTaskGroup))
  {
    m_BakeTaskGroup.Invalidate();
    if (m_pBakeJob != nullptr && m_pBakeJob->m_pResult != nullptr)
    {
      m_pSolidBake = m_pBakeJob->m_pResult;
      ++m_uiSolidBakesBuilt;
      plLog::Dev("GpuFluid: static solids of '{}': {} mesh objects, {} of {} triangles in the domain, {} bricks to bake (up to {} triangles each), binned in {} ms", GetOwner()->GetName(),
        m_pSolidBake->m_uiMeshObjects, m_pSolidBake->GetTriangleCount(), m_pSolidBake->m_uiSourceTriangles, m_pSolidBake->m_BakeBricks.GetCount(), m_pSolidBake->m_uiMaxBinCount,
        plArgF(m_pSolidBake->m_fBuildMs, 2));
    }
    m_pBakeJob = nullptr;
  }

  // The domain moved or was resized: the old bake no longer fits.
  const plVec3 vOrigin = GetDomainOrigin();
  const plVec3U32 vCells = m_ActiveSettings.m_vCells;
  if (!(vCells == m_vBakedCells) || m_ActiveSettings.m_fCellSize != m_fBakedCellSize || !(vOrigin - m_vBakedDomainOrigin).IsZero(1e-4f))
    m_bSolidsDirty = true;

  if (cvar_GpuFluidRebake.GetValue() != m_iLastRebakeCVar)
  {
    m_iLastRebakeCVar = cvar_GpuFluidRebake.GetValue();
    m_bSolidsDirty = true;
  }

  // A static collider added, removed, moved or resized: bake again (the other collision meshes need the cvar).
  plUInt64 uiColliderHash = 0;
  if (const plGpuFluidColliderComponentManager* pColliders = GetWorld()->GetComponentManager<plGpuFluidColliderComponentManager>())
  {
    for (auto it = pColliders->GetComponents(); it.IsValid(); ++it)
    {
      const plUInt64 uiHash = it->GetBakeHash();
      if (uiHash != 0)
        uiColliderHash = plHashingUtils::xxHash64(&uiHash, sizeof(uiHash), uiColliderHash);
    }
  }
  if (uiColliderHash != m_uiColliderHash)
  {
    m_uiColliderHash = uiColliderHash;
    m_bSolidsDirty = true;
  }

  if (!m_bSolidsDirty || m_BakeTaskGroup.IsValid())
    return;

  m_bSolidsDirty = false;
  m_vBakedCells = vCells;
  m_fBakedCellSize = m_ActiveSettings.m_fCellSize;
  m_vBakedDomainOrigin = vOrigin;

  // The extraction sends plMsgExtractGeometry to every object and the CPU meshes are read here, on the update thread;
  // welding, pseudonormals and binning run on a task.
  plWorldGeoExtractionUtil::MeshObjectList objects;
  plWorldGeoExtractionUtil::ExtractWorldGeometry(objects, *GetWorld(), plWorldGeoExtractionUtil::ExtractionMode::CollisionMesh);

  m_pBakeJob = PL_DEFAULT_NEW(plGpuFluidLiquidSolidBakeJob);
  m_pBakeJob->m_vDomainOrigin = vOrigin;
  m_pBakeJob->m_vCells = vCells;
  m_pBakeJob->m_fCellSize = m_fBakedCellSize;
  const plVec3 vSize = GetDomainSize();
  plGpuFluidLiquidSolidBake::GatherSourceMeshes(objects, vOrigin, vOrigin + vSize, GPU_FLUID_LIQUID_SDF_BAND_CELLS * m_fBakedCellSize, m_pBakeJob->m_Meshes);

  plGpuFluidLiquidSolidBakeJob* pJob = m_pBakeJob.Borrow();
  plSharedPtr<plTask> pTask = PL_DEFAULT_NEW(plDelegateTask<void>, "GpuFluid Static Solids", plTaskNesting::Never, [pJob]() { pJob->Run(); });
  m_BakeTaskGroup = plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning);
}

void plGpuFluidLiquidVolumeComponent::Update()
{
  if (m_pManager == nullptr)
    return;

  EnsureVolume();
  if (m_VolumeId == 0)
    return;

  UpdateStaticSolids();

  if (GetDomainSize() != m_vBoundsSize)
    GetOwner()->UpdateLocalBounds();

  const float fDx = m_ActiveSettings.m_fCellSize;
  const plVec3 vSize = GetDomainSize();
  const plVec3U32 vCells = m_ActiveSettings.m_vCells;
  const plVec3 vHalf = vSize * 0.5f;

  plGpuFluidLiquidFrameInput input = m_Tuning;
  input.m_Fills.Clear();
  input.m_Shapes.Clear();

  const float fDt = static_cast<float>(GetWorld()->GetClock().GetTimeDiff().GetSeconds());
  if (fDt > 0.0f)
  {
    ++m_uiTick;
    ++m_uiTicksSinceReset;
  }

  input.m_uiTick = m_uiTick;
  input.m_uiResetCounter = m_uiResetCounter;
  input.m_fDt = plMath::Min(fDt, 1.0f / 15.0f);
  input.m_bSimulate = fDt > 0.0f;

  // Without AlwaysSimulate a volume nobody has drawn for a while holds still.
  if (!m_bAlwaysSimulate && m_uiLastExtractedFrame + 30 < plRenderWorld::GetFrameCounter())
    input.m_bSimulate = false;

  input.m_vGravity = plVec3(0.0f, 0.0f, -9.81f);
  if (const plPhysicsWorldModuleInterface* pPhysics = GetWorld()->GetModuleReadOnly<plPhysicsWorldModuleInterface>())
    input.m_vGravity = pPhysics->GetGravity();

  input.m_vDomainOrigin = GetDomainOrigin();
  input.m_fFlipBlend = m_fFlipBlend;
  input.m_uiMaxSubsteps = m_uiMaxSubsteps > 0 ? m_uiMaxSubsteps : GetTierMaxSubsteps(m_Quality);
  input.m_uiPressureCycles = m_uiPressureCycles > 0 ? m_uiPressureCycles : GetTierPressureIterations(m_Quality);
  input.m_fWallFriction = m_fWallFriction;
  input.m_uiOpenFaces = m_OpenFaces.GetValue();

  plHybridArray<plGpuFluidLiquidFillBox, 8> fills;
  GatherModifiers(input, fills);

  // Fill boxes: across x and y the cells whose centre is inside. Up the z axis the cells from the first whose centre
  // is inside to the box top, the last of them partly filled when the top lies inside a cell (so the surface does
  // not snap to a cell face).
  for (const plGpuFluidLiquidFillBox& box : fills)
  {
    const plVec3 vMin = (box.m_vCenter - box.m_vHalfExtents + vHalf) / fDx;
    const plVec3 vMax = (box.m_vCenter + box.m_vHalfExtents + vHalf) / fDx;
    auto First = [](float f, plUInt32 n) { return static_cast<plUInt32>(plMath::Clamp(plMath::Ceil(f - 0.5f), 0.0f, static_cast<float>(n))); };
    auto End = [](float f, plUInt32 n) { return static_cast<plUInt32>(plMath::Clamp(plMath::Floor(f - 0.5f) + 1.0f, 0.0f, static_cast<float>(n))); };

    const plVec3U32 vFirst(First(vMin.x, vCells.x), First(vMin.y, vCells.y), First(vMin.z, vCells.z));
    plVec3U32 vEnd(End(vMax.x, vCells.x), End(vMax.y, vCells.y), 0);

    // Height in cells above the first layer, rounded to 1/1000 of a cell against float noise.
    const float fTop = plMath::Min(vMax.z, static_cast<float>(vCells.z));
    const float fLayers = plMath::Round(plMath::Max(fTop - static_cast<float>(vFirst.z), 0.0f) * 1000.0f) / 1000.0f;
    const float fFull = plMath::Floor(fLayers);
    float fTopFill = fLayers - fFull;
    vEnd.z = vFirst.z + static_cast<plUInt32>(fFull);
    if (fTopFill >= 0.125f)
      ++vEnd.z;
    else
      fTopFill = 1.0f;

    if (vEnd.x <= vFirst.x || vEnd.y <= vFirst.y || vEnd.z <= vFirst.z)
      continue;

    plGpuFluidLiquidFill& fill = input.m_Fills.ExpandAndGetRef();
    fill.m_vCellMin = vFirst;
    fill.m_vCellCount = vEnd - vFirst;
    fill.m_vVelocity = box.m_vVelocity;
    fill.m_fJitter = box.m_fJitter;
    fill.m_bStratified = box.m_bStratified;
    fill.m_fTopLayerFill = fTopFill;
  }

  for (const plGpuFluidLiquidObstacle& obstacle : m_Obstacles)
  {
    if (input.m_Shapes.GetCount() >= GPU_FLUID_LIQUID_MAX_SHAPES)
      break;

    plGpuFluidLiquidShape& shape = input.m_Shapes.ExpandAndGetRef();
    const plVec3 vCenter = obstacle.m_vCenter + vHalf;
    const bool bSphere = obstacle.m_Shape == plGpuFluidObstacleShape::Sphere;
    shape.CenterType = plVec4(vCenter.x, vCenter.y, vCenter.z, bSphere ? static_cast<float>(GPU_FLUID_LIQUID_SHAPE_SPHERE) : static_cast<float>(GPU_FLUID_LIQUID_SHAPE_BOX));
    shape.Size = bSphere ? plVec4(obstacle.m_fRadius, 0, 0, 0) : plVec4(obstacle.m_vHalfExtents.x, obstacle.m_vHalfExtents.y, obstacle.m_vHalfExtents.z, 0.0f);
  }

  input.m_pSolidBake = m_pSolidBake;
  input.m_bAllowSleep = m_bAllowSleep && m_Tuning.m_bAllowSleep;
  // The surface is built unless every view draws only the particles (the views showing particles alone still skip it).
  input.m_bSurface = m_RenderMode != plGpuFluidLiquidRenderMode::Points && cvar_GpuFluidLiquidRenderMode.GetValue() != 1;

  m_pManager->SetLiquidFrameInput(m_VolumeId, input);

  if (cvar_GpuFluidLiquidShowSolids.GetValue())
    DrawStaticSolids(GetWorld());
}

void plGpuFluidLiquidVolumeComponent::GatherModifiers(plGpuFluidLiquidFrameInput& inout_input, plDynamicArray<plGpuFluidLiquidFillBox>& out_fills)
{
  // The solver takes four sources (plGpuFluidLiquidSolver::s_uiMaxSources).
  constexpr plUInt32 s_uiMaxLiquidSources = 4;

  const plVec3 vOrigin = GetDomainOrigin();
  const plVec3 vSize = GetDomainSize();
  const plVec3 vHalf = vSize * 0.5f;
  const plVec3 vCenter = vOrigin + vHalf;
  const plBoundingBox domain = plBoundingBox::MakeFromMinMax(vOrigin, vOrigin + vSize);
  const plWorld& world = *GetWorld();

  // Sources: the code-set inflows (centred on the volume), then the source components whose disc centre is inside.
  inout_input.m_Sources.Clear();
  for (const plGpuFluidLiquidInflow& inflow : m_Inflows)
  {
    if (inout_input.m_Sources.GetCount() >= s_uiMaxLiquidSources)
      break;
    plGpuFluidLiquidSource& source = inout_input.m_Sources.ExpandAndGetRef();
    source.m_vCenter = inflow.m_vCenter + vHalf;
    source.m_vVelocity = inflow.m_vVelocity;
    source.m_fRadius = inflow.m_fRadius;
    source.m_fFlowRate = inflow.m_fFlowRate * 1.0e-3f;
  }
  if (const plGpuFluidLiquidSourceComponentManager* pSources = world.GetComponentManager<plGpuFluidLiquidSourceComponentManager>())
  {
    for (auto it = pSources->GetComponents(); it.IsValid(); ++it)
    {
      if (!it->IsActiveAndInitialized() || !it->m_bFlowing || !domain.Contains(it->GetWorldCenter()))
        continue;
      if (inout_input.m_Sources.GetCount() >= s_uiMaxLiquidSources)
        break;
      plGpuFluidLiquidSource& source = inout_input.m_Sources.ExpandAndGetRef();
      source.m_vCenter = it->GetWorldCenter() - vOrigin;
      source.m_vVelocity = it->GetWorldVelocity();
      source.m_fRadius = it->GetWorldRadius();
      source.m_fFlowRate = it->m_fFlowRate * 1.0e-3f;
    }
  }

  // Drains: the open ones, as boxes from the domain's corner.
  inout_input.m_Drains.Clear();
  auto AddDrain = [&](const plVec3& vDomainCenter, const plVec3& vHalfExtents)
  {
    if (inout_input.m_Drains.GetCount() >= GPU_FLUID_LIQUID_MAX_DRAINS)
      return;
    plGpuFluidLiquidShape& box = inout_input.m_Drains.ExpandAndGetRef();
    box.CenterType = plVec4(vDomainCenter.x, vDomainCenter.y, vDomainCenter.z, static_cast<float>(GPU_FLUID_LIQUID_SHAPE_BOX));
    box.Size = plVec4(vHalfExtents.x, vHalfExtents.y, vHalfExtents.z, 0.0f);
  };
  for (const plGpuFluidLiquidDrain& drain : m_Drains)
  {
    if (drain.m_bOpen)
      AddDrain(drain.m_vCenter + vHalf, drain.m_vHalfExtents);
  }
  if (const plGpuFluidLiquidDrainComponentManager* pDrains = world.GetComponentManager<plGpuFluidLiquidDrainComponentManager>())
  {
    for (auto it = pDrains->GetComponents(); it.IsValid(); ++it)
    {
      if (!it->IsActiveAndInitialized() || !it->m_bOpen)
        continue;
      const plBoundingBox box = it->GetWorldBox();
      if (domain.Overlaps(box))
        AddDrain(box.GetCenter() - vOrigin, box.GetHalfExtents());
    }
  }

  // Fills, centred on the volume like the code-set ones. A change to the fill components restarts the simulation, so
  // what the editor shows is what the scene starts with.
  out_fills = m_FillBoxes;
  plUInt64 uiFillHash = 0;
  if (const plGpuFluidLiquidFillComponentManager* pFills = world.GetComponentManager<plGpuFluidLiquidFillComponentManager>())
  {
    for (auto it = pFills->GetComponents(); it.IsValid(); ++it)
    {
      if (!it->IsActiveAndInitialized())
        continue;
      const plBoundingBox box = it->GetWorldBox();
      if (!domain.Overlaps(box))
        continue;
      plGpuFluidLiquidFillBox& fill = out_fills.ExpandAndGetRef();
      fill.m_vCenter = box.GetCenter() - vCenter;
      fill.m_vHalfExtents = box.GetHalfExtents();
      fill.m_vVelocity = it->GetWorldVelocity();
      fill.m_fJitter = it->m_fJitter;
      fill.m_bStratified = it->m_bStratified;
      const plVec4 key[3] = {plVec4(fill.m_vCenter.x, fill.m_vCenter.y, fill.m_vCenter.z, fill.m_fJitter), plVec4(fill.m_vHalfExtents.x, fill.m_vHalfExtents.y, fill.m_vHalfExtents.z, fill.m_bStratified ? 1.0f : 0.0f),
        plVec4(fill.m_vVelocity.x, fill.m_vVelocity.y, fill.m_vVelocity.z, 0.0f)};
      uiFillHash = plHashingUtils::xxHash64(key, sizeof(key), uiFillHash);
    }
  }
  if (uiFillHash != m_uiFillHash)
  {
    m_uiFillHash = uiFillHash;
    ResetSimulation();
  }
}

void plGpuFluidLiquidVolumeComponent::DrawStaticSolids(const plDebugRendererContext& context) const
{
  if (m_pSolidBake == nullptr)
    return;

  // The bake's triangles (domain-relative), tinted per object so neighbours stay apart, capped for huge levels.
  constexpr plUInt32 uiMaxTriangles = 65536;
  const plUInt32 uiCount = plMath::Min(m_pSolidBake->GetTriangleCount(), uiMaxTriangles);
  const float* pData = m_pSolidBake->m_Triangles.GetData();
  const plVec3 vOrigin = m_vBakedDomainOrigin;

  plDynamicArray<plDebugRenderer::Triangle> triangles;
  triangles.Reserve(uiCount);
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const float* t = pData + i * plGpuFluidLiquidSolidBake::s_uiFloatsPerTriangle;
    // 5 mm out along the face normal, so the triangle does not fight the mesh it came from for depth.
    const plVec3 vPush = vOrigin + plVec3(t[9], t[10], t[11]) * 0.005f;
    plDebugRenderer::Triangle& tri = triangles.ExpandAndGetRef();
    tri.m_position[0] = vPush + plVec3(t[0], t[1], t[2]);
    tri.m_position[1] = vPush + plVec3(t[3], t[4], t[5]);
    tri.m_position[2] = vPush + plVec3(t[6], t[7], t[8]);
    const plUInt32 uiObject = *reinterpret_cast<const plUInt32*>(t + 30);
    tri.m_color = plColorScheme::LightUI(static_cast<plColorScheme::Enum>(uiObject % 12));
    tri.m_color.a = 0.55f;
  }
  plDebugRenderer::DrawSolidTriangles(context, triangles, plColor::White);
  plDebugRenderer::DrawLineBox(context, plBoundingBox::MakeFromMinMax(vOrigin, vOrigin + GetDomainSize()), plColorScheme::LightUI(plColorScheme::Blue));
}

void plGpuFluidLiquidVolumeComponent::OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg)
{
  if (m_VolumeId == 0)
    return;

  m_vBoundsSize = GetDomainSize();
  const plBoundingBox box = plBoundingBox::MakeFromCenterAndHalfExtents(plVec3::MakeZero(), m_vBoundsSize * 0.5f);
  msg.AddBounds(plBoundingBoxSphere::MakeFromBox(box), GetOwner()->IsDynamic() ? plDefaultSpatialDataCategories::RenderDynamic : plDefaultSpatialDataCategories::RenderStatic);
}

void plGpuFluidLiquidVolumeComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (msg.m_OverrideCategory != plInvalidRenderDataCategory)
    return;

  m_uiLastExtractedFrame = plRenderWorld::GetFrameCounter();

  plGpuFluidLiquidPublish publish;
  if (m_pManager == nullptr || m_VolumeId == 0 || !m_pManager->GetLiquidPublish(m_VolumeId, publish))
  {
    // A fresh volume has nothing on the GPU yet.
    m_bPublished = false;
    return;
  }

  m_bPublished = true;

  // What this view draws: its render mode, else the cvars, else what the volume was given from code.
  plViewRenderMode::Enum viewMode = plViewRenderMode::None;
  if (msg.m_pView != nullptr)
    viewMode = msg.m_pView->GetViewRenderMode();
  const plInt32 iGlobalMode = cvar_GpuFluidLiquidRenderMode.GetValue();
  bool bPoints = m_RenderMode != plGpuFluidLiquidRenderMode::Surface || iGlobalMode == 1 || iGlobalMode == 2 || cvar_GpuFluidLiquidShowSleep.GetValue();
  bool bSurface = m_RenderMode != plGpuFluidLiquidRenderMode::Points && iGlobalMode != 1;
  bool bSleepColours = cvar_GpuFluidLiquidShowSleep.GetValue();
  switch (viewMode)
  {
    case plViewRenderMode::LiquidParticles:
      bPoints = true;
      bSurface = false;
      break;
    case plViewRenderMode::LiquidParticlesAndSurface:
      bPoints = true;
      bSurface = true;
      break;
    case plViewRenderMode::LiquidSleep:
      bPoints = true;
      bSurface = false;
      bSleepColours = true;
      break;
    default:
      break;
  }
  bSurface = bSurface && publish.m_bHasSurface;

  if (viewMode == plViewRenderMode::LiquidStaticSolids && msg.m_pView != nullptr)
    DrawStaticSolids(msg.m_pView->GetHandle());

  // Both are rewritten on the GPU every tick, so never cached.
  if (bSurface)
  {
    const plRenderData::Category category = plGpuFluidLiquidSurfaceRenderData::LiquidSurface;
    auto pSurface = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plGpuFluidLiquidSurfaceRenderData>(GetOwner());
    pSurface->m_vGlobalPosition = GetOwner()->GetGlobalPosition();
    pSurface->m_hVertices = publish.m_hSurfaceVertices;
    pSurface->m_hQuads = publish.m_hSurfaceQuads;
    pSurface->m_hDrawArgs = publish.m_hSurfaceDrawArgs;
    pSurface->m_uiMaxQuads = publish.m_uiMaxSurfaceQuads;
    pSurface->m_vDomainOrigin = publish.m_vDomainOrigin;
    pSurface->m_vAbsorption = m_vAbsorption;
    pSurface->m_vScatterColor = plVec3(m_ScatterColor.r, m_ScatterColor.g, m_ScatterColor.b);
    pSurface->m_fRoughness = m_fSurfaceRoughness;
    pSurface->m_fRefractionStrength = m_fRefractionStrength;
    pSurface->m_uiDebugView = static_cast<plUInt32>(plMath::Max(0, cvar_GpuFluidLiquidSurfaceDebug.GetValue()));

    msg.AddRenderData(pSurface, category, plRenderData::Caching::Never);
    msg.AddContentFlags(PL_GPUFLUID_CONTENT_FLAG_LIQUID);
    msg.AddDependency(publish.m_hSurfaceVertices, category, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
    msg.AddDependency(publish.m_hSurfaceQuads, category, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
    msg.AddDependency(publish.m_hSurfaceDrawArgs, category, plGALResourceState::IndirectArgument);
  }

  if (bPoints)
  {
    auto pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plGpuFluidLiquidDebugRenderData>(GetOwner());
    pRenderData->m_hPoints = publish.m_hPoints;
    pRenderData->m_hDrawArgs = publish.m_hDrawArgs;
    pRenderData->m_uiMaxParticles = publish.m_uiMaxParticles;
    const float fRadius = cvar_GpuFluidLiquidDebugPointRadius.GetValue() > 0.0f ? cvar_GpuFluidLiquidDebugPointRadius.GetValue() : m_fDebugPointRadius;
    pRenderData->m_fRadius = fRadius > 0.0f ? fRadius : 0.45f * m_ActiveSettings.m_fCellSize / plMath::Pow(static_cast<float>(plMath::Max(1u, m_ActiveSettings.m_uiParticlesPerCell)), 1.0f / 3.0f);
    pRenderData->m_fTopSpeed = cvar_GpuFluidLiquidDebugTopSpeed.GetValue() > 0.0f ? cvar_GpuFluidLiquidDebugTopSpeed.GetValue() : m_fDebugTopSpeed;
    pRenderData->m_bSleepColours = bSleepColours;

    msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::SimpleOpaque, plRenderData::Caching::Never);
    msg.AddDependency(publish.m_hPoints, plDefaultRenderDataCategories::SimpleOpaque, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
    msg.AddDependency(publish.m_hDrawArgs, plDefaultRenderDataCategories::SimpleOpaque, plGALResourceState::IndirectArgument);
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Components_GpuFluidLiquidVolumeComponent);
