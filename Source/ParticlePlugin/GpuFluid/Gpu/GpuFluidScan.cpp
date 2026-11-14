#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidScan.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <Shaders/GpuFluid/GpuFluidScan.hlsli>

static_assert(plGpuFluidScan::s_uiBlock == GPU_FLUID_SCAN_BLOCK);
static_assert(sizeof(plGpuFluidScanConstants) <= 128, "Push constants are limited to 128 bytes on Metal");

namespace
{
  plUInt32 BlockCount(plUInt32 uiCount)
  {
    return plMath::Max(1u, (uiCount + GPU_FLUID_SCAN_BLOCK - 1) / GPU_FLUID_SCAN_BLOCK);
  }
} // namespace

plGpuFluidScan::Layout plGpuFluidScan::ComputeLayout(plUInt32 uiCount)
{
  Layout layout;
  layout.m_uiCount = uiCount;

  plUInt32 uiLevelCount = uiCount;
  plUInt32 uiOffset = 0;
  while (layout.m_uiLevelCount < s_uiMaxLevels)
  {
    const plUInt32 uiBlocks = BlockCount(uiLevelCount);
    layout.m_LevelCounts[layout.m_uiLevelCount] = uiLevelCount;
    layout.m_LevelSumOffsets[layout.m_uiLevelCount] = uiOffset;
    ++layout.m_uiLevelCount;

    // 16 byte aligned, so every level's sums can be read with wide loads too.
    uiOffset += ((uiBlocks * 4 + 15) / 16) * 16;

    if (uiBlocks == 1)
      break;

    uiLevelCount = uiBlocks;
  }

  PL_ASSERT_DEV(BlockCount(layout.m_LevelCounts[layout.m_uiLevelCount - 1]) == 1, "GpuFluidScan: {} counts are more than three levels of 1024 can scan", uiCount);
  layout.m_uiScratchBytes = plMath::Max(uiOffset, 16u);
  return layout;
}

plGpuFluidScan::plGpuFluidScan() = default;

plGpuFluidScan::~plGpuFluidScan()
{
  Deinitialize();
}

void plGpuFluidScan::Initialize()
{
  if (m_hBlocksShader.IsValid())
    return;

  m_hBlocksShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/GpuFluidScanBlocks.plShader");
  m_hAddShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/GpuFluidScanAdd.plShader");
  m_hArgsShader = plResourceManager::LoadResource<plShaderResource>("Shaders/GpuFluid/GpuFluidScanArgs.plShader");
}

void plGpuFluidScan::Deinitialize()
{
  m_hBlocksShader.Invalidate();
  m_hAddShader.Invalidate();
  m_hArgsShader.Invalidate();
}

void plGpuFluidScan::UavBarrier(plGALCommandEncoder* pEncoder, plArrayPtr<const plGALBufferHandle> buffers)
{
  if (pEncoder == nullptr)
    return;

  for (const plGALBufferHandle& hBuffer : buffers)
  {
    if (!hBuffer.IsInvalidated())
      pEncoder->BufferBarrier(hBuffer, plGALResourceState::UnorderedAccessWrite, plGALResourceState::UnorderedAccessWrite);
  }
}

void plGpuFluidScan::RecordExclusiveScan(plRenderContext* pContext, plGALCommandEncoder* pEncoder, const Layout& layout, plGALBufferHandle hInput,
  plUInt32 uiInputOffset, plGALBufferHandle hOutput, plUInt32 uiOutputOffset, plGALBufferHandle hScratch) const
{
  if (layout.m_uiLevelCount == 0)
    return;

  const plGALBufferHandle barrier[] = {hOutput, hScratch};

  auto Blocks = [&](plUInt32 uiCount, plGALBufferHandle hIn, plUInt32 uiIn, plGALBufferHandle hOut, plUInt32 uiOut, plUInt32 uiSums)
  {
    plGpuFluidScanConstants constants = {};
    constants.GpuFluidScanRange = plVec4U32(uiCount, uiIn, uiOut, uiSums);
    pContext->SetPushConstants("plGpuFluidScanConstants", constants);
    pContext->BindShader(m_hBlocksShader);

    plBindGroupBuilder& bindGroup = pContext->GetBindGroup();
    bindGroup.BindBuffer("GpuFluidScanInput", hIn);
    bindGroup.BindBuffer("GpuFluidScanOutput", hOut);
    bindGroup.BindBuffer("GpuFluidScanBlockSums", hScratch);
    pContext->Dispatch(BlockCount(uiCount)).IgnoreResult();
    UavBarrier(pEncoder, barrier);
  };

  auto Add = [&](plUInt32 uiCount, plGALBufferHandle hOut, plUInt32 uiOut, plUInt32 uiSums)
  {
    plGpuFluidScanConstants constants = {};
    constants.GpuFluidScanRange = plVec4U32(uiCount, 0, uiOut, uiSums);
    pContext->SetPushConstants("plGpuFluidScanConstants", constants);
    pContext->BindShader(m_hAddShader);

    plBindGroupBuilder& bindGroup = pContext->GetBindGroup();
    bindGroup.BindBuffer("GpuFluidScanOutput", hOut);
    bindGroup.BindBuffer("GpuFluidScanBlockSums", hScratch);
    pContext->Dispatch(BlockCount(uiCount)).IgnoreResult();
    UavBarrier(pEncoder, barrier);
  };

  // Up: level 0 scans the input, every further level scans the block sums of the one below in place.
  Blocks(layout.m_LevelCounts[0], hInput, uiInputOffset, hOutput, uiOutputOffset, layout.m_LevelSumOffsets[0]);
  for (plUInt32 uiLevel = 1; uiLevel < layout.m_uiLevelCount; ++uiLevel)
  {
    const plUInt32 uiData = layout.m_LevelSumOffsets[uiLevel - 1];
    Blocks(layout.m_LevelCounts[uiLevel], hScratch, uiData, hScratch, uiData, layout.m_LevelSumOffsets[uiLevel]);
  }

  // Down: the scanned sums of level l are the block offsets of level l - 1.
  for (plUInt32 uiLevel = layout.m_uiLevelCount - 1; uiLevel >= 1; --uiLevel)
  {
    const plUInt32 uiData = uiLevel - 1;
    if (uiData == 0)
      Add(layout.m_LevelCounts[0], hOutput, uiOutputOffset, layout.m_LevelSumOffsets[0]);
    else
      Add(layout.m_LevelCounts[uiData], hScratch, layout.m_LevelSumOffsets[uiData - 1], layout.m_LevelSumOffsets[uiData]);
  }
}

void plGpuFluidScan::RecordIndirectArgs(plRenderContext* pContext, plGALBufferHandle hCount, plUInt32 uiCountOffset, plUInt32 uiItemsPerGroup, plGALBufferHandle hArgs, plUInt32 uiArgsOffset) const
{
  plGpuFluidScanConstants constants = {};
  constants.GpuFluidScanArgs = plVec4U32(uiCountOffset, plMath::Max(uiItemsPerGroup, 1u), uiArgsOffset, 0);
  pContext->SetPushConstants("plGpuFluidScanConstants", constants);
  pContext->BindShader(m_hArgsShader);

  plBindGroupBuilder& bindGroup = pContext->GetBindGroup();
  bindGroup.BindBuffer("GpuFluidScanCount", hCount);
  bindGroup.BindBuffer("GpuFluidScanArgsOutput", hArgs);
  pContext->Dispatch(1).IgnoreResult();
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidScan);
