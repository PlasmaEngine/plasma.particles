#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidSparseGrid.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/Buffer.h>
#include <RendererFoundation/Resources/Texture.h>
#include <Shaders/GpuFluid/GpuFluidGasConstants.h>

static_assert(sizeof(plGpuFluidGasConstants) <= 128, "Push constants are limited to 128 bytes on Metal");
static_assert(GPU_FLUID_COUNTER_WORDS == PL_ARRAY_SIZE(plGpuFluidGridDebug::m_Counters));

namespace
{
  // GasAlloc.plShader modes.
  constexpr plUInt32 s_uiAllocInit = 0;
  constexpr plUInt32 s_uiAllocReset = 1;
  constexpr plUInt32 s_uiAllocMarkEmitters = 2;
  constexpr plUInt32 s_uiAllocMarkAll = 3;
  constexpr plUInt32 s_uiAllocFree = 4;
  constexpr plUInt32 s_uiAllocPending = 5;
  constexpr plUInt32 s_uiAllocFreeList = 6;
  constexpr plUInt32 s_uiAllocAssign = 7;
  constexpr plUInt32 s_uiAllocLists = 8;
  constexpr plUInt32 s_uiAllocArgs = 9;
  constexpr plUInt32 s_uiAllocDebug = 10;

  constexpr plUInt32 s_uiAllocThreads = 64;

  plGALBufferHandle CreateUIntBuffer(plGALDevice* pDevice, plUInt32 uiCount, bool bIndirectArguments, const char* szName, bool bUnorderedAccess = true)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plUInt32);
    desc.m_uiTotalSize = plMath::Max(uiCount, 1u) * sizeof(plUInt32);
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
    if (bUnorderedAccess)
      desc.m_BufferFlags |= plGALBufferUsageFlags::UnorderedAccess;
    if (bIndirectArguments)
      desc.m_BufferFlags |= plGALBufferUsageFlags::DrawIndirect;
    desc.m_ResourceAccess.m_bImmutable = false;

    plGALBufferHandle hBuffer = pDevice->CreateBuffer(desc);
    if (!hBuffer.IsInvalidated())
      pDevice->GetBuffer(hBuffer)->SetDebugName(szName);
    return hBuffer;
  }

  /// A raw (byte address) buffer, as the scan utility needs.
  plGALBufferHandle CreateRawBuffer(plGALDevice* pDevice, plUInt32 uiBytes, const char* szName)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiTotalSize = plMath::Max((uiBytes + 15u) & ~15u, 16u);
    desc.m_BufferFlags = plGALBufferUsageFlags::ByteAddressBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
    desc.m_ResourceAccess.m_bImmutable = false;

    plGALBufferHandle hBuffer = pDevice->CreateBuffer(desc);
    if (!hBuffer.IsInvalidated())
      pDevice->GetBuffer(hBuffer)->SetDebugName(szName);
    return hBuffer;
  }

  plUInt32 DebugWords(plUInt32 uiCapacity, plUInt32 uiVirtualBricks)
  {
    return GPU_FLUID_COUNTER_WORDS + uiCapacity * (GPU_FLUID_DEBUG_SLOT_WORDS + 1) + uiVirtualBricks * 2;
  }
} // namespace

bool plGpuFluidGridDebug::Decode(plArrayPtr<const plUInt8> data, plUInt32 uiCapacity, const plVec3U32& vBrickGrid, bool bVirtual)
{
  const plUInt32 uiVirtual = vBrickGrid.x * vBrickGrid.y * vBrickGrid.z;
  if (data.GetCount() < DebugWords(uiCapacity, bVirtual ? uiVirtual : 0) * sizeof(plUInt32))
    return false;

  const plUInt32* pWords = reinterpret_cast<const plUInt32*>(data.GetPtr());
  m_uiCapacity = uiCapacity;
  m_vBrickGrid = vBrickGrid;
  for (plUInt32 i = 0; i < GPU_FLUID_COUNTER_WORDS; ++i)
    m_Counters[i] = pWords[i];

  const plUInt32* pSlots = pWords + GPU_FLUID_COUNTER_WORDS;
  m_SlotBrick.SetCountUninitialized(uiCapacity);
  m_SlotIdle.SetCountUninitialized(uiCapacity);
  m_SlotAlive.SetCountUninitialized(uiCapacity);
  m_SlotMask.SetCountUninitialized(uiCapacity);
  m_SlotBuckets.SetCountUninitialized(uiCapacity);
  for (plUInt32 s = 0; s < uiCapacity; ++s)
  {
    const plUInt32* p = pSlots + s * GPU_FLUID_DEBUG_SLOT_WORDS;
    m_SlotBrick[s] = p[0];
    m_SlotIdle[s] = p[1 + GPU_FLUID_META_IDLE];
    m_SlotAlive[s] = p[1 + GPU_FLUID_META_ALIVE];
    m_SlotMask[s] = p[1 + GPU_FLUID_META_MASK];
    m_SlotBuckets[s] = p[1 + GPU_FLUID_META_BUCKETS];
  }

  const plUInt32* pList = pSlots + uiCapacity * GPU_FLUID_DEBUG_SLOT_WORDS;
  const plUInt32 uiActive = plMath::Min(m_Counters[GPU_FLUID_COUNTER_ACTIVE], uiCapacity);
  m_ActiveList.SetCountUninitialized(uiActive);
  for (plUInt32 i = 0; i < uiActive; ++i)
    m_ActiveList[i] = pList[i];

  m_PageTable.Clear();
  m_BrickTableA.Clear();
  if (bVirtual)
  {
    const plUInt32* pVirtual = pList + uiCapacity;
    m_PageTable.SetCountUninitialized(uiVirtual);
    m_BrickTableA.SetCountUninitialized(uiVirtual);
    for (plUInt32 v = 0; v < uiVirtual; ++v)
    {
      m_PageTable[v] = pVirtual[v * 2 + 0];
      const plUInt32 uiBits = pVirtual[v * 2 + 1];
      plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&m_BrickTableA[v]), reinterpret_cast<const plUInt8*>(&uiBits), sizeof(float));
    }
  }
  return true;
}

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
  m_bInitialised = false;

  const plUInt32 uiVirtualBricks = vBrickGrid.x * vBrickGrid.y * vBrickGrid.z;
  m_uiRequestWords = (uiVirtualBricks + 31) / 32;
  const plUInt32 uiBucketWords = GPU_FLUID_ALLOC_BUCKETS * m_uiRequestWords;

  m_RequestLayout = plGpuFluidScan::ComputeLayout(uiBucketWords);
  m_SlotLayout = plGpuFluidScan::ComputeLayout(uiCapacity);

  m_hPageTable = CreateUIntBuffer(pDevice, uiVirtualBricks, false, "GpuFluid Gas PageTable");
  m_hActiveList = CreateUIntBuffer(pDevice, uiCapacity, false, "GpuFluid Gas ActiveList");
  m_hBrickCoords = CreateUIntBuffer(pDevice, uiCapacity, false, "GpuFluid Gas BrickCoords");
  m_hNeighbors = CreateUIntBuffer(pDevice, uiCapacity * GPU_FLUID_NEIGHBOR_COUNT, false, "GpuFluid Gas Neighbors");
  m_hCounters = CreateUIntBuffer(pDevice, GPU_FLUID_COUNTER_WORDS, false, "GpuFluid Gas Counters");
  m_hDispatchArgs = CreateUIntBuffer(pDevice, GPU_FLUID_ARGS_TOTAL_WORDS, true, "GpuFluid Gas DispatchArgs");
  m_hColourList = CreateUIntBuffer(pDevice, uiCapacity, false, "GpuFluid Gas ColourList");
  m_hColourArgs = CreateUIntBuffer(pDevice, 2 * GPU_FLUID_ARGS_WORDS, true, "GpuFluid Gas ColourArgs");
  m_hRequests = CreateRawBuffer(pDevice, uiBucketWords * 4, "GpuFluid Gas Requests");
  m_hPending = CreateUIntBuffer(pDevice, uiBucketWords, false, "GpuFluid Gas Pending");
  m_hRequestScan = CreateRawBuffer(pDevice, uiBucketWords * 4, "GpuFluid Gas RequestScan");
  m_hRequestScratch = CreateRawBuffer(pDevice, m_RequestLayout.m_uiScratchBytes, "GpuFluid Gas RequestScratch");
  m_hSlotScan = CreateRawBuffer(pDevice, uiCapacity * 4, "GpuFluid Gas SlotScan");
  m_hSlotScratch = CreateRawBuffer(pDevice, m_SlotLayout.m_uiScratchBytes, "GpuFluid Gas SlotScratch");
  m_hFreeList = CreateUIntBuffer(pDevice, uiCapacity, false, "GpuFluid Gas FreeList");
  m_hNewList = CreateUIntBuffer(pDevice, uiCapacity, false, "GpuFluid Gas NewList");
  m_hBrickMeta = CreateUIntBuffer(pDevice, uiCapacity * 4, false, "GpuFluid Gas BrickMeta");
  // Read-only: the CPU uploads it, so it rests in the shader-read state the device's upload path expects.
  m_hEmitterBricks = CreateUIntBuffer(pDevice, GPU_FLUID_MAX_EMITTER_BRICKS, false, "GpuFluid Gas EmitterBricks", false);

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

  for (const plGALBufferHandle* pBuffer : {&m_hPageTable, &m_hActiveList, &m_hBrickCoords, &m_hNeighbors, &m_hCounters, &m_hDispatchArgs, &m_hColourList, &m_hColourArgs, &m_hRequests,
         &m_hPending, &m_hRequestScan, &m_hRequestScratch, &m_hSlotScan, &m_hSlotScratch, &m_hFreeList, &m_hNewList, &m_hBrickMeta, &m_hEmitterBricks})
  {
    if (pBuffer->IsInvalidated())
    {
      Destroy();
      return PL_FAILURE;
    }
  }
  if (m_hIndirection.IsInvalidated())
  {
    Destroy();
    return PL_FAILURE;
  }

  if (!m_hAllocShader.IsValid())
    m_hAllocShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Gas/GasAlloc.plShader");
  m_Scan.Initialize();

  return PL_SUCCESS;
}

void plGpuFluidSparseGrid::Destroy()
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  for (plGALBufferHandle* pBuffer : {&m_hPageTable, &m_hActiveList, &m_hBrickCoords, &m_hNeighbors, &m_hCounters, &m_hDispatchArgs, &m_hColourList, &m_hColourArgs, &m_hRequests, &m_hPending,
         &m_hRequestScan, &m_hRequestScratch, &m_hSlotScan, &m_hSlotScratch, &m_hFreeList, &m_hNewList, &m_hBrickMeta, &m_hEmitterBricks, &m_hGridDebug})
  {
    if (!pBuffer->IsInvalidated())
      pDevice->DestroyBuffer(*pBuffer);
    pBuffer->Invalidate();
  }

  if (!m_hIndirection.IsInvalidated())
    pDevice->DestroyTexture(m_hIndirection);
  m_hIndirection.Invalidate();

  m_DebugReadback.Reset();
  m_bAllocated = false;
  m_bInitialised = false;
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
  grid.m_hColourList = ref_graph.ImportBuffer(m_hColourList);
  grid.m_hColourArgs = ref_graph.ImportBuffer(m_hColourArgs);
  grid.m_hNewList = ref_graph.ImportBuffer(m_hNewList);
  grid.m_hRequests = ref_graph.ImportBuffer(m_hRequests);
  grid.m_hBrickMeta = ref_graph.ImportBuffer(m_hBrickMeta);
  grid.m_hIndirection = ref_graph.ImportTexture(m_hIndirection);
  return grid;
}

void plGpuFluidSparseGrid::AddAllocPasses(
  plRenderGraph& ref_graph, const plGpuFluidSparseGridGraph& grid, plRenderGraphBufferHandle hStats, const plGpuFluidAllocInput& input, plUInt64 uiFrame)
{
  const plUInt32 uiVirtualBricks = m_vBrickGrid.x * m_vBrickGrid.y * m_vBrickGrid.z;
  const plUInt32 uiCapacity = m_uiCapacity;
  const plUInt32 uiWords = m_uiRequestWords;
  const plUInt32 uiBucketWords = GPU_FLUID_ALLOC_BUCKETS * uiWords;
  const plUInt32 uiEmitterBricks = plMath::Min(input.m_EmitterBricks.GetCount(), static_cast<plUInt32>(GPU_FLUID_MAX_EMITTER_BRICKS));

  // The grid debug readback, when asked for: its buffer is made on first use, sized for the virtual part too.
  bool bDebug = input.m_bDebugReadback;
  if (bDebug && m_hGridDebug.IsInvalidated())
  {
    m_hGridDebug = CreateUIntBuffer(plGALDevice::GetDefaultDevice(), DebugWords(uiCapacity, uiVirtualBricks), false, "GpuFluid Gas GridDebug");
    bDebug = !m_hGridDebug.IsInvalidated();
  }
  const plUInt32 uiDebugSlot = bDebug ? m_DebugReadback.ReserveSlot(uiFrame) : plInvalidIndex;
  bDebug = uiDebugSlot != plInvalidIndex;
  if (bDebug)
  {
    m_uiDebugFrames[uiDebugSlot] = uiFrame;
    m_bDebugVirtualParts[uiDebugSlot] = input.m_bDebugVirtual;
  }

  // The sources' bricks go up through the device's upload path, which runs ahead of this frame's rendering. A copy
  // recorded in a graph pass of its own conflicted with the resting state that path assumes (D3D12 debug layer).
  const plRenderGraphBufferHandle hEmitterBricks = ref_graph.ImportBuffer(m_hEmitterBricks);
  if (uiEmitterBricks > 0)
    plGALDevice::GetDefaultDevice()->UpdateBufferAheadOfTime(m_hEmitterBricks, input.m_EmitterBricks.GetArrayPtr().GetSubArray(0, uiEmitterBricks).ToByteArray());

  plGpuFluidGasConstants constants;
  plMemoryUtils::ZeroFill(&constants, 1);
  constants.GpuFluidGasGrid = plVec4U32(m_vBrickGrid.x, m_vBrickGrid.y, m_vBrickGrid.z, uiCapacity);
  constants.GpuFluidGasAtlas = plVec4U32(m_vAtlasBricks.x, m_vAtlasBricks.y, m_vAtlasBricks.z, 0);
  constants.GpuFluidGasParams0 = plVec4(static_cast<float>(m_SlotLayout.GetTotalOffset()), static_cast<float>(m_RequestLayout.GetTotalOffset()), input.m_bDebugVirtual ? 1.0f : 0.0f, 0.0f);

  const plRenderGraphBufferHandle hPending = ref_graph.ImportBuffer(m_hPending);
  const plRenderGraphBufferHandle hRequestScan = ref_graph.ImportBuffer(m_hRequestScan);
  const plRenderGraphBufferHandle hRequestScratch = ref_graph.ImportBuffer(m_hRequestScratch);
  const plRenderGraphBufferHandle hSlotScan = ref_graph.ImportBuffer(m_hSlotScan);
  const plRenderGraphBufferHandle hSlotScratch = ref_graph.ImportBuffer(m_hSlotScratch);
  const plRenderGraphBufferHandle hFreeList = ref_graph.ImportBuffer(m_hFreeList);
  const plRenderGraphBufferHandle hGridDebug = bDebug ? ref_graph.ImportBuffer(m_hGridDebug) : plRenderGraphBufferHandle();

  const bool bInit = !m_bInitialised;
  const bool bAllocateAll = input.m_bAllocateAll;
  const plGpuFluidScan::Layout slotLayout = m_SlotLayout;
  const plGpuFluidScan::Layout requestLayout = m_RequestLayout;
  const plGpuFluidScan* pScan = &m_Scan;

  // One pass builder at a time: the readback below is added after this one is done.
  {
  auto pass = ref_graph.AddComputePass("Gas/Alloc");
  pass.WriteBuffer(grid.m_hPageTable);
  pass.WriteBuffer(grid.m_hActiveList);
  pass.WriteBuffer(grid.m_hBrickCoords);
  pass.WriteBuffer(grid.m_hNeighbors);
  pass.WriteBuffer(grid.m_hCounters);
  pass.WriteBuffer(grid.m_hDispatchArgs);
  pass.WriteBuffer(grid.m_hColourList);
  pass.WriteBuffer(grid.m_hColourArgs);
  pass.WriteBuffer(grid.m_hNewList);
  pass.WriteBuffer(grid.m_hRequests);
  pass.WriteBuffer(grid.m_hBrickMeta);
  pass.WriteBuffer(hPending);
  pass.WriteBuffer(hRequestScan);
  pass.WriteBuffer(hRequestScratch);
  pass.WriteBuffer(hSlotScan);
  pass.WriteBuffer(hSlotScratch);
  pass.WriteBuffer(hFreeList);
  pass.WriteBuffer(hStats);
  pass.ReadBuffer(hEmitterBricks, plGALResourceState::ComputeShaderRead);
  if (bDebug)
    pass.WriteBuffer(hGridDebug);
  pass.WriteTexture(grid.m_hIndirection);
  pass.HasSideEffects();
  pass.SetExecuteCallback([=, hShader = m_hAllocShader](const plRenderGraphContext& ctx)
    {
      plRenderContext* pContext = ctx.GetRenderContext();
      plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();

      const plGALBufferHandle hSlotScanGAL = ctx.ResolveBuffer(hSlotScan);
      const plGALBufferHandle hSlotScratchGAL = ctx.ResolveBuffer(hSlotScratch);
      const plGALBufferHandle hRequestScanGAL = ctx.ResolveBuffer(hRequestScan);
      const plGALBufferHandle hRequestScratchGAL = ctx.ResolveBuffer(hRequestScratch);

      const plGALBufferHandle barrier[] = {ctx.ResolveBuffer(grid.m_hPageTable), ctx.ResolveBuffer(grid.m_hActiveList), ctx.ResolveBuffer(grid.m_hBrickCoords),
        ctx.ResolveBuffer(grid.m_hNeighbors), ctx.ResolveBuffer(grid.m_hCounters), ctx.ResolveBuffer(grid.m_hDispatchArgs), ctx.ResolveBuffer(grid.m_hColourList),
        ctx.ResolveBuffer(grid.m_hColourArgs), ctx.ResolveBuffer(grid.m_hNewList), ctx.ResolveBuffer(grid.m_hRequests), ctx.ResolveBuffer(grid.m_hBrickMeta),
        ctx.ResolveBuffer(hPending), hRequestScanGAL, hRequestScratchGAL, hSlotScanGAL, hSlotScratchGAL, ctx.ResolveBuffer(hFreeList), ctx.ResolveBuffer(hStats)};

      auto Bind = [&]()
      {
        pContext->BindShader(hShader);
        plBindGroupBuilder& bindGroup = pContext->GetBindGroup();
        bindGroup.BindBuffer("GpuFluidPageTableOutput", ctx.ResolveBuffer(grid.m_hPageTable));
        bindGroup.BindBuffer("GpuFluidActiveListOutput", ctx.ResolveBuffer(grid.m_hActiveList));
        bindGroup.BindBuffer("GpuFluidBrickCoordsOutput", ctx.ResolveBuffer(grid.m_hBrickCoords));
        bindGroup.BindBuffer("GpuFluidNeighborsOutput", ctx.ResolveBuffer(grid.m_hNeighbors));
        bindGroup.BindBuffer("GpuFluidCountersOutput", ctx.ResolveBuffer(grid.m_hCounters));
        bindGroup.BindBuffer("GpuFluidDispatchArgsOutput", ctx.ResolveBuffer(grid.m_hDispatchArgs));
        bindGroup.BindBuffer("GpuFluidColourListOutput", ctx.ResolveBuffer(grid.m_hColourList));
        bindGroup.BindBuffer("GpuFluidColourArgsOutput", ctx.ResolveBuffer(grid.m_hColourArgs));
        bindGroup.BindTexture("GpuFluidIndirectionOutput", ctx.ResolveTexture(grid.m_hIndirection));
        bindGroup.BindBuffer("GpuFluidRequests", ctx.ResolveBuffer(grid.m_hRequests));
        bindGroup.BindBuffer("GpuFluidPending", ctx.ResolveBuffer(hPending));
        bindGroup.BindBuffer("GpuFluidRequestScan", hRequestScanGAL);
        bindGroup.BindBuffer("GpuFluidRequestScratch", hRequestScratchGAL);
        bindGroup.BindBuffer("GpuFluidSlotScan", hSlotScanGAL);
        bindGroup.BindBuffer("GpuFluidSlotScratch", hSlotScratchGAL);
        bindGroup.BindBuffer("GpuFluidFreeList", ctx.ResolveBuffer(hFreeList));
        bindGroup.BindBuffer("GpuFluidNewList", ctx.ResolveBuffer(grid.m_hNewList));
        bindGroup.BindBuffer("GpuFluidBrickMeta", ctx.ResolveBuffer(grid.m_hBrickMeta));
        bindGroup.BindBuffer("GpuFluidEmitterBricks", ctx.ResolveBuffer(hEmitterBricks));
        bindGroup.BindBuffer("GpuFluidStats", ctx.ResolveBuffer(hStats));
        // Bound in every mode; without a debug readback the counters stand in (DEBUG does not run then).
        bindGroup.BindBuffer("GpuFluidGridDebug", bDebug ? ctx.ResolveBuffer(hGridDebug) : ctx.ResolveBuffer(grid.m_hCounters));
      };

      auto Run = [&](plUInt32 uiMode, plUInt32 uiItems)
      {
        plGpuFluidGasConstants c = constants;
        c.GpuFluidGasPass = plVec4U32(uiMode, uiWords, uiEmitterBricks, 0);
        pContext->SetPushConstants("plGpuFluidGasConstants", c);
        pContext->Dispatch(plMath::Max((uiItems + s_uiAllocThreads - 1) / s_uiAllocThreads, 1u)).IgnoreResult();
        plGpuFluidScan::UavBarrier(pEncoder, barrier);
      };

      Bind();
      if (bInit)
        Run(s_uiAllocInit, plMath::Max(plMath::Max(uiVirtualBricks, uiCapacity), plMath::Max(uiBucketWords, static_cast<plUInt32>(GPU_FLUID_ARGS_TOTAL_WORDS))));

      Run(s_uiAllocReset, GPU_FLUID_COUNTER_WORDS);
      if (uiEmitterBricks > 0)
        Run(s_uiAllocMarkEmitters, uiEmitterBricks);
      if (bAllocateAll)
        Run(s_uiAllocMarkAll, uiWords);
      Run(s_uiAllocFree, uiCapacity);
      Run(s_uiAllocPending, uiWords);

      // Free rank per slot and the free total; first rank per request word and the request total.
      pScan->RecordExclusiveScan(pContext, pEncoder, slotLayout, hSlotScanGAL, 0, hSlotScanGAL, 0, hSlotScratchGAL);
      pScan->RecordExclusiveScan(pContext, pEncoder, requestLayout, hRequestScanGAL, 0, hRequestScanGAL, 0, hRequestScratchGAL);

      Bind();
      Run(s_uiAllocFreeList, uiCapacity);
      Run(s_uiAllocAssign, uiBucketWords);
      Run(s_uiAllocLists, uiCapacity);
      Run(s_uiAllocArgs, 1);
      if (bDebug)
        Run(s_uiAllocDebug, plMath::Max(plMath::Max(uiCapacity, uiVirtualBricks), static_cast<plUInt32>(GPU_FLUID_COUNTER_WORDS)));
    });
  }

  if (bDebug)
  {
    plGpuFluidReadbackRing* pRing = &m_DebugReadback;
    const plGALBufferHandle hDebugGAL = m_hGridDebug;

    auto readback = ref_graph.AddTransferPass("Gas/Alloc Debug Readback");
    readback.ReadBuffer(hGridDebug, plGALResourceState::CopySource);
    readback.HasSideEffects();
    readback.SetExecuteCallback([pRing, uiDebugSlot, hDebugGAL](const plRenderGraphContext& ctx) { pRing->Submit(uiDebugSlot, *ctx.GetCommandEncoder(), hDebugGAL); });
  }

  m_bInitialised = true;
  m_bAllocated = true;
}

bool plGpuFluidSparseGrid::PollDebug(plGpuFluidGridDebug& out_debug)
{
  plUInt64 uiFrame = 0;
  if (!m_DebugReadback.Poll(m_DebugScratch, uiFrame))
    return false;

  bool bVirtual = false;
  for (plUInt32 i = 0; i < plGpuFluidReadbackRing::s_uiSlotCount; ++i)
  {
    if (m_uiDebugFrames[i] == uiFrame)
      bVirtual = m_bDebugVirtualParts[i];
  }
  out_debug.m_uiFrame = uiFrame;
  return out_debug.Decode(m_DebugScratch.GetArrayPtr(), m_uiCapacity, m_vBrickGrid, bVirtual);
}

void plGpuFluidSparseGrid::BindReadOnly(const plRenderGraphContext& ctx, plBindGroupBuilder& ref_bindGroup, const plGpuFluidSparseGridGraph& grid)
{
  ref_bindGroup.BindBuffer("GpuFluidPageTable", ctx.ResolveBuffer(grid.m_hPageTable));
  ref_bindGroup.BindBuffer("GpuFluidActiveList", ctx.ResolveBuffer(grid.m_hActiveList));
  ref_bindGroup.BindBuffer("GpuFluidBrickCoords", ctx.ResolveBuffer(grid.m_hBrickCoords));
  ref_bindGroup.BindBuffer("GpuFluidNeighbors", ctx.ResolveBuffer(grid.m_hNeighbors));
  ref_bindGroup.BindBuffer("GpuFluidDispatchArgs", ctx.ResolveBuffer(grid.m_hDispatchArgs));
  ref_bindGroup.BindBuffer("GpuFluidColourList", ctx.ResolveBuffer(grid.m_hColourList));
  ref_bindGroup.BindBuffer("GpuFluidColourArgs", ctx.ResolveBuffer(grid.m_hColourArgs));
  ref_bindGroup.BindBuffer("GpuFluidNewList", ctx.ResolveBuffer(grid.m_hNewList));
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidSparseGrid);
