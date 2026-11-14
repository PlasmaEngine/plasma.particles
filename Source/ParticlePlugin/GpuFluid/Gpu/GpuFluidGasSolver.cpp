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

namespace
{
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

  plGALBufferHandle CreateFloatBuffer(plGALDevice* pDevice, plUInt32 uiCount, const char* szName)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(float);
    desc.m_uiTotalSize = plMath::Max(uiCount, 1u) * sizeof(float);
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
    pContext->DispatchIndirect(ctx.ResolveBuffer(grid.m_hDispatchArgs), 0).IgnoreResult();
  }
} // namespace

plGpuFluidGasSolver::plGpuFluidGasSolver() = default;

plGpuFluidGasSolver::~plGpuFluidGasSolver()
{
  PL_ASSERT_DEV(!IsCreated(), "The gas solver must be destroyed on the render thread before it goes away");
}

plResult plGpuFluidGasSolver::Create(const plVec3U32& vAtlasBricks, plUInt32 uiCapacity)
{
  PL_ASSERT_DEV(!IsCreated(), "Create called twice");

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  m_hVelocity[0] = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Velocity 0");
  m_hVelocity[1] = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Velocity 1");
  m_hScalar[0] = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Scalar 0");
  m_hScalar[1] = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Scalar 1");
  m_hScratch = CreateAtlas(pDevice, vAtlasBricks, "GpuFluid Gas Scratch");
  m_hPressure = CreateFloatBuffer(pDevice, uiCapacity * GPU_FLUID_BRICK_CELLS, "GpuFluid Gas Pressure");
  m_hDivergence = CreateFloatBuffer(pDevice, uiCapacity * GPU_FLUID_BRICK_CELLS, "GpuFluid Gas Divergence");

  plGALSamplerStateCreationDescription samplerDesc;
  samplerDesc.m_MinFilter = plGALTextureFilterMode::Linear;
  samplerDesc.m_MagFilter = plGALTextureFilterMode::Linear;
  samplerDesc.m_MipFilter = plGALTextureFilterMode::Point;
  samplerDesc.m_AddressU = plImageAddressMode::Clamp;
  samplerDesc.m_AddressV = plImageAddressMode::Clamp;
  samplerDesc.m_AddressW = plImageAddressMode::Clamp;
  m_hLinearClamp = pDevice->CreateSamplerState(samplerDesc);

  m_uiVelocitySide = 0;

  if (m_hVelocity[0].IsInvalidated() || m_hVelocity[1].IsInvalidated() || m_hScalar[0].IsInvalidated() || m_hScalar[1].IsInvalidated() ||
      m_hScratch.IsInvalidated() || m_hPressure.IsInvalidated() || m_hDivergence.IsInvalidated() || m_hLinearClamp.IsInvalidated())
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
    m_hForcesShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasForces.plShader");
    m_hDivergenceShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasDivergence.plShader");
    m_hPressureShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasPressure.plShader");
    m_hProjectShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasProject.plShader");
    m_hFinalizeShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasFinalize.plShader");
  }

  return PL_SUCCESS;
}

void plGpuFluidGasSolver::Destroy()
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  for (plGALTextureHandle* pTexture : {&m_hVelocity[0], &m_hVelocity[1], &m_hScalar[0], &m_hScalar[1], &m_hScratch})
  {
    if (!pTexture->IsInvalidated())
      pDevice->DestroyTexture(*pTexture);
    pTexture->Invalidate();
  }

  for (plGALBufferHandle* pBuffer : {&m_hPressure, &m_hDivergence})
  {
    if (!pBuffer->IsInvalidated())
      pDevice->DestroyBuffer(*pBuffer);
    pBuffer->Invalidate();
  }

  if (!m_hLinearClamp.IsInvalidated())
    pDevice->DestroySamplerState(m_hLinearClamp);
  m_hLinearClamp.Invalidate();
}

void plGpuFluidGasSolver::AddClearPass(plRenderGraph& ref_graph, const plGpuFluidSparseGrid& grid, const plGpuFluidSparseGridGraph& gridGraph)
{
  plGpuFluidGasSimulation sim;
  const plGpuFluidGasConstants constants = MakeConstants(grid, sim, 0.0f);

  const plRenderGraphTextureHandle hVelocity0 = ref_graph.ImportTexture(m_hVelocity[0]);
  const plRenderGraphTextureHandle hVelocity1 = ref_graph.ImportTexture(m_hVelocity[1]);
  const plRenderGraphTextureHandle hScalar0 = ref_graph.ImportTexture(m_hScalar[0]);
  const plRenderGraphTextureHandle hScalar1 = ref_graph.ImportTexture(m_hScalar[1]);
  const plRenderGraphTextureHandle hScratch = ref_graph.ImportTexture(m_hScratch);
  const plRenderGraphBufferHandle hPressure = ref_graph.ImportBuffer(m_hPressure);
  const plRenderGraphBufferHandle hDivergence = ref_graph.ImportBuffer(m_hDivergence);

  auto pass = ref_graph.AddComputePass("Gas/Alloc");
  plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
  pass.WriteTexture(hVelocity0);
  pass.WriteTexture(hVelocity1);
  pass.WriteTexture(hScalar0);
  pass.WriteTexture(hScalar1);
  pass.WriteTexture(hScratch);
  pass.WriteBuffer(hPressure);
  pass.WriteBuffer(hDivergence);
  pass.HasSideEffects();
  pass.SetExecuteCallback([=, hShader = m_hClearShader](const plRenderGraphContext& ctx)
    {
      plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
      bindGroup.BindTexture("GpuFluidVelocity0", ctx.ResolveTexture(hVelocity0));
      bindGroup.BindTexture("GpuFluidVelocity1", ctx.ResolveTexture(hVelocity1));
      bindGroup.BindTexture("GpuFluidScalar0", ctx.ResolveTexture(hScalar0));
      bindGroup.BindTexture("GpuFluidScalar1", ctx.ResolveTexture(hScalar1));
      bindGroup.BindTexture("GpuFluidScratch", ctx.ResolveTexture(hScratch));
      bindGroup.BindBuffer("GpuFluidPressure", ctx.ResolveBuffer(hPressure));
      bindGroup.BindBuffer("GpuFluidDivergence", ctx.ResolveBuffer(hDivergence));
      DispatchPerBrick(ctx, gridGraph, hShader, constants);
    });

  m_uiVelocitySide = 0;
}

void plGpuFluidGasSolver::AddFramePasses(plRenderGraph& ref_graph, const plGpuFluidSparseGrid& grid, const plGpuFluidSparseGridGraph& gridGraph,
  plRenderGraphBufferHandle hStats, const plGpuFluidGasSimulation& sim)
{
  if (sim.m_uiTicks == 0)
    return;

  const plRenderGraphTextureHandle hVelocity[2] = {ref_graph.ImportTexture(m_hVelocity[0]), ref_graph.ImportTexture(m_hVelocity[1])};
  const plRenderGraphTextureHandle hScalar0 = ref_graph.ImportTexture(m_hScalar[0]);
  const plRenderGraphTextureHandle hScalar1 = ref_graph.ImportTexture(m_hScalar[1]);
  const plRenderGraphTextureHandle hScratch = ref_graph.ImportTexture(m_hScratch);
  const plRenderGraphBufferHandle hPressure = ref_graph.ImportBuffer(m_hPressure);
  const plRenderGraphBufferHandle hDivergence = ref_graph.ImportBuffer(m_hDivergence);
  const plGALSamplerStateHandle hSampler = m_hLinearClamp;

  for (plUInt32 uiTick = 0; uiTick < sim.m_uiTicks; ++uiTick)
  {
    const bool bLastTick = uiTick + 1 == sim.m_uiTicks;
    const plGpuFluidGasConstants base = MakeConstants(grid, sim, sim.m_fTime + uiTick * sim.m_fTickSeconds);

    const plRenderGraphTextureHandle hVelocityCur = hVelocity[m_uiVelocitySide];
    const plRenderGraphTextureHandle hVelocityNext = hVelocity[m_uiVelocitySide ^ 1];

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
        c.GpuFluidGasPass = plVec4U32(0, uiShape, 0, emitter.m_bDriveVelocity ? 1u : 0u);
        c.GpuFluidGasParams0 = plVec4(emitter.m_vCenterVoxels.x, emitter.m_vCenterVoxels.y, emitter.m_vCenterVoxels.z, emitter.m_fRadiusVoxels);
        c.GpuFluidGasParams1 = plVec4(emitter.m_vHalfExtentsVoxels.x, emitter.m_vHalfExtentsVoxels.y, emitter.m_vHalfExtentsVoxels.z, emitter.m_fSoftnessVoxels);
        c.GpuFluidGasParams2 = plVec4(emitter.m_fDensityRate, emitter.m_fTemperature, emitter.m_fFuelRate, emitter.m_fNoiseStrength);
        c.GpuFluidGasParams3 = plVec4(emitter.m_vVelocity.x, emitter.m_vVelocity.y, emitter.m_vVelocity.z, emitter.m_fNoiseFrequency);
      }

      auto pass = ref_graph.AddComputePass("Gas/Emit");
      plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
      pass.WriteTexture(hScalar0);
      pass.WriteTexture(hVelocityCur);
      pass.HasSideEffects();
      pass.SetExecuteCallback([=, hShader = m_hEmitShader](const plRenderGraphContext& ctx)
        {
          plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
          const plGALTextureHandle hScalarGAL = ctx.ResolveTexture(hScalar0);
          const plGALTextureHandle hVelocityGAL = ctx.ResolveTexture(hVelocityCur);

          plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
          bindGroup.BindTexture("GpuFluidScalarTarget", hScalarGAL);
          bindGroup.BindTexture("GpuFluidVelocityTarget", hVelocityGAL);

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

    // Gas/Advect: current -> next (semi-Lagrangian), the scalars' apron, and with MacCormack the reverse trace into
    // Scratch, the limited correction and the apron again.
    {
      plGpuFluidGasConstants haloConstants = base;
      haloConstants.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_SCALAR, 0, 0, 0);

      plGpuFluidGasConstants advect[3] = {base, base, base};
      for (plUInt32 uiMode = 0; uiMode < 3; ++uiMode)
        advect[uiMode].GpuFluidGasPass = plVec4U32(uiMode, 0, 0, 0);

      const bool bMacCormack = sim.m_bMacCormack;

      auto pass = ref_graph.AddComputePass("Gas/Advect");
      plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
      pass.ReadTexture(hVelocityCur, {}, plGALResourceState::ComputeShaderRead);
      pass.ReadTexture(hScalar0, {}, plGALResourceState::ComputeShaderRead);
      pass.WriteTexture(hVelocityNext);
      pass.WriteTexture(hScalar1);
      pass.WriteTexture(hScratch);
      pass.HasSideEffects();
      pass.SetExecuteCallback([=, hAdvect = m_hAdvectShader, hHalo = m_hHaloShader](const plRenderGraphContext& ctx)
        {
          plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
          const plGALTextureHandle hScalarOut = ctx.ResolveTexture(hScalar1);
          const plGALTextureHandle hScratchGAL = ctx.ResolveTexture(hScratch);

          plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
          bindGroup.BindTexture("GpuFluidVelocityIn", ctx.ResolveTexture(hVelocityCur));
          bindGroup.BindTexture("GpuFluidScalarIn", ctx.ResolveTexture(hScalar0));
          bindGroup.BindSampler("GpuFluidLinearClamp", hSampler);
          bindGroup.BindTexture("GpuFluidVelocityOut", ctx.ResolveTexture(hVelocityNext));
          bindGroup.BindTexture("GpuFluidScalarOut", hScalarOut);
          bindGroup.BindTexture("GpuFluidScratch", hScratchGAL);
          DispatchPerBrick(ctx, gridGraph, hAdvect, advect[0]);

          UavBarrier(pEncoder, hScalarOut);
          bindGroup.BindTexture("GpuFluidHaloTarget", hScalarOut);
          DispatchPerBrick(ctx, gridGraph, hHalo, haloConstants);

          if (bMacCormack)
          {
            UavBarrier(pEncoder, hScalarOut);
            DispatchPerBrick(ctx, gridGraph, hAdvect, advect[1]);
            UavBarrier(pEncoder, hScratchGAL);
            DispatchPerBrick(ctx, gridGraph, hAdvect, advect[2]);
            UavBarrier(pEncoder, hScalarOut);
            DispatchPerBrick(ctx, gridGraph, hHalo, haloConstants);
          }
        });
    }

    // Gas/Forces: buoyancy on the next velocity; cooled, dissipated, clamped scalars back to the current side.
    {
      plGpuFluidGasConstants forces = base;
      forces.GpuFluidGasParams0 = plVec4(sim.m_vUp.x, sim.m_vUp.y, sim.m_vUp.z, sim.m_fBuoyancy);
      forces.GpuFluidGasParams1 = plVec4(sim.m_fDensityWeight, sim.m_fAmbientTemperature, sim.m_fCooling, sim.m_fDissipation);
      forces.GpuFluidGasParams2 = plVec4(sim.m_fDensityMax, sim.m_fVelocityDissipation, sim.m_fFuelDissipation, sim.m_fEdgeFadeVoxels);
      forces.GpuFluidGasParams3 = plVec4(sim.m_fEdgeFadeRate, 0.0f, 0.0f, 0.0f);

      plGpuFluidGasConstants haloConstants = base;
      haloConstants.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_VELOCITY, 0, 0, 0);

      auto pass = ref_graph.AddComputePass("Gas/Forces");
      plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
      pass.ReadTexture(hScalar1, {}, plGALResourceState::ComputeShaderRead);
      pass.WriteTexture(hScalar0);
      pass.WriteTexture(hVelocityNext);
      pass.HasSideEffects();
      pass.SetExecuteCallback([=, hForces = m_hForcesShader, hHalo = m_hHaloShader](const plRenderGraphContext& ctx)
        {
          plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
          const plGALTextureHandle hVelocityGAL = ctx.ResolveTexture(hVelocityNext);

          plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
          bindGroup.BindTexture("GpuFluidScalarIn", ctx.ResolveTexture(hScalar1));
          bindGroup.BindTexture("GpuFluidScalarOut", ctx.ResolveTexture(hScalar0));
          bindGroup.BindTexture("GpuFluidVelocityTarget", hVelocityGAL);
          DispatchPerBrick(ctx, gridGraph, hForces, forces);

          UavBarrier(pEncoder, hVelocityGAL);
          bindGroup.BindTexture("GpuFluidHaloTarget", hVelocityGAL);
          DispatchPerBrick(ctx, gridGraph, hHalo, haloConstants);
        });
    }

    // Gas/Pressure: divergence, red-black SOR, projection, the velocity's apron.
    {
      plGpuFluidGasConstants divergence = base;
      divergence.GpuFluidGasPass = plVec4U32(0, 0, bLastTick ? 1u : 0u, 0);

      plGpuFluidGasConstants sweep[2] = {base, base};
      for (plUInt32 uiColour = 0; uiColour < 2; ++uiColour)
      {
        sweep[uiColour].GpuFluidGasPass = plVec4U32(0, uiColour, 0, 0);
        sweep[uiColour].GpuFluidGasParams0 = plVec4(sim.m_fOverRelaxation, 0.0f, 0.0f, 0.0f);
      }

      plGpuFluidGasConstants haloConstants = base;
      haloConstants.GpuFluidGasPass = plVec4U32(GPU_FLUID_HALO_VELOCITY_KEEP, 0, 0, 0);

      const plUInt32 uiIterations = sim.m_uiPressureIterations;

      auto pass = ref_graph.AddComputePass("Gas/Pressure");
      plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
      pass.WriteTexture(hVelocityNext);
      pass.WriteBuffer(hPressure);
      pass.WriteBuffer(hDivergence);
      pass.WriteBuffer(hStats);
      pass.HasSideEffects();
      pass.SetExecuteCallback([=, hDivergenceShader = m_hDivergenceShader, hPressureShader = m_hPressureShader, hProject = m_hProjectShader, hHalo = m_hHaloShader](const plRenderGraphContext& ctx)
        {
          plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
          const plGALTextureHandle hVelocityGAL = ctx.ResolveTexture(hVelocityNext);
          const plGALBufferHandle hPressureGAL = ctx.ResolveBuffer(hPressure);
          const plGALBufferHandle hDivergenceGAL = ctx.ResolveBuffer(hDivergence);

          plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
          bindGroup.BindTexture("GpuFluidVelocityTarget", hVelocityGAL);
          bindGroup.BindBuffer("GpuFluidPressure", hPressureGAL);
          bindGroup.BindBuffer("GpuFluidDivergence", hDivergenceGAL);
          bindGroup.BindBuffer("GpuFluidStats", ctx.ResolveBuffer(hStats));

          DispatchPerBrick(ctx, gridGraph, hDivergenceShader, divergence);
          UavBarrier(pEncoder, hDivergenceGAL);

          for (plUInt32 i = 0; i < uiIterations; ++i)
          {
            for (plUInt32 uiColour = 0; uiColour < 2; ++uiColour)
            {
              DispatchPerBrick(ctx, gridGraph, hPressureShader, sweep[uiColour]);
              UavBarrier(pEncoder, hPressureGAL);
            }
          }

          DispatchPerBrick(ctx, gridGraph, hProject, base);
          UavBarrier(pEncoder, hVelocityGAL);

          bindGroup.BindTexture("GpuFluidHaloTarget", hVelocityGAL);
          DispatchPerBrick(ctx, gridGraph, hHalo, haloConstants);
        });
    }

    m_uiVelocitySide ^= 1;
  }

  // Gas/Finalize, once per frame.
  {
    const plRenderGraphTextureHandle hVelocityCur = hVelocity[m_uiVelocitySide];

    plGpuFluidGasConstants finalize = MakeConstants(grid, sim, sim.m_fTime + sim.m_uiTicks * sim.m_fTickSeconds);
    finalize.GpuFluidGasParams0 = plVec4(sim.m_fAllocThreshold, sim.m_fMeanFloor, 0.0f, 0.0f);

    auto pass = ref_graph.AddComputePass("Gas/Finalize");
    plGpuFluidSparseGrid::DeclareReadOnly(pass, gridGraph);
    pass.WriteTexture(hScalar0);
    pass.WriteTexture(hVelocityCur);
    pass.WriteTexture(gridGraph.m_hIndirection);
    pass.WriteBuffer(hStats);
    pass.HasSideEffects();
    pass.SetExecuteCallback([=, hShader = m_hFinalizeShader](const plRenderGraphContext& ctx)
      {
        plBindGroupBuilder& bindGroup = ctx.GetRenderContext()->GetBindGroup();
        bindGroup.BindTexture("GpuFluidScalarTarget", ctx.ResolveTexture(hScalar0));
        bindGroup.BindTexture("GpuFluidVelocityTarget", ctx.ResolveTexture(hVelocityCur));
        bindGroup.BindTexture("GpuFluidIndirectionOutput", ctx.ResolveTexture(gridGraph.m_hIndirection));
        bindGroup.BindBuffer("GpuFluidStats", ctx.ResolveBuffer(hStats));
        DispatchPerBrick(ctx, gridGraph, hShader, finalize);
      });
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidGasSolver);
