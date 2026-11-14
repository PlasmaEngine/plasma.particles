#pragma once

// Geometric multigrid on the pressure equation of LiquidRhs: per liquid cell, sum over its neighbours of
// (phi_i - phi_nb) = b, where
// - a liquid neighbour adds 1 to the diagonal and phi_nb to the sum;
// - an air neighbour (or an open domain side) is the free surface: ghost fluid with phi = 0 at a fraction theta of
//   the way, which adds 1 / theta to the diagonal (on the fine level theta per face from the level set, see
//   LiqAirInvTheta);
// - a solid neighbour or a wall adds nothing (Neumann).
// Coarse levels use the same stencil on their own cell types: a coarse cell is liquid when it has a liquid child and
// no air child, solid when it has neither, air otherwise (LiquidRestrict). Their right-hand side is the restricted
// residual.
//
// GpuFluidLiqPass: x level, y colour (red-black), z sweeps (coarsest). GpuFluidLiqPassF: x SOR factor, y scale of the
// restricted residual.

#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidCells.hlsli>

RWByteAddressBuffer GpuFluidLiqMgX;
RWByteAddressBuffer GpuFluidLiqMgB;
RWByteAddressBuffer GpuFluidLiqMgType;

ByteAddressBuffer GpuFluidLiqLevelRegion; // LiquidLevelRegion

uint MgLevel() { return LIQ_CONST(GpuFluidLiqPass).x; }

/// Whether cell c of a coarse level lies in the region the V-cycle works on (LiquidLevelRegion).
bool MgInRegion(uint level, int3 c)
{
  const uint offset = level * GPU_FLUID_LIQUID_REGION_STRIDE;
  const uint4 hi = GpuFluidLiqLevelRegion.Load4(offset + 32);
  return hi.w == 0 && all(c >= int3(GpuFluidLiqLevelRegion.Load3(offset + 16))) && all(c <= int3(hi.xyz));
}

/// The cell of a thread of a dense-level kernel dispatched over its level's region; false outside it.
bool MgRegionCell(uint level, uint3 dispatchThreadId, out int3 out_cell)
{
  const uint offset = level * GPU_FLUID_LIQUID_REGION_STRIDE;
  out_cell = int3(GpuFluidLiqLevelRegion.Load3(offset + 16) + dispatchThreadId);
  return MgInRegion(level, out_cell);
}
uint MgColour() { return LIQ_CONST(GpuFluidLiqPass).y; }
uint MgSweeps() { return LIQ_CONST(GpuFluidLiqPass).z; }
float MgOmega() { return LIQ_CONST(GpuFluidLiqPassF).x; }
float MgRestrictScale() { return LIQ_CONST(GpuFluidLiqPassF).y; }

/// Type of cell c of multigrid level `level` (0 the fine grid). Level 1 is restricted brick by brick from the active
/// fine bricks only (LiquidRestrictBricks), so its cells over an inactive fine brick are air (as the dense restriction
/// wrote them from the air its children read as) whatever an earlier substep left in them.
uint MgTypeAt(RWByteAddressBuffer types, int3 c, uint3 dims, uint level)
{
  if (level == 0)
    return LiqTypeAt(types, c, dims, true);
  if (!LiqInsideDims(c, dims))
    return GPU_FLUID_LIQUID_CELL_OUTSIDE;
  if (level == 1 && GpuFluidLiqBrickActive.Load(LiqBrickIndex(c * 2) * 4) == 0)
    return GPU_FLUID_LIQUID_CELL_AIR;
  return types.Load(LiqCellIndexIn(c, dims) * 4) & 3u;
}

/// Diagonal and the sum of the liquid neighbours' solution of cell c of level `level`.
void MgStencil(RWByteAddressBuffer types, RWByteAddressBuffer xs, int3 c, uint3 dims, uint level, out float out_diag, out float out_sum)
{
  const bool bFine = level == 0;
  // The fine level takes theta per face from the cell's type word (the level set's free surface); the coarse levels,
  // which only precondition, use the constant.
  const uint ownWord = bFine ? types.Load(LiqCellIndexIn(c, dims) * 4) : 0u;
  out_diag = 0.0;
  out_sum = 0.0;

  [unroll] for (uint n = 0; n < 6; ++n)
  {
    const int3 nb = c + LiqNeighbourOffset(n);
    const uint t = MgTypeAt(types, nb, dims, level);
    const float air = bFine ? LiqAirInvTheta(ownWord, n) : LiqInvTheta();
    if (t == GPU_FLUID_LIQUID_CELL_OUTSIDE)
    {
      if (LiqNeighbourSideOpen(n))
        out_diag += air;
    }
    else if (t == GPU_FLUID_LIQUID_CELL_LIQUID)
    {
      out_diag += 1.0;
      out_sum += asfloat(xs.Load(LiqCellIndexIn(nb, dims) * 4));
    }
    else if (t == GPU_FLUID_LIQUID_CELL_AIR)
    {
      out_diag += air;
    }
  }
}

/// b - A x at liquid cell c.
float MgResidual(RWByteAddressBuffer types, RWByteAddressBuffer xs, RWByteAddressBuffer bs, int3 c, uint3 dims, uint level)
{
  float diag, sum;
  MgStencil(types, xs, c, dims, level, diag, sum);
  const uint index = LiqCellIndexIn(c, dims);
  return asfloat(bs.Load(index * 4)) - (diag * asfloat(xs.Load(index * 4)) - sum);
}

/// One red-black Gauss-Seidel (SOR) update of cell c, when it has this pass's colour and is liquid.
void MgSmoothCell(int3 c, uint3 dims, uint level)
{
  if ((((uint)(c.x + c.y + c.z)) & 1u) != MgColour())
    return;

  const uint index = LiqCellIndexIn(c, dims);
  if (MgTypeAt(GpuFluidLiqMgType, c, dims, level) != GPU_FLUID_LIQUID_CELL_LIQUID)
    return;

  float diag, sum;
  MgStencil(GpuFluidLiqMgType, GpuFluidLiqMgX, c, dims, level, diag, sum);
  if (diag <= 0.0)
    return;

  const float x = asfloat(GpuFluidLiqMgX.Load(index * 4));
  const float gs = (asfloat(GpuFluidLiqMgB.Load(index * 4)) + sum) / diag;
  GpuFluidLiqMgX.Store(index * 4, asuint(lerp(x, gs, MgOmega())));
}
