#pragma once

#include <Foundation/Types/ArrayPtr.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/RendererFoundationDLL.h>

class plRenderContext;
class plGALCommandEncoder;

/// \brief Shared GPU utility: an exclusive prefix sum over uint counts, and an indirect-argument writer.
///
/// - The scan works in blocks of 1024 counts. Each level writes one sum per block into a scratch buffer, the next
///   level scans those sums in place, and the single block of the top level writes the grand total. Up to 1024^3
///   counts. Every sum is an integer, so the result is the same whatever order the groups run in.
/// - The args writer turns a count that lives on the GPU into {groupsX, groupsY, 1, count}. Group counts above 65535
///   fold into y; a kernel finds its linear group as groupId.x + groupId.y * groupsX (GpuFluidLinearGroup).
///
/// Both record into the compute pass that is current (inside a render-graph compute callback). Buffers are raw
/// (ByteAddressBuffer) and must allow UAV access; the caller declares them to its render-graph pass. The liquid
/// solver uses it for the counting sort and its dispatch lists; the gas allocator can use it for slot assignment.
class PL_PARTICLEPLUGIN_DLL plGpuFluidScan
{
public:
  static constexpr plUInt32 s_uiBlock = 1024;
  static constexpr plUInt32 s_uiMaxLevels = 3;

  /// Where the levels of a scan of a given count keep their block sums, in the scratch buffer.
  struct Layout
  {
    plUInt32 m_uiCount = 0;
    plUInt32 m_uiLevelCount = 0;
    plUInt32 m_LevelCounts[s_uiMaxLevels] = {};     ///< elements scanned on each level
    plUInt32 m_LevelSumOffsets[s_uiMaxLevels] = {}; ///< byte offset of each level's block sums
    plUInt32 m_uiScratchBytes = 0;

    /// Byte offset of the grand total in the scratch buffer.
    plUInt32 GetTotalOffset() const { return m_LevelSumOffsets[m_uiLevelCount - 1]; }
  };

  static Layout ComputeLayout(plUInt32 uiCount);

  plGpuFluidScan();
  ~plGpuFluidScan();

  /// Loads the shaders. Call once before recording.
  void Initialize();

  /// Releases the shaders.
  void Deinitialize();

  /// Exclusive scan of layout.m_uiCount uints at hInput + uiInputOffset into hOutput + uiOutputOffset. hScratch holds
  /// at least layout.m_uiScratchBytes from offset 0; the total ends up at layout.GetTotalOffset() in it. The input and
  /// output may be the same range. Inserts the UAV barriers between its own dispatches.
  void RecordExclusiveScan(plRenderContext* pContext, plGALCommandEncoder* pEncoder, const Layout& layout, plGALBufferHandle hInput, plUInt32 uiInputOffset,
    plGALBufferHandle hOutput, plUInt32 uiOutputOffset, plGALBufferHandle hScratch) const;

  /// Writes {groupsX, groupsY, 1, count} at hArgs + uiArgsOffset from the uint count at hCount + uiCountOffset.
  void RecordIndirectArgs(plRenderContext* pContext, plGALBufferHandle hCount, plUInt32 uiCountOffset, plUInt32 uiItemsPerGroup, plGALBufferHandle hArgs, plUInt32 uiArgsOffset) const;

  /// Within one render-graph pass, makes the writes of one dispatch to these buffers visible to the next (UAV
  /// barriers; a no-op on Metal, which tracks hazards itself).
  static void UavBarrier(plGALCommandEncoder* pEncoder, plArrayPtr<const plGALBufferHandle> buffers);

private:
  plShaderResourceHandle m_hBlocksShader;
  plShaderResourceHandle m_hAddShader;
  plShaderResourceHandle m_hArgsShader;
};
