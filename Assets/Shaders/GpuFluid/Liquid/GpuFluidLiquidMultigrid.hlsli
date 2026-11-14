#pragma once

// Geometric multigrid on the pressure equation of LiquidRhs: per liquid cell, sum over its neighbours of
// (phi_i - phi_nb) = b, where
// - a liquid neighbour adds 1 to the diagonal and phi_nb to the sum;
// - an air neighbour (or an open domain side) is the free surface: ghost fluid with phi = 0 at a fraction theta of
//   the way, which adds 1 / theta to the diagonal;
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

uint MgLevel() { return LIQ_CONST(GpuFluidLiqPass).x; }
uint MgColour() { return LIQ_CONST(GpuFluidLiqPass).y; }
uint MgSweeps() { return LIQ_CONST(GpuFluidLiqPass).z; }
float MgOmega() { return LIQ_CONST(GpuFluidLiqPassF).x; }
float MgRestrictScale() { return LIQ_CONST(GpuFluidLiqPassF).y; }

/// Diagonal and the sum of the liquid neighbours' solution of cell c.
void MgStencil(RWByteAddressBuffer types, RWByteAddressBuffer xs, int3 c, uint3 dims, bool bFine, out float out_diag, out float out_sum)
{
  const float air = LiqInvTheta();
  out_diag = 0.0;
  out_sum = 0.0;

  [unroll] for (uint n = 0; n < 6; ++n)
  {
    const int3 nb = c + LiqNeighbourOffset(n);
    const uint t = LiqTypeAt(types, nb, dims, bFine);
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
float MgResidual(RWByteAddressBuffer types, RWByteAddressBuffer xs, RWByteAddressBuffer bs, int3 c, uint3 dims, bool bFine)
{
  float diag, sum;
  MgStencil(types, xs, c, dims, bFine, diag, sum);
  const uint index = LiqCellIndexIn(c, dims);
  return asfloat(bs.Load(index * 4)) - (diag * asfloat(xs.Load(index * 4)) - sum);
}

/// One red-black Gauss-Seidel (SOR) update of cell c, when it has this pass's colour and is liquid.
void MgSmoothCell(int3 c, uint3 dims, bool bFine)
{
  if ((((uint)(c.x + c.y + c.z)) & 1u) != MgColour())
    return;

  const uint index = LiqCellIndexIn(c, dims);
  if ((GpuFluidLiqMgType.Load(index * 4) & 3u) != GPU_FLUID_LIQUID_CELL_LIQUID)
    return;

  float diag, sum;
  MgStencil(GpuFluidLiqMgType, GpuFluidLiqMgX, c, dims, bFine, diag, sum);
  if (diag <= 0.0)
    return;

  const float x = asfloat(GpuFluidLiqMgX.Load(index * 4));
  const float gs = (asfloat(GpuFluidLiqMgB.Load(index * 4)) + sum) / diag;
  GpuFluidLiqMgX.Store(index * 4, asuint(lerp(x, gs, MgOmega())));
}
