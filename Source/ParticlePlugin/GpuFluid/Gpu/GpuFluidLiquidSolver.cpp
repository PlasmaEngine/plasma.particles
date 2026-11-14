#include <ParticlePlugin/ParticlePluginPCH.h>

#include <RendererFoundation/Profiling/Profiling.h>
#include <optional>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Algorithm/HashingUtils.h>
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
  m_hLevelRegion = Load("LiquidLevelRegion");
  m_hSmoothBricksBlock = Load("LiquidSmoothBricksBlock");
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
  m_hPcgInit = Load("LiquidPcgInit");
  m_hPcgDot = Load("LiquidPcgDot");
  m_hPcgApply = Load("LiquidPcgApply");
  m_hPcgReduce = Load("LiquidPcgReduce");
  m_hPcgUpdateX = Load("LiquidPcgUpdateX");
  m_hPcgUpdateP = Load("LiquidPcgUpdateP");
  m_hLevelSet = Load("LiquidLevelSet");
  m_hSurfaceBlend = Load("LiquidSurfaceBlend");
  m_hSurfaceSmooth = Load("LiquidSurfaceSmooth");
  m_hSurfaceVerts = Load("LiquidSurfaceVerts");
  m_hSurfaceQuads = Load("LiquidSurfaceQuads");
  m_hSurfaceArgs = Load("LiquidSurfaceArgs");
  m_hSource = Load("LiquidSource");
  m_hSourceFinish = Load("LiquidSourceFinish");
  m_hSolidsClear = Load("LiquidSolidsClear");
  m_hSolidsBake = Load("LiquidSolidsBake");
  m_hSolidsSign = Load("LiquidSolidsSign");
  m_hSleep = Load("LiquidSleep");
  m_hProbes = Load("LiquidProbes");
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
    void RunIndirect(plShaderResourceHandle hShader, const T& constants, plGALBufferHandle hArgs, std::initializer_list<Bind> binds, plUInt32 uiArgsOffset = 0)
    {
      Bindings(hShader, "plGpuFluidLiquidConstants", constants, binds);
      m_pContext->DispatchIndirect(hArgs, uiArgsOffset).IgnoreResult();
      After(binds);
    }
  };

  /// A GPU timing scope inside a pass (m_bProfileKernels only): its time is exported as the stat
  /// "Renderer/GPU/GpuFluid LiqProf/<stage>", summed over the frame.
  struct ProfileScope
  {
#if PL_ENABLED(PL_USE_PROFILING)
    std::optional<plProfilingScopeAndMarker> m_Scope;
    ProfileScope(bool bEnabled, plGALCommandEncoder* pEncoder, const char* szName)
    {
      if (bEnabled)
        m_Scope.emplace(pEncoder, szName);
    }
#else
    ProfileScope(bool, plGALCommandEncoder*, const char*) {}
#endif
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

plGALBufferHandle plGpuFluidLiquidSolver::CreateRawBuffer(plUInt64 uiBytes, const char* szName, bool bIndirect, bool bZero)
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

  // A new buffer's content is undefined on D3D12 and Vulkan (Metal happens to zero it). Grid, brick and counter
  // buffers start at zero: some are read before a kernel writes them (the surface stamps, the warm start, cells of
  // bricks that were never active). The particle arrays are only read up to the alive count, which seeding writes.
  plDynamicArray<plUInt8> zeros;
  if (bZero)
    zeros.SetCount(desc.m_uiTotalSize);

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  plGALBufferHandle hBuffer = pDevice->CreateBuffer(desc, zeros.GetArrayPtr());
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
  auto Make = [&](plUInt64 uiBytes, const char* szName, bool bIndirect = false, bool bZero = true)
  {
    plGALBufferHandle h = CreateRawBuffer(uiBytes, szName, bIndirect, bZero);
    bOk = bOk && !h.IsInvalidated();
    return h;
  };

  for (plUInt32 i = 0; i < 2; ++i)
  {
    m_hPositions[i] = Make(uiParticles * GPU_FLUID_LIQUID_POSITION_BYTES, i == 0 ? "GpuFluid Liquid Positions A" : "GpuFluid Liquid Positions B", false, false);
    m_hVelocities[i] = Make(uiParticles * GPU_FLUID_LIQUID_VELOCITY_BYTES, i == 0 ? "GpuFluid Liquid Velocities A" : "GpuFluid Liquid Velocities B", false, false);
    m_hAffine[i] = Make(uiParticles * GPU_FLUID_LIQUID_AFFINE_BYTES, i == 0 ? "GpuFluid Liquid Affine A" : "GpuFluid Liquid Affine B", false, false);
    m_hParticleArgs[i] = Make(GPU_FLUID_LIQUID_ARGS_BYTES, i == 0 ? "GpuFluid Liquid Args A" : "GpuFluid Liquid Args B", true);
    m_hValid[i] = Make(plUInt64(m_uiFaceCount) * 4, i == 0 ? "GpuFluid Liquid Valid A" : "GpuFluid Liquid Valid B");
  }

  m_hRank = Make(uiParticles * 4, "GpuFluid Liquid Rank", false, false);
  m_hPoints = Make(uiParticles * 16, "GpuFluid Liquid Points", false, false);
  m_hDrawArgs = Make(GPU_FLUID_LIQUID_DRAW_ARGS_BYTES, "GpuFluid Liquid Draw Args", true);

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

  m_hPcgR = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid PCG R");
  m_hPcgZ = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid PCG Z");
  m_hPcgP = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid PCG P");
  m_hPcgQ = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid PCG Q");
  m_hPcgPartials = Make(plUInt64(m_uiBrickCount) * 4, "GpuFluid Liquid PCG Partials");
  m_hPcgScalars = Make(GPU_FLUID_LIQUID_PCG_WORDS * 4, "GpuFluid Liquid PCG Scalars");
  m_hLevelRegion = Make(GPU_FLUID_LIQUID_MAX_LEVELS * GPU_FLUID_LIQUID_REGION_STRIDE, "GpuFluid Liquid Level Region", true);

  m_hCounters = Make(GPU_FLUID_LIQUID_CTR_WORDS * 4, "GpuFluid Liquid Counters");
  m_hStats = Make(GPU_FLUID_LIQUID_STATS_BYTES, "GpuFluid Liquid Stats");

  // Surface: the mesh holds at most half a vertex and half a quad per cell, between 64K and 1M of each (the plan's
  // cap). A pool's surface is a small share of its cells.
  m_uiMaxSurfaceVertices = plMath::Clamp<plUInt32>(m_uiCellCount / 2, 64 * 1024, 1024 * 1024);
  m_uiMaxSurfaceQuads = m_uiMaxSurfaceVertices;
  m_hPhiRaw = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid Phi");
  m_hPhiTmp = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid Phi Scratch");
  for (plUInt32 i = 0; i < 2; ++i)
  {
    m_hSurfacePhi[i] = Make(plUInt64(m_uiCellCount) * 4, i == 0 ? "GpuFluid Liquid Surface Phi A" : "GpuFluid Liquid Surface Phi B");
    m_hSurfaceStamp[i] = Make(plUInt64(m_uiBrickCount) * 4, i == 0 ? "GpuFluid Liquid Surface Stamp A" : "GpuFluid Liquid Surface Stamp B");
  }
  // The mesh is only read up to the counts its draw arguments carry.
  m_hSurfaceVertices = Make(plUInt64(m_uiMaxSurfaceVertices) * GPU_FLUID_LIQUID_SURFACE_VERTEX_BYTES, "GpuFluid Liquid Surface Vertices", false, false);
  m_hSurfaceQuads = Make(plUInt64(m_uiMaxSurfaceQuads) * GPU_FLUID_LIQUID_SURFACE_QUAD_BYTES, "GpuFluid Liquid Surface Quads", false, false);
  m_hSurfaceCubeMap = Make(plUInt64(m_uiFaceCount) * 4, "GpuFluid Liquid Surface Cube Map"); // (n + 1)^3 cubes, as many as padded faces
  m_hSurfaceCounters = Make(GPU_FLUID_LIQUID_SURFACE_CTR_WORDS * 4, "GpuFluid Liquid Surface Counters");
  m_hSurfaceDrawArgs = Make(GPU_FLUID_LIQUID_DRAW_ARGS_BYTES, "GpuFluid Liquid Surface Draw Args", true);
  m_uiSurfaceTag = 2; // the stamps start at 0: tag - 1 of the first extraction never matches
  m_uiSurfaceCurrent = 0;
  m_bHasSurface = false;

  // Static solids (read only while a bake holds solids) and the sleep state per brick.
  m_hSolidSdf = Make(plUInt64(m_uiCellCount) * 4, "GpuFluid Liquid Solid SDF");
  m_hBrickSpeed = Make(plUInt64(m_uiBrickCount) * 4, "GpuFluid Liquid Brick Speed");
  m_hBrickCalm = Make(plUInt64(m_uiBrickCount) * 4, "GpuFluid Liquid Brick Calm");
  m_pBakedSolids = nullptr;
  m_bHasSolidSdf = false;
  m_uiSolidTriangles = 0;
  m_bAsleep = false;
  m_bWakePending = true;
  m_uiWakeFrame = 0;
  m_uiTicksAsleep = 0;
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plGpuFluidLiquidShape);
    desc.m_uiTotalSize = sizeof(plGpuFluidLiquidShape) * GPU_FLUID_LIQUID_MAX_REGIONS;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
    desc.m_ResourceAccess.m_bImmutable = false;
    plGpuFluidLiquidShape empty[GPU_FLUID_LIQUID_MAX_REGIONS];
    plMemoryUtils::ZeroFill(empty, GPU_FLUID_LIQUID_MAX_REGIONS);
    m_hRegions = plGALDevice::GetDefaultDevice()->CreateBuffer(desc, plMakeArrayPtr(reinterpret_cast<const plUInt8*>(empty), sizeof(empty)));
    bOk = bOk && !m_hRegions.IsInvalidated();
    m_UploadedRegions.Clear();
  }

  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plGpuFluidLiquidShape);
    desc.m_uiTotalSize = sizeof(plGpuFluidLiquidShape) * GPU_FLUID_LIQUID_MAX_SHAPES;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
    desc.m_ResourceAccess.m_bImmutable = false;

    // "= {}" does not zero the pl vector members of a shader struct (Release builds leave them uninitialised).
    plGpuFluidLiquidShape empty[GPU_FLUID_LIQUID_MAX_SHAPES];
    plMemoryUtils::ZeroFill(empty, GPU_FLUID_LIQUID_MAX_SHAPES);
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
  m_uiLastSubsteps = 0;
  m_uiSpeedCount = 0;
  m_uiSpeedNext = 0;
  m_uiSpeedFrame = 0;
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
  Free(m_hPcgR);
  Free(m_hPcgZ);
  Free(m_hPcgP);
  Free(m_hPcgQ);
  Free(m_hPcgPartials);
  Free(m_hPcgScalars);
  Free(m_hLevelRegion);
  Free(m_hCounters);
  Free(m_hStats);
  Free(m_hShapes);
  Free(m_hPhiRaw);
  Free(m_hPhiTmp);
  for (plUInt32 i = 0; i < 2; ++i)
  {
    Free(m_hSurfacePhi[i]);
    Free(m_hSurfaceStamp[i]);
  }
  Free(m_hSurfaceVertices);
  Free(m_hSurfaceQuads);
  Free(m_hSurfaceCubeMap);
  Free(m_hSurfaceCounters);
  Free(m_hSurfaceDrawArgs);
  m_bHasSurface = false;
  Free(m_hSolidSdf);
  Free(m_hSolidTriangles);
  Free(m_hSolidBins);
  Free(m_hSolidIndices);
  Free(m_hSolidBricks);
  Free(m_hRegions);
  Free(m_hBrickSpeed);
  Free(m_hBrickCalm);
  m_pBakedSolids = nullptr;
  m_bHasSolidSdf = false;

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
  out_publish.m_bHasSurface = m_bHasSurface;
  out_publish.m_hSurfaceVertices = m_hSurfaceVertices;
  out_publish.m_hSurfaceQuads = m_hSurfaceQuads;
  out_publish.m_hSurfaceDrawArgs = m_hSurfaceDrawArgs;
  out_publish.m_uiMaxSurfaceQuads = m_uiMaxSurfaceQuads;
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
  out_stats.m_uiSurfaceVertices = pWords[GPU_FLUID_LIQUID_STAT_SURFACE_VERTICES];
  out_stats.m_uiSurfaceQuads = pWords[GPU_FLUID_LIQUID_STAT_SURFACE_QUADS];
  out_stats.m_uiSurfaceOverflow = pWords[GPU_FLUID_LIQUID_STAT_SURFACE_OVERFLOW];
  out_stats.m_uiAwakeBricks = pWords[GPU_FLUID_LIQUID_STAT_AWAKE];
  out_stats.m_uiSleepingBricks = pWords[GPU_FLUID_LIQUID_STAT_SLEEPING];
  out_stats.m_uiInSolid = pWords[GPU_FLUID_LIQUID_STAT_IN_SOLID];
  {
    const plUInt32 uiPacked = pWords[GPU_FLUID_LIQUID_STAT_MAX_CELL];
    const plUInt32 uiCell = uiPacked & 0xFFFFFu;
    out_stats.m_uiMaxCellParticles = uiPacked >> 20;
    out_stats.m_uiMaxCellType = pWords[GPU_FLUID_LIQUID_STAT_MAX_CELL_TYPE];
    out_stats.m_fMaxCellPhi = AsFloat(GPU_FLUID_LIQUID_STAT_MAX_CELL_PHI);
    out_stats.m_fMaxCellSdf = AsFloat(GPU_FLUID_LIQUID_STAT_MAX_CELL_SDF);
    const plVec3U32 vCells = m_vCells;
    out_stats.m_vMaxCell = vCells.x * vCells.y > 0 ? plVec3U32(uiCell % vCells.x, (uiCell / vCells.x) % vCells.y, uiCell / (vCells.x * vCells.y)) : plVec3U32::MakeZero();
  }
  out_stats.m_uiSolidCells = pWords[GPU_FLUID_LIQUID_STAT_SOLID_CELLS];
  for (plUInt32 i = 0; i < GPU_FLUID_LIQUID_MAX_PROBES; ++i)
  {
    out_stats.m_fProbeLevels[i] = AsFloat(GPU_FLUID_LIQUID_STAT_PROBES + i);
    out_stats.m_fProbePhiLevels[i] = AsFloat(GPU_FLUID_LIQUID_STAT_PROBES_PHI + i);
  }

  const double fAlive = plMath::Max(1u, out_stats.m_uiAlive);
  out_stats.m_fMeanSpeed = static_cast<float>(pWords[GPU_FLUID_LIQUID_STAT_SPEED_SUM] / GPU_FLUID_LIQUID_SPEED_SUM_SCALE / fAlive);
  const plUInt64 uiSpeedSq = (plUInt64(pWords[GPU_FLUID_LIQUID_STAT_SPEED_SQ_SUM_HI]) << 32) | pWords[GPU_FLUID_LIQUID_STAT_SPEED_SQ_SUM];
  out_stats.m_fMeanSquaredSpeed = static_cast<float>(static_cast<double>(uiSpeedSq) / GPU_FLUID_LIQUID_SPEED_SQ_SCALE / fAlive);

  // The centroid is summed in 1/16 cell units per particle into a low word and a carry word per axis (64 bits).
  auto CentroidSum = [&](plUInt32 uiAxis)
  {
    const plUInt64 uiSum = (plUInt64(pWords[GPU_FLUID_LIQUID_STAT_CENTROID_HI + uiAxis]) << 32) | pWords[GPU_FLUID_LIQUID_STAT_CENTROID + uiAxis];
    return static_cast<float>(static_cast<double>(uiSum) / 16.0 / fAlive * m_fDx);
  };
  out_stats.m_vCentroid.Set(CentroidSum(0), CentroidSum(1), CentroidSum(2));
  out_stats.m_fSimulatedTime = pWords[GPU_FLUID_LIQUID_STAT_SIM_TIME] * 1e-6;

  const plUInt32 uiBins = plMath::Min<plUInt32>(pWords[GPU_FLUID_LIQUID_STAT_HIST_COUNT], GPU_FLUID_LIQUID_HIST_BINS);
  out_stats.m_FrontHistogram.SetCountUninitialized(uiBins);
  for (plUInt32 i = 0; i < uiBins; ++i)
    out_stats.m_FrontHistogram[i] = pWords[GPU_FLUID_LIQUID_STAT_HIST + i];
}

bool plGpuFluidLiquidSolver::Record(plRenderGraph& ref_graph, const plGpuFluidLiquidShaders& shaders, const plGpuFluidLiquidFrameInput& input, const plGpuFluidLiquidStats* pStats, plUInt64 uiStatsAge, plUInt64 uiFrame)
{
  if (!m_bCreated)
    return false;

  const bool bReset = input.m_uiResetCounter != m_uiLastResetCounter;
  const bool bStep = input.m_bSimulate && input.m_fDt > 0.0f && (!m_bHasTick || input.m_uiTick != m_uiLastTick || bReset);
  if (!bReset && !bStep)
    return false;

  m_uiLastTick = input.m_uiTick;
  m_bHasTick = true;
  m_vDomainOrigin = input.m_vDomainOrigin;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  // Volume sleep. What could wake the volume: an inflow running, the drains, the solids, the open faces, gravity. Asleep,
  // a tick records nothing (the last surface stays drawn, the stats stay those of the last simulated tick) until one of
  // them changes. The volume falls asleep when stats recorded after the last wake show no brick with particles awake.
  {
    bool bInflow = false;
    for (const plGpuFluidLiquidSource& source : input.m_Sources)
      bInflow = bInflow || source.m_fFlowRate > 0.0f || (source.m_fRadius > 0.0f && !source.m_vVelocity.IsZero());

    plUInt64 uiKey = plHashingUtils::xxHash64(input.m_Drains.GetData(), input.m_Drains.GetCount() * sizeof(plGpuFluidLiquidShape), input.m_Drains.GetCount());
    uiKey = plHashingUtils::xxHash64(input.m_Shapes.GetData(), input.m_Shapes.GetCount() * sizeof(plGpuFluidLiquidShape), uiKey);
    const void* pBake = input.m_pSolidBake.Borrow();
    uiKey = plHashingUtils::xxHash64(&pBake, sizeof(pBake), uiKey);
    uiKey = plHashingUtils::xxHash64(&input.m_uiOpenFaces, sizeof(input.m_uiOpenFaces), uiKey);
    uiKey = plHashingUtils::xxHash64(&input.m_vGravity, sizeof(input.m_vGravity), uiKey);

    if (bReset || bInflow || !input.m_bAllowSleep)
    {
      if (m_bAsleep || bReset)
      {
        m_bWakePending = true;
        m_uiWakeFrame = uiFrame;
      }
      m_bAsleep = false;
      if (bReset)
        m_uiTicksAsleep = 0;
    }
    else if (m_bAsleep)
    {
      if (uiKey != m_uiAsleepKey)
      {
        m_bAsleep = false;
        m_bWakePending = true;
        m_uiWakeFrame = uiFrame;
      }
      else
      {
        ++m_uiTicksAsleep;
        return false;
      }
    }
    else if (uiKey != m_uiAsleepKey)
    {
      // Awake, and something changed (a drain opened): every brick wakes, and stats from before cannot put it to sleep.
      m_bWakePending = true;
      m_uiWakeFrame = uiFrame;
    }
    else if (pStats != nullptr && pStats->m_uiFrame > m_uiWakeFrame + 1 && pStats->m_uiTicks > input.m_uiStartupTicks && pStats->m_uiAwakeBricks == 0)
    {
      m_bAsleep = true;
      ++m_uiTicksAsleep;
      return false;
    }
    m_uiAsleepKey = uiKey; // the key of the last simulated tick
  }

  // Analytic solids: rare edits, so the buffer is recreated with the new content instead of updated in place.
  {
    const plUInt32 uiShapes = plMath::Min<plUInt32>(input.m_Shapes.GetCount(), GPU_FLUID_LIQUID_MAX_SHAPES);
    bool bChanged = uiShapes != m_UploadedShapes.GetCount();
    for (plUInt32 i = 0; !bChanged && i < uiShapes; ++i)
      bChanged = plMemoryUtils::Compare(&input.m_Shapes[i], &m_UploadedShapes[i], 1) != 0;

    if (bChanged)
    {
      plGpuFluidLiquidShape shapes[GPU_FLUID_LIQUID_MAX_SHAPES];
      plMemoryUtils::ZeroFill(shapes, GPU_FLUID_LIQUID_MAX_SHAPES);
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

  // Regions: the open drains, then the wake boxes of the sleep (around each inflow's disc and each drain, two cells
  // more).
  plUInt32 uiDrainCount = 0;
  plUInt32 uiWakeBoxCount = 0;
  {
    plHybridArray<plGpuFluidLiquidShape, GPU_FLUID_LIQUID_MAX_REGIONS> regions;
    regions.SetCount(GPU_FLUID_LIQUID_MAX_REGIONS);
    plMemoryUtils::ZeroFill(regions.GetData(), regions.GetCount());
    plHybridArray<plGpuFluidLiquidShape, GPU_FLUID_LIQUID_MAX_WAKE_BOXES> wake;
    for (const plGpuFluidLiquidShape& drain : input.m_Drains)
    {
      if (uiDrainCount >= GPU_FLUID_LIQUID_MAX_DRAINS)
        break;
      regions[uiDrainCount++] = drain;
      plGpuFluidLiquidShape& box = wake.ExpandAndGetRef();
      box.CenterType = drain.CenterType;
      box.Size = drain.Size + plVec4(2.0f * m_fDx, 2.0f * m_fDx, 2.0f * m_fDx, 0.0f);
    }
    for (const plGpuFluidLiquidSource& source : input.m_Sources)
    {
      if (wake.GetCount() >= GPU_FLUID_LIQUID_MAX_WAKE_BOXES)
        break;
      plGpuFluidLiquidShape& box = wake.ExpandAndGetRef();
      const float fHalf = source.m_fRadius + 2.0f * m_fDx;
      box.CenterType = plVec4(source.m_vCenter.x, source.m_vCenter.y, source.m_vCenter.z, 0.0f);
      box.Size = plVec4(fHalf, fHalf, fHalf, 0.0f);
    }
    uiWakeBoxCount = wake.GetCount();
    for (plUInt32 i = 0; i < uiWakeBoxCount; ++i)
      regions[GPU_FLUID_LIQUID_REGION_WAKE_FIRST + i] = wake[i];

    const bool bChanged = m_UploadedRegions.GetCount() != regions.GetCount() || plMemoryUtils::Compare(regions.GetData(), m_UploadedRegions.GetData(), regions.GetCount()) != 0;
    if (bChanged)
    {
      plGALBufferCreationDescription desc;
      desc.m_uiStructSize = sizeof(plGpuFluidLiquidShape);
      desc.m_uiTotalSize = sizeof(plGpuFluidLiquidShape) * GPU_FLUID_LIQUID_MAX_REGIONS;
      desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
      desc.m_ResourceAccess.m_bImmutable = false;
      plGALBufferHandle hNew = pDevice->CreateBuffer(desc, plMakeArrayPtr(reinterpret_cast<const plUInt8*>(regions.GetData()), desc.m_uiTotalSize));
      if (!hNew.IsInvalidated())
      {
        if (!m_hRegions.IsInvalidated())
          pDevice->DestroyBuffer(m_hRegions);
        m_hRegions = hNew;
        m_UploadedRegions = regions;
      }
    }
  }

  // Static solids: a new bake is uploaded and run on the GPU before this tick's substeps.
  const bool bBakeSolids = input.m_pSolidBake != m_pBakedSolids;
  if (bBakeSolids)
  {
    for (plGALBufferHandle* pBuffer : {&m_hSolidTriangles, &m_hSolidBins, &m_hSolidIndices, &m_hSolidBricks})
    {
      if (!pBuffer->IsInvalidated())
        pDevice->DestroyBuffer(*pBuffer);
      pBuffer->Invalidate();
    }

    m_pBakedSolids = input.m_pSolidBake;
    const plGpuFluidLiquidSolidBake* pBake = m_pBakedSolids.Borrow();
    m_bHasSolidSdf = pBake != nullptr && pBake->GetTriangleCount() > 0 && pBake->m_vCells == m_vCells && !pBake->m_BakeBricks.IsEmpty();
    m_uiSolidTriangles = m_bHasSolidSdf ? pBake->GetTriangleCount() : 0;
    if (pBake != nullptr && pBake->GetTriangleCount() > 0 && !(pBake->m_vCells == m_vCells))
      plLog::Warning("GpuFluid: a static solids bake for {} x {} x {} cells does not fit the liquid domain of {} x {} x {}", pBake->m_vCells.x, pBake->m_vCells.y, pBake->m_vCells.z, m_vCells.x, m_vCells.y, m_vCells.z);

    if (m_bHasSolidSdf)
    {
      auto Upload = [&](const void* pData, plUInt32 uiBytes, const char* szName)
      {
        plGALBufferCreationDescription desc;
        desc.m_uiTotalSize = plMath::Max<plUInt32>(16, ((uiBytes + 15) / 16) * 16);
        desc.m_BufferFlags = plGALBufferUsageFlags::ByteAddressBuffer | plGALBufferUsageFlags::ShaderResource;
        desc.m_ResourceAccess.m_bImmutable = false;
        plDynamicArray<plUInt8> bytes;
        bytes.SetCount(desc.m_uiTotalSize);
        plMemoryUtils::Copy(bytes.GetData(), static_cast<const plUInt8*>(pData), uiBytes);
        plGALBufferHandle h = pDevice->CreateBuffer(desc, bytes.GetArrayPtr());
        if (!h.IsInvalidated())
          pDevice->GetBuffer(h)->SetDebugName(szName);
        return h;
      };
      m_hSolidTriangles = Upload(pBake->m_Triangles.GetData(), pBake->m_Triangles.GetCount() * 4, "GpuFluid Liquid Solid Triangles");
      m_hSolidBins = Upload(pBake->m_BrickBins.GetData(), pBake->m_BrickBins.GetCount() * 4, "GpuFluid Liquid Solid Bins");
      m_hSolidIndices = Upload(pBake->m_Indices.GetData(), pBake->m_Indices.GetCount() * 4, "GpuFluid Liquid Solid Indices");
      m_hSolidBricks = Upload(pBake->m_BakeBricks.GetData(), pBake->m_BakeBricks.GetCount() * 4, "GpuFluid Liquid Solid Bricks");
      m_bHasSolidSdf = !m_hSolidTriangles.IsInvalidated() && !m_hSolidBins.IsInvalidated() && !m_hSolidIndices.IsInvalidated() && !m_hSolidBricks.IsInvalidated();
    }

    if (m_bHasSolidSdf)
    {
      ++m_uiSolidBakes;
      const plGALBufferHandle hSdf = m_hSolidSdf, hTriangles = m_hSolidTriangles, hBins = m_hSolidBins, hIndices = m_hSolidIndices, hBricks = m_hSolidBricks, hBakeCounters = m_hCounters;
      const plUInt32 uiBakeBricks = pBake->m_BakeBricks.GetCount();
      const plUInt32 uiCells = m_uiCellCount;
      const plVec3U32 vCells = m_vCells;
      const plGpuFluidLiquidShaders* pBakeShaders = &shaders;

      plGpuFluidLiquidConstants c = {};
      c.GpuFluidLiqCells = plVec4U32(m_vCells.x, m_vCells.y, m_vCells.z, 0);
      c.GpuFluidLiqBricks = plVec4U32(m_vBricks.x, m_vBricks.y, m_vBricks.z, m_uiCapacity);
      c.GpuFluidLiqGrid = plVec4(m_fDx, 1.0f / m_fDx, 0.0f, 0.0f);
      c.GpuFluidLiqGravity = plVec4(0.0f, 0.0f, 0.0f, 1.0f);
      c.GpuFluidLiqSolve = plVec4(0.0f, 0.0f, 0.0f, 0.0f);
      c.GpuFluidLiqWall = plVec4(0.0f, 0.0f, 0.0f, 0.0f);
      c.GpuFluidLiqPass = plVec4U32(uiBakeBricks, 0, 0, 0);
      c.GpuFluidLiqPassF = plVec4(GPU_FLUID_LIQUID_SDF_BAND_CELLS * m_fDx, 0.5f * GPU_FLUID_LIQUID_SDF_MIN_THICKNESS_CELLS * m_fDx, 0.0f, 0.0f);

      auto pass = ref_graph.AddComputePass("GpuFluid Liquid/Solids Bake");
      for (plGALBufferHandle h : {hTriangles, hBins, hIndices, hBricks})
        pass.ReadBuffer(ref_graph.ImportBuffer(h), plGALResourceState::ComputeShaderRead);
      pass.WriteBuffer(ref_graph.ImportBuffer(hSdf));
      pass.WriteBuffer(ref_graph.ImportBuffer(hBakeCounters));
      pass.HasSideEffects();
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          Recorder rec(ctx);
          rec.Run(pBakeShaders->m_hSolidsClear, c, LinearGroups(uiCells, 256), {W("GpuFluidLiqSolidSdfOut", hSdf), W("GpuFluidLiqCounters", hBakeCounters)});
          rec.Run(pBakeShaders->m_hSolidsBake, c, plVec3U32(uiBakeBricks, 1, 1),
            {R("GpuFluidLiqSolidTriangles", hTriangles), R("GpuFluidLiqSolidBins", hBins), R("GpuFluidLiqSolidIndices", hIndices), R("GpuFluidLiqSolidBricks", hBricks),
              W("GpuFluidLiqSolidSdfOut", hSdf)});
          rec.Run(pBakeShaders->m_hSolidsSign, c, plVec3U32(DivUp(vCells.x, 8), DivUp(vCells.y, 8), 1), {W("GpuFluidLiqSolidSdfOut", hSdf), W("GpuFluidLiqCounters", hBakeCounters)});
        });
    }
  }

  // Substeps. The newest max speed is two to three frames old and one readback can catch a calm moment between two
  // fast ones, so the speed used is the largest of the last few readbacks, plus what gravity can have added since
  // the newest (g * dt per frame of its age); and the count drops by at most one per tick.
  if (bReset)
  {
    m_uiSpeedCount = 0;
    m_uiSpeedNext = 0;
    m_uiSpeedFrame = 0;
    m_uiLastSubsteps = 0;
    m_uiTicksBelow = 0;
  }
  if (!bReset && pStats != nullptr && pStats->m_uiFrame != m_uiSpeedFrame)
  {
    m_uiSpeedFrame = pStats->m_uiFrame;
    m_SpeedHistory[m_uiSpeedNext] = pStats->m_fMaxSpeed;
    m_uiSpeedNext = (m_uiSpeedNext + 1) % s_uiMaxSpeedHistory;
    m_uiSpeedCount = plMath::Min(m_uiSpeedCount + 1, s_uiMaxSpeedHistory);
  }

  const float fGravity = input.m_vGravity.GetLength();
  float fSpeed = fGravity * input.m_fDt;
  const plUInt32 uiHistory = plMath::Min(m_uiSpeedCount, plMath::Clamp<plUInt32>(input.m_uiSpeedHistory, 1, s_uiMaxSpeedHistory));
  if (!bReset && uiHistory > 0)
  {
    float fRecent = 0.0f;
    for (plUInt32 k = 0; k < uiHistory; ++k)
      fRecent = plMath::Max(fRecent, m_SpeedHistory[(m_uiSpeedNext + s_uiMaxSpeedHistory - 1 - k) % s_uiMaxSpeedHistory]);
    fSpeed += fRecent + fGravity * input.m_fDt * static_cast<float>(plMath::Min<plUInt64>(uiStatsAge, 8));
  }

  const float fCflDistance = plMath::Max(input.m_fCflNumber, 0.1f) * m_fDx;
  const plUInt32 uiMaxSubsteps = plMath::Clamp<plUInt32>(input.m_uiMaxSubsteps, 1, 16);
  const plUInt32 uiMinSubsteps = plMath::Clamp<plUInt32>(input.m_uiMinSubsteps, 1, uiMaxSubsteps);
  if (bReset)
    m_uiTicksSinceReset = 0;

  plUInt32 uiSubsteps = 0;
  float fSimDt = 0.0f;
  if (bStep)
  {
    const float fNeeded = fSpeed * input.m_fDt / fCflDistance;
    uiSubsteps = plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Ceil(fNeeded)), uiMinSubsteps, uiMaxSubsteps);

    // Up at once; down by one only after one fewer would have done with a margin (m_fSubstepDownMargin of its
    // capacity) for m_uiSubstepHoldTicks ticks in a row. The pool's packing settles a little differently at each
    // substep count, so a count that flips back and forth makes the whole pool breathe.
    if (m_uiLastSubsteps > 0 && uiSubsteps < m_uiLastSubsteps)
    {
      const float fLower = static_cast<float>(m_uiLastSubsteps - 1) * plMath::Clamp(input.m_fSubstepDownMargin, 0.1f, 1.0f);
      const bool bLower = m_uiLastSubsteps > uiMinSubsteps && fNeeded <= fLower;
      m_uiTicksBelow = bLower ? m_uiTicksBelow + 1 : 0;
      if (bLower && m_uiTicksBelow >= input.m_uiSubstepHoldTicks)
      {
        uiSubsteps = plMath::Max(uiSubsteps, m_uiLastSubsteps - 1);
        m_uiTicksBelow = 0;
      }
      else
      {
        uiSubsteps = plMath::Min(m_uiLastSubsteps, uiMaxSubsteps);
      }
    }
    else
    {
      m_uiTicksBelow = 0;
    }
    if (m_uiTicksSinceReset++ < input.m_uiStartupTicks)
      uiSubsteps = uiMaxSubsteps;

    // Over the cap the tick simulates less time (slow motion), down to a quarter tick; beyond that the CFL number
    // gives way.
    fSimDt = input.m_fDt;
    if (fNeeded > static_cast<float>(uiSubsteps))
      fSimDt = input.m_fDt * plMath::Max(static_cast<float>(uiSubsteps) / fNeeded, 0.25f);

    m_uiLastSubsteps = uiSubsteps;
  }
  const float fSubDt = bStep ? fSimDt / static_cast<float>(uiSubsteps) : 0.0f;

  // Base constants of this tick.
  plGpuFluidLiquidConstants base = {};
  const plUInt32 uiShapeCount = m_UploadedShapes.GetCount();
  base.GpuFluidLiqCells = plVec4U32(m_vCells.x, m_vCells.y, m_vCells.z,
    (input.m_uiOpenFaces & 0x3Fu) | (uiShapeCount << 8) | (input.m_bLevelSet ? GPU_FLUID_LIQUID_FLAG_LEVEL_SET : 0u) | (m_bHasSolidSdf ? GPU_FLUID_LIQUID_FLAG_SOLID_SDF : 0u) |
      (uiDrainCount << GPU_FLUID_LIQUID_DRAIN_SHIFT));
  base.GpuFluidLiqBricks = plVec4U32(m_vBricks.x, m_vBricks.y, m_vBricks.z, m_uiCapacity);
  base.GpuFluidLiqGrid = plVec4(m_fDx, 1.0f / m_fDx, fSubDt, plMath::Clamp(input.m_fFlipBlend, 0.0f, 1.0f));
  base.GpuFluidLiqGravity = plVec4(input.m_vGravity.x, input.m_vGravity.y, input.m_vGravity.z, static_cast<float>(plMath::Max(1u, m_Settings.m_uiParticlesPerCell)));
  base.GpuFluidLiqSolve = plVec4(1.0f / plMath::Clamp(input.m_fTheta, 0.05f, 1.0f), input.m_fDensityGain, input.m_fVolumeGain, 1.0f);
  base.GpuFluidLiqWall = plVec4(plMath::Clamp(input.m_fWallMargin, 0.0f, 0.45f), plMath::Clamp(input.m_fWallFriction, 0.0f, 1.0f), input.m_fFrontHeight, input.m_fFrontBinSize);
  // "= {}" does not zero the pl vector members (their default constructors leave Release builds uninitialised), and
  // kernels read the pass words they are not given: LiquidClear skips the per-substep counters when x is 1, so a
  // stale 1 on the stack let the volume measure add up over several substeps and the volume control crush the pool.
  base.GpuFluidLiqPass = plVec4U32(0, 0, 0, 0);
  base.GpuFluidLiqPassF = plVec4(0.0f, 0.0f, 0.0f, 0.0f);

  const plGpuFluidLiquidShaders* pShaders = &shaders;

  // Every buffer is imported; passes declare what they touch, so the graph orders and transitions them.
  auto G = [&](plGALBufferHandle h) { return ref_graph.ImportBuffer(h); };
  const plBitflags<plGALResourceState> indirect = plGALResourceState::IndirectArgument | plGALResourceState::ComputeShaderRead;

  // Handles by value for the callbacks.
  const plGALBufferHandle hStats = m_hStats, hCounters = m_hCounters, hShapes = m_hShapes;
  const plGALBufferHandle hCellCount = m_hCellCount, hCellStart = m_hCellStart, hCellType = m_hCellType, hPressure = m_hPressure, hRhs = m_hRhs, hScratch = m_hScanScratch;
  const plGALBufferHandle hVelG = m_hVelG, hVelS = m_hVelS, hVelT = m_hVelT, hValid0 = m_hValid[0], hValid1 = m_hValid[1];
  const plGALBufferHandle hBrickOcc = m_hBrickOccupied, hBrickActive = m_hBrickActive, hBrickList = m_hBrickList, hBrickArgs = m_hBrickArgs;
  const plGALBufferHandle hLevelRegion = m_hLevelRegion;
  const plUInt32 uiLevelCount = m_uiLevelCount;
  const plGALBufferHandle hRank = m_hRank, hPoints = m_hPoints, hDrawArgs = m_hDrawArgs;
  const plGALBufferHandle hSolidSdf = m_hSolidSdf, hRegions = m_hRegions, hBrickSpeed = m_hBrickSpeed, hBrickCalm = m_hBrickCalm;
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

      // A partly filled top layer is a job of its own: fewer particles per cell, anywhere in its lower part.
      const float fTop = plMath::Clamp(fill.m_fTopLayerFill, 0.0f, 1.0f);
      const bool bPartialTop = fTop < 1.0f && vCount.z > 0;
      for (plUInt32 uiPart = 0; uiPart < (bPartialTop ? 2u : 1u); ++uiPart)
      {
        const bool bTop = uiPart == 1;
        const plVec3U32 vJobMin(vMin.x, vMin.y, bTop ? vMin.z + vCount.z - 1 : vMin.z);
        const plVec3U32 vJobCount(vCount.x, vCount.y, bTop ? 1u : (bPartialTop ? vCount.z - 1 : vCount.z));
        const plUInt32 uiJobPpc = bTop ? static_cast<plUInt32>(plMath::Round(fTop * static_cast<float>(uiPpc))) : uiPpc;

        const plUInt64 uiWanted = plUInt64(vJobCount.x) * vJobCount.y * vJobCount.z * uiJobPpc;
        const plUInt32 uiCount = static_cast<plUInt32>(plMath::Min<plUInt64>(uiWanted, m_uiCapacity - uiTotal));
        if (uiCount < uiWanted)
          plLog::Warning("GpuFluid: a fill box of a liquid volume wants {} particles, {} fit", uiWanted, uiCount);
        if (uiCount == 0)
          continue;

        SeedJob& job = jobs.ExpandAndGetRef();
        job.m_uiCount = uiCount;
        job.m_Constants.GpuFluidSeedCellMin = plVec4U32(vJobMin.x, vJobMin.y, vJobMin.z, uiJobPpc);
        job.m_Constants.GpuFluidSeedCellCount = plVec4U32(vJobCount.x, vJobCount.y, vJobCount.z, uiTotal);
        job.m_Constants.GpuFluidSeedVelocity = plVec4(fill.m_vVelocity.x, fill.m_vVelocity.y, fill.m_vVelocity.z, plMath::Clamp(fill.m_fJitter, 0.0f, 1.0f));
        // Seed mode (LiquidSeed): 1 stratified, 2 exactly n per cell anywhere in it (a stratified fill's partial top
        // layer), 0 anywhere in the box.
        const float fMode = fill.m_bStratified ? (bTop ? 2.0f : 1.0f) : 0.0f;
        job.m_Constants.GpuFluidSeedGrid = plVec4(m_fDx, bTop ? fTop : 1.0f, fMode, 0.0f);
        job.m_Constants.GpuFluidSeedInfo = plVec4U32(0x9E3779B9u * (input.m_uiResetCounter + 1) + (2 * i + uiPart) * 0x85EBCA6Bu, uiCount, 0, 0);
        uiTotal += uiCount;
      }
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

  // Inflow: each source's share of this tick (flow rate * simulated time / particle volume, the fractions carried to
  // the next tick) appended to the current set before the substeps.
  if (bReset)
  {
    for (float& fCarry : m_SourceCarry)
      fCarry = 0.0f;
  }
  if (bStep && !input.m_Sources.IsEmpty())
  {
    struct SourceJob
    {
      plGpuFluidLiquidConstants m_Constants;
      plUInt32 m_uiCount;
    };
    plHybridArray<SourceJob, s_uiMaxSources> jobs;
    const float fParticleVolume = m_fDx * m_fDx * m_fDx / static_cast<float>(plMath::Max(1u, m_Settings.m_uiParticlesPerCell));
    for (plUInt32 i = 0; i < plMath::Min<plUInt32>(input.m_Sources.GetCount(), s_uiMaxSources); ++i)
    {
      const plGpuFluidLiquidSource& source = input.m_Sources[i];
      const float fSpeed = source.m_vVelocity.GetLength();
      const float fRate = source.m_fFlowRate > 0.0f ? source.m_fFlowRate : plMath::Pi<float>() * source.m_fRadius * source.m_fRadius * fSpeed;
      const float fWanted = fRate * fSimDt / fParticleVolume + m_SourceCarry[i];
      const plUInt32 uiCount = static_cast<plUInt32>(plMath::Min(plMath::Floor(fWanted), 1.0e6f));
      m_SourceCarry[i] = fWanted - static_cast<float>(uiCount);
      if (uiCount == 0)
        continue;

      SourceJob& job = jobs.ExpandAndGetRef();
      job.m_uiCount = uiCount;
      job.m_Constants = base;
      job.m_Constants.GpuFluidLiqPassF = plVec4(source.m_vCenter.x, source.m_vCenter.y, source.m_vCenter.z, plMath::Max(source.m_fRadius, 0.0f));
      plUInt32 uiStream = 0;
      plMemoryUtils::Copy(&uiStream, reinterpret_cast<const plUInt32*>(&fSimDt), 1);
      job.m_Constants.GpuFluidLiqPass = plVec4U32(uiCount, 0x9E3779B9u * static_cast<plUInt32>(m_uiTicksSinceReset + 1) + i * 0x85EBCA6Bu, uiStream, 0);
      job.m_Constants.GpuFluidLiqWall = plVec4(source.m_vVelocity.x, source.m_vVelocity.y, source.m_vVelocity.z, 0.0f);
    }

    if (!jobs.IsEmpty())
    {
      const plUInt32 cur = m_uiCurrentSet;
      const plGALBufferHandle hPos = m_hPositions[cur], hVel = m_hVelocities[cur], hAff = m_hAffine[cur], hArgs = m_hParticleArgs[cur], hCtr = m_hCounters;
      auto pass = ref_graph.AddComputePass("GpuFluid Liquid/Source");
      for (plGALBufferHandle h : {hPos, hVel, hAff, hArgs, hCtr})
        pass.WriteBuffer(G(h));
      pass.HasSideEffects();
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          Recorder rec(ctx);
          for (const SourceJob& job : jobs)
          {
            rec.Run(pShaders->m_hSource, job.m_Constants, LinearGroups(job.m_uiCount, GPU_FLUID_LIQUID_PARTICLE_GROUP),
              {W("GpuFluidLiqParticleArgs", hArgs), W("GpuFluidLiqPositions", hPos), W("GpuFluidLiqVelocities", hVel), W("GpuFluidLiqAffine", hAff)});
            rec.Run(pShaders->m_hSourceFinish, job.m_Constants, plVec3U32(1, 1, 1), {W("GpuFluidLiqParticleArgs", hArgs), W("GpuFluidLiqCounters", hCtr)});
          }
        });
    }
  }

  // Counting sort of particle set src into dst by cell, and the active bricks of the result. Each substep starts with
  // one; Liquid/Surface runs one more after the last substep (c.GpuFluidLiqPass.x = 1 keeps the solver's counters).
  const plGpuFluidScan::Layout scanLayout = m_ScanLayout;
  auto AddSortPass = [&](plUInt32 src, plUInt32 dst, const plGpuFluidLiquidConstants& c, const char* szName)
  {
    const plGALBufferHandle hSrcPos = m_hPositions[src], hSrcVel = m_hVelocities[src], hSrcAff = m_hAffine[src], hSrcArgs = m_hParticleArgs[src];
    const plGALBufferHandle hPos = m_hPositions[dst], hVel = m_hVelocities[dst], hAff = m_hAffine[dst], hArgs = m_hParticleArgs[dst];

    auto pass = ref_graph.AddComputePass(szName);
    pass.ReadBuffer(G(hSrcArgs), indirect);
    pass.ReadBuffer(G(hSrcPos), plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(G(hSrcVel), plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(G(hSrcAff), plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(G(hRegions), plGALResourceState::ComputeShaderRead);
    for (plGALBufferHandle h : {hPos, hVel, hAff, hArgs, hRank, hCellCount, hCellStart, hScratch, hCounters, hBrickOcc, hBrickActive, hBrickList, hBrickArgs, hBrickSpeed, hBrickCalm, hLevelRegion})
      pass.WriteBuffer(G(h));
    pass.HasSideEffects();
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        Recorder rec(ctx);
        rec.Run(pShaders->m_hClear, c, LinearGroups(plMath::Max(uiCellCount, uiBrickCount), 256),
          {W("GpuFluidLiqCellCount", hCellCount), W("GpuFluidLiqBrickOccupied", hBrickOcc), W("GpuFluidLiqBrickSpeed", hBrickSpeed), W("GpuFluidLiqBrickCalm", hBrickCalm),
            W("GpuFluidLiqCounters", hCounters)});

        rec.RunIndirect(pShaders->m_hCount, c, hSrcArgs,
          {R("GpuFluidLiqParticleArgs", hSrcArgs), R("GpuFluidLiqPositions", hSrcPos), R("GpuFluidLiqRegions", hRegions), W("GpuFluidLiqCellCount", hCellCount), W("GpuFluidLiqRank", hRank),
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

        // The coarse multigrid levels' regions around the active bricks (the dense levels then skip the air).
        if (uiLevelCount > 1)
        {
          plGpuFluidLiquidConstants regionConstants = c;
          regionConstants.GpuFluidLiqPass = plVec4U32(uiLevelCount, 0, 0, 0);
          rec.Run(pShaders->m_hLevelRegion, regionConstants, plVec3U32(1, 1, 1),
            {R("GpuFluidLiqBrickArgs", hBrickArgs), R("GpuFluidLiqBrickList", hBrickList), W("GpuFluidLiqLevelRegion", hLevelRegion)});
        }
      });
  };

  // The level set kernel's radii (cells): kernel radius, particle radius, far value.
  const float fLevelSetRadius = plMath::Clamp(input.m_fLevelSetRadius, 0.5f, 1.5f);
  const plVec4 vLevelSetParams(fLevelSetRadius, plMath::Clamp(input.m_fParticleRadius, 0.0f, fLevelSetRadius), fLevelSetRadius, 0.0f);
  const bool bLevelSet = input.m_bLevelSet;
  const bool bLevelSetTheta = input.m_bLevelSetTheta;
  const bool bLevelSetOnly = input.m_bLevelSetOnly;
  const float fThetaMin = plMath::Clamp(input.m_fThetaMin, 1.0f / 30.0f, 1.0f);
  // Liquid cells: phi < tau, or holding particles and phi < sigma (never with the level set alone).
  const float fLiquidPhi = plMath::Clamp(input.m_fLiquidPhi, -1.0f, 1.5f);
  const float fSprayPhi = bLevelSetOnly ? -1.0e3f : plMath::Max(input.m_fSprayPhi, 0.0f);
  const float fDensityDeadZone = plMath::Max(input.m_fDensityDeadZone, 0.0f);
  const plVec4 vDensityParams(fDensityDeadZone, input.m_bDensityTwoSided ? 1.0f : 0.0f, plMath::Clamp(input.m_fDensityMaxError, 0.0f, 1.0f), 0.0f);
  const plUInt32 uiClassifyFlags = bLevelSetTheta ? 1u : 0u;
  const plUInt32 uiLiquidMinCount = plMath::Clamp<plUInt32>(input.m_uiLiquidMinCount, 1, 64);
  const bool bProfileKernels = input.m_bProfileKernels;
  const plGALBufferHandle hPhiRaw = m_hPhiRaw;
  const plGALBufferHandle hPhiTmp = m_hPhiTmp;
  const plUInt32 uiLevelSetSmoothPairs = (plMath::Min<plUInt32>(input.m_uiLevelSetSmoothSteps, 8) + 1) / 2;

  for (plUInt32 uiSub = 0; uiSub < uiSubsteps; ++uiSub)
  {
    const plUInt32 src = m_uiCurrentSet;
    const plUInt32 dst = 1 - src;

    plGpuFluidLiquidConstants c = base;
    // Warm start: last substep's phi = p * dt / (rho * dx), rescaled by the dt ratio. That keeps the pressure of the
    // hydrostatic part, which dominates a pool; the density and volume corrections do not scale with dt and start off
    // by the ratio, which the solve then takes out (N2 checks the residual it leaves).
    c.GpuFluidLiqSolve.w = m_fLastSubstepDt > 0.0f ? fSubDt / m_fLastSubstepDt : 0.0f;
    m_fLastSubstepDt = fSubDt;

    const plGALBufferHandle hPos = m_hPositions[dst], hVel = m_hVelocities[dst], hAff = m_hAffine[dst], hArgs = m_hParticleArgs[dst];

    // Liquid/Sort. After a wake the first substep's clear starts every brick's calm count over.
    {
      plGpuFluidLiquidConstants sortConstants = c;
      sortConstants.GpuFluidLiqPass.y = (uiSub == 0 && m_bWakePending) ? 1u : 0u;
      AddSortPass(src, dst, sortConstants, "GpuFluid Liquid/Sort");
    }
    if (uiSub == 0)
      m_bWakePending = false;

    // Liquid/P2G. With m_bProfileKernels its parts are passes of their own (stage 1: gather and extrapolation, 2: level
    // set), so each is timed.
    auto AddP2GPass = [&](const char* szName, plUInt32 uiStages)
    {
      auto pass = ref_graph.AddComputePass(szName);
      pass.ReadBuffer(G(hBrickArgs), indirect);
      for (plGALBufferHandle h : {hBrickList, hBrickActive, hCellCount, hCellStart, hPos, hVel, hAff, hShapes, hSolidSdf})
        pass.ReadBuffer(G(h), plGALResourceState::ComputeShaderRead);
      for (plGALBufferHandle h : {hVelG, hVelT, hValid0, hValid1, hCounters, hPhiRaw, hPhiTmp})
        pass.WriteBuffer(G(h));
      pass.HasSideEffects();
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          Recorder rec(ctx);
          if ((uiStages & 1u) != 0)
          {
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
          }

          // The level set the classification reads.
          if (bLevelSet && (uiStages & 2u) != 0)
          {
            plGpuFluidLiquidConstants lc = c;
            lc.GpuFluidLiqPassF = vLevelSetParams;
            rec.RunIndirect(pShaders->m_hLevelSet, lc, hBrickArgs,
              {R("GpuFluidLiqBrickArgs", hBrickArgs), R("GpuFluidLiqBrickList", hBrickList), R("GpuFluidLiqCellCount", hCellCount), R("GpuFluidLiqCellStart", hCellStart),
                R("GpuFluidLiqPositions", hPos), W("GpuFluidLiqPhiOut", hPhiRaw)});

            lc.GpuFluidLiqPassF = plVec4(vLevelSetParams.y, vLevelSetParams.z, 0.5f, 0.4f);
            for (plUInt32 i = 0; i < uiLevelSetSmoothPairs; ++i)
            {
              rec.RunIndirect(pShaders->m_hSurfaceSmooth, lc, hBrickArgs,
                {R("GpuFluidLiqBrickArgs", hBrickArgs), R("GpuFluidLiqBrickList", hBrickList), R("GpuFluidLiqBrickActive", hBrickActive), R("GpuFluidLiqShapes", hShapes),
                  R("GpuFluidLiqSolidSdf", hSolidSdf), W("GpuFluidLiqSurfPhi", hPhiRaw), W("GpuFluidLiqPhiOut", hPhiTmp)});
              rec.RunIndirect(pShaders->m_hSurfaceSmooth, lc, hBrickArgs,
                {R("GpuFluidLiqBrickArgs", hBrickArgs), R("GpuFluidLiqBrickList", hBrickList), R("GpuFluidLiqBrickActive", hBrickActive), R("GpuFluidLiqShapes", hShapes),
                  R("GpuFluidLiqSolidSdf", hSolidSdf), W("GpuFluidLiqSurfPhi", hPhiTmp), W("GpuFluidLiqPhiOut", hPhiRaw)});
            }
          }
        });
    };
    if (bProfileKernels)
    {
      AddP2GPass("GpuFluid Liquid/P2G", 1u);
      AddP2GPass("GpuFluid Liquid/P2G LevelSet", 2u);
    }
    else
    {
      AddP2GPass("GpuFluid Liquid/P2G", 3u);
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
      const bool bPcg = input.m_bPcg && uiLevels > 1;
      const bool bBlockSmooth = input.m_bBlockSmooth;
      const plUInt32 uiFineSweeps = plMath::Clamp<plUInt32>(input.m_uiFineSweeps, 1, 16);
      const plUInt32 uiFinePasses = plMath::Clamp<plUInt32>(input.m_uiFinePasses, 1, 8);
      const plGALBufferHandle hPcgR = m_hPcgR, hPcgZ = m_hPcgZ, hPcgP = m_hPcgP, hPcgQ = m_hPcgQ, hPcgPartials = m_hPcgPartials, hPcgScalars = m_hPcgScalars;

      // With m_bProfileKernels the stages are passes of their own: 1 classify, forces, right-hand side; 2 the pressure
      // solve; 4 residual, projection, extrapolation.
      auto AddSolvePass = [&](const char* szName, plUInt32 uiStages)
      {
      auto pass = ref_graph.AddComputePass(szName);
      pass.ReadBuffer(G(hBrickArgs), indirect);
      pass.ReadBuffer(G(hLevelRegion), indirect);
      for (plGALBufferHandle h : {hBrickList, hBrickActive, hCellCount, hCellStart, hPos, hShapes, hPhiRaw, hSolidSdf, hRegions})
        pass.ReadBuffer(G(h), plGALResourceState::ComputeShaderRead);
      for (plGALBufferHandle h : {hCellType, hVelG, hVelS, hVelT, hValid0, hValid1, hPressure, hRhs, hCounters, hPcgR, hPcgZ, hPcgP, hPcgQ, hPcgPartials, hPcgScalars})
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
          const Bind region = R("GpuFluidLiqLevelRegion", hLevelRegion);

          if ((uiStages & 1u) != 0)
          {
            plGpuFluidLiquidConstants classifyConstants = c;
            classifyConstants.GpuFluidLiqPass = plVec4U32(uiClassifyFlags, uiLiquidMinCount, 0, 0);
            classifyConstants.GpuFluidLiqPassF = plVec4(fThetaMin, fLiquidPhi, fSprayPhi, 0.0f);
            rec.RunIndirect(pShaders->m_hClassify, classifyConstants, hBrickArgs,
              {brickArgs, brickList, brickActive, R("GpuFluidLiqCellCount", hCellCount), R("GpuFluidLiqPhi", hPhiRaw), R("GpuFluidLiqShapes", hShapes), R("GpuFluidLiqSolidSdf", hSolidSdf),
                R("GpuFluidLiqRegions", hRegions), W("GpuFluidLiqCellType", hCellType), W("GpuFluidLiqCounters", hCounters)});

            rec.RunIndirect(pShaders->m_hForces, c, hBrickArgs,
              {brickArgs, brickList, brickActive, W("GpuFluidLiqCellType", hCellType), W("GpuFluidLiqVel", hVelG), W("GpuFluidLiqVelStar", hVelS)});

            plGpuFluidLiquidConstants rhsConstants = c;
            rhsConstants.GpuFluidLiqPassF = vDensityParams;
            rec.RunIndirect(pShaders->m_hRhs, rhsConstants, hBrickArgs,
              {brickArgs, brickList, brickActive, R("GpuFluidLiqCellCount", hCellCount), R("GpuFluidLiqCellStart", hCellStart), R("GpuFluidLiqPositions", hPos),
                W("GpuFluidLiqCellType", hCellType), W("GpuFluidLiqVel", hVelG), W("GpuFluidLiqPressure", hPressure), W("GpuFluidLiqRhs", hRhs), W("GpuFluidLiqCounters", hCounters)});
          }

          auto LevelConstants = [&](plUInt32 uiLevel, plUInt32 uiColour, float fOmega)
          {
            plGpuFluidLiquidConstants lc = c;
            lc.GpuFluidLiqPass = plVec4U32(uiLevel, uiColour, uiCoarsestSweeps, 0);
            lc.GpuFluidLiqPassF = plVec4(fOmega, fCoarseScale, 0.0f, 0.0f);
            return lc;
          };

          // Red-black sweeps; bReverse runs black before red (the post-smoothing, so a V-cycle is symmetric). On the
          // fine level x and b are the given buffers: the pressure and rhs, or z and r of the MGPCG preconditioner.
          auto Smooth = [&](plUInt32 uiLevel, plUInt32 uiSweeps, bool bReverse, plGALBufferHandle hFineX, plGALBufferHandle hFineB)
          {
            if (uiLevel == 0 && bBlockSmooth && uiSweeps > 0)
            {
              const ProfileScope scope(bProfileKernels, rec.m_pEncoder, "GpuFluid LiqProf/Smooth fine");
              plGpuFluidLiquidConstants lc = LevelConstants(0, 0, 1.0f);
              lc.GpuFluidLiqPass = plVec4U32(0, bReverse ? 1u : 0u, uiFineSweeps, 0);
              for (plUInt32 uiPass = 0; uiPass < uiFinePasses; ++uiPass)
              {
                rec.RunIndirect(pShaders->m_hSmoothBricksBlock, lc, hBrickArgs,
                  {brickArgs, brickList, brickActive, W("GpuFluidLiqMgX", hFineX), W("GpuFluidLiqMgB", hFineB), W("GpuFluidLiqMgType", levelType[0])});
              }
              return;
            }
            const ProfileScope scope(bProfileKernels && uiSweeps > 0, rec.m_pEncoder, uiLevel == 0 ? "GpuFluid LiqProf/Smooth fine" : (uiLevel == 1 ? "GpuFluid LiqProf/Smooth L1" : "GpuFluid LiqProf/Smooth L2+"));
            for (plUInt32 s = 0; s < uiSweeps; ++s)
            {
              for (plUInt32 uiStep = 0; uiStep < 2; ++uiStep)
              {
                const plUInt32 uiColour = bReverse ? 1 - uiStep : uiStep;
                const plGpuFluidLiquidConstants lc = LevelConstants(uiLevel, uiColour, 1.0f);
                if (uiLevel == 0)
                {
                  rec.RunIndirect(pShaders->m_hSmoothBricks, lc, hBrickArgs,
                    {brickArgs, brickList, brickActive, W("GpuFluidLiqMgX", hFineX), W("GpuFluidLiqMgB", hFineB), W("GpuFluidLiqMgType", levelType[0])});
                }
                else
                {
                  rec.RunIndirect(pShaders->m_hSmoothDense, lc, hLevelRegion,
                    {brickActive, region, W("GpuFluidLiqMgX", levelX[uiLevel]), W("GpuFluidLiqMgB", levelB[uiLevel]), W("GpuFluidLiqMgType", levelType[uiLevel])},
                    uiLevel * GPU_FLUID_LIQUID_REGION_STRIDE);
                }
              }
            }
          };

          // One V-cycle on A x = b of the fine level, the recursion unrolled: down with pre-smoothing and
          // restriction, the coarsest solved in one group, up with prolongation and post-smoothing.
          auto VCycle = [&](plGALBufferHandle hFineX, plGALBufferHandle hFineB)
          {
            if (uiLevels == 1)
            {
              Smooth(0, uiCoarsestSweeps, false, hFineX, hFineB);
              return;
            }

            for (plUInt32 l = 0; l + 1 < uiLevels; ++l)
            {
              Smooth(l, uiPre, false, hFineX, hFineB);
              const ProfileScope scope(bProfileKernels, rec.m_pEncoder, "GpuFluid LiqProf/Restrict");
              rec.RunIndirect(pShaders->m_hRestrict, LevelConstants(l + 1, 0, 1.0f), hLevelRegion,
                {brickActive, region, W("GpuFluidLiqMgFineX", l == 0 ? hFineX : levelX[l]), W("GpuFluidLiqMgFineB", l == 0 ? hFineB : levelB[l]), W("GpuFluidLiqMgFineType", levelType[l]),
                  W("GpuFluidLiqMgX", levelX[l + 1]), W("GpuFluidLiqMgB", levelB[l + 1]), W("GpuFluidLiqMgType", levelType[l + 1])},
                (l + 1) * GPU_FLUID_LIQUID_REGION_STRIDE);
            }

            const plUInt32 uiCoarsest = uiLevels - 1;
            {
            const ProfileScope scope(bProfileKernels, rec.m_pEncoder, "GpuFluid LiqProf/Coarsest");
            rec.Run(pShaders->m_hCoarsest, LevelConstants(uiCoarsest, 0, 1.5f), plVec3U32(1, 1, 1),
              {brickActive, region, W("GpuFluidLiqMgX", levelX[uiCoarsest]), W("GpuFluidLiqMgB", levelB[uiCoarsest]), W("GpuFluidLiqMgType", levelType[uiCoarsest])});
            }

            for (plUInt32 l = uiLevels - 1; l-- > 0;)
            {
              const plGpuFluidLiquidConstants lc = LevelConstants(l, 0, 1.0f);
              {
              const ProfileScope scope(bProfileKernels, rec.m_pEncoder, "GpuFluid LiqProf/Prolong");
              if (l == 0)
              {
                rec.RunIndirect(pShaders->m_hProlongBricks, lc, hBrickArgs,
                  {brickArgs, brickList, brickActive, W("GpuFluidLiqMgX", hFineX), W("GpuFluidLiqMgType", levelType[0]), W("GpuFluidLiqMgCoarseX", levelX[1])});
              }
              else
              {
                rec.RunIndirect(pShaders->m_hProlongDense, lc, hLevelRegion,
                  {brickActive, region, W("GpuFluidLiqMgX", levelX[l]), W("GpuFluidLiqMgType", levelType[l]), W("GpuFluidLiqMgCoarseX", levelX[l + 1])},
                  l * GPU_FLUID_LIQUID_REGION_STRIDE);
              }
              }
              Smooth(l, uiPost, true, hFineX, hFineB);
            }
          };

          const bool bPressureStage = (uiStages & 2u) != 0;
          if (bPressureStage && bPcg)
          {
            // MGPCG with the warm start as x0, one V-cycle (from zero) as the preconditioner; uiCycles V-cycles in
            // all. The scalars never leave the GPU.
            const Bind type = W("GpuFluidLiqMgType", hCellType);
            const Bind scalars = W("GpuFluidLiqPcgScalars", hPcgScalars);
            const Bind partials = W("GpuFluidLiqPcgPartials", hPcgPartials);
            auto Reduce = [&](plUInt32 uiMode)
            {
              const ProfileScope scope(bProfileKernels, rec.m_pEncoder, "GpuFluid LiqProf/PCG reduce");
              plGpuFluidLiquidConstants lc = c;
              lc.GpuFluidLiqPass = plVec4U32(0, uiMode, 0, 0);
              rec.Run(pShaders->m_hPcgReduce, lc, plVec3U32(1, 1, 1), {brickActive, partials, scalars});
            };
            auto DotRZ = [&](plUInt32 uiMode)
            {
              rec.RunIndirect(pShaders->m_hPcgDot, c, hBrickArgs, {brickArgs, brickList, brickActive, type, W("GpuFluidLiqPcgR", hPcgR), W("GpuFluidLiqPcgZ", hPcgZ), partials});
              Reduce(uiMode);
              rec.RunIndirect(pShaders->m_hPcgUpdateP, c, hBrickArgs, {brickArgs, brickList, brickActive, type, scalars, W("GpuFluidLiqPcgZ", hPcgZ), W("GpuFluidLiqPcgP", hPcgP)});
            };

            rec.RunIndirect(pShaders->m_hPcgInit, c, hBrickArgs,
              {brickArgs, brickList, brickActive, type, W("GpuFluidLiqMgX", hPressure), W("GpuFluidLiqMgB", hRhs), W("GpuFluidLiqPcgR", hPcgR), W("GpuFluidLiqPcgZ", hPcgZ),
                W("GpuFluidLiqPcgP", hPcgP)});
            VCycle(hPcgZ, hPcgR);
            DotRZ(GPU_FLUID_LIQUID_PCG_REDUCE_START);

            const plUInt32 uiIterations = plMath::Max(uiCycles, 1u);
            for (plUInt32 uiIteration = 0; uiIteration < uiIterations; ++uiIteration)
            {
              {
              const ProfileScope scope(bProfileKernels, rec.m_pEncoder, "GpuFluid LiqProf/PCG apply");
              rec.RunIndirect(pShaders->m_hPcgApply, c, hBrickArgs, {brickArgs, brickList, brickActive, type, W("GpuFluidLiqMgX", hPcgP), W("GpuFluidLiqPcgQ", hPcgQ), partials});
              }
              Reduce(GPU_FLUID_LIQUID_PCG_REDUCE_ALPHA);
              rec.RunIndirect(pShaders->m_hPcgUpdateX, c, hBrickArgs,
                {brickArgs, brickList, brickActive, type, scalars, W("GpuFluidLiqMgX", hPressure), W("GpuFluidLiqPcgR", hPcgR), W("GpuFluidLiqPcgZ", hPcgZ), W("GpuFluidLiqPcgP", hPcgP),
                  W("GpuFluidLiqPcgQ", hPcgQ)});

              if (uiIteration + 1 < uiIterations)
              {
                VCycle(hPcgZ, hPcgR);
                DotRZ(GPU_FLUID_LIQUID_PCG_REDUCE_BETA);
              }
            }
          }
          else if (bPressureStage)
          {
            for (plUInt32 uiCycle = 0; uiCycle < uiCycles; ++uiCycle)
              VCycle(levelX[0], levelB[0]);
          }

          if ((uiStages & 4u) != 0)
          {
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
          }
        });
      };
      if (bProfileKernels)
      {
        AddSolvePass("GpuFluid Liquid/Solve Setup", 1u);
        AddSolvePass("GpuFluid Liquid/Solve", 2u);
        AddSolvePass("GpuFluid Liquid/Solve Project", 4u);
      }
      else
      {
        AddSolvePass("GpuFluid Liquid/Solve", 7u);
      }
    }

    // Liquid/G2P
    {
      auto pass = ref_graph.AddComputePass("GpuFluid Liquid/G2P");
      pass.ReadBuffer(G(hArgs), indirect);
      for (plGALBufferHandle h : {hVelT, hVelS, hShapes, hCellType, hSolidSdf, hRegions})
        pass.ReadBuffer(G(h), plGALResourceState::ComputeShaderRead);
      for (plGALBufferHandle h : {hPos, hVel, hAff, hCounters, hBrickSpeed, hBrickCalm, hBrickOcc})
        pass.WriteBuffer(G(h));
      pass.HasSideEffects();
      plGpuFluidLiquidConstants g2pConstants = c;
      g2pConstants.GpuFluidLiqPass = plVec4U32((input.m_bBallisticAir ? 1u : 0u) | (input.m_bBallisticClearOfSurface ? 2u : 0u), 0, 0, 0);
      // Brick sleep after G2P: the wake boxes and whether bricks may fall asleep at all.
      plGpuFluidLiquidConstants sleepConstants = c;
      sleepConstants.GpuFluidLiqPass = plVec4U32(uiWakeBoxCount, input.m_bAllowSleep ? 1u : 0u, 0, 0);
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          Recorder rec(ctx);
          rec.RunIndirect(pShaders->m_hG2P, g2pConstants, hArgs,
            {R("GpuFluidLiqParticleArgs", hArgs), R("GpuFluidLiqVelNew", hVelT), R("GpuFluidLiqVelOld", hVelS), R("GpuFluidLiqShapes", hShapes), R("GpuFluidLiqSolidSdf", hSolidSdf),
              R("GpuFluidLiqCellType", hCellType), R("GpuFluidLiqBrickCalm", hBrickCalm), W("GpuFluidLiqBrickSpeed", hBrickSpeed), W("GpuFluidLiqPositions", hPos),
              W("GpuFluidLiqVelocities", hVel), W("GpuFluidLiqAffine", hAff), W("GpuFluidLiqCounters", hCounters)});
          rec.Run(pShaders->m_hSleep, sleepConstants, LinearGroups(uiBrickCount, 256),
            {R("GpuFluidLiqBrickOccupied", hBrickOcc), R("GpuFluidLiqBrickSpeed", hBrickSpeed), R("GpuFluidLiqRegions", hRegions), W("GpuFluidLiqBrickCalm", hBrickCalm),
              W("GpuFluidLiqCounters", hCounters)});
        });
    }

    m_uiCurrentSet = dst;
  }

  // Render: the points the debug renderer draws, the draw arguments and the frame's stats.
  {
    const plUInt32 cur = m_uiCurrentSet;
    const plGALBufferHandle hPos = m_hPositions[cur], hVel = m_hVelocities[cur], hArgs = m_hParticleArgs[cur];

    plGpuFluidLiquidConstants c = base;
    c.GpuFluidLiqPass = plVec4U32(uiSubsteps, static_cast<plUInt32>(plMath::Round(fSimDt * 1.0e6f)), 0, 0);
    c.GpuFluidLiqPassF = plVec4(input.m_vDomainOrigin.x, input.m_vDomainOrigin.y, input.m_vDomainOrigin.z, 0.0f);

    auto pass = ref_graph.AddComputePass("GpuFluid Liquid/Render");
    pass.ReadBuffer(G(hArgs), indirect);
    pass.ReadBuffer(G(hPos), plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(G(hVel), plGALResourceState::ComputeShaderRead);
    const plGALBufferHandle hProbePhi = m_hPhiRaw;
    const bool bProbes = !input.m_Probes.IsEmpty() && input.m_bLevelSet;
    if (bProbes)
    {
      pass.ReadBuffer(G(hProbePhi), plGALResourceState::ComputeShaderRead);
      pass.ReadBuffer(G(hBrickActive), plGALResourceState::ComputeShaderRead);
      pass.ReadBuffer(G(hCellCount), plGALResourceState::ComputeShaderRead);
    }
    for (plGALBufferHandle h : {hPoints, hDrawArgs, hStats, hCounters})
      pass.WriteBuffer(G(h));
    pass.HasSideEffects();

    // Level probes: the columns, packed x | y << 16.
    plGpuFluidLiquidConstants probeConstants = c;
    probeConstants.GpuFluidLiqPass = plVec4U32(GPU_FLUID_LIQUID_NO_PROBE, GPU_FLUID_LIQUID_NO_PROBE, GPU_FLUID_LIQUID_NO_PROBE, GPU_FLUID_LIQUID_NO_PROBE);
    for (plUInt32 i = 0; i < plMath::Min<plUInt32>(input.m_Probes.GetCount(), GPU_FLUID_LIQUID_MAX_PROBES); ++i)
      probeConstants.GpuFluidLiqPass.GetData()[i] = (input.m_Probes[i].x & 0xFFFFu) | (input.m_Probes[i].y << 16);

    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        Recorder rec(ctx);
        rec.RunIndirect(pShaders->m_hRenderPrep, c, hArgs,
          {R("GpuFluidLiqParticleArgs", hArgs), R("GpuFluidLiqPositions", hPos), R("GpuFluidLiqVelocities", hVel), W("GpuFluidLiqPoints", hPoints), W("GpuFluidLiqStats", hStats)});
        rec.Run(pShaders->m_hFrameEnd, c, plVec3U32(1, 1, 1),
          {R("GpuFluidLiqParticleArgs", hArgs), W("GpuFluidLiqCounters", hCounters), W("GpuFluidLiqStats", hStats), W("GpuFluidLiqDrawArgs", hDrawArgs)});
        if (bProbes)
          rec.Run(pShaders->m_hProbes, probeConstants, plVec3U32(1, 1, 1), {R("GpuFluidLiqPhi", hProbePhi), R("GpuFluidLiqBrickActive", hBrickActive), R("GpuFluidLiqCellCount", hCellCount), W("GpuFluidLiqStats", hStats)});
      });
  }

  if (bReset)
    m_bHasSurface = false;

  // Liquid/Surface: once per frame that steps. By default the level set of the last substep (from the particles at its
  // start, one substep before the final positions) is the raw surface. With m_bSurfaceOwnLevelSet the particles are
  // sorted again (the next substep's sort then finds them in order) and the level set is computed at their final
  // positions. Then the temporal blend with last frame's surface, smoothing and surface nets over the active bricks.
  const bool bSurfaceLevelSet = input.m_bSurfaceOwnLevelSet || !bLevelSet;
  if (input.m_bSurface && bStep && uiSubsteps > 0)
  {
    const plUInt32 src = m_uiCurrentSet;
    const plUInt32 dst = bSurfaceLevelSet ? 1 - src : src;
    if (bSurfaceLevelSet)
    {
      plGpuFluidLiquidConstants c = base;
      c.GpuFluidLiqPass = plVec4U32(1, 0, 0, 0);
      AddSortPass(src, dst, c, "GpuFluid Liquid/Surface Sort");
    }
    m_uiCurrentSet = dst;

    const plUInt32 uiPrev = m_uiSurfaceCurrent;
    const plUInt32 uiCur = 1 - uiPrev;
    const plUInt32 uiTag = m_uiSurfaceTag++;
    m_uiSurfaceCurrent = uiCur;

    const plGALBufferHandle hPos = m_hPositions[dst];
    const plGALBufferHandle hPhiPrev = m_hSurfacePhi[uiPrev], hPhiCur = m_hSurfacePhi[uiCur];
    const plGALBufferHandle hStampPrev = m_hSurfaceStamp[uiPrev], hStampCur = m_hSurfaceStamp[uiCur];
    const plGALBufferHandle hVertices = m_hSurfaceVertices, hQuads = m_hSurfaceQuads, hCubeMap = m_hSurfaceCubeMap, hSurfCounters = m_hSurfaceCounters,
                            hSurfArgs = m_hSurfaceDrawArgs;
    const plUInt32 uiMaxVertices = m_uiMaxSurfaceVertices, uiMaxQuads = m_uiMaxSurfaceQuads;
    // The time since the last extraction, for carrying last frame's surface along (0 when there was none).
    const float fBlend = m_bHasSurface ? plMath::Clamp(input.m_fSurfaceBlend, 0.0f, 0.9f) : 0.0f;
    const float fCellsPerVelocity = fSimDt / m_fDx;
    const plUInt32 uiSmoothPairs = (plMath::Min<plUInt32>(input.m_uiSurfaceSmoothSteps, 8) + 1) / 2;
    const float fSmoothGain = plMath::Clamp(input.m_fSurfaceSmoothGain, 0.0f, 1.0f);
    const float fSmoothClamp = plMath::Clamp(input.m_fSurfaceSmoothClamp, 0.0f, 1.0f);
    // Outside the domain the surface closes at about the wall: phi there is the particle radius.
    const float fOutside = plMath::Max(vLevelSetParams.y, 0.05f);
    const float fFar = vLevelSetParams.z;

    auto pass = ref_graph.AddComputePass("GpuFluid Liquid/Surface");
    pass.ReadBuffer(G(hBrickArgs), indirect);
    for (plGALBufferHandle h : {hBrickList, hBrickActive, hCellCount, hCellStart, hPos, hVelT, hPhiPrev, hStampPrev, hShapes, hSolidSdf})
      pass.ReadBuffer(G(h), plGALResourceState::ComputeShaderRead);
    for (plGALBufferHandle h : {hPhiRaw, hPhiTmp, hPhiCur, hStampCur, hVertices, hQuads, hCubeMap, hSurfCounters, hSurfArgs, hStats})
      pass.WriteBuffer(G(h));
    pass.HasSideEffects();
    pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
      {
        Recorder rec(ctx);
        const Bind brickArgs = R("GpuFluidLiqBrickArgs", hBrickArgs);
        const Bind brickList = R("GpuFluidLiqBrickList", hBrickList);
        const Bind brickActive = R("GpuFluidLiqBrickActive", hBrickActive);
        const Bind shapes = R("GpuFluidLiqShapes", hShapes);
        const Bind solids = R("GpuFluidLiqSolidSdf", hSolidSdf);

        plGpuFluidLiquidConstants c = base;
        if (bSurfaceLevelSet)
        {
          c.GpuFluidLiqPassF = vLevelSetParams;
          rec.RunIndirect(pShaders->m_hLevelSet, c, hBrickArgs,
            {brickArgs, brickList, R("GpuFluidLiqCellCount", hCellCount), R("GpuFluidLiqCellStart", hCellStart), R("GpuFluidLiqPositions", hPos), W("GpuFluidLiqPhiOut", hPhiRaw)});
        }

        c.GpuFluidLiqPass = plVec4U32(uiTag, 0, 0, 0);
        c.GpuFluidLiqPassF = plVec4(fOutside, fFar, fBlend, fCellsPerVelocity);
        rec.RunIndirect(pShaders->m_hSurfaceBlend, c, hBrickArgs,
          {brickArgs, brickList, brickActive, shapes, solids,W("GpuFluidLiqSurfPhi", hPhiRaw), R("GpuFluidLiqPhiPrev", hPhiPrev), R("GpuFluidLiqStampPrev", hStampPrev), R("GpuFluidLiqVel", hVelT),
            W("GpuFluidLiqPhiOut", hPhiCur), W("GpuFluidLiqStampOut", hStampCur)});

        c.GpuFluidLiqPassF = plVec4(fOutside, fFar, fSmoothGain, fSmoothClamp);
        for (plUInt32 i = 0; i < uiSmoothPairs; ++i)
        {
          rec.RunIndirect(pShaders->m_hSurfaceSmooth, c, hBrickArgs, {brickArgs, brickList, brickActive, shapes, solids,W("GpuFluidLiqSurfPhi", hPhiCur), W("GpuFluidLiqPhiOut", hPhiTmp)});
          rec.RunIndirect(pShaders->m_hSurfaceSmooth, c, hBrickArgs, {brickArgs, brickList, brickActive, shapes, solids,W("GpuFluidLiqSurfPhi", hPhiTmp), W("GpuFluidLiqPhiOut", hPhiCur)});
        }

        c.GpuFluidLiqPass = plVec4U32(0, uiMaxQuads, uiMaxVertices, 0);
        rec.Run(pShaders->m_hSurfaceArgs, c, plVec3U32(1, 1, 1), {W("GpuFluidLiqSurfCounters", hSurfCounters), W("GpuFluidLiqSurfDrawArgs", hSurfArgs), W("GpuFluidLiqStats", hStats)});

        c.GpuFluidLiqPass = plVec4U32(uiMaxVertices, uiMaxQuads, 0, 0);
        c.GpuFluidLiqPassF = plVec4(fOutside, fFar, 0.0f, 0.0f);
        rec.RunIndirect(pShaders->m_hSurfaceVerts, c, hBrickArgs,
          {brickArgs, brickList, brickActive, shapes, solids,W("GpuFluidLiqSurfPhi", hPhiCur), W("GpuFluidLiqSurfVertices", hVertices), W("GpuFluidLiqSurfCubeMap", hCubeMap),
            W("GpuFluidLiqSurfCounters", hSurfCounters)});
        rec.RunIndirect(pShaders->m_hSurfaceQuads, c, hBrickArgs,
          {brickArgs, brickList, brickActive, shapes, solids,W("GpuFluidLiqSurfPhi", hPhiCur), W("GpuFluidLiqSurfCubeMap", hCubeMap), W("GpuFluidLiqSurfQuads", hQuads),
            W("GpuFluidLiqSurfCounters", hSurfCounters)});

        c.GpuFluidLiqPass = plVec4U32(1, uiMaxQuads, uiMaxVertices, 0);
        rec.Run(pShaders->m_hSurfaceArgs, c, plVec3U32(1, 1, 1), {W("GpuFluidLiqSurfCounters", hSurfCounters), W("GpuFluidLiqSurfDrawArgs", hSurfArgs), W("GpuFluidLiqStats", hStats)});
      });

    m_bHasSurface = true;
  }

  m_bRecorded = true;
  return true;
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidLiquidSolver);
