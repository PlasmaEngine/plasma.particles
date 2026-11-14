#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGasSolver.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/Buffer.h>
#include <RendererFoundation/Resources/Texture.h>
#include <Shaders/GpuFluid/GpuFluidGasConstants.h>

static_assert(sizeof(plGpuFluidSolidShape) == 128 && sizeof(plGpuFluidSolid) == sizeof(plGpuFluidSolidShape), "plGpuFluidSolid must match plGpuFluidSolidShape");
static_assert(plGpuFluidMaxSolidShapes == GPU_FLUID_MAX_SOLID_SHAPES && plGpuFluidMaxDenseLevels == GPU_FLUID_MG_MAX_DENSE_LEVELS && plGpuFluidStatProbeCount == GPU_FLUID_STAT_PROBE_COUNT);
static_assert(plGpuFluidPressureSolver::RedBlackSOR == GPU_FLUID_PRESSURE_RBSOR && plGpuFluidPressureSolver::Multigrid == GPU_FLUID_PRESSURE_MULTIGRID);
static_assert(plGpuFluidProbeInitialError < GPU_FLUID_STAT_PROBE_COUNT);

namespace
{
  /// Every buffer and atlas of a frame as render-graph handles, captured by value by the pass callbacks.
  struct FrameResources
  {
    plRenderGraphTextureHandle m_hVelocity[3];
    plRenderGraphTextureHandle m_hScalar[2];
    plRenderGraphTextureHandle m_hScratch;
    plRenderGraphBufferHandle m_hPressure[2];
    plRenderGraphBufferHandle m_hDivergence;
    plRenderGraphBufferHandle m_hSolidFlags;
    plRenderGraphBufferHandle m_hFaceCodes;
    plRenderGraphBufferHandle m_hMGLocal;
    plRenderGraphBufferHandle m_hMGCoeff;
    plRenderGraphBufferHandle m_hMGMask;
    plRenderGraphBufferHandle m_hMGDense;
    plRenderGraphBufferHandle m_hMGDenseCoeff;
    plRenderGraphBufferHandle m_hSolidShapes;
    plRenderGraphBufferHandle m_hStats;
  };

  /// The shaders of the pressure phase, captured by value.
  struct PressureKernels
  {
    plShaderResourceHandle m_hSetup;
    plShaderResourceHandle m_hDivergence;
    plShaderResourceHandle m_hSOR;
    plShaderResourceHandle m_hFine;
    plShaderResourceHandle m_hMid;
    plShaderResourceHandle m_hDense;
    plShaderResourceHandle m_hPoisson;
    plShaderResourceHandle m_hProject;
    plShaderResourceHandle m_hHalo;
  };

  /// The dense levels of the multigrid and where the single-group tail starts.
  struct DenseLevels
  {
    plUInt32 m_uiCount = 0;
    plUInt32 m_uiCells[GPU_FLUID_MG_MAX_DENSE_LEVELS] = {};
    plUInt32 m_uiTailStart = 0;
  };

  plGALTextureHandle CreateAtlas(plGALDevice* pDevice, const plVec3U32& vAtlasBricks, const char* szName)
  {
    plGALTextureCreationDescription desc;
    desc.m_Type = plGALTextureType::Texture3D;
    desc.m_Format = plGALResourceFormat::RGBAHalf;
    desc.m_uiWidth = vAtlasBricks.x * GPU_FLUID_BRICK_STORE;
    desc.m_uiHeight = vAtlasBricks.y * GPU_FLUID_BRICK_STORE;
    desc.m_uiDepth = vAtlasBricks.z * GPU_FLUID_BRICK_STORE;
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

  plGALBufferHandle CreateStructuredBuffer(plGALDevice* pDevice, plUInt32 uiStructSize, plUInt32 uiCount, const char* szName)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = uiStructSize;
    desc.m_uiTotalSize = plMath::Max(uiCount, 1u) * uiStructSize;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
    desc.m_ResourceAccess.m_bImmutable = false;

    plGALBufferHandle hBuffer = pDevice->CreateBuffer(desc);
    if (!hBuffer.IsInvalidated())
      pDevice->GetBuffer(hBuffer)->SetDebugName(szName);
    return hBuffer;
  }

  /// The fields every kernel of a tick shares.
  plGpuFluidGasConstants MakeConstants(const plGpuFluidSparseGrid& grid, const plGpuFluidGasSimulation& sim, float fTime)
  {
    plGpuFluidGasConstants c;
    plMemoryUtils::ZeroFill(&c, 1);

    const plVec3U32 vGrid = grid.GetBrickGrid();
    const plVec3U32 vAtlas = grid.GetAtlasBricks();
    const float fVoxel = plMath::Max(sim.m_fVoxelSize, 1e-4f);

    c.GpuFluidGasGrid = plVec4U32(vGrid.x, vGrid.y, vGrid.z, grid.GetCapacity());
    c.GpuFluidGasAtlas = plVec4U32(vAtlas.x, vAtlas.y, vAtlas.z, sim.m_OpenFaces.GetValue());
    c.GpuFluidGasStep = plVec4(sim.m_fTickSeconds, fVoxel, 1.0f / fVoxel, fTime);
    return c;
  }

  void UavBarrier(plGALCommandEncoder* pEncoder, plGALTextureHandle hTexture)
  {
    pEncoder->TextureBarrier(hTexture, plGALResourceState::UnorderedAccessWrite, plGALResourceState::UnorderedAccessWrite);
  }

  void UavBarrier(plGALCommandEncoder* pEncoder, plGALBufferHandle hBuffer)
  {
    pEncoder->BufferBarrier(hBuffer, plGALResourceState::UnorderedAccessWrite, plGALResourceState::UnorderedAccessWrite);
  }

  /// Binds the shader and the grid, dispatches one group per active brick.
  void DispatchPerBrick(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const plShaderResourceHandle& hShader, const plGpuFluidGasConstants& constants)
  {
    plRenderContext* pContext = ctx.GetRenderContext();
    pContext->BindShader(hShader);
    plGpuFluidSparseGrid::BindReadOnly(ctx, pContext->GetBindGroup(), grid);
    pContext->SetPushConstants("plGpuFluidGasConstants", constants);
    pContext->DispatchIndirect(ctx.ResolveBuffer(grid.m_hDispatchArgs), GPU_FLUID_ARGS_ACTIVE * sizeof(plUInt32)).IgnoreResult();
  }

  /// Binds the shader and the grid, dispatches GPU_FLUID_SLABS groups per active brick (groupId.z is the slab).
  void DispatchPerBrickSlabs(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const plShaderResourceHandle& hShader, const plGpuFluidGasConstants& constants)
  {
    plRenderContext* pContext = ctx.GetRenderContext();
    pContext->BindShader(hShader);
    plGpuFluidSparseGrid::BindReadOnly(ctx, pContext->GetBindGroup(), grid);
    pContext->SetPushConstants("plGpuFluidGasConstants", constants);
    pContext->DispatchIndirect(ctx.ResolveBuffer(grid.m_hDispatchArgs), GPU_FLUID_ARGS_SLABS * sizeof(plUInt32)).IgnoreResult();
  }

  /// Binds the shader and the grid, dispatches one group per brick assigned this frame.
  void DispatchPerNewBrick(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const plShaderResourceHandle& hShader, const plGpuFluidGasConstants& constants)
  {
    plRenderContext* pContext = ctx.GetRenderContext();
    pContext->BindShader(hShader);
    plGpuFluidSparseGrid::BindReadOnly(ctx, pContext->GetBindGroup(), grid);
    pContext->SetPushConstants("plGpuFluidGasConstants", constants);
    pContext->DispatchIndirect(ctx.ResolveBuffer(grid.m_hDispatchArgs), GPU_FLUID_ARGS_NEW * sizeof(plUInt32)).IgnoreResult();
  }

  /// One group per active brick of one brick colour (Pass.y 0 or 1), or of every brick (2).
  void DispatchPerBrickOfColour(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const plShaderResourceHandle& hShader, const plGpuFluidGasConstants& constants)
  {
    const plUInt32 uiColour = constants.GpuFluidGasPass.y;
    if (uiColour >= 2)
    {
      DispatchPerBrick(ctx, grid, hShader, constants);
      return;
    }

    plRenderContext* pContext = ctx.GetRenderContext();
    pContext->BindShader(hShader);
    plGpuFluidSparseGrid::BindReadOnly(ctx, pContext->GetBindGroup(), grid);
    pContext->SetPushConstants("plGpuFluidGasConstants", constants);
    pContext->DispatchIndirect(ctx.ResolveBuffer(grid.m_hColourArgs), uiColour * GPU_FLUID_ARGS_WORDS * sizeof(plUInt32)).IgnoreResult();
  }

  /// Binds the shader and the grid, dispatches uiGroups groups.
  void DispatchGroups(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const plShaderResourceHandle& hShader, const plGpuFluidGasConstants& constants, plUInt32 uiGroups)
  {
    plRenderContext* pContext = ctx.GetRenderContext();
    pContext->BindShader(hShader);
    plGpuFluidSparseGrid::BindReadOnly(ctx, pContext->GetBindGroup(), grid);
    pContext->SetPushConstants("plGpuFluidGasConstants", constants);
    pContext->Dispatch(plMath::Max(uiGroups, 1u), 1, 1).IgnoreResult();
  }

  constexpr plUInt32 s_uiDenseThreads = 256;

  /// Binds every buffer of the pressure phase under the names its kernels use.
  void BindPressureBuffers(const plRenderGraphContext& ctx, const FrameResources& res, plRenderGraphTextureHandle hVelocity)
  {
    plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
    bindGroup.BindTexture("GpuFluidVelocityTarget", ctx.ResolveTexture(hVelocity));
    bindGroup.BindTexture("GpuFluidHaloTarget", ctx.ResolveTexture(hVelocity));
    bindGroup.BindTexture("GpuFluidHaloTarget2", ctx.ResolveTexture(hVelocity));
    bindGroup.BindBuffer("GpuFluidPressure", ctx.ResolveBuffer(res.m_hPressure[0]));
    bindGroup.BindBuffer("GpuFluidPressure1", ctx.ResolveBuffer(res.m_hPressure[1]));
    bindGroup.BindBuffer("GpuFluidDivergence", ctx.ResolveBuffer(res.m_hDivergence));
    bindGroup.BindBuffer("GpuFluidSolidFlags", ctx.ResolveBuffer(res.m_hSolidFlags));
    bindGroup.BindBuffer("GpuFluidFaceCodes", ctx.ResolveBuffer(res.m_hFaceCodes));
    bindGroup.BindBuffer("GpuFluidMGLocal", ctx.ResolveBuffer(res.m_hMGLocal));
    bindGroup.BindBuffer("GpuFluidMGCoeff", ctx.ResolveBuffer(res.m_hMGCoeff));
    bindGroup.BindBuffer("GpuFluidMGMask", ctx.ResolveBuffer(res.m_hMGMask));
    bindGroup.BindBuffer("GpuFluidMGDense", ctx.ResolveBuffer(res.m_hMGDense));
    bindGroup.BindBuffer("GpuFluidMGDenseCoeff", ctx.ResolveBuffer(res.m_hMGDenseCoeff));
    bindGroup.BindBuffer("GpuFluidStats", ctx.ResolveBuffer(res.m_hStats));
  }

  /// Declares every buffer of the pressure phase on its pass.
  template <typename PassBuilder>
  void DeclarePressureBuffers(PassBuilder& ref_pass, const FrameResources& res, plRenderGraphTextureHandle hVelocity)
  {
    ref_pass.WriteTexture(hVelocity);
    ref_pass.WriteBuffer(res.m_hPressure[0]);
    ref_pass.WriteBuffer(res.m_hPressure[1]);
    ref_pass.WriteBuffer(res.m_hDivergence);
    ref_pass.WriteBuffer(res.m_hSolidFlags);
    ref_pass.WriteBuffer(res.m_hFaceCodes);
    ref_pass.WriteBuffer(res.m_hMGLocal);
    ref_pass.WriteBuffer(res.m_hMGCoeff);
    ref_pass.WriteBuffer(res.m_hMGMask);
    ref_pass.WriteBuffer(res.m_hMGDense);
    ref_pass.WriteBuffer(res.m_hMGDenseCoeff);
    ref_pass.WriteBuffer(res.m_hStats);
  }

  /// The multigrid's constants: Params0 (omega, pre, post, dense level), Params1 (coarsest sweeps).
  plGpuFluidGasConstants MGConstants(const plGpuFluidGasConstants& base, const plGpuFluidMultigridSettings& mg, plUInt32 uiMode, plUInt32 uiLevel = 0, plUInt32 uiColour = 0)
  {
    plGpuFluidGasConstants c = base;
    c.GpuFluidGasPass = plVec4U32(uiMode, uiColour, 0, 0);
    c.GpuFluidGasParams0 = plVec4(mg.m_fOmega, static_cast<float>(mg.m_uiPreSmooth), static_cast<float>(mg.m_uiPostSmooth), static_cast<float>(uiLevel));
    c.GpuFluidGasParams1 = plVec4(static_cast<float>(mg.m_uiCoarsestSweeps), 0.0f, mg.m_fProlongScale, 0.0f);
    return c;
  }

  plUInt32 DenseGroups(plUInt32 uiCells)
  {
    return (uiCells + s_uiDenseThreads - 1) / s_uiDenseThreads;
  }

  /// GasMGSetup and the dense levels' setup: the operator of this tick on every level.
  void RecordSetup(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const PressureKernels& kernels, const FrameResources& res,
    const plGpuFluidGasConstants& base, const plGpuFluidMultigridSettings& mg, const DenseLevels& dense, bool bDense)
  {
    plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();

    DispatchPerBrick(ctx, grid, kernels.m_hSetup, base);
    UavBarrier(pEncoder, ctx.ResolveBuffer(res.m_hFaceCodes));
    UavBarrier(pEncoder, ctx.ResolveBuffer(res.m_hMGCoeff));
    UavBarrier(pEncoder, ctx.ResolveBuffer(res.m_hMGMask));
    UavBarrier(pEncoder, ctx.ResolveBuffer(res.m_hMGDenseCoeff));

    if (!bDense)
      return;

    const plGALBufferHandle hDense = ctx.ResolveBuffer(res.m_hMGDense);
    const plGALBufferHandle hDenseCoeff = ctx.ResolveBuffer(res.m_hMGDenseCoeff);

    if (dense.m_uiTailStart > 0)
    {
      for (plUInt32 k = 0; k <= dense.m_uiTailStart; ++k)
      {
        DispatchGroups(ctx, grid, kernels.m_hDense, MGConstants(base, mg, GPU_FLUID_MG_DENSE_SETUP, k), DenseGroups(dense.m_uiCells[k]));
        UavBarrier(pEncoder, hDense);
        UavBarrier(pEncoder, hDenseCoeff);
      }
    }

    DispatchGroups(ctx, grid, kernels.m_hDense, MGConstants(base, mg, GPU_FLUID_MG_DENSE_SETUP_TAIL, dense.m_uiTailStart), 1);
    UavBarrier(pEncoder, hDense);
    UavBarrier(pEncoder, hDenseCoeff);
  }

  void RecordSOR(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const PressureKernels& kernels, const FrameResources& res,
    const plGpuFluidGasConstants& base, float fOmega, plUInt32 uiIterations);

  void RecordDense(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const PressureKernels& kernels, const plGpuFluidGasConstants& base,
    const plGpuFluidMultigridSettings& mg, const DenseLevels& dense, plGALBufferHandle hDense);

  /// Stages of a V-cycle, so a profiling run can time each in its own pass.
  constexpr plUInt32 s_uiStageFineDown = PL_BIT(0);
  constexpr plUInt32 s_uiStageMidDown = PL_BIT(1);
  constexpr plUInt32 s_uiStageDense = PL_BIT(2);
  constexpr plUInt32 s_uiStageMidUp = PL_BIT(3);
  constexpr plUInt32 s_uiStageFineUp = PL_BIT(4);
  constexpr plUInt32 s_uiStageAll = 0x1F;
  const char* s_szStageNames[] = {"Gas/MG Fine Down", "Gas/MG Mid Down", "Gas/MG Dense", "Gas/MG Mid Up", "Gas/MG Fine Up"};

  /// One V-cycle: Phi (side 0) -> pre-smoothed (side 1) -> ... -> corrected and post-smoothed (side 0).
  void RecordVCycle(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const PressureKernels& kernels, const FrameResources& res,
    const plGpuFluidGasConstants& base, const plGpuFluidMultigridSettings& mg, const DenseLevels& dense, plUInt32 uiStages = s_uiStageAll, bool bShort = false,
    plUInt32 uiDivergenceTick = 0)
  {
    // uiDivergenceTick: 0 the divergence buffer is ready; 1 the first fine launches take it from the velocity; 2 the
    // same on the frame's last tick (with its stats).
    plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
    plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
    const plGALBufferHandle hPhi[2] = {ctx.ResolveBuffer(res.m_hPressure[0]), ctx.ResolveBuffer(res.m_hPressure[1])};
    const plGALBufferHandle hLocal = ctx.ResolveBuffer(res.m_hMGLocal);
    const plGALBufferHandle hDense = ctx.ResolveBuffer(res.m_hMGDense);

    const plGpuFluidMultigridSettings& fine = mg;

    // The brick colour that smooths the fine level first on the way down, and the other.
    const plUInt32 a = mg.m_uiDownColour & 1u;
    const plUInt32 b = a ^ 1u;

    // Every smoothing kernel runs block red-black: the bricks of one colour, then the other's, which sees the first
    // colour's new values in its halo (GasMGFine.plShader). Pass.y is the brick colour, Pass.w the second colour's flags.
    auto Brick = [&](const plGpuFluidMultigridSettings& settings, plUInt32 uiMode, plUInt32 uiColour, plUInt32 uiFlags)
    {
      plGpuFluidGasConstants c = MGConstants(base, settings, uiMode);
      c.GpuFluidGasPass.y = uiColour;
      c.GpuFluidGasPass.w = uiFlags;
      return c;
    };

    // Down, two fine and three small launches (each level's smoothing, residual and restriction fused where both
    // colours' data is ready):
    //   a: F pre-smoothing (side 0 -> 1), residual against b's old Phi -> partial b of M;
    //   b: F pre-smoothing against a's new Phi, residual -> b of M, M pre-smoothing (halo 0);
    //   a: b of M corrected by b's change, M pre-smoothing against b's x of M, M residual -> b of C, C pre-smoothing;
    //   b: M residual -> b of C, C pre-smoothing against a's x of C, C residual -> D0;
    //   a: C residual -> D0.
    if (uiStages & s_uiStageFineDown)
    {
      // The tick's first cycle takes the divergence on the way (GasMGFine, GPU_FLUID_MG_DIVERGENCE = 8).
      const plUInt32 uiDivergence = uiDivergenceTick > 0 ? 8u : 0u;
      plGpuFluidGasConstants first = Brick(fine, GPU_FLUID_MG_FINE_SMOOTH_RESTRICT, a, 4 | uiDivergence);
      first.GpuFluidGasPass.z = uiDivergenceTick == 2 ? 1u : 0u;
      plGpuFluidGasConstants second = Brick(fine, GPU_FLUID_MG_FINE_SMOOTH_RESTRICT, b, uiDivergence);
      second.GpuFluidGasPass.z = first.GpuFluidGasPass.z;

      bindGroup.BindBuffer("GpuFluidPhiIn", hPhi[0]);
      bindGroup.BindBuffer("GpuFluidPhiHalo", hPhi[0]);
      bindGroup.BindBuffer("GpuFluidPhiOut", hPhi[1]);
      DispatchPerBrickOfColour(ctx, grid, kernels.m_hFine, first);
      UavBarrier(pEncoder, hPhi[1]);
      UavBarrier(pEncoder, hLocal);
      UavBarrier(pEncoder, ctx.ResolveBuffer(res.m_hDivergence));

      bindGroup.BindBuffer("GpuFluidPhiHalo", hPhi[1]);
      DispatchPerBrickOfColour(ctx, grid, kernels.m_hFine, second);
      UavBarrier(pEncoder, hPhi[1]);
      UavBarrier(pEncoder, hLocal);
      UavBarrier(pEncoder, ctx.ResolveBuffer(res.m_hDivergence));
    }

    if (uiStages & s_uiStageMidDown)
    {
      // Phi before (side 0) and after (side 1) the pre-smoothing, for a's correction.
      bindGroup.BindBuffer("GpuFluidPhiIn", hPhi[0]);
      bindGroup.BindBuffer("GpuFluidPhiHalo", hPhi[1]);
      DispatchPerBrickOfColour(ctx, grid, kernels.m_hMid, Brick(mg, GPU_FLUID_MG_MID_FIX_RESTRICT, a, 0));
      UavBarrier(pEncoder, hLocal);

      if (!bShort)
      {
        DispatchPerBrickOfColour(ctx, grid, kernels.m_hMid, Brick(mg, GPU_FLUID_MG_MID_RESTRICT_COARSE, b, 1));
        UavBarrier(pEncoder, hLocal);
        UavBarrier(pEncoder, hDense);

        DispatchPerBrickOfColour(ctx, grid, kernels.m_hMid, Brick(mg, GPU_FLUID_MG_COARSE_RESTRICT, a, 0));
        UavBarrier(pEncoder, hDense);
      }
    }

    if ((uiStages & s_uiStageDense) && !bShort)
      RecordDense(ctx, grid, kernels, base, mg, dense, hDense);

    if (uiStages & s_uiStageMidUp)
    {
      // Up: C += P(D0), M += P(C), each post-smoothed. A short cycle stops at M: its M levels are only post-smoothed.
      if (!bShort)
      {
        DispatchPerBrickOfColour(ctx, grid, kernels.m_hMid, Brick(mg, GPU_FLUID_MG_COARSE_PROLONG, a, 0));
        UavBarrier(pEncoder, hLocal);
        DispatchPerBrickOfColour(ctx, grid, kernels.m_hMid, Brick(mg, GPU_FLUID_MG_COARSE_PROLONG, b, 1));
        UavBarrier(pEncoder, hLocal);
      }

      plGpuFluidMultigridSettings mid = mg;
      if (bShort)
        mid.m_fProlongScale = 0.0f;
      DispatchPerBrickOfColour(ctx, grid, kernels.m_hMid, Brick(mid, GPU_FLUID_MG_MID_PROLONG, b, 0));
      UavBarrier(pEncoder, hLocal);
      DispatchPerBrickOfColour(ctx, grid, kernels.m_hMid, Brick(mid, GPU_FLUID_MG_MID_PROLONG, a, 1));
      UavBarrier(pEncoder, hLocal);
    }

    if (uiStages & s_uiStageFineUp)
    {
      // F += P(M), post-smoothed, side 1 -> 0.
      bindGroup.BindBuffer("GpuFluidPhiIn", hPhi[1]);
      bindGroup.BindBuffer("GpuFluidPhiHalo", hPhi[1]);
      bindGroup.BindBuffer("GpuFluidPhiOut", hPhi[0]);
      DispatchPerBrickOfColour(ctx, grid, kernels.m_hFine, Brick(fine, GPU_FLUID_MG_FINE_PROLONG, a, 0));
      UavBarrier(pEncoder, hPhi[0]);
      bindGroup.BindBuffer("GpuFluidPhiHalo", hPhi[0]);
      DispatchPerBrickOfColour(ctx, grid, kernels.m_hFine, Brick(fine, GPU_FLUID_MG_FINE_PROLONG, b, 1));
      UavBarrier(pEncoder, hPhi[0]);
    }
  }

  /// The dense levels of a V-cycle: large ones a dispatch per step, the rest in one group.
  void RecordDense(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const PressureKernels& kernels, const plGpuFluidGasConstants& base,
    const plGpuFluidMultigridSettings& mg, const DenseLevels& dense, plGALBufferHandle hDense)
  {
    plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
    for (plUInt32 k = 0; k < dense.m_uiTailStart; ++k)
    {
      for (plUInt32 s = 0; s < mg.m_uiPreSmooth * 2; ++s)
      {
        DispatchGroups(ctx, grid, kernels.m_hDense, MGConstants(base, mg, GPU_FLUID_MG_DENSE_SMOOTH, k, s & 1u), DenseGroups(dense.m_uiCells[k]));
        UavBarrier(pEncoder, hDense);
      }
      DispatchGroups(ctx, grid, kernels.m_hDense, MGConstants(base, mg, GPU_FLUID_MG_DENSE_RESTRICT, k), DenseGroups(dense.m_uiCells[k + 1]));
      UavBarrier(pEncoder, hDense);
    }

    DispatchGroups(ctx, grid, kernels.m_hDense, MGConstants(base, mg, GPU_FLUID_MG_DENSE_CYCLE, dense.m_uiTailStart), 1);
    UavBarrier(pEncoder, hDense);

    for (plUInt32 k = dense.m_uiTailStart; k-- > 0;)
    {
      DispatchGroups(ctx, grid, kernels.m_hDense, MGConstants(base, mg, GPU_FLUID_MG_DENSE_PROLONG, k), DenseGroups(dense.m_uiCells[k]));
      UavBarrier(pEncoder, hDense);
      for (plUInt32 s = 0; s < mg.m_uiPostSmooth * 2; ++s)
      {
        DispatchGroups(ctx, grid, kernels.m_hDense, MGConstants(base, mg, GPU_FLUID_MG_DENSE_SMOOTH, k, s & 1u), DenseGroups(dense.m_uiCells[k]));
        UavBarrier(pEncoder, hDense);
      }
    }
  }

  /// A V-cycle, then the consistent red-black sweeps over the fine level.
  void RecordMultigridCycle(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const PressureKernels& kernels, const FrameResources& res,
    const plGpuFluidGasConstants& base, const plGpuFluidMultigridSettings& mg, const DenseLevels& dense, plUInt32 uiCycle, plUInt32 uiDivergenceTick = 0)
  {
    RecordVCycle(ctx, grid, kernels, res, base, mg, dense, s_uiStageAll, uiCycle > 0 && mg.m_bShortLaterCycles, uiCycle == 0 ? uiDivergenceTick : 0);
    RecordSOR(ctx, grid, kernels, res, base, 1.0f, mg.m_uiFinalSweeps);
  }

  /// Red-black SOR in place on Phi (side 0).
  void RecordSOR(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const PressureKernels& kernels, const FrameResources& res,
    const plGpuFluidGasConstants& base, float fOmega, plUInt32 uiIterations)
  {
    plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
    const plGALBufferHandle hPhi = ctx.ResolveBuffer(res.m_hPressure[0]);

    for (plUInt32 i = 0; i < uiIterations; ++i)
    {
      for (plUInt32 uiColour = 0; uiColour < 2; ++uiColour)
      {
        plGpuFluidGasConstants sweep = base;
        sweep.GpuFluidGasPass = plVec4U32(0, uiColour, 0, 0);
        sweep.GpuFluidGasParams0 = plVec4(fOmega, 0.0f, 0.0f, 0.0f);
        DispatchPerBrick(ctx, grid, kernels.m_hSOR, sweep);
        UavBarrier(pEncoder, hPhi);
      }
    }
  }

  /// GasPoisson's residual or error into a probe word.
  void RecordProbe(const plRenderGraphContext& ctx, const plGpuFluidSparseGridGraph& grid, const PressureKernels& kernels, const FrameResources& res,
    const plGpuFluidGasConstants& base, plUInt32 uiMode, plUInt32 uiProbe, plUInt32 uiCategoryProbe = 0)
  {
    plGpuFluidGasConstants c = base;
    c.GpuFluidGasPass = plVec4U32(uiMode, 0, 0, 0);
    c.GpuFluidGasParams0 = plVec4(static_cast<float>(GPU_FLUID_STAT_PROBE + uiProbe), uiCategoryProbe > 0 ? static_cast<float>(GPU_FLUID_STAT_PROBE + uiCategoryProbe) : 0.0f, 0.0f, 0.0f);
    DispatchPerBrick(ctx, grid, kernels.m_hPoisson, c);
    UavBarrier(ctx.GetCommandEncoder(), ctx.ResolveBuffer(res.m_hStats));
  }
} // namespace

plGpuFluidGasSolver::plGpuFluidGasSolver() = default;

plGpuFluidGasSolver::~plGpuFluidGasSolver()
{
  PL_ASSERT_DEV(!IsCreated(), "The gas solver must be destroyed on the render thread before it goes away");
}

plUInt32 plGpuFluidGasSolver::ComputeDenseLevels(const plVec3U32& vBrickGrid, plUInt32* pOutCells, plUInt32* pOutTotalCells)
{
  // Mirrors GpuFluidDenseLevelCount in GpuFluidMG.hlsli.
  plVec3U32 vDims = vBrickGrid;
  plUInt32 uiCount = 0;
  plUInt32 uiTotal = 0;
  while (true)
  {
    const plUInt32 uiCells = vDims.x * vDims.y * vDims.z;
    if (pOutCells != nullptr)
      pOutCells[uiCount] = uiCells;
    uiTotal += uiCells;
    ++uiCount;

    if (uiCells <= GPU_FLUID_MG_COARSEST_CELLS || uiCount >= GPU_FLUID_MG_MAX_DENSE_LEVELS)
      break;
    vDims = plVec3U32((vDims.x + 1) / 2, (vDims.y + 1) / 2, (vDims.z + 1) / 2);
  }

  if (pOutTotalCells != nullptr)
    *pOutTotalCells = uiTotal;
  return uiCount;
}

plResult plGpuFluidGasSolver::Create(const plVec3U32& vBrickGrid, const plVec3U32& vAtlasBricks, plUInt32 uiCapacity)
{
  PL_ASSERT_DEV(!IsCreated(), "Create called twice");

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  m_vBrickGrid = vBrickGrid;
  plUInt32 uiDenseTotal = 0;
  m_uiDenseLevels = ComputeDenseLevels(vBrickGrid, m_uiDenseCells, &uiDenseTotal);

  m_hVelocity[0] = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Velocity 0");
  m_hVelocity[1] = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Velocity 1");
  m_hVelocity[2] = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Velocity 2");
  m_hScalar[0] = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Scalar 0");
  m_hScalar[1] = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Scalar 1");
  m_hScratch = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Scratch");
  m_hPressure[0] = CreateStructuredBuffer(pDevice, sizeof(float), uiCapacity * GPU_FLUID_BRICK_CELLS, "GpuFluid Gas Pressure");
  m_hPressure[1] = CreateStructuredBuffer(pDevice, sizeof(float), uiCapacity * GPU_FLUID_BRICK_CELLS, "GpuFluid Gas Pressure 1");
  m_hDivergence = CreateStructuredBuffer(pDevice, sizeof(float), uiCapacity * GPU_FLUID_BRICK_CELLS, "GpuFluid Gas Divergence");
  m_hSolidFlags = CreateStructuredBuffer(pDevice, sizeof(plUInt32), uiCapacity * GPU_FLUID_SOLID_FLAG_WORDS, "GpuFluid Gas SolidFlags");
  m_hFaceCodes = CreateStructuredBuffer(pDevice, sizeof(plUInt32), uiCapacity * GPU_FLUID_BRICK_CELLS, "GpuFluid Gas FaceCodes");
  m_hMGLocal = CreateStructuredBuffer(pDevice, sizeof(float), uiCapacity * GPU_FLUID_MG_LOCAL_STRIDE, "GpuFluid Gas MGLocal");
  m_hMGCoeff = CreateStructuredBuffer(pDevice, sizeof(plVec4), uiCapacity * GPU_FLUID_MG_COEFF_STRIDE, "GpuFluid Gas MGCoeff");
  m_hMGMask = CreateStructuredBuffer(pDevice, sizeof(plUInt32), uiCapacity * GPU_FLUID_MG_MASK_STRIDE, "GpuFluid Gas MGMask");
  m_hMGDense = CreateStructuredBuffer(pDevice, sizeof(plVec2), uiDenseTotal, "GpuFluid Gas MGDense");
  m_hMGDenseCoeff = CreateStructuredBuffer(pDevice, sizeof(plVec4), uiDenseTotal * 2, "GpuFluid Gas MGDenseCoeff");
  m_hSolidShapes = CreateStructuredBuffer(pDevice, sizeof(plGpuFluidSolidShape), plGpuFluidMaxGasTicksPerFrame * GPU_FLUID_MAX_SOLID_SHAPES, "GpuFluid Gas SolidShapes");

  plGALSamplerStateCreationDescription samplerDesc;
  samplerDesc.m_MinFilter = plGALTextureFilterMode::Linear;
  samplerDesc.m_MagFilter = plGALTextureFilterMode::Linear;
  samplerDesc.m_MipFilter = plGALTextureFilterMode::Point;
  samplerDesc.m_AddressU = plImageAddressMode::Clamp;
  samplerDesc.m_AddressV = plImageAddressMode::Clamp;
  samplerDesc.m_AddressW = plImageAddressMode::Clamp;
  m_hLinearClamp = pDevice->CreateSamplerState(samplerDesc);

  m_uiVelocitySide = 0;

  bool bFailed = m_hLinearClamp.IsInvalidated();
  for (plGALTextureHandle hTexture : {m_hVelocity[0], m_hVelocity[1], m_hVelocity[2], m_hScalar[0], m_hScalar[1], m_hScratch})
    bFailed = bFailed || hTexture.IsInvalidated();
  for (plGALBufferHandle hBuffer : {m_hPressure[0], m_hPressure[1], m_hDivergence, m_hSolidFlags, m_hFaceCodes, m_hMGLocal, m_hMGCoeff, m_hMGMask, m_hMGDense, m_hMGDenseCoeff, m_hSolidShapes})
    bFailed = bFailed || hBuffer.IsInvalidated();

  if (bFailed)
  {
    Destroy();
    return PL_FAILURE;
  }

  if (!m_hClearShader.IsValid())
  {
    m_hClearShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasClear.plShader");
    m_hEmitShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasEmit.plShader");
    m_hAdvectShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasAdvect.plShader");
    m_hHaloShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasHalo.plShader");
    m_hCurlShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasCurl.plShader");
    m_hForcesShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasForces.plShader");
    m_hSolidsShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasSolids.plShader");
    m_hDivergenceShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasDivergence.plShader");
    m_hPressureShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasPressure.plShader");
    m_hMGSetupShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasMGSetup.plShader");
    m_hMGFineShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasMGFine.plShader");
    m_hMGMidShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasMGMid.plShader");
    m_hMGDenseShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasMGDense.plShader");
    m_hPoissonShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasPoisson.plShader");
    m_hProjectShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasProject.plShader");
    m_hFinalizeShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasFinalize.plShader");
    m_hHashShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasHash.plShader");
  }

  return PL_SUCCESS;
}

void plGpuFluidGasSolver::Destroy()
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  for (plGALTextureHandle* pTexture : {&m_hVelocity[0], &m_hVelocity[1], &m_hVelocity[2], &m_hScalar[0], &m_hScalar[1], &m_hScratch})
  {
    if (!pTexture->IsInvalidated())
      pDevice->DestroyTexture(*pTexture);
    pTexture->Invalidate();
  }

  for (plGALBufferHandle* pBuffer : {&m_hPressure[0], &m_hPressure[1], &m_hDivergence, &m_hSolidFlags, &m_hFaceCodes, &m_hMGLocal, &m_hMGCoeff, &m_hMGMask, &m_hMGDense, &m_hMGDenseCoeff, &m_hSolidShapes,
         &m_hStateHash})
  {
    if (!pBuffer->IsInvalidated())
      pDevice->DestroyBuffer(*pBuffer);
    pBuffer->Invalidate();
  }
  m_HashReadback.Reset();

  if (!m_hLinearClamp.IsInvalidated())
    pDevice->DestroySamplerState(m_hLinearClamp);
  m_hLinearClamp.Invalidate();
}

void plGpuFluidGasSolver::AddClearPass(plRenderGraph& ref_graph, const plGpuFluidSparseGrid& grid, const plGpuFluidSparseGridGraph& gridGraph, const plVec3& vAmbientWind,
  bool bAllocationChanged, plBitflags<plGpuFluidFaces> openFaces)
{
  plGpuFluidGasSimulation sim;
  sim.m_OpenFaces = openFaces;
  plGpuFluidGasConstants constants = MakeConstants(grid, sim, 0.0f);
  constants.GpuFluidGasParams0 = plVec4(vAmbientWind.x, vAmbientWind.y, vAmbientWind.z, 0.0f);

  // The new bricks' aprons in the current sides, scalars and velocity in one launch.
  plGpuFluidGasConstants halo = constants;
  halo.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_SCALAR, GPU_FLUID_HALO_VELOCITY + 1, 0, 1);

  const plRenderGraphTextureHandle hVelocity0 = ref_graph.ImportTexture(m_hVelocity[0]);
  const plRenderGraphTextureHandle hVelocity1 = ref_graph.ImportTexture(m_hVelocity[1]);
  const plRenderGraphTextureHandle hVelocity2 = ref_graph.ImportTexture(m_hVelocity[2]);
  const plRenderGraphTextureHandle hVelocityCur = m_uiVelocitySide == 0 ? hVelocity0 : (m_uiVelocitySide == 1 ? hVelocity1 : hVelocity2);
  const plRenderGraphTextureHandle hScalar0 = ref_graph.ImportTexture(m_hScalar[0]);
  const plRenderGraphTextureHandle hScalar1 = ref_graph.ImportTexture(m_hScalar[1]);
  const plRenderGraphTextureHandle hScratch = ref_graph.ImportTexture(m_hScratch);
  const plRenderGraphBufferHandle hPressure = ref_graph.ImportBuffer(m_hPressure[0]);
  const plRenderGraphBufferHandle hPressure1 = ref_graph.ImportBuffer(m_hPressure[1]);
  const plRenderGraphBufferHandle hDivergence = ref_graph.ImportBuffer(m_hDivergence);
  const plRenderGraphBufferHandle hSolidFlags = ref_graph.ImportBuffer(m_hSolidFlags);
  const plRenderGraphBufferHandle hFaceCodes = ref_graph.ImportBuffer(m_hFaceCodes);
  const plRenderGraphBufferHandle hMGLocal = ref_graph.ImportBuffer(m_hMGLocal);
  const plRenderGraphBufferHandle hMGCoeff = ref_graph.ImportBuffer(m_hMGCoeff);

  auto pass = ref_graph.AddComputePass("Gas/Alloc Clear");
  plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
  pass.WriteTexture(hVelocity0);
  pass.WriteTexture(hVelocity1);
  pass.WriteTexture(hVelocity2);
  pass.WriteTexture(hScalar0);
  pass.WriteTexture(hScalar1);
  pass.WriteTexture(hScratch);
  pass.WriteBuffer(hPressure);
  pass.WriteBuffer(hPressure1);
  pass.WriteBuffer(hDivergence);
  pass.WriteBuffer(hSolidFlags);
  pass.WriteBuffer(hFaceCodes);
  pass.WriteBuffer(hMGLocal);
  pass.WriteBuffer(hMGCoeff);
  pass.HasSideEffects();
  pass.SetExecuteCallback([=, hShader = m_hClearShader, hHalo = m_hHaloShader](const plRenderGraphContext& ctx)
    {
      plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
      bindGroup.BindTexture("GpuFluidVelocity0", ctx.ResolveTexture(hVelocity0));
      bindGroup.BindTexture("GpuFluidVelocity1", ctx.ResolveTexture(hVelocity1));
      bindGroup.BindTexture("GpuFluidVelocity2", ctx.ResolveTexture(hVelocity2));
      bindGroup.BindTexture("GpuFluidScalar0", ctx.ResolveTexture(hScalar0));
      bindGroup.BindTexture("GpuFluidScalar1", ctx.ResolveTexture(hScalar1));
      bindGroup.BindTexture("GpuFluidScratch", ctx.ResolveTexture(hScratch));
      bindGroup.BindBuffer("GpuFluidPressure", ctx.ResolveBuffer(hPressure));
      bindGroup.BindBuffer("GpuFluidPressure1", ctx.ResolveBuffer(hPressure1));
      bindGroup.BindBuffer("GpuFluidDivergence", ctx.ResolveBuffer(hDivergence));
      bindGroup.BindBuffer("GpuFluidSolidFlags", ctx.ResolveBuffer(hSolidFlags));
      bindGroup.BindBuffer("GpuFluidFaceCodes", ctx.ResolveBuffer(hFaceCodes));
      bindGroup.BindBuffer("GpuFluidMGLocal", ctx.ResolveBuffer(hMGLocal));
      bindGroup.BindBuffer("GpuFluidMGCoeff", ctx.ResolveBuffer(hMGCoeff));
      DispatchPerNewBrick(ctx, gridGraph, hShader, constants);

      plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
      const plGALTextureHandle hScalarGAL = ctx.ResolveTexture(hScalar0);
      const plGALTextureHandle hVelocityGAL = ctx.ResolveTexture(hVelocityCur);
      UavBarrier(pEncoder, hScalarGAL);
      UavBarrier(pEncoder, hVelocityGAL);
      bindGroup.BindTexture("GpuFluidHaloTarget", hScalarGAL);
      bindGroup.BindTexture("GpuFluidHaloTarget2", hVelocityGAL);
      DispatchPerNewBrick(ctx, gridGraph, hHalo, halo);
    });

  // The clear zeroes the new bricks' face codes and coefficients.
  if (bAllocationChanged)
    m_bOperatorValid = false;
}

void plGpuFluidGasSolver::AddFramePasses(plRenderGraph& ref_graph, const plGpuFluidSparseGrid& grid, const plGpuFluidSparseGridGraph& gridGraph,
  plRenderGraphBufferHandle hStats, const plGpuFluidGasSimulation& sim)
{
  const bool bPoisson = sim.m_PoissonTest.m_bEnabled;
  if (sim.m_uiTicks == 0 && !bPoisson)
    return;

  FrameResources res;
  res.m_hVelocity[0] = ref_graph.ImportTexture(m_hVelocity[0]);
  res.m_hVelocity[1] = ref_graph.ImportTexture(m_hVelocity[1]);
  res.m_hVelocity[2] = ref_graph.ImportTexture(m_hVelocity[2]);
  res.m_hScalar[0] = ref_graph.ImportTexture(m_hScalar[0]);
  res.m_hScalar[1] = ref_graph.ImportTexture(m_hScalar[1]);
  res.m_hScratch = ref_graph.ImportTexture(m_hScratch);
  res.m_hPressure[0] = ref_graph.ImportBuffer(m_hPressure[0]);
  res.m_hPressure[1] = ref_graph.ImportBuffer(m_hPressure[1]);
  res.m_hDivergence = ref_graph.ImportBuffer(m_hDivergence);
  res.m_hSolidFlags = ref_graph.ImportBuffer(m_hSolidFlags);
  res.m_hFaceCodes = ref_graph.ImportBuffer(m_hFaceCodes);
  res.m_hMGLocal = ref_graph.ImportBuffer(m_hMGLocal);
  res.m_hMGCoeff = ref_graph.ImportBuffer(m_hMGCoeff);
  res.m_hMGMask = ref_graph.ImportBuffer(m_hMGMask);
  res.m_hMGDense = ref_graph.ImportBuffer(m_hMGDense);
  res.m_hMGDenseCoeff = ref_graph.ImportBuffer(m_hMGDenseCoeff);
  res.m_hSolidShapes = ref_graph.ImportBuffer(m_hSolidShapes);
  res.m_hStats = hStats;
  const plGALSamplerStateHandle hSampler = m_hLinearClamp;

  PressureKernels kernels;
  kernels.m_hSetup = m_hMGSetupShader;
  kernels.m_hDivergence = m_hDivergenceShader;
  kernels.m_hSOR = m_hPressureShader;
  kernels.m_hFine = m_hMGFineShader;
  kernels.m_hMid = m_hMGMidShader;
  kernels.m_hDense = m_hMGDenseShader;
  kernels.m_hPoisson = m_hPoissonShader;
  kernels.m_hProject = m_hProjectShader;
  kernels.m_hHalo = m_hHaloShader;

  DenseLevels dense;
  dense.m_uiCount = m_uiDenseLevels;
  for (plUInt32 k = 0; k < m_uiDenseLevels; ++k)
    dense.m_uiCells[k] = m_uiDenseCells[k];
  // The tail starts at the first level small enough for one group.
  while (dense.m_uiTailStart + 1 < dense.m_uiCount && dense.m_uiCells[dense.m_uiTailStart] > sim.m_Multigrid.m_uiDenseGroupCells)
    ++dense.m_uiTailStart;

  const plGpuFluidMultigridSettings mg = sim.m_Multigrid;
  const bool bMultigrid = sim.m_PressureSolver == plGpuFluidPressureSolver::Multigrid;
  const plVec4 vWind(sim.m_vAmbientWind.x, sim.m_vAmbientWind.y, sim.m_vAmbientWind.z, 0.0f);

  // The frame's solids, one upload for every tick. A copy is not allowed inside a compute pass.
  const plUInt32 uiSolidsPerTick = plMath::Min(sim.m_uiSolidsPerTick, static_cast<plUInt32>(GPU_FLUID_MAX_SOLID_SHAPES));
  if (uiSolidsPerTick > 0 && !sim.m_Solids.IsEmpty())
  {
    const plUInt32 uiCount = plMath::Min(sim.m_Solids.GetCount(), plGpuFluidMaxGasTicksPerFrame * static_cast<plUInt32>(GPU_FLUID_MAX_SOLID_SHAPES));
    plDynamicArray<plGpuFluidSolid> shapes;
    shapes.SetCountUninitialized(uiCount);
    plMemoryUtils::Copy(shapes.GetData(), sim.m_Solids.GetData(), uiCount);

    const plRenderGraphBufferHandle hShapes = res.m_hSolidShapes;
    auto pass = ref_graph.AddTransferPass("Gas/Solids Upload");
    pass.WriteBuffer(hShapes, plGALResourceState::CopyDestination);
    pass.HasSideEffects();
    pass.SetExecuteCallback([hShapes, shapes = std::move(shapes)](const plRenderGraphContext& ctx)
      { ctx.GetCommandEncoder()->UpdateBuffer(ctx.ResolveBuffer(hShapes), 0, shapes.GetArrayPtr().ToByteArray(), plGALUpdateMode::AheadOfTime); });
  }

  if (bPoisson)
  {
    // N3: one cold-start solve of the manufactured problem per frame; no gas moves.
    const plGpuFluidGasConstants base = MakeConstants(grid, sim, sim.m_fTime);
    const bool bProbeEach = sim.m_PoissonTest.m_bProbeEachCycle;
    const plUInt32 uiProbeInterval = plMath::Max(sim.m_PoissonTest.m_uiProbeInterval, 1u);
    const plUInt32 uiCycles = sim.m_uiPressureCycles;
    const plUInt32 uiIterations = sim.m_uiPressureIterations;
    const float fOmega = sim.m_fOverRelaxation;
    const plRenderGraphTextureHandle hVelocity = res.m_hVelocity[m_uiVelocitySide];

    {
      auto pass = ref_graph.AddComputePass("Gas/Poisson Setup");
      plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
      DeclarePressureBuffers(pass, res, hVelocity);
      pass.ReadBuffer(res.m_hSolidShapes, plGALResourceState::ComputeShaderRead);
      pass.WriteTexture(res.m_hScalar[0]);
      pass.HasSideEffects();
      pass.SetExecuteCallback([=, hSolids = m_hSolidsShader](const plRenderGraphContext& ctx)
        {
          BindPressureBuffers(ctx, res, hVelocity);
          plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
          bindGroup.BindBuffer("GpuFluidSolidShapes", ctx.ResolveBuffer(res.m_hSolidShapes));
          bindGroup.BindTexture("GpuFluidScalarTarget", ctx.ResolveTexture(res.m_hScalar[0]));

          // No shapes: the solid flags are cleared.
          DispatchPerBrick(ctx, gridGraph, hSolids, base);
          UavBarrier(ctx.GetCommandEncoder(), ctx.ResolveBuffer(res.m_hSolidFlags));
          RecordSetup(ctx, gridGraph, kernels, res, base, mg, dense, bMultigrid);

          plGpuFluidGasConstants setup = base;
          setup.GpuFluidGasPass = plVec4U32(GPU_FLUID_POISSON_SETUP, 0, 0, 0);
          DispatchPerBrick(ctx, gridGraph, kernels.m_hPoisson, setup);
          UavBarrier(ctx.GetCommandEncoder(), ctx.ResolveBuffer(res.m_hPressure[0]));
          UavBarrier(ctx.GetCommandEncoder(), ctx.ResolveBuffer(res.m_hDivergence));

          RecordProbe(ctx, gridGraph, kernels, res, base, GPU_FLUID_POISSON_RESIDUAL, 0);
          RecordProbe(ctx, gridGraph, kernels, res, base, GPU_FLUID_POISSON_ERROR, plGpuFluidProbeInitialError);
        });
    }

    if (bMultigrid && sim.m_PoissonTest.m_bProfileStages)
    {
      for (plUInt32 c = 0; c < uiCycles; ++c)
      {
        for (plUInt32 uiStage = 0; uiStage < 5; ++uiStage)
        {
          auto pass = ref_graph.AddComputePass(s_szStageNames[uiStage]);
          plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
          DeclarePressureBuffers(pass, res, hVelocity);
          pass.HasSideEffects();
          pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
            {
              BindPressureBuffers(ctx, res, hVelocity);
              RecordVCycle(ctx, gridGraph, kernels, res, base, mg, dense, PL_BIT(uiStage), c > 0 && mg.m_bShortLaterCycles);
            });
        }
      }
    }
    else
    {
      auto pass = ref_graph.AddComputePass("Gas/Pressure");
      plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
      DeclarePressureBuffers(pass, res, hVelocity);
      pass.HasSideEffects();
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          BindPressureBuffers(ctx, res, hVelocity);
          if (bMultigrid)
          {
            for (plUInt32 c = 0; c < uiCycles; ++c)
            {
              RecordMultigridCycle(ctx, gridGraph, kernels, res, base, mg, dense, c);
              if (bProbeEach && c + 1 < plGpuFluidProbeFinalResidual)
                RecordProbe(ctx, gridGraph, kernels, res, base, GPU_FLUID_POISSON_RESIDUAL, c + 1);
            }
          }
          else
          {
            for (plUInt32 i = 0; i < uiIterations; i += uiProbeInterval)
            {
              const plUInt32 uiChunk = plMath::Min(uiProbeInterval, uiIterations - i);
              RecordSOR(ctx, gridGraph, kernels, res, base, fOmega, uiChunk);
              const plUInt32 uiProbe = (i + uiChunk) / uiProbeInterval;
              if (bProbeEach && uiProbe < plGpuFluidProbeFinalResidual)
                RecordProbe(ctx, gridGraph, kernels, res, base, GPU_FLUID_POISSON_RESIDUAL, uiProbe);
            }
          }
        });
    }

    {
      auto pass = ref_graph.AddComputePass("Gas/Poisson Probe");
      plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
      DeclarePressureBuffers(pass, res, hVelocity);
      pass.HasSideEffects();
      pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
        {
          BindPressureBuffers(ctx, res, hVelocity);
          // With few enough cycles the probe words 9..11 are free: the final residual by where the cells are.
          RecordProbe(ctx, gridGraph, kernels, res, base, GPU_FLUID_POISSON_RESIDUAL, plGpuFluidProbeFinalResidual, (bMultigrid ? uiCycles : uiIterations / uiProbeInterval) < 9 ? 9 : 0);
          RecordProbe(ctx, gridGraph, kernels, res, base, GPU_FLUID_POISSON_ERROR, plGpuFluidProbeFinalError);
        });
    }
  }

  for (plUInt32 uiTick = 0; uiTick < (bPoisson ? 0u : sim.m_uiTicks); ++uiTick)
  {
    const bool bLastTick = uiTick + 1 == sim.m_uiTicks;
    const plGpuFluidGasConstants base = MakeConstants(grid, sim, sim.m_fTime + uiTick * sim.m_fTickSeconds);

    // The sides of this tick. Velocity: current -> semi-Lagrangian -> MacCormack (the next current; without the
    // velocity correction the semi-Lagrangian side is it). Scalars: Scalar[0] (current, the march reads it) ->
    // semi-Lagrangian Scalar[1] -> MacCormack into Scratch; GasForces writes them back to Scalar[0]. The curl takes
    // whichever of Scalar[1] and Scratch the scalars are done with.
    const bool bMacCormack = sim.m_bMacCormack;
    const bool bMacCormackVelocity = sim.m_bMacCormackVelocity;
    const plUInt32 uiVelocitySL = (m_uiVelocitySide + 1) % 3;
    const plUInt32 uiVelocitySpare = (m_uiVelocitySide + 2) % 3;
    const plUInt32 uiVelocityNext = bMacCormackVelocity ? uiVelocitySpare : uiVelocitySL;

    const plRenderGraphTextureHandle hVelocityCur = res.m_hVelocity[m_uiVelocitySide];
    const plRenderGraphTextureHandle hVelocitySL = res.m_hVelocity[uiVelocitySL];
    const plRenderGraphTextureHandle hVelocitySpare = res.m_hVelocity[uiVelocitySpare];
    const plRenderGraphTextureHandle hVelocityNext = res.m_hVelocity[uiVelocityNext];
    const plRenderGraphTextureHandle hScalar0 = res.m_hScalar[0];
    const plRenderGraphTextureHandle hScalar1 = res.m_hScalar[1];
    const plRenderGraphTextureHandle hScratch = res.m_hScratch;
    const plRenderGraphTextureHandle hScalarAdvected = bMacCormack ? hScratch : hScalar1;
    const plRenderGraphTextureHandle hCurlTarget = bMacCormack ? hScalar1 : hScratch;

    // Gas/Emit: each source in place on the current scalars and velocity.
    {
      plHybridArray<plGpuFluidGasConstants, 4> emitters;
      for (const plGpuFluidGasEmitter& emitter : sim.m_Emitters)
      {
        if (emitter.m_Shape == plGpuFluidEmitterShape::None)
          continue;

        plGpuFluidGasConstants& c = emitters.ExpandAndGetRef();
        c = base;
        const plUInt32 uiShape = emitter.m_Shape == plGpuFluidEmitterShape::Box ? GPU_FLUID_EMITTER_BOX : GPU_FLUID_EMITTER_SPHERE;
        // Test hook: the first source of the frame's first tick writes one NaN (GpuFluid.Debug.InjectNaN).
        const bool bInjectNaN = sim.m_bInjectNaN && uiTick == 0 && emitters.GetCount() == 1;
        c.GpuFluidGasPass = plVec4U32(0, uiShape, 0, (emitter.m_bDriveVelocity ? 1u : 0u) | (bInjectNaN ? 2u : 0u));
        c.GpuFluidGasParams0 = plVec4(emitter.m_vCenterVoxels.x, emitter.m_vCenterVoxels.y, emitter.m_vCenterVoxels.z, emitter.m_fRadiusVoxels);
        c.GpuFluidGasParams1 = plVec4(emitter.m_vHalfExtentsVoxels.x, emitter.m_vHalfExtentsVoxels.y, emitter.m_vHalfExtentsVoxels.z, emitter.m_fSoftnessVoxels);
        c.GpuFluidGasParams2 = plVec4(emitter.m_fDensityRate, emitter.m_fTemperature, emitter.m_fFuelRate, emitter.m_fNoiseStrength);
        c.GpuFluidGasParams3 = plVec4(emitter.m_vVelocity.x, emitter.m_vVelocity.y, emitter.m_vVelocity.z, emitter.m_fNoiseFrequency);
      }

      auto pass = ref_graph.AddComputePass("Gas/Emit");
      plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
      pass.WriteTexture(hScalar0);
      pass.WriteTexture(hVelocityCur);
      pass.WriteBuffer(hStats);
      pass.HasSideEffects();
      pass.SetExecuteCallback([=, hShader = m_hEmitShader](const plRenderGraphContext& ctx)
        {
          plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
          const plGALTextureHandle hScalarGAL = ctx.ResolveTexture(hScalar0);
          const plGALTextureHandle hVelocityGAL = ctx.ResolveTexture(hVelocityCur);

          plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
          bindGroup.BindTexture("GpuFluidScalarTarget", hScalarGAL);
          bindGroup.BindTexture("GpuFluidVelocityTarget", hVelocityGAL);
          bindGroup.BindBuffer("GpuFluidStats", ctx.ResolveBuffer(hStats));

          for (plUInt32 i = 0; i < emitters.GetCount(); ++i)
          {
            if (i > 0)
            {
              UavBarrier(pEncoder, hScalarGAL);
              UavBarrier(pEncoder, hVelocityGAL);
            }
            DispatchPerBrick(ctx, gridGraph, hShader, emitters[i]);
          }
        });
    }

    // Gas/Advect: current -> semi-Lagrangian sides and their aprons; with MacCormack the correction (reverse trace and
    // limiter in one launch) into the third sides and their aprons. Each launch does the scalars and the velocity
    // together.
    {
      const plUInt32 uiCorrected = (bMacCormack ? GPU_FLUID_ADVECT_FIELD_SCALARS : 0u) | (bMacCormackVelocity ? GPU_FLUID_ADVECT_FIELD_VELOCITY : 0u);
      const plUInt32 uiSampling = sim.m_bExactSampling ? GPU_FLUID_PASS_EXACT_SAMPLING : 0u;
      // The launch that writes the final scalars sums the mass for the closed-box correction.
      const plUInt32 uiMassSL = sim.m_bMassCorrection && !bMacCormack ? GPU_FLUID_PASS_MASS_SUMS : 0u;
      const plUInt32 uiMassMC = sim.m_bMassCorrection && bMacCormack ? GPU_FLUID_PASS_MASS_SUMS : 0u;

      plGpuFluidGasConstants semiLagrangian = base;
      semiLagrangian.GpuFluidGasPass = plVec4U32(GPU_FLUID_ADVECT_SEMI_LAGRANGIAN, uiTick, 0, GPU_FLUID_ADVECT_FIELD_SCALARS | GPU_FLUID_ADVECT_FIELD_VELOCITY | uiSampling | uiMassSL);
      semiLagrangian.GpuFluidGasParams0 = vWind;

      plGpuFluidGasConstants macCormack = base;
      macCormack.GpuFluidGasPass = plVec4U32(GPU_FLUID_ADVECT_MACCORMACK, uiTick, 0, uiCorrected | uiSampling | uiMassMC);
      macCormack.GpuFluidGasParams0 = vWind;

      // Both semi-Lagrangian sides' aprons in one launch.
      plGpuFluidGasConstants haloSL = base;
      haloSL.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_SCALAR, GPU_FLUID_HALO_VELOCITY + 1, 0, 0);
      haloSL.GpuFluidGasParams0 = vWind;

      // The corrected sides' aprons: target 1 the scalars (when corrected), target 2 the velocity (when corrected).
      plGpuFluidGasConstants haloMC = base;
      if (bMacCormack)
        haloMC.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_SCALAR, bMacCormackVelocity ? GPU_FLUID_HALO_VELOCITY + 1 : 0u, 0, 0);
      else
        haloMC.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_VELOCITY, 0, 0, 0);
      haloMC.GpuFluidGasParams0 = vWind;

      // Steps of the phase, one pass normally; with GpuFluid.Gas.ProfileKernels each in a pass of its own.
      enum AdvectStep : plUInt32
      {
        SemiLagrangian,
        HaloSL,
        MacCormack,
        HaloMC,
        AdvectStepCount
      };
      static const char* s_szAdvectSteps[] = {"Gas/Advect SL", "Gas/Advect Halo", "Gas/Advect MacCormack", "Gas/Advect Halo 2"};

      plHybridArray<plUInt32, AdvectStepCount> steps;
      steps.PushBack(SemiLagrangian);
      steps.PushBack(HaloSL);
      if (uiCorrected != 0)
      {
        steps.PushBack(MacCormack);
        steps.PushBack(HaloMC);
      }

      auto RecordAdvect = [=, hAdvect = m_hAdvectShader, hHalo = m_hHaloShader](const plRenderGraphContext& ctx, plUInt32 uiFirst, plUInt32 uiEnd)
      {
        plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
        const plGALTextureHandle hScalarSLGAL = ctx.ResolveTexture(hScalar1);
        const plGALTextureHandle hVelocitySLGAL = ctx.ResolveTexture(hVelocitySL);
        const plGALTextureHandle hScratchGAL = ctx.ResolveTexture(hScratch);
        const plGALTextureHandle hSpareGAL = ctx.ResolveTexture(hVelocitySpare);

        plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
        bindGroup.BindTexture("GpuFluidVelocityIn", ctx.ResolveTexture(hVelocityCur));
        bindGroup.BindTexture("GpuFluidScalarIn", ctx.ResolveTexture(hScalar0));
        bindGroup.BindSampler("GpuFluidLinearClamp", hSampler);
        bindGroup.BindBuffer("GpuFluidStats", ctx.ResolveBuffer(hStats));

        for (plUInt32 i = uiFirst; i < uiEnd; ++i)
        {
          if (i > uiFirst)
          {
            UavBarrier(pEncoder, hScalarSLGAL);
            UavBarrier(pEncoder, hVelocitySLGAL);
            UavBarrier(pEncoder, hScratchGAL);
            UavBarrier(pEncoder, hSpareGAL);
          }

          switch (steps[i])
          {
            case SemiLagrangian:
              bindGroup.BindTexture("GpuFluidVelocitySL", ctx.ResolveTexture(hVelocityCur)); // not read in this mode
              bindGroup.BindTexture("GpuFluidScalarSL", ctx.ResolveTexture(hScalar0));
              bindGroup.BindTexture("GpuFluidVelocityOut", hVelocitySLGAL);
              bindGroup.BindTexture("GpuFluidScalarOut", hScalarSLGAL);
              DispatchPerBrickSlabs(ctx, gridGraph, hAdvect, semiLagrangian);
              break;
            case HaloSL:
              bindGroup.BindTexture("GpuFluidHaloTarget", hScalarSLGAL);
              bindGroup.BindTexture("GpuFluidHaloTarget2", hVelocitySLGAL);
              DispatchPerBrick(ctx, gridGraph, hHalo, haloSL);
              break;
            case MacCormack:
              // The semi-Lagrangian sides are read through read-only views (the pass holds them for writing); the
              // outputs are the third sides, or the spare one standing in for a field that is not corrected.
              pEncoder->TextureBarrier(hScalarSLGAL, plGALResourceState::UnorderedAccessWrite, plGALResourceState::ComputeShaderRead);
              pEncoder->TextureBarrier(hVelocitySLGAL, plGALResourceState::UnorderedAccessWrite, plGALResourceState::ComputeShaderRead);
              bindGroup.BindTexture("GpuFluidVelocitySL", hVelocitySLGAL);
              bindGroup.BindTexture("GpuFluidScalarSL", hScalarSLGAL);
              bindGroup.BindTexture("GpuFluidVelocityOut", hSpareGAL);
              bindGroup.BindTexture("GpuFluidScalarOut", hScratchGAL);
              DispatchPerBrickSlabs(ctx, gridGraph, hAdvect, macCormack);
              pEncoder->TextureBarrier(hScalarSLGAL, plGALResourceState::ComputeShaderRead, plGALResourceState::UnorderedAccessWrite);
              pEncoder->TextureBarrier(hVelocitySLGAL, plGALResourceState::ComputeShaderRead, plGALResourceState::UnorderedAccessWrite);
              break;
            case HaloMC:
              bindGroup.BindTexture("GpuFluidHaloTarget", bMacCormack ? hScratchGAL : hSpareGAL);
              bindGroup.BindTexture("GpuFluidHaloTarget2", hSpareGAL);
              DispatchPerBrick(ctx, gridGraph, hHalo, haloMC);
              break;
          }
        }
      };

      const bool bProfile = sim.m_bProfileKernels;
      for (plUInt32 uiPass = 0; uiPass < (bProfile ? steps.GetCount() : 1u); ++uiPass)
      {
        auto pass = ref_graph.AddComputePass(bProfile ? s_szAdvectSteps[steps[uiPass]] : "Gas/Advect");
        plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
        pass.ReadTexture(hVelocityCur, {}, plGALResourceState::ComputeShaderRead);
        pass.ReadTexture(hScalar0, {}, plGALResourceState::ComputeShaderRead);
        pass.WriteTexture(hVelocitySL);
        pass.WriteTexture(hVelocitySpare);
        pass.WriteTexture(hScalar1);
        pass.WriteTexture(hScratch);
        pass.WriteBuffer(hStats);
        pass.HasSideEffects();
        const plUInt32 uiFirst = bProfile ? uiPass : 0u;
        const plUInt32 uiEnd = bProfile ? uiPass + 1 : steps.GetCount();
        pass.SetExecuteCallback([=](const plRenderGraphContext& ctx) { RecordAdvect(ctx, uiFirst, uiEnd); });
      }
    }

    // Gas/Forces: the curl, buoyancy and confinement on the next velocity; cooled, dissipated, clamped scalars back to
    // the current side; then the solids.
    {
      plGpuFluidGasConstants forces = base;
      forces.GpuFluidGasParams0 = plVec4(sim.m_vUp.x, sim.m_vUp.y, sim.m_vUp.z, sim.m_fBuoyancy);
      forces.GpuFluidGasParams1 = plVec4(sim.m_fDensityWeight, sim.m_fAmbientTemperature, sim.m_fCooling, sim.m_fDissipation);
      forces.GpuFluidGasParams2 = plVec4(sim.m_fDensityMax, sim.m_fVelocityDissipation, sim.m_fFuelDissipation, sim.m_fEdgeFadeVoxels);
      forces.GpuFluidGasParams3 = plVec4(sim.m_fEdgeFadeRate, sim.m_fVorticity, sim.m_fVorticityRate, sim.m_bMassCorrection ? 1.0f : 0.0f);
      forces.GpuFluidGasPass.y = uiTick;

      plGpuFluidGasConstants velocityHalo = base;
      velocityHalo.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_VELOCITY, 0, 0, 0);
      velocityHalo.GpuFluidGasParams0 = vWind;

      plGpuFluidGasConstants copyHalo = base;
      copyHalo.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_COPY, 0, 0, 0);

      plGpuFluidGasConstants solids = base;
      solids.GpuFluidGasPass = plVec4U32(0, 0, bLastTick ? 1u : 0u, 0);
      solids.GpuFluidGasParams0 = plVec4(static_cast<float>(uiSolidsPerTick), static_cast<float>(uiTick * uiSolidsPerTick), 0.0f, 0.0f);

      const bool bVorticity = sim.m_fVorticity > 0.0f;

      enum ForcesStep : plUInt32
      {
        Curl,
        CurlHalo,
        Forces,
        Solids,
        VelocityHalo,
        ForcesStepCount
      };
      static const char* s_szForcesSteps[] = {"Gas/Forces Curl", "Gas/Forces Curl Apron", "Gas/Forces Forces", "Gas/Forces Solids", "Gas/Forces Halo"};

      // The advection left the next velocity's apron filled, which the curl needs.
      plHybridArray<plUInt32, ForcesStepCount> steps;
      if (bVorticity)
      {
        steps.PushBack(Curl);
        steps.PushBack(CurlHalo);
      }
      steps.PushBack(Forces);
      steps.PushBack(Solids);
      steps.PushBack(VelocityHalo);

      auto RecordForces = [=, hForces = m_hForcesShader, hHalo = m_hHaloShader, hCurl = m_hCurlShader, hSolids = m_hSolidsShader](const plRenderGraphContext& ctx, plUInt32 uiFirst, plUInt32 uiEnd)
      {
        plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
        const plGALTextureHandle hVelocityGAL = ctx.ResolveTexture(hVelocityNext);
        const plGALTextureHandle hCurlGAL = ctx.ResolveTexture(hCurlTarget);
        const plGALTextureHandle hScalarGAL = ctx.ResolveTexture(hScalar0);

        plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
        bindGroup.BindTexture("GpuFluidScalarIn", ctx.ResolveTexture(hScalarAdvected));
        bindGroup.BindTexture("GpuFluidScalarOut", hScalarGAL);
        bindGroup.BindTexture("GpuFluidScalarTarget", hScalarGAL);
        bindGroup.BindTexture("GpuFluidVelocityTarget", hVelocityGAL);
        bindGroup.BindTexture("GpuFluidScratch", hCurlGAL);
        bindGroup.BindBuffer("GpuFluidStats", ctx.ResolveBuffer(hStats));
        bindGroup.BindBuffer("GpuFluidSolidFlags", ctx.ResolveBuffer(res.m_hSolidFlags));
        bindGroup.BindBuffer("GpuFluidSolidShapes", ctx.ResolveBuffer(res.m_hSolidShapes));

        for (plUInt32 i = uiFirst; i < uiEnd; ++i)
        {
          if (i > uiFirst)
          {
            UavBarrier(pEncoder, hVelocityGAL);
            UavBarrier(pEncoder, hCurlGAL);
            UavBarrier(pEncoder, hScalarGAL);
          }

          switch (steps[i])
          {
            case Curl:
              // Read through a read-only view: the pass holds the velocity for writing, so it changes state around it.
              pEncoder->TextureBarrier(hVelocityGAL, plGALResourceState::UnorderedAccessWrite, plGALResourceState::ComputeShaderRead);
              bindGroup.BindTexture("GpuFluidVelocityIn", hVelocityGAL);
              DispatchPerBrickSlabs(ctx, gridGraph, hCurl, base);
              pEncoder->TextureBarrier(hVelocityGAL, plGALResourceState::ComputeShaderRead, plGALResourceState::UnorderedAccessWrite);
              break;
            case CurlHalo:
              bindGroup.BindTexture("GpuFluidHaloTarget", hCurlGAL);
              bindGroup.BindTexture("GpuFluidHaloTarget2", hCurlGAL);
              DispatchPerBrick(ctx, gridGraph, hHalo, copyHalo);
              break;
            case Forces:
              // The curl through a read-only view as well.
              pEncoder->TextureBarrier(hCurlGAL, plGALResourceState::UnorderedAccessWrite, plGALResourceState::ComputeShaderRead);
              bindGroup.BindTexture("GpuFluidCurl", hCurlGAL);
              DispatchPerBrick(ctx, gridGraph, hForces, forces);
              pEncoder->TextureBarrier(hCurlGAL, plGALResourceState::ComputeShaderRead, plGALResourceState::UnorderedAccessWrite);
              break;
            case Solids:
              DispatchPerBrick(ctx, gridGraph, hSolids, solids);
              break;
            case VelocityHalo:
              bindGroup.BindTexture("GpuFluidHaloTarget", hVelocityGAL);
              bindGroup.BindTexture("GpuFluidHaloTarget2", hVelocityGAL);
              DispatchPerBrick(ctx, gridGraph, hHalo, velocityHalo);
              break;
          }
        }
      };

      const bool bProfile = sim.m_bProfileKernels;
      for (plUInt32 uiPass = 0; uiPass < (bProfile ? steps.GetCount() : 1u); ++uiPass)
      {
        auto pass = ref_graph.AddComputePass(bProfile ? s_szForcesSteps[steps[uiPass]] : "Gas/Forces");
        plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
        pass.ReadTexture(hScalarAdvected, {}, plGALResourceState::ComputeShaderRead);
        pass.ReadBuffer(res.m_hSolidShapes, plGALResourceState::ComputeShaderRead);
        pass.WriteTexture(hScalar0);
        pass.WriteTexture(hVelocityNext);
        pass.WriteTexture(hCurlTarget);
        pass.WriteBuffer(res.m_hSolidFlags);
        pass.WriteBuffer(hStats);
        pass.HasSideEffects();
        const plUInt32 uiFirst = bProfile ? uiPass : 0u;
        const plUInt32 uiEnd = bProfile ? uiPass + 1 : steps.GetCount();
        pass.SetExecuteCallback([=](const plRenderGraphContext& ctx) { RecordForces(ctx, uiFirst, uiEnd); });
      }
    }

    // Gas/Pressure: the operator, divergence, the solver, projection, the velocity's apron.
    {
      plGpuFluidGasConstants divergence = base;
      divergence.GpuFluidGasPass = plVec4U32(0, 0, bLastTick ? 1u : 0u, 0);

      plGpuFluidGasConstants keepHalo = base;
      keepHalo.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_VELOCITY_KEEP, 0, 0, 0);
      keepHalo.GpuFluidGasParams0 = vWind;

      const plUInt32 uiIterations = sim.m_uiPressureIterations;
      const plUInt32 uiCycles = sim.m_uiPressureCycles;
      const float fOmega = sim.m_fOverRelaxation;

      // The operator depends only on the solid flags, the allocation and the open faces: without solids now or on the
      // last tick, and nothing else changed, last tick's is still right.
      const plUInt8 uiFaces = sim.m_OpenFaces.GetValue();
      const bool bSetup = !m_bOperatorValid || uiSolidsPerTick > 0 || m_bOperatorHadSolids || m_uiOperatorFaces != uiFaces || (bMultigrid && !m_bOperatorDense);
      m_bOperatorValid = true;
      m_bOperatorHadSolids = uiSolidsPerTick > 0;
      m_uiOperatorFaces = uiFaces;
      m_bOperatorDense = bMultigrid || (m_bOperatorDense && !bSetup);

      // Steps of the phase: 0 operator, 1 + c the solve's cycle c (or the divergence and SOR), then the projection. One
      // pass normally; with GpuFluid.Gas.ProfilePressure each step in a pass of its own, so each is timed.
      const bool bSolveMG = bMultigrid && uiCycles > 0;
      const plUInt32 uiSolveSteps = bSolveMG ? uiCycles : 1;
      auto RecordStep = [=](const plRenderGraphContext& ctx, plUInt32 uiStep)
      {
        plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
        if (uiStep == 0)
        {
          if (bSetup)
            RecordSetup(ctx, gridGraph, kernels, res, base, mg, dense, bMultigrid);
        }
        else if (uiStep <= uiSolveSteps)
        {
          if (bSolveMG)
          {
            // The first cycle's first launches take the divergence.
            RecordMultigridCycle(ctx, gridGraph, kernels, res, base, mg, dense, uiStep - 1, bLastTick ? 2u : 1u);
          }
          else
          {
            DispatchPerBrick(ctx, gridGraph, kernels.m_hDivergence, divergence);
            UavBarrier(pEncoder, ctx.ResolveBuffer(res.m_hDivergence));
            RecordSOR(ctx, gridGraph, kernels, res, base, fOmega, bMultigrid ? 0u : uiIterations);
          }
        }
        else
        {
          DispatchPerBrick(ctx, gridGraph, kernels.m_hProject, base);
          UavBarrier(pEncoder, ctx.ResolveTexture(hVelocityNext));
          DispatchPerBrick(ctx, gridGraph, kernels.m_hHalo, keepHalo);
        }
      };

      const plUInt32 uiSteps = uiSolveSteps + 2;
      const bool bProfile = sim.m_Multigrid.m_bProfilePressure;
      for (plUInt32 uiPass = 0; uiPass < (bProfile ? uiSteps : 1u); ++uiPass)
      {
        plStringBuilder sName = "Gas/Pressure";
        if (bProfile)
        {
          if (uiPass == 0)
            sName = "Gas/Pressure Operator";
          else if (uiPass + 1 == uiSteps)
            sName = "Gas/Pressure Project";
          else
            sName.SetFormat("Gas/Pressure Solve {}", uiPass);
        }

        auto pass = ref_graph.AddComputePass(sName);
        plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
        DeclarePressureBuffers(pass, res, hVelocityNext);
        pass.HasSideEffects();
        pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
          {
            BindPressureBuffers(ctx, res, hVelocityNext);
            for (plUInt32 uiStep = (bProfile ? uiPass : 0u); uiStep < (bProfile ? uiPass + 1 : uiSteps); ++uiStep)
              RecordStep(ctx, uiStep);
          });
      }
    }

    m_uiVelocitySide = uiVelocityNext;
  }

  // Gas/Finalize, once per frame.
  {
    const plRenderGraphTextureHandle hVelocityCur = res.m_hVelocity[m_uiVelocitySide];
    const plRenderGraphTextureHandle hScalar0 = res.m_hScalar[0];
    const plRenderGraphBufferHandle hFaceCodes = res.m_hFaceCodes;

    plGpuFluidGasConstants finalize = MakeConstants(grid, sim, sim.m_fTime + sim.m_uiTicks * sim.m_fTickSeconds);
    finalize.GpuFluidGasParams0 = plVec4(sim.m_fAllocThreshold, sim.m_fMeanFloor, sim.m_fAllocLookahead, static_cast<float>(grid.GetRequestWords()));

    auto pass = ref_graph.AddComputePass("Gas/Finalize");
    plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
    pass.WriteTexture(hScalar0);
    pass.WriteTexture(hVelocityCur);
    pass.WriteTexture(gridGraph.m_hIndirection);
    pass.WriteBuffer(hStats);
    pass.WriteBuffer(hFaceCodes);
    pass.WriteBuffer(gridGraph.m_hRequests);
    pass.WriteBuffer(gridGraph.m_hBrickMeta);
    pass.HasSideEffects();
    pass.SetExecuteCallback([=, hShader = m_hFinalizeShader](const plRenderGraphContext& ctx)
      {
        plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
        bindGroup.BindTexture("GpuFluidScalarTarget", ctx.ResolveTexture(hScalar0));
        bindGroup.BindTexture("GpuFluidVelocityTarget", ctx.ResolveTexture(hVelocityCur));
        bindGroup.BindTexture("GpuFluidIndirectionOutput", ctx.ResolveTexture(gridGraph.m_hIndirection));
        bindGroup.BindBuffer("GpuFluidStats", ctx.ResolveBuffer(hStats));
        bindGroup.BindBuffer("GpuFluidFaceCodes", ctx.ResolveBuffer(hFaceCodes));
        bindGroup.BindBuffer("GpuFluidRequests", ctx.ResolveBuffer(gridGraph.m_hRequests));
        bindGroup.BindBuffer("GpuFluidBrickMeta", ctx.ResolveBuffer(gridGraph.m_hBrickMeta));
        DispatchPerBrick(ctx, gridGraph, hShader, finalize);
      });
  }
}

void plGpuFluidGasSolver::AddHashPasses(plRenderGraph& ref_graph, const plGpuFluidSparseGrid& grid, const plGpuFluidSparseGridGraph& gridGraph, plUInt64 uiFrame)
{
  const plVec3U32 vGrid = grid.GetBrickGrid();
  const plUInt32 uiVirtualBricks = vGrid.x * vGrid.y * vGrid.z;

  if (m_hStateHash.IsInvalidated())
  {
    m_hStateHash = CreateStructuredBuffer(plGALDevice::GetDefaultDevice(), sizeof(plUInt32), uiVirtualBricks * 2, "GpuFluid Gas StateHash");
    if (m_hStateHash.IsInvalidated())
      return;
  }

  const plUInt32 uiSlot = m_HashReadback.ReserveSlot(uiFrame);
  if (uiSlot == plInvalidIndex)
    return;

  plGpuFluidGasSimulation sim;
  plGpuFluidGasConstants clear = MakeConstants(grid, sim, 0.0f);
  plGpuFluidGasConstants hash = clear;
  clear.GpuFluidGasPass = plVec4U32(0, 0, 0, 0);
  hash.GpuFluidGasPass = plVec4U32(1, 0, 0, 0);

  const plRenderGraphBufferHandle hHash = ref_graph.ImportBuffer(m_hStateHash);
  const plRenderGraphTextureHandle hScalar = ref_graph.ImportTexture(m_hScalar[0]);
  const plRenderGraphTextureHandle hVelocity = ref_graph.ImportTexture(m_hVelocity[m_uiVelocitySide]);
  const plUInt32 uiClearGroups = (uiVirtualBricks + GPU_FLUID_BRICK_CELLS - 1) / GPU_FLUID_BRICK_CELLS;

  {
    auto pass = ref_graph.AddComputePass("Gas/Hash");
    plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
    pass.WriteBuffer(hHash);
    pass.WriteTexture(hScalar);
    pass.WriteTexture(hVelocity);
    pass.HasSideEffects();
    pass.SetExecuteCallback([=, hShader = m_hHashShader](const plRenderGraphContext& ctx)
      {
        plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
        bindGroup.BindBuffer("GpuFluidStateHash", ctx.ResolveBuffer(hHash));
        bindGroup.BindTexture("GpuFluidScalarTarget", ctx.ResolveTexture(hScalar));
        bindGroup.BindTexture("GpuFluidVelocityTarget", ctx.ResolveTexture(hVelocity));
        DispatchGroups(ctx, gridGraph, hShader, clear, uiClearGroups);
        UavBarrier(ctx.GetCommandEncoder(), ctx.ResolveBuffer(hHash));
        DispatchPerBrick(ctx, gridGraph, hShader, hash);
      });
  }

  {
    plGpuFluidReadbackRing* pRing = &m_HashReadback;
    const plGALBufferHandle hHashGAL = m_hStateHash;
    auto pass = ref_graph.AddTransferPass("Gas/Hash Readback");
    pass.ReadBuffer(hHash, plGALResourceState::CopySource);
    pass.HasSideEffects();
    pass.SetExecuteCallback([pRing, uiSlot, hHashGAL](const plRenderGraphContext& ctx) { pRing->Submit(uiSlot, *ctx.GetCommandEncoder(), hHashGAL); });
  }
}

bool plGpuFluidGasSolver::PollStateHash(plUInt64& out_uiHash, plUInt64& out_uiFrame)
{
  if (!m_HashReadback.Poll(m_HashScratch, out_uiFrame))
    return false;

  plUInt64 uiHash = 0xCBF29CE484222325ull;
  for (plUInt8 uiByte : m_HashScratch)
  {
    uiHash ^= uiByte;
    uiHash *= 0x100000001B3ull;
  }
  out_uiHash = uiHash;
  return true;
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidGasSolver);
