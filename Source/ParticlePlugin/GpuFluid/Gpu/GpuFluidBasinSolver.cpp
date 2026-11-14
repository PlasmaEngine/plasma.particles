#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidBasinSolver.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/Buffer.h>
#include <Shaders/GpuFluid/Basin/GpuFluidBasinConstants.h>

static_assert(sizeof(plGpuFluidBasinConstants) <= 128, "Push constants are limited to 128 bytes on Metal");

namespace
{
  plUInt32 DivUp(plUInt32 a, plUInt32 b) { return (a + b - 1) / b; }
} // namespace

plGpuFluidBasinSolver::plGpuFluidBasinSolver() = default;

plGpuFluidBasinSolver::~plGpuFluidBasinSolver()
{
  Destroy();
}

plGALBufferHandle plGpuFluidBasinSolver::CreateBuffer(plUInt32 uiBytes, const char* szName, plArrayPtr<const plUInt8> initialData, bool bWritable, bool bIndirect)
{
  plGALBufferCreationDescription desc;
  desc.m_uiTotalSize = plMath::Max<plUInt32>(16, ((uiBytes + 15) / 16) * 16);
  desc.m_BufferFlags = plGALBufferUsageFlags::ByteAddressBuffer | plGALBufferUsageFlags::ShaderResource;
  if (bWritable)
    desc.m_BufferFlags |= plGALBufferUsageFlags::UnorderedAccess;
  if (bIndirect)
    desc.m_BufferFlags |= plGALBufferUsageFlags::DrawIndirect;
  desc.m_ResourceAccess.m_bImmutable = false;

  // The content of a new buffer is undefined on D3D12 and Vulkan: every buffer starts from the given data or zeros.
  plDynamicArray<plUInt8> data;
  data.SetCount(desc.m_uiTotalSize);
  if (!initialData.IsEmpty())
    plMemoryUtils::Copy(data.GetData(), initialData.GetPtr(), plMath::Min(initialData.GetCount(), desc.m_uiTotalSize));

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  plGALBufferHandle hBuffer = pDevice->CreateBuffer(desc, data.GetArrayPtr());
  if (!hBuffer.IsInvalidated())
    pDevice->GetBuffer(hBuffer)->SetDebugName(szName);
  return hBuffer;
}

bool plGpuFluidBasinSolver::Create(const plVec2U32& vColumns, float fColumnSize)
{
  Destroy();

  m_vColumns = plVec2U32(plMath::Max(vColumns.x, 1u), plMath::Max(vColumns.y, 1u));
  m_fColumnSize = plMath::Max(fColumnSize, 1e-3f);

  const plUInt32 uiColumns = m_vColumns.x * m_vColumns.y;
  const plUInt32 uiCornersX = m_vColumns.x + 1;
  const plUInt32 uiCorners = uiCornersX * (m_vColumns.y + 1);

  m_hHeight[0] = CreateBuffer(uiColumns * 4, "GpuFluid Basin Height A", {}, true, false);
  m_hHeight[1] = CreateBuffer(uiColumns * 4, "GpuFluid Basin Height B", {}, true, false);
  m_hVertices = CreateBuffer(uiCorners * GPU_FLUID_BASIN_VERTEX_BYTES, "GpuFluid Basin Vertices", {}, true, false);

  // One quad per column, counter-clockwise seen from above (the outside of the water), never changing.
  plDynamicArray<plUInt32> quads;
  quads.SetCountUninitialized(uiColumns * 4);
  for (plUInt32 y = 0; y < m_vColumns.y; ++y)
  {
    for (plUInt32 x = 0; x < m_vColumns.x; ++x)
    {
      const plUInt32 v = x + uiCornersX * y;
      plUInt32* q = &quads[(x + m_vColumns.x * y) * 4];
      q[0] = v;
      q[1] = v + 1;
      q[2] = v + 1 + uiCornersX;
      q[3] = v + uiCornersX;
    }
  }
  m_hQuads = CreateBuffer(uiColumns * GPU_FLUID_BASIN_QUAD_BYTES, "GpuFluid Basin Quads", plArrayPtr<const plUInt8>(reinterpret_cast<const plUInt8*>(quads.GetData()), quads.GetCount() * 4), false, false);

  // The draw arguments the liquid surface pass reads: mono at byte 0, stereo at byte 16.
  const plUInt32 args[8] = {uiColumns * 6, 1, 0, 0, uiColumns * 6, 2, 0, 0};
  m_hDrawArgs = CreateBuffer(sizeof(args), "GpuFluid Basin Draw Args", plArrayPtr<const plUInt8>(reinterpret_cast<const plUInt8*>(args), sizeof(args)), false, true);

  m_hWaveShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Basin/BasinWave.plShader");
  m_hImpulseShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Basin/BasinImpulse.plShader");
  m_hMeshShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/Basin/BasinMesh.plShader");

  m_bCreated = !m_hHeight[0].IsInvalidated() && !m_hHeight[1].IsInvalidated() && !m_hVertices.IsInvalidated() && !m_hQuads.IsInvalidated() && !m_hDrawArgs.IsInvalidated();
  if (!m_bCreated)
  {
    plLog::Error("GpuFluid: could not create the GPU resources of a flood basin ({} x {} columns)", m_vColumns.x, m_vColumns.y);
    Destroy();
  }
  return m_bCreated;
}

void plGpuFluidBasinSolver::Destroy()
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  for (plGALBufferHandle* pBuffer : {&m_hHeight[0], &m_hHeight[1], &m_hDepth, &m_hVertices, &m_hQuads, &m_hDrawArgs})
  {
    if (!pBuffer->IsInvalidated() && pDevice != nullptr)
      pDevice->DestroyBuffer(*pBuffer);
    pBuffer->Invalidate();
  }
  m_pUploadedDepths = nullptr;
  m_bCreated = false;
  m_bRecorded = false;
  m_bHasTick = false;
  m_uiCurrent = 0;
}

void plGpuFluidBasinSolver::GetPublish(plGpuFluidBasinPublish& out_publish) const
{
  out_publish.m_hVertices = m_hVertices;
  out_publish.m_hQuads = m_hQuads;
  out_publish.m_hDrawArgs = m_hDrawArgs;
  out_publish.m_uiQuads = m_vColumns.x * m_vColumns.y;
}

bool plGpuFluidBasinSolver::Record(plRenderGraph& ref_graph, const plGpuFluidBasinFrameInput& input)
{
  if (!m_bCreated || input.m_pDepths == nullptr || input.m_pDepths->m_Depths.GetCount() != m_vColumns.x * m_vColumns.y)
    return false;

  const bool bStep = !m_bHasTick || input.m_uiTick != m_uiLastTick;
  m_uiLastTick = input.m_uiTick;
  m_bHasTick = true;

  // The depths change when the level moves: a new buffer with the new content (rare, so no in-place update).
  if (input.m_pDepths != m_pUploadedDepths)
  {
    const plDynamicArray<float>& depths = input.m_pDepths->m_Depths;
    plGALBufferHandle hNew = CreateBuffer(depths.GetCount() * 4, "GpuFluid Basin Depth", plArrayPtr<const plUInt8>(reinterpret_cast<const plUInt8*>(depths.GetData()), depths.GetCount() * 4), false, false);
    if (hNew.IsInvalidated())
      return false;
    if (!m_hDepth.IsInvalidated())
      plGALDevice::GetDefaultDevice()->DestroyBuffer(m_hDepth);
    m_hDepth = hNew;
    m_pUploadedDepths = input.m_pDepths;
  }

  plGpuFluidBasinConstants c = {};
  c.GpuFluidBasinDims = plVec4U32(m_vColumns.x, m_vColumns.y, 0, 0);
  c.GpuFluidBasinGrid = plVec4(m_fColumnSize, 1.0f / m_fColumnSize, input.m_fStepDt, plMath::Clamp(input.m_fDamping, 0.0f, 1.0f));
  c.GpuFluidBasinLevel = plVec4(input.m_fLevel, input.m_fGravity, plMath::Max(input.m_fMaxWaveHeight, 0.0f), plMath::Max(input.m_fMaxSpeedDepth, 0.01f));
  c.GpuFluidBasinImpulse = plVec4::MakeZero();

  const plUInt32 uiSteps = bStep ? plMath::Clamp<plUInt32>(input.m_uiSteps, 0, 16) : 0;
  plHybridArray<plGpuFluidBasinConstants, 16> impulses;
  if (bStep)
  {
    for (const plVec4& impulse : input.m_Impulses)
    {
      // The square of columns the bump covers.
      const float fRadius = plMath::Max(impulse.z, m_fColumnSize);
      const plInt32 iX0 = plMath::Clamp(static_cast<plInt32>(plMath::Floor((impulse.x - fRadius) / m_fColumnSize)), 0, static_cast<plInt32>(m_vColumns.x) - 1);
      const plInt32 iY0 = plMath::Clamp(static_cast<plInt32>(plMath::Floor((impulse.y - fRadius) / m_fColumnSize)), 0, static_cast<plInt32>(m_vColumns.y) - 1);
      const plInt32 iX1 = plMath::Clamp(static_cast<plInt32>(plMath::Ceil((impulse.x + fRadius) / m_fColumnSize)), 0, static_cast<plInt32>(m_vColumns.x) - 1);
      const plInt32 iY1 = plMath::Clamp(static_cast<plInt32>(plMath::Ceil((impulse.y + fRadius) / m_fColumnSize)), 0, static_cast<plInt32>(m_vColumns.y) - 1);
      plGpuFluidBasinConstants& ic = impulses.ExpandAndGetRef();
      ic = c;
      ic.GpuFluidBasinDims.z = static_cast<plUInt32>(iX0);
      ic.GpuFluidBasinDims.w = static_cast<plUInt32>(iY0);
      ic.GpuFluidBasinImpulse = plVec4(impulse.x, impulse.y, fRadius, impulse.w);
      // The square's size rides in the unused y of the step's constants: the dispatch covers it.
      ic.GpuFluidBasinImpulse.z = fRadius;
      ic.GpuFluidBasinGrid.z = static_cast<float>(iX1 - iX0 + 1);
      ic.GpuFluidBasinGrid.w = static_cast<float>(iY1 - iY0 + 1);
    }
  }

  const plGALBufferHandle hDepth = m_hDepth, hVertices = m_hVertices;
  const plGALBufferHandle hA = m_hHeight[m_uiCurrent], hB = m_hHeight[1 - m_uiCurrent];
  const plShaderResourceHandle hWave = m_hWaveShader, hImpulse = m_hImpulseShader, hMesh = m_hMeshShader;
  const plVec2U32 vColumns = m_vColumns;

  auto pass = ref_graph.AddComputePass("GpuFluid Basin");
  pass.ReadBuffer(ref_graph.ImportBuffer(hDepth), plGALResourceState::ComputeShaderRead);
  pass.WriteBuffer(ref_graph.ImportBuffer(hA));
  pass.WriteBuffer(ref_graph.ImportBuffer(hB));
  pass.WriteBuffer(ref_graph.ImportBuffer(hVertices));
  pass.HasSideEffects();
  pass.SetExecuteCallback([=](const plRenderGraphContext& ctx)
    {
      plRenderContext* pContext = ctx.GetRenderContext();
      plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
      auto Barrier = [&](plGALBufferHandle h)
      { pEncoder->BufferBarrier(h, plGALResourceState::UnorderedAccessWrite, plGALResourceState::UnorderedAccessWrite); };

      // Bumps into the current heights.
      for (const plGpuFluidBasinConstants& ic : impulses)
      {
        plGpuFluidBasinConstants k = ic;
        const plUInt32 uiW = static_cast<plUInt32>(k.GpuFluidBasinGrid.z);
        const plUInt32 uiH = static_cast<plUInt32>(k.GpuFluidBasinGrid.w);
        k.GpuFluidBasinGrid = c.GpuFluidBasinGrid;
        pContext->SetPushConstants("plGpuFluidBasinConstants", k);
        pContext->BindShader(hImpulse);
        plBindGroupBuilder& bindGroup = pContext->GetBindGroup();
        bindGroup.BindBuffer("GpuFluidBasinHeight", hA);
        bindGroup.BindBuffer("GpuFluidBasinDepth", hDepth);
        pContext->Dispatch(DivUp(uiW, GPU_FLUID_BASIN_GROUP), DivUp(uiH, GPU_FLUID_BASIN_GROUP), 1).IgnoreResult();
        Barrier(hA);
      }

      // Steps, swapping the two height buffers.
      plGALBufferHandle hCur = hA, hOld = hB;
      for (plUInt32 s = 0; s < uiSteps; ++s)
      {
        pContext->SetPushConstants("plGpuFluidBasinConstants", c);
        pContext->BindShader(hWave);
        plBindGroupBuilder& bindGroup = pContext->GetBindGroup();
        bindGroup.BindBuffer("GpuFluidBasinHeight", hCur);
        bindGroup.BindBuffer("GpuFluidBasinHeightOld", hOld);
        bindGroup.BindBuffer("GpuFluidBasinDepth", hDepth);
        pContext->Dispatch(DivUp(vColumns.x, GPU_FLUID_BASIN_GROUP), DivUp(vColumns.y, GPU_FLUID_BASIN_GROUP), 1).IgnoreResult();
        // The next step writes what this one read: both wait.
        Barrier(hOld);
        Barrier(hCur);
        plMath::Swap(hCur, hOld);
      }

      // The mesh from the newest heights.
      pContext->SetPushConstants("plGpuFluidBasinConstants", c);
      pContext->BindShader(hMesh);
      plBindGroupBuilder& bindGroup = pContext->GetBindGroup();
      bindGroup.BindBuffer("GpuFluidBasinHeight", hCur);
      bindGroup.BindBuffer("GpuFluidBasinDepth", hDepth);
      bindGroup.BindBuffer("GpuFluidBasinVertices", hVertices);
      pContext->Dispatch(DivUp(vColumns.x + 1, GPU_FLUID_BASIN_GROUP), DivUp(vColumns.y + 1, GPU_FLUID_BASIN_GROUP), 1).IgnoreResult();
      Barrier(hVertices);
    });

  if (uiSteps % 2 == 1)
    m_uiCurrent = 1 - m_uiCurrent;
  m_bRecorded = true;
  return true;
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidBasinSolver);
