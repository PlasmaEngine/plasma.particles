#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Vec3.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidReadback.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidScan.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererCore/RenderGraph/Declarations.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/Descriptors/Enumerations.h>
#include <RendererFoundation/RendererFoundationDLL.h>

class plRenderGraph;
class plRenderGraphContext;
class plBindGroupBuilder;

/// The sparse grid's buffers as handles of one frame's render graph.
struct plGpuFluidSparseGridGraph
{
  plRenderGraphBufferHandle m_hPageTable;
  plRenderGraphBufferHandle m_hActiveList;
  plRenderGraphBufferHandle m_hBrickCoords;
  plRenderGraphBufferHandle m_hNeighbors;
  plRenderGraphBufferHandle m_hCounters;
  plRenderGraphBufferHandle m_hDispatchArgs;
  plRenderGraphBufferHandle m_hColourList;
  plRenderGraphBufferHandle m_hColourArgs;
  plRenderGraphBufferHandle m_hNewList;
  plRenderGraphBufferHandle m_hRequests;
  plRenderGraphBufferHandle m_hBrickMeta;
  plRenderGraphTextureHandle m_hIndirection;
};

/// What the allocator needs for one frame, gathered on the update thread.
struct plGpuFluidAllocInput
{
  /// Linear virtual bricks the frame's sources overlap (bucket 0), at most GPU_FLUID_MAX_EMITTER_BRICKS.
  plDynamicArray<plUInt32> m_EmitterBricks;

  /// The closed-box mode: every brick of the grid is requested in bucket 0 every frame, so the whole box (up to the
  /// pool) stays allocated, in virtual order.
  bool m_bAllocateAll = false;

  /// Copy the grid's state into the debug readback this frame (N1, the brick debug view).
  bool m_bDebugReadback = false;

  /// Include the per virtual brick part (page table, brick table A) in it.
  bool m_bDebugVirtual = false;
};

/// The grid debug readback, decoded (two to three frames late). See GPU_FLUID_DEBUG_* in GpuFluidConstants.h.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidGridDebug
{
  plUInt64 m_uiFrame = 0;
  plUInt32 m_uiCapacity = 0;
  plVec3U32 m_vBrickGrid = plVec3U32::MakeZero();

  plUInt32 m_Counters[16] = {};

  /// Per pool slot: the packed brick (x | y << 10 | z << 20) or 0xFFFFFFFF when free, and the meta words (idle steps,
  /// alive, neighbour request mask, buckets that requested it).
  plDynamicArray<plUInt32> m_SlotBrick;
  plDynamicArray<plUInt32> m_SlotIdle;
  plDynamicArray<plUInt32> m_SlotAlive;
  plDynamicArray<plUInt32> m_SlotMask;
  plDynamicArray<plUInt32> m_SlotBuckets;

  /// The active list (m_Counters[0] entries).
  plDynamicArray<plUInt32> m_ActiveList;

  /// Per virtual brick, when asked for: the page table entry and the brick table's A (as float bits).
  plDynamicArray<plUInt32> m_PageTable;
  plDynamicArray<float> m_BrickTableA;

  /// Decodes a readback of a grid of this size.
  bool Decode(plArrayPtr<const plUInt8> data, plUInt32 uiCapacity, const plVec3U32& vBrickGrid, bool bVirtual);
};

/// \brief The brick bookkeeping of a sparse gas volume: which virtual brick owns which pool slot, the requests that
/// decide it, and the lists every solver kernel dispatches over.
///
/// | Buffer        | Content                                                                    |
/// |---------------|----------------------------------------------------------------------------|
/// | PageTable     | virtual brick -> pool slot or 0xFFFFFFFF                                   |
/// | ActiveList    | active index -> pool slot, ascending                                       |
/// | BrickCoords   | pool slot -> packed virtual brick, 0xFFFFFFFF when free                    |
/// | Neighbors     | 27 per pool slot: slot, or "unallocated" / "outside the domain"            |
/// | Counters      | active, denied, colour counts, new, freed, denied per bucket               |
/// | DispatchArgs  | three sets (x, y, z, count): every active brick, the same in slabs, the new list (2D fold) |
/// | ColourList    | the active slots by brick colour (coordinate sum parity): 0 at the front, 1 at the back |
/// | ColourArgs    | DispatchArgs for each brick colour (the multigrid's block red-black launches) |
/// | Requests      | 4 priority buckets x 1 bit per virtual brick (GasFinalize, the emitter list) |
/// | Pending, RequestScan, SlotScan, FreeList, NewList | the allocation's work lists (GasAlloc.plShader) |
/// | BrickMeta     | 4 words per slot: idle steps, alive, neighbour request mask, buckets       |
/// | Indirection   | the VDB brick table (RGBA16F): rgb = atlas brick / 255, a = brick mean      |
///
/// Every frame that runs ticks, Gas/Alloc frees idle slots and assigns free ones to the requests, deterministically
/// (design plan section 3): the pool never resizes, and the k-th request in bucket order, then virtual order, takes the
/// k-th free slot. The solver clears the new bricks (it owns the atlases), and GasFinalize marks the next frame's
/// requests.
///
/// GPU resources are created and destroyed on the render thread.
class PL_PARTICLEPLUGIN_DLL plGpuFluidSparseGrid
{
public:
  plGpuFluidSparseGrid();
  ~plGpuFluidSparseGrid();

  /// Pool slots are mapped to atlas bricks roughly cubically, never more than 204 per axis.
  static plVec3U32 ComputeAtlasBricks(plUInt32 uiCapacity);

  plResult Create(const plVec3U32& vBrickGrid, plUInt32 uiCapacity);
  void Destroy();

  bool IsCreated() const { return !m_hPageTable.IsInvalidated(); }

  /// True once the allocation passes were recorded; the solver may only run after that.
  bool IsAllocated() const { return m_bAllocated; }

  const plVec3U32& GetBrickGrid() const { return m_vBrickGrid; }
  const plVec3U32& GetAtlasBricks() const { return m_vAtlasBricks; }
  plUInt32 GetCapacity() const { return m_uiCapacity; }
  plUInt32 GetRequestWords() const { return m_uiRequestWords; }
  plGALTextureHandle GetIndirection() const { return m_hIndirection; }

  plGpuFluidSparseGridGraph Import(plRenderGraph& ref_graph) const;

  /// Phase Gas/Alloc: frees idle slots, assigns free ones to the requests, rebuilds the lists and the dispatch
  /// arguments, and writes the frame's allocation numbers into hStats (cleared before). The solver adds the second
  /// pass, which clears every new brick. The first call also initialises the grid.
  void AddAllocPasses(plRenderGraph& ref_graph, const plGpuFluidSparseGridGraph& grid, plRenderGraphBufferHandle hStats, const plGpuFluidAllocInput& input,
    plUInt64 uiFrame);

  /// The newest grid debug readback that has arrived since the last call, if any.
  bool PollDebug(plGpuFluidGridDebug& out_debug);

  /// Binds the read-only grid buffers every solver kernel includes (GpuFluidSparse.hlsli).
  static void BindReadOnly(const plRenderGraphContext& ctx, plBindGroupBuilder& ref_bindGroup, const plGpuFluidSparseGridGraph& grid);

  /// Declares the read-only grid buffers on a solver pass, including the dispatch arguments as indirect arguments.
  template <typename PassBuilder>
  static void DeclareReadOnly(PassBuilder& ref_pass, const plGpuFluidSparseGridGraph& grid);

private:
  plVec3U32 m_vBrickGrid = plVec3U32::MakeZero();
  plVec3U32 m_vAtlasBricks = plVec3U32::MakeZero();
  plUInt32 m_uiCapacity = 0;
  plUInt32 m_uiRequestWords = 0;
  bool m_bAllocated = false;
  bool m_bInitialised = false;

  plGALBufferHandle m_hPageTable;
  plGALBufferHandle m_hActiveList;
  plGALBufferHandle m_hBrickCoords;
  plGALBufferHandle m_hNeighbors;
  plGALBufferHandle m_hCounters;
  plGALBufferHandle m_hDispatchArgs;
  plGALBufferHandle m_hColourList;
  plGALBufferHandle m_hColourArgs;
  plGALBufferHandle m_hRequests;
  plGALBufferHandle m_hPending;
  plGALBufferHandle m_hRequestScan;
  plGALBufferHandle m_hRequestScratch;
  plGALBufferHandle m_hSlotScan;
  plGALBufferHandle m_hSlotScratch;
  plGALBufferHandle m_hFreeList;
  plGALBufferHandle m_hNewList;
  plGALBufferHandle m_hBrickMeta;
  plGALBufferHandle m_hEmitterBricks;
  plGALBufferHandle m_hGridDebug;
  plGALTextureHandle m_hIndirection;

  plGpuFluidScan::Layout m_RequestLayout;
  plGpuFluidScan::Layout m_SlotLayout;
  plGpuFluidScan m_Scan;

  plGpuFluidReadbackRing m_DebugReadback;
  /// Per readback slot: the frame it was recorded in, and whether it holds the virtual part.
  plUInt64 m_uiDebugFrames[plGpuFluidReadbackRing::s_uiSlotCount] = {};
  bool m_bDebugVirtualParts[plGpuFluidReadbackRing::s_uiSlotCount] = {};
  plDynamicArray<plUInt8> m_DebugScratch;

  plShaderResourceHandle m_hAllocShader;
};

template <typename PassBuilder>
void plGpuFluidSparseGrid::DeclareReadOnly(PassBuilder& ref_pass, const plGpuFluidSparseGridGraph& grid)
{
  ref_pass.ReadBuffer(grid.m_hPageTable, plGALResourceState::ComputeShaderRead);
  ref_pass.ReadBuffer(grid.m_hActiveList, plGALResourceState::ComputeShaderRead);
  ref_pass.ReadBuffer(grid.m_hBrickCoords, plGALResourceState::ComputeShaderRead);
  ref_pass.ReadBuffer(grid.m_hNeighbors, plGALResourceState::ComputeShaderRead);
  ref_pass.ReadBuffer(grid.m_hDispatchArgs, plGALResourceState::IndirectArgument | plGALResourceState::ComputeShaderRead);
  ref_pass.ReadBuffer(grid.m_hColourList, plGALResourceState::ComputeShaderRead);
  ref_pass.ReadBuffer(grid.m_hColourArgs, plGALResourceState::IndirectArgument | plGALResourceState::ComputeShaderRead);
  ref_pass.ReadBuffer(grid.m_hNewList, plGALResourceState::ComputeShaderRead);
}
