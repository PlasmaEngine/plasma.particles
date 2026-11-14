#pragma once

// Cell-type lookups of the grid kernels. A cell of a brick that is not active this substep was not classified, so it
// reads as air: no liquid cell is ever next to one (the active set is the occupied bricks plus one around).

#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidCommon.hlsli>

ByteAddressBuffer GpuFluidLiqBrickActive;

#define GPU_FLUID_LIQUID_CELL_OUTSIDE 3u

/// Type of cell c of a level with the given dims; GPU_FLUID_LIQUID_CELL_OUTSIDE past the domain. On the fine level
/// (bFine) cells of inactive bricks read as air.
uint LiqTypeAt(RWByteAddressBuffer types, int3 c, uint3 dims, bool bFine)
{
  if (!LiqInsideDims(c, dims))
    return GPU_FLUID_LIQUID_CELL_OUTSIDE;
  if (bFine && GpuFluidLiqBrickActive.Load(LiqBrickIndex(c) * 4) == 0)
    return GPU_FLUID_LIQUID_CELL_AIR;
  return types.Load(LiqCellIndexIn(c, dims) * 4) & 3u;
}

/// The whole type word of fine cell c (type bits and the theta codes), or air past the domain and in inactive bricks.
uint LiqTypeWordAt(RWByteAddressBuffer types, int3 c)
{
  if (!LiqInside(c) || GpuFluidLiqBrickActive.Load(LiqBrickIndex(c) * 4) == 0)
    return GPU_FLUID_LIQUID_CELL_AIR;
  return types.Load(LiqCellIndex(c) * 4);
}

/// 1 / theta of the free surface between a liquid cell with type word typeWord and its air neighbour n (see
/// GPU_FLUID_LIQUID_THETA_SHIFT). The stencil, the residual and the projection all take it from here, so they agree.
float LiqAirInvTheta(uint typeWord, uint n)
{
  const uint code = (typeWord >> (GPU_FLUID_LIQUID_THETA_SHIFT + GPU_FLUID_LIQUID_THETA_BITS * n)) & ((1u << GPU_FLUID_LIQUID_THETA_BITS) - 1u);
  if (code == GPU_FLUID_LIQUID_THETA_CONSTANT)
    return LiqInvTheta();
  return GPU_FLUID_LIQUID_THETA_STEPS / float(max(code, 1u));
}

/// Neighbour n (0..5: -x, +x, -y, +y, -z, +z) of a cell.
int3 LiqNeighbourOffset(uint n)
{
  return LiqAxis(n >> 1) * ((n & 1u) != 0 ? 1 : -1);
}

/// Whether the domain side neighbour n leads to is open (air outside) rather than a wall.
bool LiqNeighbourSideOpen(uint n)
{
  const uint side = (1u << (n & 1u)) << ((n >> 1) * 2);
  return (LiqOpenMask() & side) != 0;
}

/// Cells per axis of multigrid level l: the fine grid halved l times, rounding up.
uint3 LiqLevelDims(uint level)
{
  const uint3 cells = LiqCells();
  const uint scale = 1u << level;
  return (cells + scale - 1) >> level;
}
