#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidSparseGrid.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/Buffer.h>
#include <RendererFoundation/Resources/Texture.h>
#include <Shaders/GpuFluid/GpuFluidGasConstants.h>

static_assert(sizeof(plGpuFluidGasConstants) <= 128, "Push constants are limited to 128 bytes on Metal");

namespace
{
  // GasAlloc.plShader modes.
  constexpr plUInt32 s_uiAllocReset = 0;
  constexpr plUInt32 s_uiAllocAssign = 1;
  constexpr plUInt32 s_uiAllocLists = 2;

  plGALBufferHandle CreateUIntBuffer(plGALDevice* pDevice, plUInt32 uiCount, bool bIndirectArguments, const char* szName)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plUInt32);
    desc.m_uiTotalSize = plMath::Max(uiCount, 1u) * sizeof(plUInt32);
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
    if (bIndirectArguments)
      desc.m_BufferFlags |= plGALBufferUsageFlags::DrawIndirect;
    desc.m_ResourceAccess.m_bImmutable = false;

    plGALBufferHandle hBuffer = pDevice->CreateBuffer(desc);
    if (!hBuffer.IsInvalidated())
      pDevice->GetBuffer(hBuffer)->SetDebugName(szName);
    return hBuffer;
  }
} // namespace

plGpuFluidSparseGrid::plGpuFluidSparseGrid() = default;

plGpuFluidSparseGrid::~plGpuFluidSparseGrid()
{
  PL_ASSERT_DEV(!IsCreated(), "The sparse grid must be destroyed on the render thread before it goes away");
}

plVec3U32 plGpuFluidSparseGrid::ComputeAtlasBricks(plUInt32 uiCapacity)
{
  plUInt32 uiSide = 1;
  while (uiSide * uiSide * uiSide < uiCapacity && uiSide < GPU_FLUID_MAX_ATLAS_BRICKS_PER_AXIS)
    ++uiSide;

  const plUInt32 uiDepth = plMath::Clamp<plUInt32>((uiCapacity + uiSide * uiSide - 1) / (uiSide * uiSide), 1, GPU_FLUID_MAX_ATLAS_BRICKS_PER_AXIS);
  return plVec3U32(uiSide, uiSide, uiDepth);
}

plResult plGpuFluidSparseGrid::Create(const plVec3U32& vBrickGrid, plUInt32 uiCapacity)
{
  PL_ASSERT_DEV(!IsCreated(), "Create called twice");

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  m_vBrickGrid = vBrickGrid;
  m_uiCapacity = uiCapacity;
  m_vAtlasBricks = ComputeAtlasBricks(uiCapacity);
  m_bAllocated = false;

  const plUInt32 uiVirtualBricks = vBrickGrid.x * vBrickGrid.y * vBrickGrid.z;

  m_hPageTable = CreateUIntBuffer(pDevice, uiVirtualBricks, false, "GpuFluid Gas PageTable");
  m_hActiveList = CreateUIntBuffer(pDevice, uiCapacity, false, "GpuFluid Gas ActiveList");
  m_hBrickCoords = CreateUIntBuffer(pDevice, uiCapacity, false, "GpuFluid Gas BrickCoords");
  m_hNeighbors = CreateUIntBuffer(pDevice, uiCapacity * GPU_FLUID_NEIGHBOR_COUNT, false, "GpuFluid Gas Neighbors");
  m_hCounters = CreateUIntBuffer(pDevice, GPU_FLUID_COUNTER_WORDS, false, "GpuFluid Gas Counters");
  m_hDispatchArgs = CreateUIntBuffer(pDevice, GPU_FLUID_ARGS_WORDS, true, "GpuFluid Gas DispatchArgs");

  {
    plGALTextureCreationDescription desc;
    desc.m_Type = plGALTextureType::Texture3D;
    desc.m_Format = plGALResourceFormat::RGBAHalf;
    desc.m_uiWidth = vBrickGrid.x;
    desc.m_uiHeight = vBrickGrid.y;
    desc.m_uiDepth = vBrickGrid.z;
    desc.m_uiMipLevelCount = 1;
    desc.m_uiArraySize = 1;
    desc.m_bAllowShaderResourceView = true;
    desc.m_bAllowUAV = true;
    desc.m_ResourceAccess.m_bImmutable = false;
    m_hIndirection = pDevice->CreateTexture(desc);
    if (!m_hIndirection.IsInvalidated())
      pDevice->GetTexture(m_hIndirection)->SetDebugName("GpuFluid Gas Brick Table");
  }

  if (m_hPageTable.IsInvalidated() || m_hActiveList.IsInvalidated() || m_hBrickCoords.IsInvalidated() || m_hNeighbors.IsInvalidated() ||
      m_hCounters.IsInvalidated() || m_hDispatchArgs.IsInvalidated() || m_hIndirection.IsInvalidated())
  {
    Destroy();
    return PL_FAILURE;
  }

  if (!m_hAllocShader.IsValid())
    m_hAllocShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasAlloc.plShader");

  return PL_SUCCESS;
}

void plGpuFluidSparseGrid::Destroy()
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  for (plGALBufferHandle* pBuffer : {&m_hPageTable, &m_hActiveList, &m_hBrickCoords, &m_hNeighbors, &m_hCounters, &m_hDispatchArgs})
  {
    if (!pBuffer->IsInvalidated())
      pDevice->DestroyBuffer(*pBuffer);
    pBuffer->Invalidate();
  }

  if (!m_hIndirection.IsInvalidated())
    pDevice->DestroyTexture(m_hIndirection);
  m_hIndirection.Invalidate();

  m_bAllocated = false;
}

plGpuFluidSparseGridGraph plGpuFluidSparseGrid::Import(plRenderGraph& ref_graph) const
{
  plGpuFluidSparseGridGraph grid;
  grid.m_hPageTable = ref_graph.ImportBuffer(m_hPageTable);
  grid.m_hActiveList = ref_graph.ImportBuffer(m_hActiveList);
  grid.m_hBrickCoords = ref_graph.ImportBuffer(m_hBrickCoords);
  grid.m_hNeighbors = ref_graph.ImportBuffer(m_hNeighbors);
  grid.m_hCounters = ref_graph.ImportBuffer(m_hCounters);
  grid.m_hDispatchArgs = ref_graph.ImportBuffer(m_hDispatchArgs);
  grid.m_hIndirection = ref_graph.ImportTexture(m_hIndirection);
  return grid;
}

void plGpuFluidSparseGrid::AddAllocPasses(plRenderGraph& ref_graph, const plGpuFluidSparseGridGraph& grid, plUInt8 uiOpenFaces)
{
  plGpuFluidGasConstants constants;
  plMemoryUtils::ZeroFill(&constants, 1);
  constants.GpuFluidGasGrid = plVec4U32(m_vBrickGrid.x, m_vBrickGrid.y, m_vBrickGrid.z, m_uiCapacity);
  constants.GpuFluidGasAtlas = plVec4U32(m_vAtlasBricks.x, m_vAtlasBricks.y, m_vAtlasBricks.z, uiOpenFaces);

  const plUInt32 uiVirtualBricks = m_vBrickGrid.x * m_vBrickGrid.y * m_vBrickGrid.z;
  const plUInt32 uiCapacity = m_uiCapacity;

  auto pass = ref_graph.AddComputePass("Gas/Alloc");
  pass.WriteBuffer(grid.m_hPageTable);
  pass.WriteBuffer(grid.m_hActiveList);
  pass.WriteBuffer(grid.m_hBrickCoords);
  pass.WriteBuffer(grid.m_hNeighbors);
  pass.WriteBuffer(grid.m_hCounters);
  pass.WriteBuffer(grid.m_hDispatchArgs);
  pass.WriteTexture(grid.m_hIndirection);
  pass.HasSideEffects();
  pass.SetExecuteCallback([grid, constants, uiVirtualBricks, uiCapacity, hShader = m_hAllocShader](const plRenderGraphContext& ctx)
    {
      plRenderContext* pContext = ctx.GetRenderContext();
      plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();

      pContext->BindShader(hShader);

      plBindGroupBuilder& bindGroup = pContext->GetBindGroup();
      bindGroup.BindBuffer("GpuFluidPageTableOutput", ctx.ResolveBuffer(grid.m_hPageTable));
      bindGroup.BindBuffer("GpuFluidActiveListOutput", ctx.ResolveBuffer(grid.m_hActiveList));
      bindGroup.BindBuffer("GpuFluidBrickCoordsOutput", ctx.ResolveBuffer(grid.m_hBrickCoords));
      bindGroup.BindBuffer("GpuFluidNeighborsOutput", ctx.ResolveBuffer(grid.m_hNeighbors));
      bindGroup.BindBuffer("GpuFluidCountersOutput", ctx.ResolveBuffer(grid.m_hCounters));
      bindGroup.BindBuffer("GpuFluidDispatchArgsOutput", ctx.ResolveBuffer(grid.m_hDispatchArgs));
      bindGroup.BindTexture("GpuFluidIndirectionOutput", ctx.ResolveTexture(grid.m_hIndirection));

      const plGALBufferHandle hCounters = ctx.ResolveBuffer(grid.m_hCounters);
      const plGALBufferHandle hPageTable = ctx.ResolveBuffer(grid.m_hPageTable);
      const plGALBufferHandle hBrickCoords = ctx.ResolveBuffer(grid.m_hBrickCoords);

      plGpuFluidGasConstants c = constants;

      c.GpuFluidGasPass = plVec4U32(s_uiAllocReset, 0, 0, 0);
      pContext->SetPushConstants("plGpuFluidGasConstants", c);
      pContext->Dispatch(1).IgnoreResult();
      pEncoder->BufferBarrier(hCounters, plGALResourceState::UnorderedAccessWrite, plGALResourceState::UnorderedAccessWrite);

      c.GpuFluidGasPass = plVec4U32(s_uiAllocAssign, 0, 0, 0);
      pContext->SetPushConstants("plGpuFluidGasConstants", c);
      pContext->Dispatch((uiVirtualBricks + 63) / 64).IgnoreResult();
      pEncoder->BufferBarrier(hCounters, plGALResourceState::UnorderedAccessWrite, plGALResourceState::UnorderedAccessWrite);
      pEncoder->BufferBarrier(hPageTable, plGALResourceState::UnorderedAccessWrite, plGALResourceState::UnorderedAccessWrite);
      pEncoder->BufferBarrier(hBrickCoords, plGALResourceState::UnorderedAccessWrite, plGALResourceState::UnorderedAccessWrite);

      c.GpuFluidGasPass = plVec4U32(s_uiAllocLists, 0, 0, 0);
      pContext->SetPushConstants("plGpuFluidGasConstants", c);
      pContext->Dispatch((uiCapacity + 63) / 64).IgnoreResult();
    });

  m_bAllocated = true;
}

void plGpuFluidSparseGrid::BindReadOnly(const plRenderGraphContext& ctx, plBindGroupBuilder& ref_bindGroup, const plGpuFluidSparseGridGraph& grid)
{
  ref_bindGroup.BindBuffer("GpuFluidPageTable", ctx.ResolveBuffer(grid.m_hPageTable));
  ref_bindGroup.BindBuffer("GpuFluidActiveList", ctx.ResolveBuffer(grid.m_hActiveList));
  ref_bindGroup.BindBuffer("GpuFluidBrickCoords", ctx.ResolveBuffer(grid.m_hBrickCoords));
  ref_bindGroup.BindBuffer("GpuFluidNeighbors", ctx.ResolveBuffer(grid.m_hNeighbors));
  ref_bindGroup.BindBuffer("GpuFluidDispatchArgs", ctx.ResolveBuffer(grid.m_hDispatchArgs));
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidSparseGrid);
