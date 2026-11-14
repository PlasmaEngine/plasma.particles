#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Utilities/Stats.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGpuManager.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidReadback.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphManager.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/Buffer.h>
#include <RendererFoundation/Resources/Texture.h>
#include <Shaders/GpuFluid/GpuFluidAnalyticConstants.h>

static_assert(sizeof(plGpuFluidAnalyticConstants) <= 128, "Push constants are limited to 128 bytes on Metal");
static_assert(GPU_FLUID_BRICK_SIZE == plGpuFluidBrickSize && GPU_FLUID_BRICK_STORE == plGpuFluidBrickStore);
static_assert(GPU_FLUID_FACE_NEG_X == plGpuFluidFaces::NegX && GPU_FLUID_FACE_POS_X == plGpuFluidFaces::PosX && GPU_FLUID_FACE_NEG_Y == plGpuFluidFaces::NegY &&
              GPU_FLUID_FACE_POS_Y == plGpuFluidFaces::PosY && GPU_FLUID_FACE_NEG_Z == plGpuFluidFaces::NegZ && GPU_FLUID_FACE_POS_Z == plGpuFluidFaces::PosZ);

struct plGpuFluidGpuManager::GasVolume
{
  plGpuFluidVolumeId m_Id = 0;
  plGpuFluidGasSettings m_Settings;
  plUInt32 m_uiCapacity = 0;

  // Render thread.
  plGpuFluidSparseGrid m_Grid;
  plGpuFluidGasSolver m_Solver;
  plGALBufferHandle m_hStats;
  plGpuFluidReadbackRing m_Readback;
  plDynamicArray<plUInt8> m_ReadbackScratch;
  bool m_bResourcesCreated = false;
  bool m_bHasLastInput = false;
  plGpuFluidGasFrameInput m_LastInput;

  /// Ticks of the frames whose stats are in flight, by readback slot.
  plUInt32 m_uiReadbackTicks[plGpuFluidReadbackRing::s_uiSlotCount] = {};

  // Under the manager's mutex.
  plGpuFluidGasFrameInput m_Input[2];
  bool m_bInputValid[2] = {false, false};
  bool m_bPublishable = false;
  bool m_bStatsValid = false;
  plGpuFluidGasStats m_Stats;
  float m_fVoxelSize = 0.0625f;
  float m_fTickSeconds = plGpuFluidGasTick;
  bool m_bGridDebugValid = false;
  plGpuFluidGridDebug m_GridDebug;
  bool m_bPublishBrickView = false; ///< the last frame's Finalize wrote the brick view, which is published instead
};

struct plGpuFluidGpuManager::LiquidVolume
{
  plGpuFluidVolumeId m_Id = 0;
  plGpuFluidLiquidSettings m_Settings;

  // Render thread.
  plGpuFluidLiquidSolver m_Solver;
  plGpuFluidReadbackRing m_Readback;
  plDynamicArray<plUInt8> m_ReadbackScratch;
  bool m_bResourcesCreated = false;
  bool m_bHasLastInput = false;
  plGpuFluidLiquidFrameInput m_LastInput;
  plUInt32 m_uiLastResetCounter = 0xFFFFFFFFu;
  plUInt64 m_uiResetFrame = 0;

  // Under the manager's mutex.
  plGpuFluidLiquidFrameInput m_Input[2];
  bool m_bInputValid[2] = {false, false};
  bool m_bPublishable = false;
  bool m_bStatsValid = false;
  plGpuFluidLiquidStats m_Stats;
  plGpuFluidLiquidPublish m_Publish;
};

namespace
{
  plMutex s_ManagersMutex;
  plHybridArray<plUniquePtr<plGpuFluidGpuManager>, 4> s_Managers;
  plEventSubscriptionID s_uiRenderEventId = 0;
  bool s_bStarted = false;

  float AsFloat(plUInt32 uiBits)
  {
    float f = 0.0f;
    plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&f), reinterpret_cast<const plUInt8*>(&uiBits), sizeof(float));
    return f;
  }

  /// Bucket 0 of the allocator, binned on the CPU: every virtual brick a source of the frame can write into, with a
  /// margin of two voxels for what its gas reaches within the frame's ticks (GasFinalize's marking takes over after).
  void CollectEmitterBricks(const plGpuFluidGasSimulation& sim, const plVec3U32& vBrickGrid, plDynamicArray<plUInt32>& out_bricks)
  {
    out_bricks.Clear();
    constexpr float fMargin = 2.0f;

    for (const plGpuFluidGasEmitter& emitter : sim.m_Emitters)
    {
      if (emitter.m_Shape == plGpuFluidEmitterShape::None)
        continue;

      const plVec3 vHalf = (emitter.m_Shape == plGpuFluidEmitterShape::Box ? emitter.m_vHalfExtentsVoxels : plVec3(emitter.m_fRadiusVoxels)) + plVec3(fMargin);
      const plVec3 vMin = (emitter.m_vCenterVoxels - vHalf) / static_cast<float>(plGpuFluidBrickSize);
      const plVec3 vMax = (emitter.m_vCenterVoxels + vHalf) / static_cast<float>(plGpuFluidBrickSize);

      auto Lo = [](float f, plUInt32 uiCount) { return static_cast<plUInt32>(plMath::Clamp(plMath::Floor(f), 0.0f, static_cast<float>(uiCount) - 1.0f)); };
      if (vMax.x < 0.0f || vMax.y < 0.0f || vMax.z < 0.0f || vMin.x >= vBrickGrid.x || vMin.y >= vBrickGrid.y || vMin.z >= vBrickGrid.z)
        continue;

      const plVec3U32 vLo(Lo(vMin.x, vBrickGrid.x), Lo(vMin.y, vBrickGrid.y), Lo(vMin.z, vBrickGrid.z));
      const plVec3U32 vHi(Lo(vMax.x, vBrickGrid.x), Lo(vMax.y, vBrickGrid.y), Lo(vMax.z, vBrickGrid.z));
      for (plUInt32 z = vLo.z; z <= vHi.z; ++z)
        for (plUInt32 y = vLo.y; y <= vHi.y; ++y)
          for (plUInt32 x = vLo.x; x <= vHi.x; ++x)
          {
            if (out_bricks.GetCount() >= GPU_FLUID_MAX_EMITTER_BRICKS)
              return;
            out_bricks.PushBack(x + vBrickGrid.x * (y + vBrickGrid.y * z));
          }
    }
  }
} // namespace

plGpuFluidGpuManager::plGpuFluidGpuManager(plUInt32 uiWorldIndex)
  : m_uiWorldIndex(uiWorldIndex)
{
}

plGpuFluidGpuManager::~plGpuFluidGpuManager()
{
  DestroyAll();
}

plGpuFluidGpuManager* plGpuFluidGpuManager::GetForWorld(plUInt32 uiWorldIndex)
{
  PL_LOCK(s_ManagersMutex);

  if (uiWorldIndex >= s_Managers.GetCount())
    s_Managers.SetCount(uiWorldIndex + 1);

  if (s_Managers[uiWorldIndex] == nullptr)
    s_Managers[uiWorldIndex] = PL_DEFAULT_NEW(plGpuFluidGpuManager, uiWorldIndex);

  return s_Managers[uiWorldIndex].Borrow();
}

void plGpuFluidGpuManager::Startup()
{
  if (s_bStarted)
    return;

  s_uiRenderEventId = plRenderWorld::GetRenderEvent().AddEventHandler(&plGpuFluidGpuManager::OnRenderEvent);
  s_bStarted = true;
}

void plGpuFluidGpuManager::Shutdown()
{
  if (!s_bStarted)
    return;

  plRenderWorld::GetRenderEvent().RemoveEventHandler(s_uiRenderEventId);
  s_uiRenderEventId = 0;
  s_bStarted = false;

  // The device is still alive here (high level systems go first), so every resource and graph can be freed now
  // rather than leaking into the render graph manager's shutdown checks.
  PL_LOCK(s_ManagersMutex);
  s_Managers.Clear();
}

plGpuFluidVolumeId plGpuFluidGpuManager::CreateGasVolume(const plGpuFluidGasSettings& settings)
{
  PL_LOCK(m_Mutex);

  plUniquePtr<GasVolume> pVolume = PL_DEFAULT_NEW(GasVolume);
  pVolume->m_Id = m_NextId++;
  pVolume->m_Settings = settings;
  pVolume->m_Settings.m_vBrickGrid = settings.m_vBrickGrid.CompMax(plVec3U32(1)).CompMin(plVec3U32(GPU_FLUID_MAX_BRICK_GRID_PER_AXIS));

  const plUInt32 uiMaxAtlas = GPU_FLUID_MAX_ATLAS_BRICKS_PER_AXIS * GPU_FLUID_MAX_ATLAS_BRICKS_PER_AXIS * GPU_FLUID_MAX_ATLAS_BRICKS_PER_AXIS;
  pVolume->m_uiCapacity = plMath::Clamp<plUInt32>(settings.m_uiMaxBricks, 1, uiMaxAtlas);

  const plGpuFluidVolumeId id = pVolume->m_Id;
  m_GasVolumes.Insert(id, std::move(pVolume));
  return id;
}

void plGpuFluidGpuManager::DestroyVolume(plGpuFluidVolumeId id)
{
  PL_LOCK(m_Mutex);

  plUniquePtr<LiquidVolume> pLiquid;
  if (m_LiquidVolumes.Remove(id, &pLiquid) && pLiquid != nullptr)
  {
    // Render data naming its buffers can still be waiting to render.
    RetiredLiquid& retired = m_RetiredLiquid.ExpandAndGetRef();
    retired.m_pVolume = std::move(pLiquid);
    retired.m_uiDestroyAtFrame = plRenderWorld::GetFrameCounter() + s_uiRetireFrames;
    return;
  }

  plUniquePtr<GasVolume> pVolume;
  if (!m_GasVolumes.Remove(id, &pVolume) || pVolume == nullptr)
    return;

  // Render data naming these textures can still be waiting to render, so they outlive the volume by a few frames.
  Retired& retired = m_Retired.ExpandAndGetRef();
  retired.m_pVolume = std::move(pVolume);
  retired.m_uiDestroyAtFrame = plRenderWorld::GetFrameCounter() + s_uiRetireFrames;
}

void plGpuFluidGpuManager::SetGasFrameInput(plGpuFluidVolumeId id, const plGpuFluidGasFrameInput& input)
{
  PL_LOCK(m_Mutex);

  plUniquePtr<GasVolume>* pVolume = nullptr;
  if (!m_GasVolumes.TryGetValue(id, pVolume))
    return;

  const plUInt32 uiIndex = plRenderWorld::GetDataIndexForExtraction();
  (*pVolume)->m_Input[uiIndex] = input;
  (*pVolume)->m_bInputValid[uiIndex] = true;
  (*pVolume)->m_fVoxelSize = input.m_Simulation.m_fVoxelSize;
  (*pVolume)->m_fTickSeconds = input.m_Simulation.m_fTickSeconds;
}

bool plGpuFluidGpuManager::GetGasPublish(plGpuFluidVolumeId id, plGpuFluidGasPublish& out_publish) const
{
  PL_LOCK(m_Mutex);

  const plUniquePtr<GasVolume>* pVolume = nullptr;
  if (!m_GasVolumes.TryGetValue(id, pVolume))
    return false;

  const GasVolume& volume = **pVolume;
  if (!volume.m_bPublishable)
    return false;

  out_publish.m_bBrickView = volume.m_bPublishBrickView;
  out_publish.m_hAtlas = volume.m_bPublishBrickView ? volume.m_Solver.GetBrickViewAtlas() : volume.m_Solver.GetScalarAtlas();
  out_publish.m_hScalarAtlas = volume.m_Solver.GetScalarAtlas();
  out_publish.m_hIndirection = volume.m_Grid.GetIndirection();
  out_publish.m_vBrickGrid = volume.m_Settings.m_vBrickGrid;
  out_publish.m_vAtlasBricks = volume.m_Grid.GetAtlasBricks();

  // The readback box is two to three frames old. It is grown by one brick, plus how far the fastest gas can move in
  // that time (three frames of two ticks each at most). Until the first readback the whole grid is marched, which is
  // always safe.
  out_publish.m_BoundsUVW = plBoundingBox::MakeFromMinMax(plVec3::MakeZero(), plVec3(1.0f));
  if (volume.m_bStatsValid && volume.m_Stats.HasBounds())
  {
    const float fBrickMetres = plMath::Max(volume.m_fVoxelSize, 1e-4f) * plGpuFluidBrickSize;
    const float fLatencySeconds = 3.0f * plGpuFluidMaxGasTicksPerFrame * plMath::Max(volume.m_fTickSeconds, plGpuFluidGasTick);
    const float fGrow = 1.0f + plMath::Ceil(volume.m_Stats.m_fMaxSpeed * fLatencySeconds / fBrickMetres);

    const plVec3 vGrid(static_cast<float>(volume.m_Settings.m_vBrickGrid.x), static_cast<float>(volume.m_Settings.m_vBrickGrid.y), static_cast<float>(volume.m_Settings.m_vBrickGrid.z));
    const plVec3 vMin(static_cast<float>(volume.m_Stats.m_vBoundsMinBrick.x), static_cast<float>(volume.m_Stats.m_vBoundsMinBrick.y), static_cast<float>(volume.m_Stats.m_vBoundsMinBrick.z));
    const plVec3 vMax(static_cast<float>(volume.m_Stats.m_vBoundsMaxBrick.x), static_cast<float>(volume.m_Stats.m_vBoundsMaxBrick.y), static_cast<float>(volume.m_Stats.m_vBoundsMaxBrick.z));

    const plVec3 vMinUVW = (vMin - plVec3(fGrow)).CompDiv(vGrid).CompMax(plVec3::MakeZero());
    const plVec3 vMaxUVW = (vMax + plVec3(1.0f + fGrow)).CompDiv(vGrid).CompMin(plVec3(1.0f));
    out_publish.m_BoundsUVW = plBoundingBox::MakeFromMinMax(vMinUVW, vMaxUVW);
  }

  return true;
}

bool plGpuFluidGpuManager::GetGasStats(plGpuFluidVolumeId id, plGpuFluidGasStats& out_stats) const
{
  PL_LOCK(m_Mutex);

  const plUniquePtr<GasVolume>* pVolume = nullptr;
  if (!m_GasVolumes.TryGetValue(id, pVolume) || !(*pVolume)->m_bStatsValid)
    return false;

  out_stats = (*pVolume)->m_Stats;
  return true;
}

bool plGpuFluidGpuManager::GetGasGridDebug(plGpuFluidVolumeId id, plGpuFluidGridDebug& out_debug) const
{
  PL_LOCK(m_Mutex);

  const plUniquePtr<GasVolume>* pVolume = nullptr;
  if (!m_GasVolumes.TryGetValue(id, pVolume) || !(*pVolume)->m_bGridDebugValid)
    return false;

  out_debug = (*pVolume)->m_GridDebug;
  return true;
}

plUInt32 plGpuFluidGpuManager::GetGasCapacity(plGpuFluidVolumeId id) const
{
  PL_LOCK(m_Mutex);

  const plUniquePtr<GasVolume>* pVolume = nullptr;
  if (!m_GasVolumes.TryGetValue(id, pVolume))
    return 0;
  return (*pVolume)->m_uiCapacity;
}

plUInt32 plGpuFluidGpuManager::GetNumVolumes() const
{
  PL_LOCK(m_Mutex);
  return m_GasVolumes.GetCount() + m_LiquidVolumes.GetCount();
}

plGpuFluidVolumeId plGpuFluidGpuManager::CreateLiquidVolume(const plGpuFluidLiquidSettings& settings)
{
  PL_LOCK(m_Mutex);

  plUniquePtr<LiquidVolume> pVolume = PL_DEFAULT_NEW(LiquidVolume);
  pVolume->m_Id = m_NextId++;
  pVolume->m_Settings = settings;

  const plGpuFluidVolumeId id = pVolume->m_Id;
  m_LiquidVolumes.Insert(id, std::move(pVolume));
  return id;
}

void plGpuFluidGpuManager::SetLiquidFrameInput(plGpuFluidVolumeId id, const plGpuFluidLiquidFrameInput& input)
{
  PL_LOCK(m_Mutex);

  plUniquePtr<LiquidVolume>* pVolume = nullptr;
  if (!m_LiquidVolumes.TryGetValue(id, pVolume))
    return;

  const plUInt32 uiIndex = plRenderWorld::GetDataIndexForExtraction();
  (*pVolume)->m_Input[uiIndex] = input;
  (*pVolume)->m_bInputValid[uiIndex] = true;
}

bool plGpuFluidGpuManager::GetLiquidPublish(plGpuFluidVolumeId id, plGpuFluidLiquidPublish& out_publish) const
{
  PL_LOCK(m_Mutex);

  const plUniquePtr<LiquidVolume>* pVolume = nullptr;
  if (!m_LiquidVolumes.TryGetValue(id, pVolume) || !(*pVolume)->m_bPublishable)
    return false;

  out_publish = (*pVolume)->m_Publish;
  return true;
}

bool plGpuFluidGpuManager::GetLiquidStats(plGpuFluidVolumeId id, plGpuFluidLiquidStats& out_stats) const
{
  PL_LOCK(m_Mutex);

  const plUniquePtr<LiquidVolume>* pVolume = nullptr;
  if (!m_LiquidVolumes.TryGetValue(id, pVolume) || !(*pVolume)->m_bStatsValid)
    return false;

  out_stats = (*pVolume)->m_Stats;
  return true;
}

void plGpuFluidGpuManager::ReadLiquidStats(LiquidVolume& ref_volume)
{
  plUInt64 uiFrame = 0;
  if (!ref_volume.m_Readback.Poll(ref_volume.m_ReadbackScratch, uiFrame) || ref_volume.m_ReadbackScratch.GetCount() < GPU_FLUID_LIQUID_STATS_BYTES)
    return;

  // Numbers recorded before the last reset describe a simulation that is gone.
  if (uiFrame < ref_volume.m_uiResetFrame)
    return;

  ref_volume.m_Solver.DecodeStats(ref_volume.m_ReadbackScratch, ref_volume.m_Stats);
  ref_volume.m_Stats.m_uiFrame = uiFrame;
  ref_volume.m_Stats.m_fFrontBinSize = ref_volume.m_LastInput.m_fFrontBinSize;
  ref_volume.m_bStatsValid = true;

  plStats::SetStat("GpuFluid/Liquid/Particles", ref_volume.m_Stats.m_uiAlive);
  plStats::SetStat("GpuFluid/Liquid/ActiveBricks", ref_volume.m_Stats.m_uiActiveBricks);
  plStats::SetStat("GpuFluid/Liquid/Substeps", ref_volume.m_Stats.m_uiSubsteps);
  plStats::SetStat("GpuFluid/Liquid/NaNCount", ref_volume.m_Stats.m_uiNaN);
  plStats::SetStat("GpuFluid/Liquid/AwakeBricks", ref_volume.m_Stats.m_uiAwakeBricks);
  plStats::SetStat("GpuFluid/Liquid/SleepingBricks", ref_volume.m_Stats.m_uiSleepingBricks);
  plStats::SetStat("GpuFluid/Liquid/SolidCells", ref_volume.m_Stats.m_uiSolidCells);
}

void plGpuFluidGpuManager::RecordLiquidVolumes(plRenderGraph& ref_graph, plUInt64 uiFrame, bool& inout_bAnyPass)
{
  if (m_LiquidVolumes.IsEmpty())
    return;

  m_LiquidShaders.Load();

  const plUInt32 uiInputIndex = plRenderWorld::GetDataIndexForRendering();

  for (auto it : m_LiquidVolumes)
  {
    LiquidVolume& volume = *it.Value();

    if (!volume.m_bResourcesCreated)
    {
      volume.m_bResourcesCreated = true; // a failure is not retried every frame
      volume.m_Solver.Create(volume.m_Settings);
    }

    ReadLiquidStats(volume);

    // A frame without a new input steps nothing: the solver steps once per tick it is given.
    bool bNewInput = false;
    if (volume.m_bInputValid[uiInputIndex])
    {
      volume.m_LastInput = volume.m_Input[uiInputIndex];
      volume.m_bInputValid[uiInputIndex] = false;
      volume.m_bHasLastInput = true;
      bNewInput = true;
    }

    if (!volume.m_Solver.IsCreated() || !volume.m_bHasLastInput)
    {
      volume.m_bPublishable = false;
      continue;
    }

    if (volume.m_LastInput.m_uiResetCounter != volume.m_uiLastResetCounter)
    {
      volume.m_uiLastResetCounter = volume.m_LastInput.m_uiResetCounter;
      volume.m_uiResetFrame = uiFrame;
      volume.m_bStatsValid = false;
    }

    if (bNewInput)
    {
      const plGpuFluidLiquidStats* pStats = volume.m_bStatsValid ? &volume.m_Stats : nullptr;
      const plUInt64 uiAge = pStats != nullptr && uiFrame > pStats->m_uiFrame ? uiFrame - pStats->m_uiFrame : 1;
      const bool bRecorded = volume.m_Solver.Record(ref_graph, m_LiquidShaders, volume.m_LastInput, pStats, uiAge, uiFrame);

      // The CPU's own numbers, current: whether the volume sleeps, and the static solids on the GPU.
      volume.m_Stats.m_bVolumeAsleep = volume.m_Solver.IsAsleep();
      volume.m_Stats.m_uiTicksAsleep = volume.m_Solver.GetTicksAsleep();
      volume.m_Stats.m_uiFramesBanked = volume.m_Solver.GetFramesBanked();
      volume.m_Stats.m_uiSolidTriangles = volume.m_Solver.GetSolidTriangles();
      volume.m_Stats.m_uiSolidBakes = volume.m_Solver.GetSolidBakes();

      const plUInt32 uiReadbackSlot = bRecorded ? volume.m_Readback.ReserveSlot(uiFrame) : plInvalidIndex;
      if (uiReadbackSlot != plInvalidIndex)
      {
        plGpuFluidReadbackRing* pRing = &volume.m_Readback;
        const plGALBufferHandle hStatsGAL = volume.m_Solver.GetStatsBuffer();

        auto pass = ref_graph.AddTransferPass("GpuFluid Liquid/Readback");
        pass.ReadBuffer(ref_graph.ImportBuffer(hStatsGAL), plGALResourceState::CopySource);
        pass.HasSideEffects();
        pass.SetExecuteCallback([pRing, uiReadbackSlot, hStatsGAL](const plRenderGraphContext& ctx)
          { pRing->Submit(uiReadbackSlot, *ctx.GetCommandEncoder(), hStatsGAL); });
      }

      inout_bAnyPass = inout_bAnyPass || bRecorded;
    }

    // Drawn from the next extraction on: the points it names are written by this frame's graph, before the views.
    if (volume.m_Solver.HasRecorded())
    {
      volume.m_Solver.GetPublish(volume.m_Publish);
      volume.m_bPublishable = true;
    }
  }
}

void plGpuFluidGpuManager::OnRenderEvent(const plRenderWorldRenderEvent& e)
{
  // Graphs must be enqueued once rendering has begun; from extraction they would miss this frame's drain.
  if (e.m_Type != plRenderWorldRenderEvent::Type::BeginRender)
    return;

  PL_LOCK(s_ManagersMutex);
  for (auto& pManager : s_Managers)
  {
    if (pManager != nullptr)
      pManager->BeginRender(e.m_uiFrameCounter);
  }
}

void plGpuFluidGpuManager::CreateResources(GasVolume& ref_volume)
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  plResult result = ref_volume.m_Grid.Create(ref_volume.m_Settings.m_vBrickGrid, ref_volume.m_uiCapacity);
  if (result.Succeeded())
    result = ref_volume.m_Solver.Create(ref_volume.m_Settings.m_vBrickGrid, ref_volume.m_Grid.GetAtlasBricks(), ref_volume.m_uiCapacity);

  plGALBufferCreationDescription statsDesc;
  statsDesc.m_uiTotalSize = GPU_FLUID_STATS_BYTES;
  statsDesc.m_BufferFlags = plGALBufferUsageFlags::ByteAddressBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
  statsDesc.m_ResourceAccess.m_bImmutable = false;
  ref_volume.m_hStats = pDevice->CreateBuffer(statsDesc);
  if (!ref_volume.m_hStats.IsInvalidated())
    pDevice->GetBuffer(ref_volume.m_hStats)->SetDebugName("GpuFluid Gas Stats");

  ref_volume.m_bResourcesCreated = true;

  if (result.Failed() || ref_volume.m_hStats.IsInvalidated())
  {
    plLog::Error("GpuFluid: could not create the GPU resources of a gas volume ({} x {} x {} bricks, {} in the pool)", ref_volume.m_Settings.m_vBrickGrid.x,
      ref_volume.m_Settings.m_vBrickGrid.y, ref_volume.m_Settings.m_vBrickGrid.z, ref_volume.m_uiCapacity);
    DestroyResources(ref_volume);
    ref_volume.m_bResourcesCreated = true; // do not retry every frame
  }
}

void plGpuFluidGpuManager::DestroyResources(GasVolume& ref_volume)
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  ref_volume.m_Grid.Destroy();
  ref_volume.m_Solver.Destroy();

  if (!ref_volume.m_hStats.IsInvalidated())
    pDevice->DestroyBuffer(ref_volume.m_hStats);

  ref_volume.m_hStats.Invalidate();
  ref_volume.m_Readback.Reset();
  ref_volume.m_bResourcesCreated = false;
}

void plGpuFluidGpuManager::DestroyAll()
{
  PL_LOCK(m_Mutex);

  for (auto it : m_GasVolumes)
    DestroyResources(*it.Value());
  m_GasVolumes.Clear();

  for (Retired& retired : m_Retired)
    DestroyResources(*retired.m_pVolume);
  m_Retired.Clear();

  for (auto it : m_LiquidVolumes)
  {
    it.Value()->m_Solver.Destroy();
    it.Value()->m_Readback.Reset();
  }
  m_LiquidVolumes.Clear();

  for (RetiredLiquid& retired : m_RetiredLiquid)
  {
    retired.m_pVolume->m_Solver.Destroy();
    retired.m_pVolume->m_Readback.Reset();
  }
  m_RetiredLiquid.Clear();
  m_LiquidShaders.Release();

  m_pGraph.Clear();
}

void plGpuFluidGpuManager::ReadStats(GasVolume& ref_volume)
{
  plUInt64 uiFrame = 0;
  if (!ref_volume.m_Readback.Poll(ref_volume.m_ReadbackScratch, uiFrame) || ref_volume.m_ReadbackScratch.GetCount() < GPU_FLUID_STATS_BYTES)
    return;

  const plUInt32* pWords = reinterpret_cast<const plUInt32*>(ref_volume.m_ReadbackScratch.GetData());
  const bool bSimulated = ref_volume.m_Settings.m_bSimulate;

  plGpuFluidGasStats& stats = ref_volume.m_Stats;
  stats.m_uiFrame = uiFrame;
  stats.m_uiTicks = ref_volume.m_uiReadbackTicks[uiFrame % plGpuFluidReadbackRing::s_uiSlotCount];
  stats.m_uiRequestedBricks = pWords[GPU_FLUID_STAT_SLOTS_REQUESTED];
  stats.m_uiAllocatedBricks = bSimulated ? pWords[GPU_FLUID_STAT_CONTENT_BRICKS] : plMath::Min(pWords[GPU_FLUID_STAT_SLOTS_REQUESTED], ref_volume.m_uiCapacity);
  stats.m_uiActiveBricks = pWords[GPU_FLUID_STAT_ACTIVE_BRICKS];
  stats.m_uiDeniedBricks = pWords[GPU_FLUID_STAT_DENIED];
  stats.m_uiNaNCount = pWords[GPU_FLUID_STAT_NAN];
  stats.m_uiOverflowCount = pWords[GPU_FLUID_STAT_OVERFLOW];
  stats.m_uiSpeedClamped = pWords[GPU_FLUID_STAT_SPEED_CLAMPED];
  stats.m_vBoundsMinBrick.Set(pWords[GPU_FLUID_STAT_BOUNDS_MIN + 0], pWords[GPU_FLUID_STAT_BOUNDS_MIN + 1], pWords[GPU_FLUID_STAT_BOUNDS_MIN + 2]);
  stats.m_vBoundsMaxBrick.Set(pWords[GPU_FLUID_STAT_BOUNDS_MAX + 0], pWords[GPU_FLUID_STAT_BOUNDS_MAX + 1], pWords[GPU_FLUID_STAT_BOUNDS_MAX + 2]);
  stats.m_fMass = static_cast<float>(static_cast<double>(pWords[GPU_FLUID_STAT_MASS]) / GPU_FLUID_MASS_SCALE);

  stats.m_vCentroid.SetZero();
  if (pWords[GPU_FLUID_STAT_MASS] > 0)
  {
    const double fMass = static_cast<double>(pWords[GPU_FLUID_STAT_MASS]);
    stats.m_vCentroid.Set(static_cast<float>(pWords[GPU_FLUID_STAT_CENTROID + 0] / fMass), static_cast<float>(pWords[GPU_FLUID_STAT_CENTROID + 1] / fMass),
      static_cast<float>(pWords[GPU_FLUID_STAT_CENTROID + 2] / fMass));
  }

  stats.m_fMaxDensity = AsFloat(pWords[GPU_FLUID_STAT_MAX_DENSITY]);
  stats.m_fMaxTemperature = AsFloat(pWords[GPU_FLUID_STAT_MAX_TEMPERATURE]);
  stats.m_fMaxSpeed = AsFloat(pWords[GPU_FLUID_STAT_MAX_SPEED]);
  stats.m_fMaxDivergenceBefore = AsFloat(pWords[GPU_FLUID_STAT_DIV_PRE]);
  stats.m_fMaxDivergenceAfter = AsFloat(pWords[GPU_FLUID_STAT_DIV_POST]);

  const double fCells = plMath::Max(1.0, static_cast<double>(stats.m_uiActiveBricks) * GPU_FLUID_BRICK_CELLS);
  stats.m_fMeanDivergenceBefore = static_cast<float>(pWords[GPU_FLUID_STAT_DIV_PRE_SUM] / (GPU_FLUID_DIV_SCALE * fCells));
  stats.m_fMeanDivergenceAfter = static_cast<float>(pWords[GPU_FLUID_STAT_DIV_POST_SUM] / (GPU_FLUID_DIV_SCALE * fCells));

  stats.m_uiNewBricks = pWords[GPU_FLUID_STAT_ALLOC_NEW];
  stats.m_uiFreedBricks = pWords[GPU_FLUID_STAT_ALLOC_FREED];
  stats.m_uiFreeSlots = pWords[GPU_FLUID_STAT_ALLOC_FREE];
  stats.m_uiAllocRequests = pWords[GPU_FLUID_STAT_ALLOC_REQUESTS];
  stats.m_uiAllocActive = pWords[GPU_FLUID_STAT_ALLOC_ACTIVE];
  stats.m_uiEvictedBricks = pWords[GPU_FLUID_STAT_ALLOC_EVICTED];
  stats.m_fMassDropped = static_cast<float>(static_cast<double>(pWords[GPU_FLUID_STAT_MASS_DROPPED]) / GPU_FLUID_MASS_SCALE);
  for (plUInt32 b = 0; b < 4; ++b)
  {
    stats.m_uiDeniedPerBucket[b] = pWords[GPU_FLUID_STAT_DENIED_BUCKET + b];
    stats.m_uiRequestsPerBucket[b] = pWords[GPU_FLUID_STAT_REQUESTS_BUCKET + b];
  }

  stats.m_fMassCorrection = pWords[GPU_FLUID_STAT_MASS_CORRECTION] != 0 ? AsFloat(pWords[GPU_FLUID_STAT_MASS_CORRECTION]) : 1.0f;
  for (plUInt32 i = 0; i < 4; ++i)
  {
    const plUInt64 uiSum = static_cast<plUInt64>(pWords[GPU_FLUID_STAT_MASS_ADVECT + 2 * i]) | (static_cast<plUInt64>(pWords[GPU_FLUID_STAT_MASS_ADVECT + 2 * i + 1]) << 32);
    stats.m_fMassAdvect[i] = static_cast<float>(static_cast<double>(uiSum) / GPU_FLUID_MEAN_SCALE);
  }

  stats.m_uiSolidCells = pWords[GPU_FLUID_STAT_SOLID_CELLS];
  stats.m_fSolidContactDensity = AsFloat(pWords[GPU_FLUID_STAT_SOLID_CONTACT]);
  stats.m_fSolidDensity = AsFloat(pWords[GPU_FLUID_STAT_SOLID_DENSITY]);
  stats.m_fSolidCleared = static_cast<float>(static_cast<double>(pWords[GPU_FLUID_STAT_SOLID_CLEARED]) / GPU_FLUID_MASS_SCALE);
  stats.m_fEmitted = static_cast<float>(static_cast<double>(pWords[GPU_FLUID_STAT_EMITTED]) / GPU_FLUID_MASS_SCALE);
  for (plUInt32 i = 0; i < GPU_FLUID_STAT_PROBE_COUNT; ++i)
    stats.m_fProbes[i] = AsFloat(pWords[GPU_FLUID_STAT_PROBE + i]);

  ref_volume.m_bStatsValid = true;

  plStats::SetStat("GpuFluid/Gas/AllocatedBricks", stats.m_uiAllocatedBricks);
  plStats::SetStat("GpuFluid/Gas/ActiveBricks", stats.m_uiActiveBricks);
  plStats::SetStat("GpuFluid/Gas/BricksDenied", stats.m_uiDeniedBricks);
  plStats::SetStat("GpuFluid/Gas/NaNCount", stats.m_uiNaNCount);
  plStats::SetStat("GpuFluid/Gas/OverflowCount", stats.m_uiOverflowCount);
  plStats::SetStat("GpuFluid/Gas/SpeedClamped", stats.m_uiSpeedClamped);
}

void plGpuFluidGpuManager::BeginRender(plUInt64 uiFrame)
{
  PL_PROFILE_SCOPE("GpuFluid BeginRender");
  PL_LOCK(m_Mutex);

  // Retired volumes whose render data can no longer be in flight.
  for (plUInt32 i = m_Retired.GetCount(); i-- > 0;)
  {
    if (m_Retired[i].m_uiDestroyAtFrame <= uiFrame)
    {
      DestroyResources(*m_Retired[i].m_pVolume);
      m_Retired.RemoveAtAndSwap(i);
    }
  }

  for (plUInt32 i = m_RetiredLiquid.GetCount(); i-- > 0;)
  {
    if (m_RetiredLiquid[i].m_uiDestroyAtFrame <= uiFrame)
    {
      m_RetiredLiquid[i].m_pVolume->m_Solver.Destroy();
      m_RetiredLiquid[i].m_pVolume->m_Readback.Reset();
      m_RetiredLiquid.RemoveAtAndSwap(i);
    }
  }

  if (m_GasVolumes.IsEmpty() && m_LiquidVolumes.IsEmpty())
    return;

  if (!m_hAnalyticSphereShader.IsValid())
  {
    m_hAnalyticSphereShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/GasAnalyticSphere.plShader");
    m_hStatsClearShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/GpuFluidStatsClear.plShader");
  }

  if (m_pGraph == nullptr)
    m_pGraph = plRenderGraphManager::CreateRenderGraph("GpuFluid", plRenderGraphPhase::PreRender);

  m_pGraph->Reset();
  plRenderGraph& graph = *m_pGraph;

  const plUInt32 uiInputIndex = plRenderWorld::GetDataIndexForRendering();
  bool bAnyPass = false;

  for (auto it : m_GasVolumes)
  {
    GasVolume& volume = *it.Value();

    if (!volume.m_bResourcesCreated)
      CreateResources(volume);

    ReadStats(volume);

    // A simulated volume steps only on fresh input: repeating a frame's ticks would run the clock twice.
    bool bFreshInput = false;
    if (volume.m_bInputValid[uiInputIndex])
    {
      volume.m_LastInput = volume.m_Input[uiInputIndex];
      volume.m_bInputValid[uiInputIndex] = false;
      volume.m_bHasLastInput = true;
      bFreshInput = true;
    }

    if (!volume.m_Solver.IsCreated() || !volume.m_bHasLastInput)
    {
      volume.m_bPublishable = false;
      continue;
    }

    const plGpuFluidGasFrameInput& input = volume.m_LastInput;
    const bool bAnalytic = !volume.m_Settings.m_bSimulate;
    if (bAnalytic && input.m_Source == plGpuFluidDebugSource::None)
    {
      volume.m_bPublishable = false;
      continue;
    }

    const plGpuFluidSparseGridGraph gridGraph = volume.m_Grid.Import(graph);
    const plRenderGraphTextureHandle hAtlas = graph.ImportTexture(volume.m_Solver.GetScalarAtlas());
    const plRenderGraphBufferHandle hStats = graph.ImportBuffer(volume.m_hStats);
    const plGALBufferHandle hStatsGAL = volume.m_hStats;

    plGpuFluidGridDebug gridDebug;
    if (volume.m_Grid.PollDebug(gridDebug))
    {
      volume.m_GridDebug = std::move(gridDebug);
      volume.m_bGridDebugValid = true;
    }

    {
      plUInt64 uiHash = 0;
      plUInt64 uiHashFrame = 0;
      if (volume.m_Solver.IsCreated() && volume.m_Solver.PollStateHash(uiHash, uiHashFrame))
      {
        volume.m_Stats.m_uiStateHash = uiHash;
        volume.m_Stats.m_uiHashFrame = uiHashFrame;
      }
    }

    const plUInt32 uiTicks = !bAnalytic && bFreshInput ? plMath::Min(input.m_Simulation.m_uiTicks, plGpuFluidMaxGasTicksPerFrame) : 0;
    const bool bCounters = bAnalytic || uiTicks > 0;

    // Reset the counters this frame's kernels add to.
    if (bCounters)
    {
      auto pass = graph.AddComputePass("GpuFluid Stats Clear");
      pass.WriteBuffer(hStats, plGALResourceState::UnorderedAccessWrite);
      pass.HasSideEffects();
      pass.SetExecuteCallback([hStats, hShader = m_hStatsClearShader](const plRenderGraphContext& ctx)
        {
          plRenderContext* pContext = ctx.GetRenderContext();
          pContext->BindShader(hShader);
          pContext->GetBindGroup().BindBuffer("GpuFluidStats", ctx.ResolveBuffer(hStats));
          pContext->Dispatch(1, 1, 1).IgnoreResult();
        });
    }

    if (bAnalytic)
    {
      // The analytic source: content, brick table and atlas in one dispatch over the virtual grid.
      const plVec3U32 vGrid = volume.m_Settings.m_vBrickGrid;
      const plVec3U32 vAtlasBricks = volume.m_Grid.GetAtlasBricks();
      const plRenderGraphTextureHandle hIndirection = gridGraph.m_hIndirection;

      plGpuFluidAnalyticConstants constants;
      constants.GpuFluidBrickGrid = plVec4U32(vGrid.x, vGrid.y, vGrid.z, volume.m_uiCapacity);
      constants.GpuFluidAtlasBricks = plVec4U32(vAtlasBricks.x, vAtlasBricks.y, vAtlasBricks.z, 0);
      constants.GpuFluidSphere = plVec4(input.m_Sphere.m_vCenterVoxels.x, input.m_Sphere.m_vCenterVoxels.y, input.m_Sphere.m_vCenterVoxels.z, input.m_Sphere.m_fRadiusVoxels);
      constants.GpuFluidSphereShape = plVec4(input.m_Sphere.m_fSoftnessVoxels, input.m_Sphere.m_fPeakDensity, input.m_Sphere.m_fPeakTemperature, 0.0f);
      constants.GpuFluidBrickRules = plVec4(input.m_fAllocThreshold, input.m_fMeanFloor, 0.0f, 0.0f);

      auto pass = graph.AddComputePass("GpuFluid Analytic Sphere");
      pass.WriteTexture(hAtlas, {}, plGALResourceState::UnorderedAccessWrite);
      pass.WriteTexture(hIndirection, {}, plGALResourceState::UnorderedAccessWrite);
      pass.WriteBuffer(hStats, plGALResourceState::UnorderedAccessWrite);
      pass.HasSideEffects();
      pass.SetExecuteCallback([hAtlas, hIndirection, hStats, vGrid, constants, hShader = m_hAnalyticSphereShader](const plRenderGraphContext& ctx)
        {
          plRenderContext* pContext = ctx.GetRenderContext();
          pContext->SetPushConstants("plGpuFluidAnalyticConstants", constants);
          pContext->BindShader(hShader);

          plBindGroupBuilder& bindGroup = pContext->GetBindGroup();
          bindGroup.BindTexture("GpuFluidAtlasOutput", ctx.ResolveTexture(hAtlas));
          bindGroup.BindTexture("GpuFluidIndirectionOutput", ctx.ResolveTexture(hIndirection));
          bindGroup.BindBuffer("GpuFluidStats", ctx.ResolveBuffer(hStats));
          pContext->Dispatch(vGrid.x, vGrid.y, vGrid.z).IgnoreResult();
        });
    }
    else
    {
      // Phase Gas/Alloc, every frame that runs ticks: free idle bricks, give free slots to the requests (the sources'
      // bricks and what the last Finalize marked), clear the new bricks.
      // A volume's first GPU frame always initialises its grid, so its brick table is never undefined.
      if (uiTicks > 0 || !volume.m_Grid.IsAllocated())
      {
        plGpuFluidAllocInput alloc;
        alloc.m_bAllocateAll = input.m_bAllocateAll;
        alloc.m_bDebugReadback = input.m_bDebugGrid;
        alloc.m_bDebugVirtual = input.m_bDebugGridVirtual;
        alloc.m_bReverseSlots = input.m_bReverseSlots;
        CollectEmitterBricks(input.m_Simulation, volume.m_Settings.m_vBrickGrid, alloc.m_EmitterBricks);

        // In the closed-box mode nothing changes once the box is allocated.
        const bool bAllocationChanged = !input.m_bAllocateAll || !volume.m_Grid.IsAllocated();
        volume.m_Grid.AddAllocPasses(graph, gridGraph, hStats, alloc, uiFrame);
        volume.m_Solver.AddClearPass(graph, volume.m_Grid, gridGraph, input.m_Simulation.m_vAmbientWind, bAllocationChanged, input.m_Simulation.m_OpenFaces);
      }

      plGpuFluidGasSimulation simulation = input.m_Simulation;
      simulation.m_uiTicks = uiTicks;
      // The Poisson test hook solves once per fresh input, like a tick; its readback needs the counters.
      simulation.m_PoissonTest.m_bEnabled = simulation.m_PoissonTest.m_bEnabled && bCounters;
      volume.m_Solver.AddFramePasses(graph, volume.m_Grid, gridGraph, hStats, simulation);
      if (input.m_bHashState && uiTicks > 0)
        volume.m_Solver.AddHashPasses(graph, volume.m_Grid, gridGraph, uiFrame);
      if (uiTicks > 0)
        volume.m_bPublishBrickView = simulation.m_bBrickView;
    }

    // Stats to the CPU, through a ring of three so nothing waits. A copy is not allowed inside a compute pass.
    if (bCounters)
    {
      const plUInt32 uiReadbackSlot = volume.m_Readback.ReserveSlot(uiFrame);
      if (uiReadbackSlot != plInvalidIndex)
      {
        volume.m_uiReadbackTicks[uiFrame % plGpuFluidReadbackRing::s_uiSlotCount] = uiTicks;
        plGpuFluidReadbackRing* pRing = &volume.m_Readback;

        auto pass = graph.AddTransferPass("GpuFluid Stats Readback");
        pass.ReadBuffer(hStats, plGALResourceState::CopySource);
        pass.HasSideEffects();
        pass.SetExecuteCallback([pRing, uiReadbackSlot, hStatsGAL](const plRenderGraphContext& ctx)
          { pRing->Submit(uiReadbackSlot, *ctx.GetCommandEncoder(), hStatsGAL); });
      }
    }

    // The march, the froxel inject and the path tracer read these without declaring it, from other graphs. This
    // leaves them in a shader-read state for those.
    {
      const plRenderGraphTextureHandle hIndirection = gridGraph.m_hIndirection;

      auto pass = graph.AddComputePass("GpuFluid Publish");
      pass.ReadTexture(hAtlas, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::ComputeShader);
      if (volume.m_bPublishBrickView)
        pass.ReadTexture(graph.ImportTexture(volume.m_Solver.GetBrickViewAtlas()), {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::ComputeShader);
      pass.ReadTexture(hIndirection, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::ComputeShader);
      pass.HasSideEffects();
      pass.SetExecuteCallback([](const plRenderGraphContext&) {});
    }

    bAnyPass = true;

    // From the next extraction on, the march may see this volume: the textures it names are written by this
    // frame's graph and by every later one, always before the views render.
    volume.m_bPublishable = true;
  }

  RecordLiquidVolumes(graph, uiFrame, bAnyPass);

  if (bAnyPass)
    plRenderGraphManager::EnqueueRenderGraph(m_pGraph);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidGpuManager);
