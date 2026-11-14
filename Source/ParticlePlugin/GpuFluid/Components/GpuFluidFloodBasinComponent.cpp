#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Messages/CommonMessages.h>
#include <Core/Messages/UpdateLocalBoundsMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Utilities/Stats.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidColliderComponent.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidFloodBasinComponent.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidLiquidModifierComponents.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidBasinColumns.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGpuManager.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidPass.h>
#include <ParticlePlugin/GpuFluid/Rendering/GpuFluidLiquidSurfaceRenderer.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Utils/WorldGeoExtractionUtil.h>

extern plCVarInt cvar_GpuFluidRebake;
extern plCVarInt cvar_GpuFluidLiquidSurfaceDebug;

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plGpuFluidFloodBasinComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(15.0f, 15.0f, 2.0f)), new plClampValueAttribute(plVec3(0.1f), plVec3(512.0f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("ColumnSize", m_fColumnSize)->AddAttributes(new plDefaultValueAttribute(0.05f), new plClampValueAttribute(0.01f, 1.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("InitialLevel", m_fInitialLevel)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("DrainCoefficient", m_fDrainCoefficient)->AddAttributes(new plDefaultValueAttribute(0.6f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("WaveDamping", m_fWaveDamping)->AddAttributes(new plGroupAttribute("Waves"), new plDefaultValueAttribute(0.003f), new plClampValueAttribute(0.0f, 0.5f)),
    PL_MEMBER_PROPERTY("MaxWaveHeight", m_fMaxWaveHeight)->AddAttributes(new plDefaultValueAttribute(0.12f), new plClampValueAttribute(0.0f, 2.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("WaveStrength", m_fWaveStrength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("Absorption", m_vAbsorption)->AddAttributes(new plGroupAttribute("Look"), new plDefaultValueAttribute(plVec3(2.5f, 1.0f, 0.7f)), new plClampValueAttribute(plVec3(0.0f), plVariant()), new plSuffixAttribute(" 1/m")),
    PL_MEMBER_PROPERTY("ScatterColor", m_ScatterColor)->AddAttributes(new plDefaultValueAttribute(plColor(0.035f, 0.075f, 0.075f))),
    PL_MEMBER_PROPERTY("SurfaceRoughness", m_fSurfaceRoughness)->AddAttributes(new plDefaultValueAttribute(0.05f), new plClampValueAttribute(0.02f, 1.0f)),
    PL_MEMBER_PROPERTY("RefractionStrength", m_fRefractionStrength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 2.0f)),
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
    new plBoxVisualizerAttribute("Extents", 1.0f, plColorScheme::LightUI(plColorScheme::Teal)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE

PL_BEGIN_COMPONENT_TYPE(plGpuFluidFloodOpeningComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("HalfExtents", m_vHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.3f, 0.6f, 1.0f)), new plClampValueAttribute(plVec3(0.01f), plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Open", m_bOpen)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgSetPlaying, OnMsgSetPlaying),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/GpuFluid"),
    new plBoxVisualizerAttribute("HalfExtents", 2.0f, plColorScheme::LightUI(plColorScheme::Teal)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

namespace
{
  float GravityOf(const plWorld& world)
  {
    if (const plPhysicsWorldModuleInterface* pPhysics = world.GetModuleReadOnly<plPhysicsWorldModuleInterface>())
    {
      const float g = pPhysics->GetGravity().GetLength();
      if (g > 1e-3f)
        return g;
    }
    return 9.81f;
  }

  /// Litres per second a source pours, in m^3/s.
  float SourceFlow(const plGpuFluidLiquidSourceComponent& source)
  {
    if (source.m_fFlowRate > 0.0f)
      return source.m_fFlowRate * 1.0e-3f;
    const float r = source.GetWorldRadius();
    return plMath::Pi<float>() * r * r * source.m_fSpeed;
  }
} // namespace

plGpuFluidFloodBasinComponent::plGpuFluidFloodBasinComponent() = default;
plGpuFluidFloodBasinComponent::~plGpuFluidFloodBasinComponent() = default;

void plGpuFluidFloodBasinComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_vExtents;
  s << m_fColumnSize;
  s << m_fInitialLevel;
  s << m_fDrainCoefficient;
  s << m_fWaveDamping;
  s << m_fMaxWaveHeight;
  s << m_fWaveStrength;
  s << m_vAbsorption;
  s << m_ScatterColor;
  s << m_fSurfaceRoughness;
  s << m_fRefractionStrength;
}

void plGpuFluidFloodBasinComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();

  s >> m_vExtents;
  s >> m_fColumnSize;
  s >> m_fInitialLevel;
  s >> m_fDrainCoefficient;
  s >> m_fWaveDamping;
  s >> m_fMaxWaveHeight;
  s >> m_fWaveStrength;
  s >> m_vAbsorption;
  s >> m_ScatterColor;
  s >> m_fSurfaceRoughness;
  s >> m_fRefractionStrength;
}

void plGpuFluidFloodBasinComponent::OnActivated()
{
  SUPER::OnActivated();

  m_pManager = plGpuFluidGpuManager::GetForWorld(GetWorld()->GetIndex());
  m_bColumnsDirty = true;
  m_iLastRebakeCVar = cvar_GpuFluidRebake.GetValue();
  ResetWater();
  GetOwner()->UpdateLocalBounds();
}

void plGpuFluidFloodBasinComponent::OnDeactivated()
{
  if (m_pManager != nullptr && m_BasinId != 0)
    m_pManager->DestroyVolume(m_BasinId);
  m_BasinId = 0;
  m_pColumns = nullptr;
  m_pDepths = nullptr;

  GetOwner()->UpdateLocalBounds();
  SUPER::OnDeactivated();
}

plBoundingBox plGpuFluidFloodBasinComponent::GetWorldBox() const
{
  const plVec3 vHalf = m_vExtents * 0.5f;
  const plVec3 vCenter = GetOwner()->GetGlobalPosition();
  return plBoundingBox::MakeFromMinMax(vCenter - vHalf, vCenter + vHalf);
}

float plGpuFluidFloodBasinComponent::GetWorldLevel() const
{
  return GetWorldBox().m_vMin.z + m_fLevel;
}

plVec2U32 plGpuFluidFloodBasinComponent::GetColumns() const
{
  const float fDx = plMath::Max(m_fColumnSize, 0.01f);
  auto Axis = [&](float fExtent) { return plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Ceil(fExtent / fDx - 1e-4f)), 1u, 2048u); };
  return plVec2U32(Axis(m_vExtents.x), Axis(m_vExtents.y));
}

void plGpuFluidFloodBasinComponent::AddVolume(double fCubicMetres)
{
  m_fVolume = plMath::Max(m_fVolume + fCubicMetres, 0.0);
  if (m_pColumns != nullptr)
    m_fVolume = plMath::Min(m_fVolume, m_pColumns->GetVolumeBelow(m_pColumns->m_fHeight));
}

float plGpuFluidFloodBasinComponent::GetFreeArea() const
{
  return m_pColumns != nullptr ? m_pColumns->GetFreeArea(m_fLevel) : m_vExtents.x * m_vExtents.y;
}

void plGpuFluidFloodBasinComponent::AddWave(const plVec3& vWorldPosition, float fRadius, float fHeight)
{
  if (m_PendingWaves.GetCount() >= 16)
    return;
  const plVec3 vMin = GetWorldBox().m_vMin;
  m_PendingWaves.PushBack(plVec4(vWorldPosition.x - vMin.x, vWorldPosition.y - vMin.y, fRadius, fHeight * m_fWaveStrength));
}

void plGpuFluidFloodBasinComponent::ResetWater()
{
  // The volume is set from the initial level once the solid profile is known.
  m_bVolumeFromInitialLevel = true;
  m_fVolume = 0.0;
  m_fLevel = 0.0f;
  m_fDepthsLevel = -1.0f;
  m_PendingWaves.Clear();
  // A new GPU basin starts still.
  if (m_pManager != nullptr && m_BasinId != 0)
    m_pManager->DestroyVolume(m_BasinId);
  m_BasinId = 0;
}

plGpuFluidFloodBasinComponent* plGpuFluidFloodBasinComponent::FindBasinAt(plWorld& ref_world, const plVec3& vWorldPosition)
{
  plGpuFluidFloodBasinComponentManager* pManager = ref_world.GetComponentManager<plGpuFluidFloodBasinComponentManager>();
  if (pManager == nullptr)
    return nullptr;
  for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
  {
    if (it->IsActiveAndInitialized() && it->GetWorldBox().Contains(vWorldPosition))
      return it;
  }
  return nullptr;
}

void plGpuFluidFloodBasinComponent::UpdateColumns()
{
  const plBoundingBox box = GetWorldBox();
  const plVec2U32 vColumns = GetColumns();

  if (cvar_GpuFluidRebake.GetValue() != m_iLastRebakeCVar)
  {
    m_iLastRebakeCVar = cvar_GpuFluidRebake.GetValue();
    m_bColumnsDirty = true;
  }

  // Static colliders moved, added or removed.
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
    m_bColumnsDirty = true;
  }

  if (m_pColumns != nullptr && (!(m_pColumns->m_vColumns == vColumns) || !(m_pColumns->m_vOrigin - box.m_vMin).IsZero(1e-4f) || plMath::Abs(m_pColumns->m_fHeight - m_vExtents.z) > 1e-4f))
    m_bColumnsDirty = true;

  // The world initializes its components in batches over several frames, so a bake on the first update can miss
  // collision geometry that is not initialized yet: for a while after a bake, a change in how many collision meshes
  // the world holds bakes again.
  plWorldGeoExtractionUtil::MeshObjectList objects;
  if (!m_bColumnsDirty && m_uiBakeCheckFrames > 0)
  {
    --m_uiBakeCheckFrames;
    plWorldGeoExtractionUtil::ExtractWorldGeometry(objects, *GetWorld(), plWorldGeoExtractionUtil::ExtractionMode::CollisionMesh);
    if (objects.GetCount() != m_uiBakedObjects)
      m_bColumnsDirty = true;
  }

  if (!m_bColumnsDirty)
    return;
  m_bColumnsDirty = false;

  // The same collision meshes the liquid bakes, read here and turned into the basin's solid profile.
  if (objects.IsEmpty())
    plWorldGeoExtractionUtil::ExtractWorldGeometry(objects, *GetWorld(), plWorldGeoExtractionUtil::ExtractionMode::CollisionMesh);
  if (objects.GetCount() != m_uiBakedObjects)
    m_uiBakeCheckFrames = 60;
  m_uiBakedObjects = objects.GetCount();
  plDynamicArray<plGpuFluidLiquidSolidBake::SourceMesh> meshes;
  plGpuFluidLiquidSolidBake::GatherSourceMeshes(objects, box.m_vMin, box.m_vMax, m_fColumnSize, meshes);

  // The water stays: only the level it stands at changes with the new profile.
  plSharedPtr<plGpuFluidBasinColumns> pColumns = PL_DEFAULT_NEW(plGpuFluidBasinColumns);
  pColumns->Build(meshes, box.m_vMin, vColumns, plMath::Max(m_fColumnSize, 0.01f), m_vExtents.z);
  m_pColumns = pColumns;
  m_fDepthsLevel = -1.0f;

  plLog::Dev("GpuFluid: flood basin '{}': {} x {} columns, {} triangles, {} solid intervals, {} m^3 below the top, built in {} ms", GetOwner()->GetName(), vColumns.x, vColumns.y,
    pColumns->m_uiTriangles, pColumns->m_Intervals.GetCount(), plArgF(pColumns->GetVolumeBelow(pColumns->m_fHeight), 1), plArgF(pColumns->m_fBuildMs, 1));

  if (m_bVolumeFromInitialLevel)
  {
    m_fVolume = pColumns->GetVolumeBelow(plMath::Clamp(m_fInitialLevel, 0.0f, m_vExtents.z));
    m_bVolumeFromInitialLevel = false;
  }
}

void plGpuFluidFloodBasinComponent::UpdateFlows(float fDt)
{
  if (fDt <= 0.0f || m_pColumns == nullptr)
    return;

  plWorld& world = *GetWorld();
  const plBoundingBox box = GetWorldBox();
  const float fWorldLevel = GetWorldLevel();
  const float g = GravityOf(world);

  // Sources in the box pour straight in; one inside a liquid volume pours particles, which drain into the basin.
  double fIn = 0.0;
  if (const plGpuFluidLiquidSourceComponentManager* pSources = world.GetComponentManager<plGpuFluidLiquidSourceComponentManager>())
  {
    for (auto it = pSources->GetComponents(); it.IsValid(); ++it)
    {
      const plVec3 vCenter = it->GetWorldCenter();
      if (it->IsActiveAndInitialized() && it->m_bFlowing && box.Contains(vCenter) && plGpuFluidLiquidModifiers::FindVolumeAt(world, vCenter) == nullptr)
        fIn += SourceFlow(*it);
    }
  }

  // Open drains below the water: an orifice in the floor (or a wall), Cd * area * sqrt(2 g head).
  double fOut = 0.0;
  if (const plGpuFluidLiquidDrainComponentManager* pDrains = world.GetComponentManager<plGpuFluidLiquidDrainComponentManager>())
  {
    for (auto it = pDrains->GetComponents(); it.IsValid(); ++it)
    {
      if (!it->IsActiveAndInitialized() || !it->m_bOpen)
        continue;
      const plBoundingBox drain = it->GetWorldBox();
      if (!box.Overlaps(drain))
        continue;
      const float fHead = fWorldLevel - drain.m_vMax.z;
      if (fHead <= 0.0f)
        continue;
      const float fArea = (drain.m_vMax.x - drain.m_vMin.x) * (drain.m_vMax.y - drain.m_vMin.y);
      fOut += m_fDrainCoefficient * fArea * plMath::Sqrt(2.0f * g * fHead);
    }
  }

  AddVolume((fIn - fOut) * fDt);
}

void plGpuFluidFloodBasinComponent::GatherWaves(float fDt)
{
  if (fDt <= 0.0f)
    return;

  plWorld& world = *GetWorld();
  const plBoundingBox box = GetWorldBox();
  const float fWorldLevel = GetWorldLevel();
  const float g = GravityOf(world);
  auto Random = [this]()
  {
    m_uiRandom ^= m_uiRandom << 13;
    m_uiRandom ^= m_uiRandom >> 7;
    m_uiRandom ^= m_uiRandom << 17;
    return static_cast<float>(m_uiRandom >> 40) / static_cast<float>(1ull << 24);
  };

  // Jets: where each source's stream lands on the water (a ballistic arc from the nozzle), a few pushes a frame
  // scattered over the stream's width, as strong as the water arriving there.
  if (const plGpuFluidLiquidSourceComponentManager* pSources = world.GetComponentManager<plGpuFluidLiquidSourceComponentManager>())
  {
    for (auto it = pSources->GetComponents(); it.IsValid(); ++it)
    {
      if (!it->IsActiveAndInitialized() || !it->m_bFlowing)
        continue;
      const plVec3 p = it->GetWorldCenter();
      const plVec3 v = it->GetWorldVelocity();
      const float fDrop = p.z - fWorldLevel;
      float t = 0.0f;
      if (fDrop > 0.0f)
        t = (v.z + plMath::Sqrt(v.z * v.z + 2.0f * g * fDrop)) / g;
      const plVec3 vHit(p.x + v.x * t, p.y + v.y * t, fWorldLevel);
      if (!box.Contains(vHit))
        continue;

      const float fRadius = plMath::Max(2.0f * it->GetWorldRadius(), 3.0f * m_fColumnSize);
      const float fSpeed = (v + plVec3(0.0f, 0.0f, -g * t)).GetLength();
      const float fHeight = -plMath::Min(0.06f, 0.05f * fSpeed * plMath::Sqrt(SourceFlow(*it)) * fDt * 60.0f);
      for (plUInt32 k = 0; k < 3; ++k)
      {
        // Small, scattered pushes: the churn where the stream lands, rings of many sizes spreading from it.
        const float a = Random() * 2.0f * plMath::Pi<float>();
        const float r = plMath::Sqrt(Random()) * fRadius;
        const float fBump = plMath::Max(fRadius * (0.25f + 0.35f * Random()), 2.0f * m_fColumnSize);
        AddWave(vHit + plVec3(plMath::Cos(plAngle::MakeFromRadian(a)) * r, plMath::Sin(plAngle::MakeFromRadian(a)) * r, 0.0f), fBump, fHeight * (0.5f + Random()));
      }
    }
  }

  // Moving colliders crossing the water: a bow wave ahead and a trough behind, as strong as they are fast.
  if (const plGpuFluidColliderComponentManager* pColliders = world.GetComponentManager<plGpuFluidColliderComponentManager>())
  {
    for (auto it = pColliders->GetComponents(); it.IsValid(); ++it)
    {
      const plGpuFluidColliderComponent& collider = *it;
      if (!collider.IsActiveAndInitialized() || collider.m_bStatic)
        continue;
      const plVec3 vPos = collider.GetOwner()->GetGlobalPosition();
      const plUInt64 uiKey = reinterpret_cast<plUInt64>(&collider);
      plVec3 vLast = vPos;
      m_ColliderPositions.TryGetValue(uiKey, vLast);
      m_ColliderPositions[uiKey] = vPos;

      const plVec3 vScale = collider.GetOwner()->GetGlobalScaling();
      const float fSize = collider.m_Shape == plGpuFluidColliderShape::Box ? plMath::Max(collider.m_vHalfExtents.x * vScale.x, collider.m_vHalfExtents.y * vScale.y)
                                                                            : collider.m_fRadius * plMath::Max(vScale.x, vScale.y, vScale.z);
      const float fHalfHeight = collider.m_Shape == plGpuFluidColliderShape::Box ? collider.m_vHalfExtents.z * vScale.z : fSize;
      if (!box.Contains(plVec3(vPos.x, vPos.y, fWorldLevel)) || plMath::Abs(vPos.z - fWorldLevel) > fHalfHeight)
        continue;

      plVec3 vMove = vPos - vLast;
      vMove.z = 0.0f;
      const float fSpeed = vMove.GetLength() / fDt;
      if (fSpeed < 0.05f)
        continue;
      const plVec3 vDir = vMove.GetNormalized();
      const float fHeight = plMath::Min(0.05f, 0.012f * fSpeed);
      AddWave(vPos + vDir * fSize, fSize, fHeight);
      AddWave(vPos - vDir * fSize, fSize, -fHeight);
    }
  }
}

void plGpuFluidFloodBasinComponent::Update()
{
  if (m_pManager == nullptr)
    return;

  UpdateColumns();
  if (m_pColumns == nullptr)
    return;

  const plVec2U32 vColumns = GetColumns();
  const float fColumnSize = plMath::Max(m_fColumnSize, 0.01f);
  if (m_BasinId == 0 || !(vColumns == m_vActiveColumns) || fColumnSize != m_fActiveColumnSize)
  {
    if (m_BasinId != 0)
      m_pManager->DestroyVolume(m_BasinId);
    m_BasinId = m_pManager->CreateBasin(vColumns, fColumnSize);
    m_vActiveColumns = vColumns;
    m_fActiveColumnSize = fColumnSize;
  }

  if (m_vExtents != m_vBoundsSize)
    GetOwner()->UpdateLocalBounds();

  const float fDt = plMath::Min(static_cast<float>(GetWorld()->GetClock().GetTimeDiff().GetSeconds()), 1.0f / 15.0f);
  UpdateFlows(fDt);
  m_fLevel = m_pColumns->GetLevelForVolume(m_fVolume);

  // The column depths follow the level in 3 mm steps (a new upload each time).
  if (m_pDepths == nullptr || plMath::Abs(m_fLevel - m_fDepthsLevel) > 0.003f)
  {
    plSharedPtr<plGpuFluidBasinDepths> pDepths = PL_DEFAULT_NEW(plGpuFluidBasinDepths);
    m_pColumns->ComputeDepths(m_fLevel, *pDepths);
    m_pDepths = pDepths;
    m_fDepthsLevel = m_fLevel;
  }

  GatherWaves(fDt);

  const float g = GravityOf(*GetWorld());
  plGpuFluidBasinFrameInput input;
  if (fDt > 0.0f)
    ++m_uiTick;
  input.m_uiTick = m_uiTick;
  input.m_fLevel = m_fLevel;
  input.m_fGravity = g;
  input.m_fDamping = m_fWaveDamping;
  input.m_fMaxWaveHeight = m_fMaxWaveHeight;
  input.m_fMaxSpeedDepth = 1.0f;
  input.m_pDepths = m_pDepths;
  input.m_Impulses = m_PendingWaves;
  m_PendingWaves.Clear();

  // Steps so a wave crosses at most half a column per step at the deepest water's speed.
  const float fWaveSpeed = plMath::Sqrt(g * plMath::Clamp(m_fLevel, 0.01f, input.m_fMaxSpeedDepth));
  input.m_uiSteps = fDt > 0.0f ? plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Ceil(fWaveSpeed * fDt / (0.5f * fColumnSize))), 1u, 16u) : 0u;
  input.m_fStepDt = input.m_uiSteps > 0 ? fDt / static_cast<float>(input.m_uiSteps) : 0.0f;
  m_pManager->SetBasinFrameInput(m_BasinId, input);

  {
    plStringBuilder sStat;
    sStat.SetFormat("GpuFluid/Basin/{}/Level", GetOwner()->GetName());
    plStats::SetStat(sStat, m_fLevel);
    sStat.SetFormat("GpuFluid/Basin/{}/Volume", GetOwner()->GetName());
    plStats::SetStat(sStat, m_fVolume);
  }

  // While editing: the level, and how much water stands at it.
  if (!GetWorld()->GetWorldSimulationEnabled())
  {
    const plBoundingBox box = GetWorldBox();
    const float z = GetWorldLevel();
    const plColor color = plColorScheme::LightUI(plColorScheme::Teal);
    plDebugRenderer::DrawLineBox(GetWorld(), plBoundingBox::MakeFromMinMax(plVec3(box.m_vMin.x, box.m_vMin.y, z), plVec3(box.m_vMax.x, box.m_vMax.y, z)), color);
    plDebugRenderer::Draw3DText(GetWorld(), plFmt("Water level {} m, {} m^3", plArgF(m_fLevel, 2), plArgF(m_fVolume, 1)), plVec3(box.GetCenter().x, box.GetCenter().y, z), color, 14);
  }
}

void plGpuFluidFloodBasinComponent::OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg)
{
  m_vBoundsSize = m_vExtents;
  const plBoundingBox box = plBoundingBox::MakeFromCenterAndHalfExtents(plVec3::MakeZero(), m_vExtents * 0.5f);
  msg.AddBounds(plBoundingBoxSphere::MakeFromBox(box), GetOwner()->IsDynamic() ? plDefaultSpatialDataCategories::RenderDynamic : plDefaultSpatialDataCategories::RenderStatic);
}

void plGpuFluidFloodBasinComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (msg.m_OverrideCategory != plInvalidRenderDataCategory)
    return;

  plGpuFluidBasinPublish publish;
  if (m_pManager == nullptr || m_BasinId == 0 || m_fLevel < 0.002f || !m_pManager->GetBasinPublish(m_BasinId, publish))
    return;

  // The liquid surface pass draws it like a liquid volume's surface: no back faces, so the water's thickness runs to
  // the floor (the opaque scene).
  const plRenderData::Category category = plGpuFluidLiquidSurfaceRenderData::LiquidSurface;
  auto pSurface = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plGpuFluidLiquidSurfaceRenderData>(GetOwner());
  pSurface->m_vGlobalPosition = GetOwner()->GetGlobalPosition();
  pSurface->m_hVertices = publish.m_hVertices;
  pSurface->m_hQuads = publish.m_hQuads;
  pSurface->m_hDrawArgs = publish.m_hDrawArgs;
  pSurface->m_uiMaxQuads = publish.m_uiQuads;
  pSurface->m_vDomainOrigin = GetWorldBox().m_vMin;
  pSurface->m_vAbsorption = m_vAbsorption;
  pSurface->m_vScatterColor = plVec3(m_ScatterColor.r, m_ScatterColor.g, m_ScatterColor.b);
  pSurface->m_fRoughness = m_fSurfaceRoughness;
  pSurface->m_fRefractionStrength = m_fRefractionStrength;
  pSurface->m_fMaxThickness = plMath::Max(4.0f, m_vExtents.z);
  pSurface->m_uiDebugView = static_cast<plUInt32>(plMath::Max(0, cvar_GpuFluidLiquidSurfaceDebug.GetValue()));

  msg.AddRenderData(pSurface, category, plRenderData::Caching::Never);
  msg.AddContentFlags(PL_GPUFLUID_CONTENT_FLAG_LIQUID);
  msg.AddDependency(publish.m_hVertices, category, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
  msg.AddDependency(publish.m_hQuads, category, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
  msg.AddDependency(publish.m_hDrawArgs, category, plGALResourceState::IndirectArgument);
}

//////////////////////////////////////////////////////////////////////////
// Opening

plGpuFluidFloodOpeningComponent::plGpuFluidFloodOpeningComponent() = default;
plGpuFluidFloodOpeningComponent::~plGpuFluidFloodOpeningComponent() = default;

void plGpuFluidFloodOpeningComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();
  s << m_vHalfExtents;
  s << m_bOpen;
}

void plGpuFluidFloodOpeningComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();
  s >> m_vHalfExtents;
  s >> m_bOpen;
}

void plGpuFluidFloodOpeningComponent::OnMsgSetPlaying(plMsgSetPlaying& ref_msg)
{
  m_bOpen = ref_msg.m_bPlay;
}

void plGpuFluidFloodOpeningComponent::Update()
{
  m_fLastFlow = 0.0f;
  plWorld& world = *GetWorld();
  const plBoundingBox opening = plGpuFluidLiquidModifiers::GetWorldBox(GetOwner()->GetGlobalTransform(), m_vHalfExtents);

  // The two basins the opening joins.
  plGpuFluidFloodBasinComponent* pBasins[2] = {nullptr, nullptr};
  plUInt32 uiFound = 0;
  if (plGpuFluidFloodBasinComponentManager* pManager = world.GetComponentManager<plGpuFluidFloodBasinComponentManager>())
  {
    for (auto it = pManager->GetComponents(); it.IsValid() && uiFound < 2; ++it)
    {
      if (it->IsActiveAndInitialized() && it->GetWorldBox().Overlaps(opening))
        pBasins[uiFound++] = it;
    }
  }

  if (!world.GetWorldSimulationEnabled() && uiFound < 2)
  {
    const plColor red = plColorScheme::LightUI(plColorScheme::Red);
    plDebugRenderer::DrawLineBox(&world, opening, red);
    plDebugRenderer::Draw3DText(&world, "An opening needs two flood basins", opening.GetCenter(), red, 14);
  }

  const float fDt = plMath::Min(static_cast<float>(world.GetClock().GetTimeDiff().GetSeconds()), 1.0f / 15.0f);
  if (uiFound < 2 || !m_bOpen || fDt <= 0.0f)
    return;

  const float zA = pBasins[0]->GetWorldLevel();
  const float zB = pBasins[1]->GetWorldLevel();
  const bool bAHigher = zA >= zB;
  plGpuFluidFloodBasinComponent* pHigh = bAHigher ? pBasins[0] : pBasins[1];
  plGpuFluidFloodBasinComponent* pLow = bAHigher ? pBasins[1] : pBasins[0];
  const float zHigh = plMath::Max(zA, zB), zLow = plMath::Min(zA, zB);

  // Over the sill as over a weir; the opening's top caps both heads (above it the wall holds the water).
  const float fSill = opening.m_vMin.z;
  const float fTop = opening.m_vMax.z - fSill;
  const float fHigh = plMath::Clamp(zHigh - fSill, 0.0f, fTop);
  const float fLow = plMath::Clamp(zLow - fSill, 0.0f, fTop);
  if (fHigh <= fLow)
    return;
  const float fWidth = plMath::Max(opening.m_vMax.x - opening.m_vMin.x, opening.m_vMax.y - opening.m_vMin.y);
  const float g = GravityOf(world);
  const float fFlow = pHigh->m_fDrainCoefficient * fWidth * (2.0f / 3.0f) * plMath::Sqrt(2.0f * g) * (plMath::Pow(fHigh, 1.5f) - plMath::Pow(fLow, 1.5f));

  // Never past level: at most half of what would make both stand at the same height this frame.
  const float fAreaHigh = plMath::Max(pHigh->GetFreeArea(), 0.01f), fAreaLow = plMath::Max(pLow->GetFreeArea(), 0.01f);
  const double fToLevel = (zHigh - zLow) * fAreaHigh * fAreaLow / (fAreaHigh + fAreaLow);
  const double fMove = plMath::Min(plMath::Min(static_cast<double>(fFlow * fDt), 0.5 * fToLevel), pHigh->GetVolume());
  pHigh->AddVolume(-fMove);
  pLow->AddVolume(fMove);
  m_fLastFlow = static_cast<float>(fMove / fDt) * (bAHigher ? 1.0f : -1.0f);

  // The stream through the opening stirs the lower basin's water.
  if (fMove > 1e-6)
  {
    const plVec3 vCenter = opening.GetCenter();
    pLow->AddWave(plVec3(vCenter.x, vCenter.y, zLow), 0.5f * fWidth, -plMath::Min(0.03f, static_cast<float>(fMove / fDt) * 0.02f));
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Components_GpuFluidFloodBasinComponent);
