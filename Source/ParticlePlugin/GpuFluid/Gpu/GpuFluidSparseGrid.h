#pragma once

#include <Foundation/Math/Vec3.h>
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
  plRenderGraphTextureHandle m_hIndirection;
};

/// \brief The brick bookkeeping of a sparse gas volume: which virtual brick owns which pool slot, and the lists every
/// solver kernel dispatches over.
///
/// | Buffer        | Content                                                                    |
/// |---------------|----------------------------------------------------------------------------|
/// | PageTable     | virtual brick -> pool slot or 0xFFFFFFFF                                   |
/// | ActiveList    | active index -> pool slot                                                  |
/// | BrickCoords   | pool slot -> packed virtual brick                                          |
/// | Neighbors     | 27 per pool slot: slot, or "unallocated" / "outside the domain"            |
/// | Counters      | active count, denied count                                                 |
/// | DispatchArgs  | groups x, y, z for "one group per active brick" (2D fold) and the count    |
/// | Indirection   | the VDB brick table (RGBA16F): rgb = atlas brick / 255, a = brick mean      |
///
/// P1 allocates the whole box once (Gas/Alloc on the first frame). P2 replaces only the assign step with the
/// mark / scan / assign allocator; the list rebuild, the clear of new bricks and every consumer stay as they are.
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
  plGALTextureHandle GetIndirection() const { return m_hIndirection; }

  plGpuFluidSparseGridGraph Import(plRenderGraph& ref_graph) const;

  /// Phase Gas/Alloc: assigns slots, rebuilds the lists and the dispatch arguments. The solver adds the second pass,
  /// which clears every new brick (it owns the atlases).
  void AddAllocPasses(plRenderGraph& ref_graph, const plGpuFluidSparseGridGraph& grid, plUInt8 uiOpenFaces);

  /// Binds the read-only grid buffers every solver kernel includes (GpuFluidSparse.hlsli).
  static void BindReadOnly(const plRenderGraphContext& ctx, plBindGroupBuilder& ref_bindGroup, const plGpuFluidSparseGridGraph& grid);

  /// Declares the read-only grid buffers on a solver pass, including the dispatch arguments as indirect arguments.
  template <typename PassBuilder>
  static void DeclareReadOnly(PassBuilder& ref_pass, const plGpuFluidSparseGridGraph& grid);

private:
  plVec3U32 m_vBrickGrid = plVec3U32::MakeZero();
  plVec3U32 m_vAtlasBricks = plVec3U32::MakeZero();
  plUInt32 m_uiCapacity = 0;
  bool m_bAllocated = false;

  plGALBufferHandle m_hPageTable;
  plGALBufferHandle m_hActiveList;
  plGALBufferHandle m_hBrickCoords;
  plGALBufferHandle m_hNeighbors;
  plGALBufferHandle m_hCounters;
  plGALBufferHandle m_hDispatchArgs;
  plGALTextureHandle m_hIndirection;

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
}
