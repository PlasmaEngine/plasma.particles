#pragma once

#include <Shaders/GpuFluid/GpuFluidSparse.hlsli>

// Pressure of the gas solver: Phi = p dt / rho in m^2/s, one float per interior cell (512 per pool slot, no apron), so
// the projection is u -= grad Phi. Boundary rules, per neighbour of a cell:
// - in the same brick or an allocated neighbour brick: its Phi;
// - an unallocated brick (open air inside the domain): Dirichlet, Phi = 0;
// - beyond an open domain face: Dirichlet, Phi = 0;
// - beyond a closed domain face: Neumann, the neighbour does not take part (and the face between carries no flow).

/// Phi of the neighbour of cell `local` (0..7) of `slot` along `axis` on `side`. Returns false for a Neumann neighbour.
bool GpuFluidNeighborPressure(RWStructuredBuffer<float> pressure, uint slot, int3 local, uint axis, int side, out float out_phi)
{
  int3 other = local;
  other[axis] += side;

  out_phi = 0.0;

  if (other[axis] >= 0 && other[axis] < GPU_FLUID_BRICK_SIZE)
  {
    out_phi = pressure[slot * GPU_FLUID_BRICK_CELLS + GpuFluidCellIndex(uint3(other))];
    return true;
  }

  int3 d = int3(0, 0, 0);
  d[axis] = side;
  const uint neighbor = GpuFluidNeighbor(slot, d);

  if (neighbor == GPU_FLUID_NEIGHBOR_OUTSIDE)
    return GpuFluidFaceOpen(axis, side); // open: Dirichlet 0, closed: Neumann

  if (neighbor == GPU_FLUID_NEIGHBOR_UNALLOCATED)
    return true; // open air

  other[axis] -= side * GPU_FLUID_BRICK_SIZE;
  out_phi = pressure[neighbor * GPU_FLUID_BRICK_CELLS + GpuFluidCellIndex(uint3(other))];
  return true;
}
