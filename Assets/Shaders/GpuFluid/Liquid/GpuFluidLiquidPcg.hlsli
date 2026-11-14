#pragma once

// MGPCG on the pressure equation of GpuFluidLiquidMultigrid.hlsli: conjugate gradients on the liquid cells of the
// fine level, preconditioned by one multigrid V-cycle (symmetric: the post-smoothing runs the colours in the reverse
// order of the pre-smoothing, the coarsest sweeps alternate their order, restriction is a multiple of the transpose
// of the piecewise-constant prolongation). Dot products are reduced in a fixed order: per active brick in
// groupshared memory into a slot of that brick, then by one group over the bricks in index order
// (LiquidPcgReduce), so the result does not depend on the order of the brick list. The scalars stay on the GPU.

#define GPU_FLUID_LIQUID_BRICK_DISPATCH
#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidMultigrid.hlsli>

#define GPU_FLUID_LIQUID_BRICK_CELLS (GPU_FLUID_LIQUID_BRICK * GPU_FLUID_LIQUID_BRICK * GPU_FLUID_LIQUID_BRICK)

RWByteAddressBuffer GpuFluidLiqPcgScalars;
RWByteAddressBuffer GpuFluidLiqPcgPartials; // one float per brick (by brick index)

groupshared float gs_fPcgSum[GPU_FLUID_LIQUID_BRICK_CELLS];

float PcgScalar(uint word) { return asfloat(GpuFluidLiqPcgScalars.Load(word * 4)); }

bool PcgIsLiquid(int3 cell)
{
  return (GpuFluidLiqMgType.Load(LiqCellIndex(cell) * 4) & 3u) == GPU_FLUID_LIQUID_CELL_LIQUID;
}

/// Sums value over the group (fixed tree order) and stores it as this brick's partial. Every thread of the group
/// calls it; bCell is false for the threads of a group past the end of the brick list, which then stores nothing.
void PcgStorePartial(uint groupIndex, bool bCell, int3 cell, float value)
{
  gs_fPcgSum[groupIndex] = bCell ? value : 0.0;
  GroupMemoryBarrierWithGroupSync();

  [unroll] for (uint stride = GPU_FLUID_LIQUID_BRICK_CELLS / 2; stride > 0; stride >>= 1)
  {
    if (groupIndex < stride)
      gs_fPcgSum[groupIndex] += gs_fPcgSum[groupIndex + stride];
    GroupMemoryBarrierWithGroupSync();
  }

  if (groupIndex == 0 && bCell)
    GpuFluidLiqPcgPartials.Store(LiqBrickIndex(cell) * 4, asuint(gs_fPcgSum[0]));
}
