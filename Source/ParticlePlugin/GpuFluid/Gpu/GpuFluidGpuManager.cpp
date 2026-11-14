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
#include <Shaders/GpuFluid/GpuFluidConstants.h>

static_assert(sizeof(plGpuFluidAnalyticConstants) <= 128, "Push constants are limited to 128 bytes on Metal");
static_assert(GPU_FLUID_BRICK_SIZE == plGpuFluidBrickSize && GPU_FLUID_BRICK_STORE == plGpuFluidBrickStore);

struct plGpuFluidGpuManager::GasVolume
{
  plGpuFluidVolumeId m_Id = 0;
  plGpuFluidGasSettings m_Settings;
  plVec3U32 m_vAtlasBricks = plVec3U32::MakeZero();
  plUInt32 m_uiCapacity = 0;

  // Render thread.
  plGALTextureHandle m_hAtlas;
  plGALTextureHandle m_hIndirection;
  plGALBufferHandle m_hStats;
  plGpuFluidReadbackRing m_Readback;
  plDynamicArray<plUInt8> m_ReadbackScratch;
  bool m_bResourcesCreated = false;
  bool m_bHasLastInput = false;
  plGpuFluidGasFrameInput m_LastInput;

  // Under the manager's mutex.
  plGpuFluidGasFrameInput m_Input[2];
  bool m_bInputValid[2] = {false, false};
  bool m_bPublishable = false;
  bool m_bStatsValid = false;
  plGpuFluidGasStats m_Stats;
};

namespace
{
  plMutex s_ManagersMutex;
  plHybridArray<plUniquePtr<plGpuFluidGpuManager>, 4> s_Managers;
  plEventSubscriptionID s_uiRenderEventId = 0;
  bool s_bStarted = false;

  /// Roughly cubic, so no axis runs away, and never more than 204 bricks per axis (2048 texels / 10).
  plVec3U32 ComputeAtlasBricks(plUInt32 uiCapacity)
  {
    plUInt32 uiSide = 1;
    while (uiSide * uiSide * uiSide < uiCapacity && uiSide < GPU_FLUID_MAX_ATLAS_BRICKS_PER_AXIS)
      ++uiSide;

    const plUInt32 uiDepth = plMath::Clamp<plUInt32>((uiCapacity + uiSide * uiSide - 1) / (uiSide * uiSide), 1, GPU_FLUID_MAX_ATLAS_BRICKS_PER_AXIS);
    return plVec3U32(uiSide, uiSide, uiDepth);
  }

  plGALTextureHandle Create3DTexture(plGALDevice* pDevice, const plVec3U32& vSize, const char* szName)
  {
    plGALTextureCreationDescription desc;
    desc.m_Type = plGALTextureType::Texture3D;
    desc.m_Format = plGALResourceFormat::RGBAHalf;
    desc.m_uiWidth = vSize.x;
    desc.m_uiHeight = vSize.y;
    desc.m_uiDepth = vSize.z;
    desc.m_uiMipLevelCount = 1;
    desc.m_uiArraySize = 1;
    desc.m_bAllowShaderResourceView = true;
    desc.m_bAllowUAV = true;
    desc.m_ResourceAccess.m_bImmutable = false;

    plGALTextureHandle hTexture = pDevice->CreateTexture(desc);
    if (!hTexture.IsInvalidated())
      pDevice->GetTexture(hTexture)->SetDebugName(szName);
    return hTexture;
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
  pVolume->m_vAtlasBricks = ComputeAtlasBricks(pVolume->m_uiCapacity);

  const plGpuFluidVolumeId id = pVolume->m_Id;
  m_GasVolumes.Insert(id, std::move(pVolume));
  return id;
}

void plGpuFluidGpuManager::DestroyVolume(plGpuFluidVolumeId id)
{
  PL_LOCK(m_Mutex);

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

  out_publish.m_hAtlas = volume.m_hAtlas;
  out_publish.m_hIndirection = volume.m_hIndirection;
  out_publish.m_vBrickGrid = volume.m_Settings.m_vBrickGrid;
  out_publish.m_vAtlasBricks = volume.m_vAtlasBricks;

  // The readback box is two to three frames old, so it is grown by a brick on every side. Until the first readback
  // the whole grid is marched, which is always safe.
  out_publish.m_BoundsUVW = plBoundingBox::MakeFromMinMax(plVec3::MakeZero(), plVec3(1.0f));
  if (volume.m_bStatsValid && volume.m_Stats.HasBounds())
  {
    const plVec3 vGrid(static_cast<float>(volume.m_Settings.m_vBrickGrid.x), static_cast<float>(volume.m_Settings.m_vBrickGrid.y), static_cast<float>(volume.m_Settings.m_vBrickGrid.z));
    const plVec3 vMin(static_cast<float>(volume.m_Stats.m_vBoundsMinBrick.x), static_cast<float>(volume.m_Stats.m_vBoundsMinBrick.y), static_cast<float>(volume.m_Stats.m_vBoundsMinBrick.z));
    const plVec3 vMax(static_cast<float>(volume.m_Stats.m_vBoundsMaxBrick.x), static_cast<float>(volume.m_Stats.m_vBoundsMaxBrick.y), static_cast<float>(volume.m_Stats.m_vBoundsMaxBrick.z));

    const plVec3 vMinUVW = (vMin - plVec3(1.0f)).CompDiv(vGrid).CompMax(plVec3::MakeZero());
    const plVec3 vMaxUVW = (vMax + plVec3(2.0f)).CompDiv(vGrid).CompMin(plVec3(1.0f));
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

plUInt32 plGpuFluidGpuManager::GetNumVolumes() const
{
  PL_LOCK(m_Mutex);
  return m_GasVolumes.GetCount();
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

  ref_volume.m_hAtlas = Create3DTexture(pDevice, ref_volume.m_vAtlasBricks * plGpuFluidBrickStore, "GpuFluid Gas Atlas");
  ref_volume.m_hIndirection = Create3DTexture(pDevice, ref_volume.m_Settings.m_vBrickGrid, "GpuFluid Gas Brick Table");

  plGALBufferCreationDescription statsDesc;
  statsDesc.m_uiTotalSize = GPU_FLUID_STATS_BYTES;
  statsDesc.m_BufferFlags = plGALBufferUsageFlags::ByteAddressBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
  statsDesc.m_ResourceAccess.m_bImmutable = false;
  ref_volume.m_hStats = pDevice->CreateBuffer(statsDesc);
  if (!ref_volume.m_hStats.IsInvalidated())
    pDevice->GetBuffer(ref_volume.m_hStats)->SetDebugName("GpuFluid Gas Stats");

  ref_volume.m_bResourcesCreated = true;

  if (ref_volume.m_hAtlas.IsInvalidated() || ref_volume.m_hIndirection.IsInvalidated() || ref_volume.m_hStats.IsInvalidated())
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

  if (!ref_volume.m_hAtlas.IsInvalidated())
    pDevice->DestroyTexture(ref_volume.m_hAtlas);
  if (!ref_volume.m_hIndirection.IsInvalidated())
    pDevice->DestroyTexture(ref_volume.m_hIndirection);
  if (!ref_volume.m_hStats.IsInvalidated())
    pDevice->DestroyBuffer(ref_volume.m_hStats);

  ref_volume.m_hAtlas.Invalidate();
  ref_volume.m_hIndirection.Invalidate();
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

  m_pGraph.Clear();
}

void plGpuFluidGpuManager::ReadStats(GasVolume& ref_volume)
{
  plUInt64 uiFrame = 0;
  if (!ref_volume.m_Readback.Poll(ref_volume.m_ReadbackScratch, uiFrame) || ref_volume.m_ReadbackScratch.GetCount() < GPU_FLUID_STATS_BYTES)
    return;

  const plUInt32* pWords = reinterpret_cast<const plUInt32*>(ref_volume.m_ReadbackScratch.GetData());

  plGpuFluidGasStats& stats = ref_volume.m_Stats;
  stats.m_uiFrame = uiFrame;
  stats.m_uiRequestedBricks = pWords[GPU_FLUID_STAT_SLOTS_REQUESTED];
  stats.m_uiAllocatedBricks = plMath::Min(pWords[GPU_FLUID_STAT_SLOTS_REQUESTED], ref_volume.m_uiCapacity);
  stats.m_uiDeniedBricks = pWords[GPU_FLUID_STAT_DENIED];
  stats.m_uiNaNCount = pWords[GPU_FLUID_STAT_NAN];
  stats.m_vBoundsMinBrick.Set(pWords[GPU_FLUID_STAT_BOUNDS_MIN + 0], pWords[GPU_FLUID_STAT_BOUNDS_MIN + 1], pWords[GPU_FLUID_STAT_BOUNDS_MIN + 2]);
  stats.m_vBoundsMaxBrick.Set(pWords[GPU_FLUID_STAT_BOUNDS_MAX + 0], pWords[GPU_FLUID_STAT_BOUNDS_MAX + 1], pWords[GPU_FLUID_STAT_BOUNDS_MAX + 2]);
  stats.m_fMass = static_cast<float>(static_cast<double>(pWords[GPU_FLUID_STAT_MASS]) / GPU_FLUID_MASS_SCALE);

  float fMaxDensity = 0.0f;
  plMemoryUtils::Copy(reinterpret_cast<plUInt32*>(&fMaxDensity), &pWords[GPU_FLUID_STAT_MAX_DENSITY], 1);
  stats.m_fMaxDensity = fMaxDensity;

  ref_volume.m_bStatsValid = true;

  plStats::SetStat("GpuFluid/Gas/AllocatedBricks", stats.m_uiAllocatedBricks);
  plStats::SetStat("GpuFluid/Gas/BricksDenied", stats.m_uiDeniedBricks);
  plStats::SetStat("GpuFluid/Gas/NaNCount", stats.m_uiNaNCount);
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

  if (m_GasVolumes.IsEmpty())
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

    if (volume.m_bInputValid[uiInputIndex])
    {
      volume.m_LastInput = volume.m_Input[uiInputIndex];
      volume.m_bInputValid[uiInputIndex] = false;
      volume.m_bHasLastInput = true;
    }

    if (volume.m_hAtlas.IsInvalidated() || !volume.m_bHasLastInput || volume.m_LastInput.m_Source == plGpuFluidDebugSource::None)
    {
      volume.m_bPublishable = false;
      continue;
    }

    const plGALTextureHandle hAtlasGAL = volume.m_hAtlas;
    const plGALTextureHandle hIndirectionGAL = volume.m_hIndirection;
    const plGALBufferHandle hStatsGAL = volume.m_hStats;

    const plRenderGraphTextureHandle hAtlas = graph.ImportTexture(hAtlasGAL);
    const plRenderGraphTextureHandle hIndirection = graph.ImportTexture(hIndirectionGAL);
    const plRenderGraphBufferHandle hStats = graph.ImportBuffer(hStatsGAL);

    // Gas/Alloc of the solver to come: reset the counters this frame's kernels add to.
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

    // The analytic source: content, brick table and atlas in one dispatch over the virtual grid.
    {
      const plGpuFluidGasFrameInput& input = volume.m_LastInput;
      const plVec3U32 vGrid = volume.m_Settings.m_vBrickGrid;

      plGpuFluidAnalyticConstants constants;
      constants.GpuFluidBrickGrid = plVec4U32(vGrid.x, vGrid.y, vGrid.z, volume.m_uiCapacity);
      constants.GpuFluidAtlasBricks = plVec4U32(volume.m_vAtlasBricks.x, volume.m_vAtlasBricks.y, volume.m_vAtlasBricks.z, 0);
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

    // Stats to the CPU, through a ring of three so nothing waits. A copy is not allowed inside a compute pass.
    const plUInt32 uiReadbackSlot = volume.m_Readback.ReserveSlot(uiFrame);
    if (uiReadbackSlot != plInvalidIndex)
    {
      plGpuFluidReadbackRing* pRing = &volume.m_Readback;

      auto pass = graph.AddTransferPass("GpuFluid Stats Readback");
      pass.ReadBuffer(hStats, plGALResourceState::CopySource);
      pass.HasSideEffects();
      pass.SetExecuteCallback([pRing, uiReadbackSlot, hStatsGAL](const plRenderGraphContext& ctx)
        { pRing->Submit(uiReadbackSlot, *ctx.GetCommandEncoder(), hStatsGAL); });
    }

    // The march, the froxel inject and the path tracer read these without declaring it, from other graphs. This
    // leaves them in a shader-read state for those.
    {
      auto pass = graph.AddComputePass("GpuFluid Publish");
      pass.ReadTexture(hAtlas, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::ComputeShader);
      pass.ReadTexture(hIndirection, {}, plGALResourceState::ShaderRead, plGALShaderStageFlags::ComputeShader);
      pass.HasSideEffects();
      pass.SetExecuteCallback([](const plRenderGraphContext&) {});
    }

    bAnyPass = true;

    // From the next extraction on, the march may see this volume: the textures it names are written by this
    // frame's graph and by every later one, always before the views render.
    volume.m_bPublishable = true;
  }

  if (bAnyPass)
    plRenderGraphManager::EnqueueRenderGraph(m_pGraph);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidGpuManager);
