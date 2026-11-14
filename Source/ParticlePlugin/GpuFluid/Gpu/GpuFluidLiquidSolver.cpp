#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidLiquidSolver.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/Buffer.h>

#include <initializer_list>

static_assert(sizeof(plGpuFluidLiquidConstants) <= 128, "Push constants are limited to 128 bytes on Metal");
static_assert(sizeof(plGpuFluidLiquidSeedConstants) <= 128, "Push constants are limited to 128 bytes on Metal");

void plGpuFluidLiquidShaders::Load()
{
  if (IsLoaded())
    return;

  auto Load = [](const char* szName)
  {
    plStringBuilder sPath("Shaders/GpuFluid/Liquid/", szName, ".plShader");
    return plResourceManager::LoadResource<plShaderResource>(sPath);
  };

  m_hStatsClear = Load("LiquidStatsClear");
  m_hSeed = Load("LiquidSeed");
  m_hSeedFinish = Load("LiquidSeedFinish");
  m_hClear = Load("LiquidClear");
  m_hCount = Load("LiquidCount");
  m_hScatter = Load("LiquidScatter");
  m_hBricks = Load("LiquidBricks");
  m_hP2G = Load("LiquidP2G");
  m_hExtrapolate = Load("LiquidExtrapolate");
  m_hClassify = Load("LiquidClassify");
  m_hForces = Load("LiquidForces");
  m_hRhs = Load("LiquidRhs");
  m_hSmoothBricks = Load("LiquidSmoothBricks");
  m_hSmoothDense = Load("LiquidSmoothDense");
  m_hRestrict = Load("LiquidRestrict");
  m_hProlongBricks = Load("LiquidProlongBricks");
  m_hProlongDense = Load("LiquidProlongDense");
  m_hCoarsest = Load("LiquidCoarsest");
  m_hResidual = Load("LiquidResidual");
  m_hProject = Load("LiquidProject");
  m_hG2P = Load("LiquidG2P");
  m_hRenderPrep = Load("LiquidRenderPrep");
  m_hFrameEnd = Load("LiquidFrameEnd");
  m_Scan.Initialize();
}

void plGpuFluidLiquidShaders::Release()
{
  *this = plGpuFluidLiquidShaders();
}

namespace
{
  /// A buffer a kernel binds, and whether the kernel writes it (then later kernels of the same pass wait for it).
  struct Bind
  {
    const char* m_szName = nullptr;
    plGALBufferHandle m_hBuffer;
    bool m_bWritten = true;
  };

  Bind R(const char* szName, plGALBufferHandle hBuffer) { return {szName, hBuffer, false}; }
  Bind W(const char* szName, plGALBufferHandle hBuffer) { return {szName, hBuffer, true}; }

  /// Records the kernels of one render-graph pass: push constants, bindings, dispatch, then a UAV barrier on what the
  /// kernel wrote, so the next kernel of the pass sees it (a no-op on Metal).
  struct Recorder
  {
    plRenderContext* m_pContext = nullptr;
    plGALCommandEncoder* m_pEncoder = nullptr;

    explicit Recorder(const plRenderGraphContext& ctx)
      : m_pContext(ctx.GetRenderContext())
      , m_pEncoder(ctx.GetCommandEncoder())
    {
    }

    template <typename T>
    void Bindings(plShaderResourceHandle hShader, const char* szConstants, const T& constants, std::initializer_list<Bind> binds)
    {
      m_pContext->SetPushConstants(plTempHashedString(plStringView(szConstants)), constants);
      m_pContext->BindShader(hShader);
      plBindGroupBuilder& bindGroup = m_pContext->GetBindGroup();
      for (const Bind& bind : binds)
        bindGroup.BindBuffer(plTempHashedString(plStringView(bind.m_szName)), bind.m_hBuffer);
    }

    void After(std::initializer_list<Bind> binds)
    {
      for (const Bind& bind : binds)
      {
        if (bind.m_bWritten)
          m_pEncoder->BufferBarrier(bind.m_hBuffer, plGALResourceState::UnorderedAccessWrite, plGALResourceState::UnorderedAccessWrite);
      }
    }

    template <typename T>
    void Run(plShaderResourceHandle hShader, const T& constants, plVec3U32 vGroups, std::initializer_list<Bind> binds, const char* szConstants = "plGpuFluidLiquidConstants")
    {
      if (vGroups.x == 0 || vGroups.y == 0 || vGroups.z == 0)
        return;
      Bindings(hShader, szConstants, constants, binds);
      m_pContext->Dispatch(vGroups.x, vGroups.y, vGroups.z).IgnoreResult();
      After(binds);
    }

    template <typename T>
    void RunIndirect(plShaderResourceHandle hShader, const T& constants, plGALBufferHandle hArgs, std::initializer_list<Bind> binds)
    {
      Bindings(hShader, "plGpuFluidLiquidConstants", constants, binds);
      m_pContext->DispatchIndirect(hArgs, 0).IgnoreResult();
      After(binds);
    }
  };

  plUInt32 DivUp(plUInt32 a, plUInt32 b) { return (a + b - 1) / b; }

  plVec3U32 DenseGroups(const plVec3U32& vDims)
  {
    return plVec3U32(DivUp(vDims.x, GPU_FLUID_LIQUID_LEVEL_GROUP), DivUp(vDims.y, GPU_FLUID_LIQUID_LEVEL_GROUP), DivUp(vDims.z, GPU_FLUID_LIQUID_LEVEL_GROUP));
  }

  plVec3U32 LinearGroups(plUInt32 uiCount, plUInt32 uiGroupSize)
  {
    return plVec3U32(plMath::Max(1u, DivUp(uiCount, uiGroupSize)), 1, 1);
  }
} // namespace

plGpuFluidLiquidSolver::plGpuFluidLiquidSolver() = default;

plGpuFluidLiquidSolver::~plGpuFluidLiquidSolver()
{
  Destroy();
}

plGALBufferHandle plGpuFluidLiquidSolver::CreateRawBuffer(plUInt64 uiBytes, const char* szName, bool bIndirect)
{
  const plUInt64 uiSize = plMath::Max<plUInt64>(16, ((uiBytes + 15) / 16) * 16);
  if (uiSize > 0xFFFFFFF0ull)
  {
    plLog::Error("GpuFluid: the liquid buffer '{}' would need {} bytes, more than a buffer can hold", szName, uiSize);
    return {};
  }

  plGALBufferCreationDescription desc;
  desc.m_uiTotalSize = static_cast<plUInt32>(uiSize);
  desc.m_BufferFlags = plGALBufferUsageFlags::ByteAddressBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
  if (bIndirect)
    desc.m_BufferFlags |= plGALBufferUsageFlags::DrawIndirect;
  desc.m_ResourceAccess.m_bImmutable = false;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  plGALBufferHandle hBuffer = pDevice->CreateBuffer(desc);
  if (!hBuffer.IsInvalidated())
    pDevice->GetBuffer(hBuffer)->SetDebugName(szName);
  return hBuffer;
}

bool plGpuFluidLiquidSolver::Create(const plGpuFluidLiquidSettings& settings)
{
  Destroy();

  m_Settings = settings;
  m_fDx = plMath::Max(settings.m_fCellSize, 1e-3f);
  const plUInt32 uiBrick = GPU_FLUID_LIQUID_BRICK;
  m_vBricks = plVec3U32(DivUp(plMath::Max(settings.m_vCells.x, uiBrick), uiBrick), DivUp(plMath::Max(settings.m_vCells.y, uiBrick), uiBrick), DivUp(plMath::Max(settings.m_vCells.z, uiBrick), uiBrick));
  m_vCells = plVec3U32(m_vBricks.x * uiBrick, m_vBricks.y * uiBrick, m_vBricks.z * uiBrick);
  m_uiCellCount = m_vCells.x * m_vCells.y * m_vCells.z;
  m_uiFaceCount = (m_vCells.x + 1) * (m_vCells.y + 1) * (m_vCells.z + 1);
  m_uiBrickCount = m_vBricks.x * m_vBricks.y * m_vBricks.z;
  m_uiCapacity = plMath::Max(settings.m_uiMaxParticles, 256u);
  m_ScanLayout = plGpuFluidScan::ComputeLayout(m_uiCellCount);

  // Multigrid levels: halve (rounding up) until the coarsest fits the one-group solve.
  m_uiLevelCount = 1;
  m_LevelDims[0] = m_vCells;
  while (m_uiLevelCount < GPU_FLUID_LIQUID_MAX_LEVELS)
  {
    const plVec3U32 vPrev = m_LevelDims[m_uiLevelCount - 1];
    if (vPrev.x * vPrev.y * vPrev.z <= GPU_FLUID_LIQUID_COARSEST_MAX_CELLS)
      break;
    m_LevelDims[m_uiLevelCount] = plVec3U32(DivUp(vPrev.x, 2), DivUp(vPrev.y, 2), DivUp(vPrev.z, 2));
    ++m_uiLevelCount;
  }

  const plUInt64 uiParticles = m_uiCapacity;
  bool bOk = true;
  auto Make = [&](plUInt64 uiBytes, const char* szName, bool bIndirect = false)
  {
    plGALBufferHandle h = CreateRawBuffer(uiBytes, szName, bIndirect);
    bOk = bOk && !h.IsInvalidated();
    return h;
  };

  for (plUInt32 i = 0; i < 2; ++i)
  {
    m_hPositions[i] = Make(uiParticles * GPU_FLUID_LIQUID_POSITION_BYTES, i == 0 ? "GpuFluid Liquid Positions A" : "GpuFluid Liquid Positions B");
    m_hVelocities[i] = Make(uiParticles * GPU_FLUID_LIQUID_VELOCITY_BYTES, i == 0 ? "GpuFluid Liquid Velocities A" : "GpuFluid Liquid Velocities B");
    m_hAffine[i] = Make(uiParticles * GPU_FLUID_LIQUID_AFFINE_BYTES, i == 0 ? "GpuFluid Liquid Affine A" : "GpuFluid Liquid Affine B");
    m_hParticleArgs[i] = Make(GPU_FLUID_LIQUID_ARGS_BYTES, i == 0 ? "GpuFluid Liquid Args A" : "GpuFluid Liquid Args B", true);
    m_hValid[i] = Make(plUInt64(m_uiFaceCount) * 4, i == 0 ? "GpuFluid Liquid Valid A" : "GpuFluid Liquid Valid B");
  }

  m_hRank = Make(uiParticles * 4, "GpuFluid Liquid Rank");
  m_hPoints = Make(uiParticles * 16, "GpuFluid Liquid Points");
  m_hDrawArgs = Make(16, "GpuFluid Liquid Draw Args", true);

  m_hCellCount = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid Cell Count");
  m_hCellStart = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid Cell Start");
  m_hCellType = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid Cell Type");
  m_hPressure = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid Pressure");
  m_hRhs = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid Rhs");
  m_hScanScratch = Make(m_ScanLayout.m_uiScratchBytes, "GpuFluid Liquid Scan Scratch");

  m_hVelG = Make(plUInt64(m_uiFaceCount) * 12, "GpuFluid Liquid Velocity");
  m_hVelS = Make(plUInt64(m_uiFaceCount) * 12, "GpuFluid Liquid Velocity Star");
  m_hVelT = Make(plUInt64(m_uiFaceCount) * 12, "GpuFluid Liquid Velocity Scratch");

  m_hBrickOccupied = Make(plUInt64(m_uiBrickCount) * 4, "GpuFluid Liquid Brick Occupied");
  m_hBrickActive = Make(plUInt64(m_uiBrickCount) * 4, "GpuFluid Liquid Brick Active");
  m_hBrickList = Make(plUInt64(m_uiBrickCount) * 4, "GpuFluid Liquid Brick List");
  m_hBrickArgs = Make(GPU_FLUID_LIQUID_ARGS_BYTES, "GpuFluid Liquid Brick Args", true);

  for (plUInt32 l = 1; l < m_uiLevelCount; ++l)
  {
    const plUInt64 uiCells = plUInt64(m_LevelDims[l].x) * m_LevelDims[l].y * m_LevelDims[l].z;
    m_hLevelX[l] = Make(uiCells * 4, "GpuFluid Liquid MG X");
    m_hLevelB[l] = Make(uiCells * 4, "GpuFluid Liquid MG B");
    m_hLevelType[l] = Make(uiCells * 4, "GpuFluid Liquid MG Type");
  }

  m_hCounters = Make(GPU_FLUID_LIQUID_CTR_WORDS * 4, "GpuFluid Liquid Counters");
  m_hStats = Make(GPU_FLUID_LIQUID_STATS_BYTES, "GpuFluid Liquid Stats");

  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plGpuFluidLiquidShape);
    desc.m_uiTotalSize = sizeof(plGpuFluidLiquidShape) * GPU_FLUID_LIQUID_MAX_SHAPES;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
    desc.m_ResourceAccess.m_bImmutable = false;

    plGpuFluidLiquidShape empty[GPU_FLUID_LIQUID_MAX_SHAPES] = {};
    m_hShapes = plGALDevice::GetDefaultDevice()->CreateBuffer(desc, plMakeArrayPtr(reinterpret_cast<const plUInt8*>(empty), sizeof(empty)));
    bOk = bOk && !m_hShapes.IsInvalidated();
    m_UploadedShapes.Clear();
    m_bShapesUploaded = true;
  }

  m_bCreated = true;
  if (!bOk)
  {
    plLog::Error("GpuFluid: could not create the GPU resources of a liquid volume ({} x {} x {} cells, {} particles)", m_vCells.x, m_vCells.y, m_vCells.z, m_uiCapacity);
    Destroy();
    return false;
  }

  m_bRecorded = false;
  m_bHasTick = false;
  m_uiCurrentSet = 0;
  m_uiLastResetCounter = 0xFFFFFFFFu;
  m_fLastSubstepDt = 0.0f;
  return true;
}

void plGpuFluidLiquidSolver::Destroy()
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  auto Free = [&](plGALBufferHandle& ref_hBuffer)
  {
    if (!ref_hBuffer.IsInvalidated() && pDevice != nullptr)
      pDevice->DestroyBuffer(ref_hBuffer);
    ref_hBuffer.Invalidate();
  };

  for (plUInt32 i = 0; i < 2; ++i)
  {
    Free(m_hPositions[i]);
    Free(m_hVelocities[i]);
    Free(m_hAffine[i]);
    Free(m_hParticleArgs[i]);
    Free(m_hValid[i]);
  }
  Free(m_hRank);
  Free(m_hPoints);
  Free(m_hDrawArgs);
  Free(m_hCellCount);
  Free(m_hCellStart);
  Free(m_hCellType);
  Free(m_hPressure);
  Free(m_hRhs);
  Free(m_hScanScratch);
  Free(m_hVelG);
  Free(m_hVelS);
  Free(m_hVelT);
  Free(m_hBrickOccupied);
  Free(m_hBrickActive);
  Free(m_hBrickList);
  Free(m_hBrickArgs);
  for (plUInt32 l = 0; l < GPU_FLUID_LIQUID_MAX_LEVELS; ++l)
  {
    Free(m_hLevelX[l]);
    Free(m_hLevelB[l]);
    Free(m_hLevelType[l]);
  }
  Free(m_hCounters);
  Free(m_hStats);
  Free(m_hShapes);

  m_bCreated = false;
  m_bRecorded = false;
}

void plGpuFluidLiquidSolver::GetPublish(plGpuFluidLiquidPublish& out_publish) const
{
  out_publish.m_hPoints = m_hPoints;
  out_publish.m_hDrawArgs = m_hDrawArgs;
  out_publish.m_uiMaxParticles = m_uiCapacity;
  out_publish.m_vDomainOrigin = m_vDomainOrigin;
  out_publish.m_vDomainSize = plVec3(static_cast<float>(m_vCells.x), static_cast<float>(m_vCells.y), static_cast<float>(m_vCells.z)) * m_fDx;
}

void plGpuFluidLiquidSolver::DecodeStats(plArrayPtr<const plUInt8> data, plGpuFluidLiquidStats& out_stats) const
{
  if (data.GetCount() < GPU_FLUID_LIQUID_STATS_BYTES)
    return;

  const plUInt32* pWords = reinterpret_cast<const plUInt32*>(data.GetPtr());
  auto AsFloat = [&](plUInt32 uiWord)
  {
    float f = 0.0f;
    plMemoryUtils::Copy(reinterpret_cast<plUInt32*>(&f), &pWords[uiWord], 1);
    return f;
  };

  out_stats.m_uiAlive = pWords[GPU_FLUID_LIQUID_STAT_ALIVE];
  out_stats.m_uiSeeded = pWords[GPU_FLUID_LIQUID_STAT_SEEDED];
  out_stats.m_uiDrained = pWords[GPU_FLUID_LIQUID_STAT_DRAINED];
  out_stats.m_uiLost = pWords[GPU_FLUID_LIQUID_STAT_LOST];
  out_stats.m_uiSubstepsTotal = pWords[GPU_FLUID_LIQUID_STAT_SUBSTEPS_TOTAL];
  out_stats.m_uiTicks = pWords[GPU_FLUID_LIQUID_STAT_TICKS];
  out_stats.m_uiActiveBricks = pWords[GPU_FLUID_LIQUID_STAT_ACTIVE_BRICKS];
  out_stats.m_uiOccupiedBricks = pWords[GPU_FLUID_LIQUID_STAT_OCCUPIED];
  out_stats.m_uiLiquidCells = pWords[GPU_FLUID_LIQUID_STAT_LIQUID_CELLS];
  out_stats.m_fVolumeCells = static_cast<float>(pWords[GPU_FLUID_LIQUID_STAT_VOLUME] / GPU_FLUID_LIQUID_VOLUME_SCALE);
  out_stats.m_fTargetVolumeCells = static_cast<float>(static_cast<double>(out_stats.m_uiSeeded - out_stats.m_uiDrained) / plMath::Max(1u, m_Settings.m_uiParticlesPerCell));
  out_stats.m_fMaxSpeed = AsFloat(GPU_FLUID_LIQUID_STAT_MAX_SPEED);
  out_stats.m_uiNaN = pWords[GPU_FLUID_LIQUID_STAT_NAN];
  out_stats.m_uiOverflow = pWords[GPU_FLUID_LIQUID_STAT_OVERFLOW];
  out_stats.m_fRhsMax = AsFloat(GPU_FLUID_LIQUID_STAT_RHS_MAX);
  out_stats.m_fResidualMax = AsFloat(GPU_FLUID_LIQUID_STAT_RESIDUAL_MAX);
  out_stats.m_uiSubsteps = pWords[GPU_FLUID_LIQUID_STAT_SUBSTEPS];
  out_stats.m_vBoundsMinCell.Set(pWords[GPU_FLUID_LIQUID_STAT_BOUNDS_MIN + 0], pWords[GPU_FLUID_LIQUID_STAT_BOUNDS_MIN + 1], pWords[GPU_FLUID_LIQUID_STAT_BOUNDS_MIN + 2]);
  out_stats.m_vBoundsMaxCell.Set(pWords[GPU_FLUID_LIQUID_STAT_BOUNDS_MAX + 0], pWords[GPU_FLUID_LIQUID_STAT_BOUNDS_MAX + 1], pWords[GPU_FLUID_LIQUID_STAT_BOUNDS_MAX + 2]);
  out_stats.m_uiAbove1cm = pWords[GPU_FLUID_LIQUID_STAT_ABOVE_1CM];
  out_stats.m_uiAbove1mm = pWords[GPU_FLUID_LIQUID_STAT_ABOVE_1MM];

  const double fAlive = plMath::Max(1u, out_stats.m_uiAlive);
  out_stats.m_fMeanSpeed = static_cast<float>(pWords[GPU_FLUID_LIQUID_STAT_SPEED_SUM] / GPU_FLUID_LIQUID_SPEED_SUM_SCALE / fAlive);

  // The centroid is summed in 1/16 cell units per particle into three uints: exact while alive * cells * 16 < 2^32.
  out_stats.m_vCentroid.Set(static_cast<float>(pWords[GPU_FLUID_LIQUID_STAT_CENTROID + 0] / 16.0 / fAlive * m_fDx),
    static_cast<float>(pWords[GPU_FLUID_LIQUID_STAT_CENTROID + 1] / 16.0 / fAlive * m_fDx), static_cast<float>(pWords[GPU_FLUID_LIQUID_STAT_CENTROID + 2] / 16.0 / fAlive * m_fDx));

  const plUInt32 uiBins = plMath::Min<plUInt32>(pWords[GPU_FLUID_LIQUID_STAT_HIST_COUNT], GPU_FLUID_LIQUID_HIST_BINS);
  out_stats.m_FrontHistogram.SetCountUninitialized(uiBins);
  for (plUInt32 i = 0; i < uiBins; ++i)
    out_stats.m_FrontHistogram[i] = pWords[GPU_FLUID_LIQUID_STAT_HIST + i];
}

void plGpuFluidLiquidSolver::Record(plRenderGraph& ref_graph, const plGpuFluidLiquidShaders& shaders, const plGpuFluidLiquidFrameInput& input, const plGpuFluidLiquidStats* pStats, plUInt64 uiStatsAge)
{
  if (!m_bCreated)
    return;

  const bool bReset = input.m_uiResetCounter != m_uiLastResetCounter;
  const bool bStep = input.m_bSimulate && input.m_fDt > 0.0f && (!m_bHasTick || input.m_uiTick != m_uiLastTick || bReset);
  if (!bReset && !bStep)
    return;

  m_uiLastTick = input.m_uiTick;
  m_bHasTick = true;
  m_vDomainOrigin = input.m_vDomainOrigin;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  // Analytic solids: rare edits, so the buffer is recreated with the new content instead of updated in place.
  {
    const plUInt32 uiShapes = plMath::Min<plUInt32>(input.m_Shapes.GetCount(), GPU_FLUID_LIQUID_MAX_SHAPES);
    bool bChanged = uiShapes != m_UploadedShapes.GetCount();
    for (plUInt32 i = 0; !bChanged && i < uiShapes; ++i)
      bChanged = plMemoryUtils::Compare(&input.m_Shapes[i], &m_UploadedShapes[i], 1) != 0;

    if (bChanged)
    {
      plGpuFluidLiquidShape shapes[GPU_FLUID_LIQUID_MAX_SHAPES] = {};
      for (plUInt32 i = 0; i < uiShapes; ++i)
        shapes[i] = input.m_Shapes[i];

      plGALBufferCreationDescription desc;
      desc.m_uiStructSize = sizeof(plGpuFluidLiquidShape);
      desc.m_uiTotalSize = sizeof(plGpuFluidLiquidShape) * GPU_FLUID_LIQUID_MAX_SHAPES;
      desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
      desc.m_ResourceAccess.m_bImmutable = false;
      plGALBufferHandle hNew = pDevice->CreateBuffer(desc, plMakeArrayPtr(reinterpret_cast<const plUInt8*>(shapes), sizeof(shapes)));
      if (!hNew.IsInvalidated())
      {
        if (!m_hShapes.IsInvalidated())
          pDevice->DestroyBuffer(m_hShapes);
        m_hShapes = hNew;
        m_UploadedShapes.Clear();
        for (plUInt32 i = 0; i < uiShapes; ++i)
          m_UploadedShapes.PushBack(input.m_Shapes[i]);
      }
    }
  }

  // Substeps from the newest max speed, which is two to three frames old: gravity can have added g * dt per frame since.
  const float fGravity = input.m_vGravity.GetLength();
  float fSpeed = fGravity * input.m_fDt;
  if (!bReset && pStats != nullptr)
    fSpeed += pStats->m_fMaxSpeed + fGravity * input.m_fDt * static_cast<float>(plMath::Min<plUInt64>(uiStatsAge, 8));
  const float fCfl = plMath::Max(input.m_fCflNumber, 0.1f);
  const plUInt32 uiMaxSubsteps = plMath::Clamp<plUInt32>(input.m_uiMaxSubsteps, 1, 16);
  plUInt32 uiSubsteps = bStep ? plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Ceil(fSpeed * input.m_fDt / (fCfl * m_fDx))), 1, uiMaxSubsteps) : 0;
  if (bReset)
    m_uiTicksSinceReset = 0;
  if (bStep && m_uiTicksSinceReset++ < input.m_uiStartupTicks)
    uiSubsteps = uiMaxSubsteps;
  const float fSubDt = bStep ? input.m_fDt / static_cast<float>(uiSubsteps) : 0.0f;
  m_uiLastSubsteps = uiSubsteps;

  // Base constants of this tick.
  plGpuFluidLiquidConstants base = {};
  const plUInt32 uiShapeCount = m_UploadedShapes.GetCount();
  base.GpuFluidLiqCells = plVec4U32(m_vCells.x, m_vCells.y, m_vCells.z, (input.m_uiOpenFaces & 0x3Fu) | (uiShapeCount << 8));
  base.GpuFluidLiqBricks = plVec4U32(m_vBricks.x, m_vBricks.y, m_vBricks.z, m_uiCapacity);
  base.GpuFluidLiqGrid = plVec4(m_fDx, 1.0f / m_fDx, fSubDt, plMath::Clamp(input.m_fFlipBlend, 0.0f, 1.0f));
  base.GpuFluidLiqGravity = plVec4(input.m_vGravity.x, input.m_vGravity.y, input.m_vGravity.z, static_cast<float>(plMath::Max(1u, m_Settings.m_uiParticlesPerCell)));
  base.GpuFluidLiqSolve = plVec4(1.0f / plMath::Clamp(input.m_fTheta, 0.05f, 1.0f), input.m_fDensityGain, input.m_fVolumeGain, 1.0f);
  base.GpuFluidLiqWall = plVec4(plMath::Clamp(input.m_fWallMargin, 0.0f, 0.45f), plMath::Clamp(input.m_fWallFriction, 0.0f, 1.0f), input.m_fFrontHeight, input.m_fFrontBinSize);

  const plGpuFluidLiquidShaders* pShaders = &shaders;

  // Every buffer is imported; passes declare what they touch, so the graph orders and transitions them.
  auto G = [&](plGALBufferHandle h) { return ref_graph.ImportBuffer(h); };
  const plBitflags<plGALResourceState> indirect = plGALResourceState::IndirectArgument | plGALResourceState::ComputeShaderRead;

  // Handles by value for the callbacks.
  const plGALBufferHandle hStats = m_hStats, hCounters = m_hCounters, hShapes = m_hShapes;
  const plGALBufferHandle hCellCount = m_hCellCount, hCellStart = m_hCellStart, hCellType = m_hCellType, hPressure = m_hPressure, hRhs = m_hRhs, hScratch = m_hScanScratch;
  const plGALBufferHandle hVelG = m_hVelG, hVelS = m_hVelS, hVelT = m_hVelT, hValid0 = m_hValid[0], hValid1 = m_hValid[1];
  const plGALBufferHandle hBrickOcc = m_hBrickOccupied, hBrickActive = m_hBrickActive, hBrickList = m_hBrickList, hBrickArgs = m_hBrickArgs;
  const plGALBufferHandle hRank = m_hRank, hPoints = m_hPoints, hDrawArgs = m_hDrawArgs;
  const plUInt32 uiCellCount = m_uiCellCount, uiBrickCount = m_uiBrickCount;

  // Begin: per-frame stats, and on a reset every counter.
  {
    auto pass = ref_graph.AddComputePass("GpuFluid Liquid/Begin");
    pass.WriteBuffer(G(hStats));
    pass.WriteBuffer(G(hCounters));
    pass.HasSideEffects();
    plGpuFluidLiquidConstants c = base;
    c.GpuFluidLiqPass = plVec4U32(bReset ? 1u : 0u, 0, 0, 0);
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        Recorder rec(ctx);
        rec.Run(pShaders->m_hStatsClear, c, LinearGroups(GPU_FLUID_LIQUID_STAT_WORD_COUNT, 256), {W("GpuFluidLiqStats", hStats), W("GpuFluidLiqCounters", hCounters)});
      });
  }

  // Seed: the fill boxes into set A, the alive count with them.
  if (bReset)
  {
    m_uiLastResetCounter = input.m_uiResetCounter;
    m_uiCurrentSet = 0;
    m_fLastSubstepDt = 0.0f;

    struct SeedJob
    {
      plGpuFluidLiquidSeedConstants m_Constants;
      plUInt32 m_uiCount;
    };
    plHybridArray<SeedJob, 4> jobs;
    plUInt32 uiTotal = 0;
    const plUInt32 uiPpc = plMath::Max(1u, m_Settings.m_uiParticlesPerCell);
    for (plUInt32 i = 0; i < input.m_Fills.GetCount(); ++i)
    {
      const plGpuFluidLiquidFill& fill = input.m_Fills[i];
      const plVec3U32 vMin = fill.m_vCellMin.CompMin(m_vCells);
      const plVec3U32 vCount = fill.m_vCellCount.CompMin(m_vCells - vMin);
      const plUInt64 uiWanted = plUInt64(vCount.x) * vCount.y * vCount.z * uiPpc;
      const plUInt32 uiCount = static_cast<plUInt32>(plMath::Min<plUInt64>(uiWanted, m_uiCapacity - uiTotal));
      if (uiCount < uiWanted)
        plLog::Warning("GpuFluid: a fill box of a liquid volume wants {} particles, {} fit", uiWanted, uiCount);
      if (uiCount == 0)
        continue;

      SeedJob& job = jobs.ExpandAndGetRef();
      job.m_uiCount = uiCount;
      job.m_Constants.GpuFluidSeedCellMin = plVec4U32(vMin.x, vMin.y, vMin.z, uiPpc);
      job.m_Constants.GpuFluidSeedCellCount = plVec4U32(vCount.x, vCount.y, vCount.z, uiTotal);
      job.m_Constants.GpuFluidSeedVelocity = plVec4(fill.m_vVelocity.x, fill.m_vVelocity.y, fill.m_vVelocity.z, plMath::Clamp(fill.m_fJitter, 0.0f, 1.0f));
      job.m_Constants.GpuFluidSeedGrid = plVec4(m_fDx, 0, 0, 0);
      job.m_Constants.GpuFluidSeedInfo = plVec4U32(0x9E3779B9u * (input.m_uiResetCounter + 1) + i * 0x85EBCA6Bu, uiCount, 0, 0);
      uiTotal += uiCount;
    }

    const plGALBufferHandle hPos = m_hPositions[0], hVel = m_hVelocities[0], hAff = m_hAffine[0], hArgs = m_hParticleArgs[0];
    auto pass = ref_graph.AddComputePass("GpuFluid Liquid/Seed");
    pass.WriteBuffer(G(hPos));
    pass.WriteBuffer(G(hVel));
    pass.WriteBuffer(G(hAff));
    pass.WriteBuffer(G(hArgs));
    pass.WriteBuffer(G(hCounters));
    pass.HasSideEffects();
    plGpuFluidLiquidConstants c = base;
    c.GpuFluidLiqPass = plVec4U32(uiTotal, 0, 0, 0);
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        Recorder rec(ctx);
        for (const SeedJob& job : jobs)
        {
          rec.Run(pShaders->m_hSeed, job.m_Constants, LinearGroups(job.m_uiCount, GPU_FLUID_LIQUID_PARTICLE_GROUP),
            {W("GpuFluidLiqPositions", hPos), W("GpuFluidLiqVelocities", hVel), W("GpuFluidLiqAffine", hAff)}, "plGpuFluidLiquidSeedConstants");
        }
        rec.Run(pShaders->m_hSeedFinish, c, plVec3U32(1, 1, 1), {W("GpuFluidLiqArgsOutput", hArgs), W("GpuFluidLiqCounters", hCounters)});
      });
  }

  for (plUInt32 uiSub = 0; uiSub < uiSubsteps; ++uiSub)
  {
    const plUInt32 src = m_uiCurrentSet;
    const plUInt32 dst = 1 - src;

    plGpuFluidLiquidConstants c = base;
    // Warm start: last substep's pressure, rescaled, since the stored value scales with dt.
    c.GpuFluidLiqSolve.w = m_fLastSubstepDt > 0.0f ? fSubDt / m_fLastSubstepDt : 0.0f;
    m_fLastSubstepDt = fSubDt;

    const plGALBufferHandle hSrcPos = m_hPositions[src], hSrcVel = m_hVelocities[src], hSrcAff = m_hAffine[src], hSrcArgs = m_hParticleArgs[src];
    const plGALBufferHandle hPos = m_hPositions[dst], hVel = m_hVelocities[dst], hAff = m_hAffine[dst], hArgs = m_hParticleArgs[dst];
    const plGpuFluidScan::Layout scanLayout = m_ScanLayout;

    // Liquid/Sort
    {
      auto pass = ref_graph.AddComputePass("GpuFluid Liquid/Sort");
      pass.ReadBuffer(G(hSrcArgs), indirect);
      pass.ReadBuffer(G(hSrcPos), plGALResourceState::ComputeShaderRead);
      pass.ReadBuffer(G(hSrcVel), plGALResourceState::ComputeShaderRead);
      pass.ReadBuffer(G(hSrcAff), plGALResourceState::ComputeShaderRead);
      for (plGALBufferHandle h : {hPos, hVel, hAff, hArgs, hRank, hCellCount, hCellStart, hScratch, hCounters, hBrickOcc, hBrickActive, hBrickList, hBrickArgs})
        pass.WriteBuffer(G(h));
      pass.HasSideEffects();
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          Recorder rec(ctx);
          rec.Run(pShaders->m_hClear, c, LinearGroups(plMath::Max(uiCellCount, uiBrickCount), 256),
            {W("GpuFluidLiqCellCount", hCellCount), W("GpuFluidLiqBrickOccupied", hBrickOcc), W("GpuFluidLiqCounters", hCounters)});

          rec.RunIndirect(pShaders->m_hCount, c, hSrcArgs,
            {R("GpuFluidLiqParticleArgs", hSrcArgs), R("GpuFluidLiqPositions", hSrcPos), W("GpuFluidLiqCellCount", hCellCount), W("GpuFluidLiqRank", hRank),
              W("GpuFluidLiqBrickOccupied", hBrickOcc), W("GpuFluidLiqCounters", hCounters)});

          pShaders->m_Scan.RecordExclusiveScan(rec.m_pContext, rec.m_pEncoder, scanLayout, hCellCount, 0, hCellStart, 0, hScratch);
          pShaders->m_Scan.RecordIndirectArgs(rec.m_pContext, hScratch, scanLayout.GetTotalOffset(), GPU_FLUID_LIQUID_PARTICLE_GROUP, hArgs, 0);
          rec.After({W("", hArgs)});

          rec.RunIndirect(pShaders->m_hScatter, c, hSrcArgs,
            {R("GpuFluidLiqParticleArgs", hSrcArgs), R("GpuFluidLiqPositions", hSrcPos), R("GpuFluidLiqVelocities", hSrcVel), R("GpuFluidLiqAffine", hSrcAff),
              W("GpuFluidLiqRank", hRank), W("GpuFluidLiqCellStart", hCellStart), W("GpuFluidLiqPositionsOut", hPos), W("GpuFluidLiqVelocitiesOut", hVel),
              W("GpuFluidLiqAffineOut", hAff)});

          rec.Run(pShaders->m_hBricks, c, LinearGroups(uiBrickCount, 256),
            {W("GpuFluidLiqBrickOccupied", hBrickOcc), W("GpuFluidLiqBrickActive", hBrickActive), W("GpuFluidLiqBrickListOut", hBrickList), W("GpuFluidLiqCounters", hCounters)});

          pShaders->m_Scan.RecordIndirectArgs(rec.m_pContext, hCounters, GPU_FLUID_LIQUID_CTR_ACTIVE_BRICKS * 4, 1, hBrickArgs, 0);
          rec.After({W("", hBrickArgs)});
        });
    }

    // Liquid/P2G
    {
      auto pass = ref_graph.AddComputePass("GpuFluid Liquid/P2G");
      pass.ReadBuffer(G(hBrickArgs), indirect);
      for (plGALBufferHandle h : {hBrickList, hCellCount, hCellStart, hPos, hVel, hAff})
        pass.ReadBuffer(G(h), plGALResourceState::ComputeShaderRead);
      for (plGALBufferHandle h : {hVelG, hVelT, hValid0, hValid1, hCounters})
        pass.WriteBuffer(G(h));
      pass.HasSideEffects();
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          Recorder rec(ctx);
          rec.RunIndirect(pShaders->m_hP2G, c, hBrickArgs,
            {R("GpuFluidLiqBrickArgs", hBrickArgs), R("GpuFluidLiqBrickList", hBrickList), R("GpuFluidLiqCellCount", hCellCount), R("GpuFluidLiqCellStart", hCellStart),
              R("GpuFluidLiqPositions", hPos), R("GpuFluidLiqVelocities", hVel), R("GpuFluidLiqAffine", hAff), W("GpuFluidLiqVelOut", hVelT),
              W("GpuFluidLiqValidOut", hValid0), W("GpuFluidLiqCounters", hCounters)});

          // Three layers into the air: T -> G -> T -> G.
          const plGALBufferHandle from[3] = {hVelT, hVelG, hVelT};
          const plGALBufferHandle to[3] = {hVelG, hVelT, hVelG};
          const plGALBufferHandle validFrom[3] = {hValid0, hValid1, hValid0};
          const plGALBufferHandle validTo[3] = {hValid1, hValid0, hValid1};
          for (plUInt32 i = 0; i < 3; ++i)
          {
            rec.RunIndirect(pShaders->m_hExtrapolate, c, hBrickArgs,
              {R("GpuFluidLiqBrickArgs", hBrickArgs), R("GpuFluidLiqBrickList", hBrickList), W("GpuFluidLiqVelIn", from[i]), W("GpuFluidLiqValidIn", validFrom[i]),
                W("GpuFluidLiqVelOut", to[i]), W("GpuFluidLiqValidOut", validTo[i])});
          }
        });
    }

    // Liquid/Solve
    {
      const plUInt32 uiLevels = m_uiLevelCount;
      plVec3U32 levelDims[GPU_FLUID_LIQUID_MAX_LEVELS];
      plGALBufferHandle levelX[GPU_FLUID_LIQUID_MAX_LEVELS];
      plGALBufferHandle levelB[GPU_FLUID_LIQUID_MAX_LEVELS];
      plGALBufferHandle levelType[GPU_FLUID_LIQUID_MAX_LEVELS];
      for (plUInt32 l = 0; l < GPU_FLUID_LIQUID_MAX_LEVELS; ++l)
      {
        levelDims[l] = l < uiLevels ? m_LevelDims[l] : plVec3U32::MakeZero();
        levelX[l] = l == 0 ? hPressure : m_hLevelX[l];
        levelB[l] = l == 0 ? hRhs : m_hLevelB[l];
        levelType[l] = l == 0 ? hCellType : m_hLevelType[l];
      }

      const plUInt32 uiCycles = plMath::Clamp<plUInt32>(input.m_uiPressureCycles, 0, 32);
      const plUInt32 uiPre = plMath::Clamp<plUInt32>(input.m_uiPreSmooth, 0, 16);
      const plUInt32 uiPost = plMath::Clamp<plUInt32>(input.m_uiPostSmooth, 0, 16);
      const plUInt32 uiCoarsestSweeps = plMath::Clamp<plUInt32>(input.m_uiCoarsestSweeps, 1, 512);
      const float fCoarseScale = input.m_fCoarseScale;

      auto pass = ref_graph.AddComputePass("GpuFluid Liquid/Solve");
      pass.ReadBuffer(G(hBrickArgs), indirect);
      for (plGALBufferHandle h : {hBrickList, hBrickActive, hCellCount, hShapes})
        pass.ReadBuffer(G(h), plGALResourceState::ComputeShaderRead);
      for (plGALBufferHandle h : {hCellType, hVelG, hVelS, hVelT, hValid0, hValid1, hPressure, hRhs, hCounters})
        pass.WriteBuffer(G(h));
      for (plUInt32 l = 1; l < uiLevels; ++l)
      {
        pass.WriteBuffer(G(levelX[l]));
        pass.WriteBuffer(G(levelB[l]));
        pass.WriteBuffer(G(levelType[l]));
      }
      pass.HasSideEffects();
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          Recorder rec(ctx);
          const Bind brickArgs = R("GpuFluidLiqBrickArgs", hBrickArgs);
          const Bind brickList = R("GpuFluidLiqBrickList", hBrickList);
          const Bind brickActive = R("GpuFluidLiqBrickActive", hBrickActive);

          rec.RunIndirect(pShaders->m_hClassify, c, hBrickArgs,
            {brickArgs, brickList, R("GpuFluidLiqCellCount", hCellCount), R("GpuFluidLiqShapes", hShapes), W("GpuFluidLiqCellType", hCellType), W("GpuFluidLiqCounters", hCounters)});

          rec.RunIndirect(pShaders->m_hForces, c, hBrickArgs,
            {brickArgs, brickList, brickActive, W("GpuFluidLiqCellType", hCellType), W("GpuFluidLiqVel", hVelG), W("GpuFluidLiqVelStar", hVelS)});

          rec.RunIndirect(pShaders->m_hRhs, c, hBrickArgs,
            {brickArgs, brickList, R("GpuFluidLiqCellCount", hCellCount), W("GpuFluidLiqCellType", hCellType), W("GpuFluidLiqVel", hVelG), W("GpuFluidLiqPressure", hPressure),
              W("GpuFluidLiqRhs", hRhs), W("GpuFluidLiqCounters", hCounters)});

          auto LevelConstants = [&](plUInt32 uiLevel, plUInt32 uiColour, float fOmega)
          {
            plGpuFluidLiquidConstants lc = c;
            lc.GpuFluidLiqPass = plVec4U32(uiLevel, uiColour, uiCoarsestSweeps, 0);
            lc.GpuFluidLiqPassF = plVec4(fOmega, fCoarseScale, 0.0f, 0.0f);
            return lc;
          };

          auto Smooth = [&](plUInt32 uiLevel, plUInt32 uiSweeps)
          {
            for (plUInt32 s = 0; s < uiSweeps; ++s)
            {
              for (plUInt32 uiColour = 0; uiColour < 2; ++uiColour)
              {
                const plGpuFluidLiquidConstants lc = LevelConstants(uiLevel, uiColour, 1.0f);
                if (uiLevel == 0)
                {
                  rec.RunIndirect(pShaders->m_hSmoothBricks, lc, hBrickArgs,
                    {brickArgs, brickList, brickActive, W("GpuFluidLiqMgX", levelX[0]), W("GpuFluidLiqMgB", levelB[0]), W("GpuFluidLiqMgType", levelType[0])});
                }
                else
                {
                  rec.Run(pShaders->m_hSmoothDense, lc, DenseGroups(levelDims[uiLevel]),
                    {brickActive, W("GpuFluidLiqMgX", levelX[uiLevel]), W("GpuFluidLiqMgB", levelB[uiLevel]), W("GpuFluidLiqMgType", levelType[uiLevel])});
                }
              }
            }
          };

          // V-cycles, the recursion unrolled: down with pre-smoothing and restriction, the coarsest solved in one
          // group, up with prolongation and post-smoothing.
          for (plUInt32 uiCycle = 0; uiCycle < uiCycles; ++uiCycle)
          {
            if (uiLevels == 1)
            {
              Smooth(0, uiCoarsestSweeps);
              continue;
            }

            for (plUInt32 l = 0; l + 1 < uiLevels; ++l)
            {
              Smooth(l, uiPre);
              rec.Run(pShaders->m_hRestrict, LevelConstants(l + 1, 0, 1.0f), DenseGroups(levelDims[l + 1]),
                {brickActive, W("GpuFluidLiqMgFineX", levelX[l]), W("GpuFluidLiqMgFineB", levelB[l]), W("GpuFluidLiqMgFineType", levelType[l]), W("GpuFluidLiqMgX", levelX[l + 1]),
                  W("GpuFluidLiqMgB", levelB[l + 1]), W("GpuFluidLiqMgType", levelType[l + 1])});
            }

            const plUInt32 uiCoarsest = uiLevels - 1;
            rec.Run(pShaders->m_hCoarsest, LevelConstants(uiCoarsest, 0, 1.5f), plVec3U32(1, 1, 1),
              {brickActive, W("GpuFluidLiqMgX", levelX[uiCoarsest]), W("GpuFluidLiqMgB", levelB[uiCoarsest]), W("GpuFluidLiqMgType", levelType[uiCoarsest])});

            for (plUInt32 l = uiLevels - 1; l-- > 0;)
            {
              const plGpuFluidLiquidConstants lc = LevelConstants(l, 0, 1.0f);
              if (l == 0)
              {
                rec.RunIndirect(pShaders->m_hProlongBricks, lc, hBrickArgs,
                  {brickArgs, brickList, brickActive, W("GpuFluidLiqMgX", levelX[0]), W("GpuFluidLiqMgType", levelType[0]), W("GpuFluidLiqMgCoarseX", levelX[1])});
              }
              else
              {
                rec.Run(pShaders->m_hProlongDense, lc, DenseGroups(levelDims[l]),
                  {brickActive, W("GpuFluidLiqMgX", levelX[l]), W("GpuFluidLiqMgType", levelType[l]), W("GpuFluidLiqMgCoarseX", levelX[l + 1])});
              }
              Smooth(l, uiPost);
            }
          }

          rec.RunIndirect(pShaders->m_hResidual, LevelConstants(0, 0, 1.0f), hBrickArgs,
            {brickArgs, brickList, brickActive, W("GpuFluidLiqMgX", levelX[0]), W("GpuFluidLiqMgB", levelB[0]), W("GpuFluidLiqMgType", levelType[0]), W("GpuFluidLiqCounters", hCounters)});

          rec.RunIndirect(pShaders->m_hProject, c, hBrickArgs,
            {brickArgs, brickList, brickActive, W("GpuFluidLiqCellType", hCellType), W("GpuFluidLiqPressure", hPressure), W("GpuFluidLiqVel", hVelG), W("GpuFluidLiqValidOut", hValid0)});

          // Three layers into the air again: G -> T -> G -> T. The result is in T.
          const plGALBufferHandle from[3] = {hVelG, hVelT, hVelG};
          const plGALBufferHandle to[3] = {hVelT, hVelG, hVelT};
          const plGALBufferHandle validFrom[3] = {hValid0, hValid1, hValid0};
          const plGALBufferHandle validTo[3] = {hValid1, hValid0, hValid1};
          for (plUInt32 i = 0; i < 3; ++i)
          {
            rec.RunIndirect(pShaders->m_hExtrapolate, c, hBrickArgs,
              {brickArgs, brickList, W("GpuFluidLiqVelIn", from[i]), W("GpuFluidLiqValidIn", validFrom[i]), W("GpuFluidLiqVelOut", to[i]), W("GpuFluidLiqValidOut", validTo[i])});
          }
        });
    }

    // Liquid/G2P
    {
      auto pass = ref_graph.AddComputePass("GpuFluid Liquid/G2P");
      pass.ReadBuffer(G(hArgs), indirect);
      for (plGALBufferHandle h : {hVelT, hVelS, hShapes})
        pass.ReadBuffer(G(h), plGALResourceState::ComputeShaderRead);
      for (plGALBufferHandle h : {hPos, hVel, hAff, hCounters})
        pass.WriteBuffer(G(h));
      pass.HasSideEffects();
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          Recorder rec(ctx);
          rec.RunIndirect(pShaders->m_hG2P, c, hArgs,
            {R("GpuFluidLiqParticleArgs", hArgs), R("GpuFluidLiqVelNew", hVelT), R("GpuFluidLiqVelOld", hVelS), R("GpuFluidLiqShapes", hShapes), W("GpuFluidLiqPositions", hPos),
              W("GpuFluidLiqVelocities", hVel), W("GpuFluidLiqAffine", hAff), W("GpuFluidLiqCounters", hCounters)});
        });
    }

    m_uiCurrentSet = dst;
  }

  // Render: the points the debug renderer draws, the draw arguments and the frame's stats.
  {
    const plUInt32 cur = m_uiCurrentSet;
    const plGALBufferHandle hPos = m_hPositions[cur], hVel = m_hVelocities[cur], hArgs = m_hParticleArgs[cur];

    plGpuFluidLiquidConstants c = base;
    c.GpuFluidLiqPass = plVec4U32(uiSubsteps, 0, 0, 0);
    c.GpuFluidLiqPassF = plVec4(input.m_vDomainOrigin.x, input.m_vDomainOrigin.y, input.m_vDomainOrigin.z, 0.0f);

    auto pass = ref_graph.AddComputePass("GpuFluid Liquid/Render");
    pass.ReadBuffer(G(hArgs), indirect);
    pass.ReadBuffer(G(hPos), plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(G(hVel), plGALResourceState::ComputeShaderRead);
    for (plGALBufferHandle h : {hPoints, hDrawArgs, hStats, hCounters})
      pass.WriteBuffer(G(h));
    pass.HasSideEffects();
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        Recorder rec(ctx);
        rec.RunIndirect(pShaders->m_hRenderPrep, c, hArgs,
          {R("GpuFluidLiqParticleArgs", hArgs), R("GpuFluidLiqPositions", hPos), R("GpuFluidLiqVelocities", hVel), W("GpuFluidLiqPoints", hPoints), W("GpuFluidLiqStats", hStats)});
        rec.Run(pShaders->m_hFrameEnd, c, plVec3U32(1, 1, 1),
          {R("GpuFluidLiqParticleArgs", hArgs), W("GpuFluidLiqCounters", hCounters), W("GpuFluidLiqStats", hStats), W("GpuFluidLiqDrawArgs", hDrawArgs)});
      });
  }

  m_bRecorded = true;
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidLiquidSolver);
