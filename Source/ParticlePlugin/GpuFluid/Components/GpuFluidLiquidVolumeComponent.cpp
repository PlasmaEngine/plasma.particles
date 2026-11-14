#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Messages/UpdateLocalBoundsMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidLiquidVolumeComponent.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidDebugRenderer.h>
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

PL_BEGIN_COMPONENT_TYPE(plGpuFluidLiquidVolumeComponent, 2, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(4.0f, 3.0f, 2.0f)), new plClampValueAttribute(plVec3(0.1f), plVec3(1024.0f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("CellSize", m_fCellSize)->AddAttributes(new plDefaultValueAttribute(0.0625f), new plClampValueAttribute(0.005f, 4.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("ParticlesPerCell", m_uiParticlesPerCell)->AddAttributes(new plDefaultValueAttribute(4), new plClampValueAttribute(1, 8)),
    PL_MEMBER_PROPERTY("MaxParticles", m_uiMaxParticles)->AddAttributes(new plDefaultValueAttribute(1024 * 1024), new plClampValueAttribute(256, 16 * 1024 * 1024)),
    PL_ENUM_MEMBER_PROPERTY("Quality", plGpuFluidQuality, m_Quality),
    PL_BITFLAGS_MEMBER_PROPERTY("OpenFaces", plGpuFluidDomainFaces, m_OpenFaces),
    PL_ARRAY_MEMBER_PROPERTY("FillBoxes", m_FillBoxes),
    PL_ARRAY_MEMBER_PROPERTY("Obstacles", m_Obstacles),
    PL_MEMBER_PROPERTY("FlipBlend", m_fFlipBlend)->AddAttributes(new plGroupAttribute("Solver"), new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("MaxSubsteps", m_uiMaxSubsteps)->AddAttributes(new plDefaultValueAttribute(4), new plClampValueAttribute(1, 16)),
    PL_MEMBER_PROPERTY("PressureCycles", m_uiPressureCycles)->AddAttributes(new plDefaultValueAttribute(8), new plClampValueAttribute(0, 32)),
    PL_MEMBER_PROPERTY("WallFriction", m_fWallFriction)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("AlwaysSimulate", m_bAlwaysSimulate)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("DebugPointRadius", m_fDebugPointRadius)->AddAttributes(new plGroupAttribute("Debug"), new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("DebugTopSpeed", m_fDebugTopSpeed)->AddAttributes(new plDefaultValueAttribute(3.0f), new plClampValueAttribute(0.01f, plVariant()), new plSuffixAttribute(" m/s")),
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
}

void plGpuFluidLiquidVolumeComponent::OnActivated()
{
  SUPER::OnActivated();

  m_pManager = plGpuFluidGpuManager::GetForWorld(GetWorld()->GetIndex());
  EnsureVolume();
  ResetSimulation();
  GetOwner()->UpdateLocalBounds();
}

void plGpuFluidLiquidVolumeComponent::OnDeactivated()
{
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

void plGpuFluidLiquidVolumeComponent::Update()
{
  if (m_pManager == nullptr)
    return;

  EnsureVolume();
  if (m_VolumeId == 0)
    return;

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
  input.m_uiMaxSubsteps = m_uiMaxSubsteps;
  input.m_uiPressureCycles = m_uiPressureCycles;
  input.m_fWallFriction = m_fWallFriction;
  input.m_uiOpenFaces = m_OpenFaces.GetValue();

  // Fill boxes: across x and y the cells whose centre is inside. Up the z axis the cells from the first whose centre
  // is inside to the box top, the last of them partly filled when the top lies inside a cell (so the surface does
  // not snap to a cell face).
  for (const plGpuFluidLiquidFillBox& box : m_FillBoxes)
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

  m_pManager->SetLiquidFrameInput(m_VolumeId, input);
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

  auto pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plGpuFluidLiquidDebugRenderData>(GetOwner());
  pRenderData->m_hPoints = publish.m_hPoints;
  pRenderData->m_hDrawArgs = publish.m_hDrawArgs;
  pRenderData->m_uiMaxParticles = publish.m_uiMaxParticles;
  pRenderData->m_fRadius = m_fDebugPointRadius > 0.0f ? m_fDebugPointRadius : 0.45f * m_ActiveSettings.m_fCellSize / plMath::Pow(static_cast<float>(plMath::Max(1u, m_ActiveSettings.m_uiParticlesPerCell)), 1.0f / 3.0f);
  pRenderData->m_fTopSpeed = m_fDebugTopSpeed;

  m_bPublished = true;

  // Rewritten on the GPU every tick, so never cached.
  msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::SimpleOpaque, plRenderData::Caching::Never);
  msg.AddDependency(publish.m_hPoints, plDefaultRenderDataCategories::SimpleOpaque, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
  msg.AddDependency(publish.m_hDrawArgs, plDefaultRenderDataCategories::SimpleOpaque, plGALResourceState::IndirectArgument);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Components_GpuFluidLiquidVolumeComponent);
