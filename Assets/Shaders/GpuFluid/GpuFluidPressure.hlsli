#pragma once

#include <Shaders/GpuFluid/GpuFluidMG.hlsli>

// Pressure of the gas solver: Phi = p dt / rho in m^2/s, one float per interior cell (512 per pool slot, no apron), so
// the projection is u -= grad Phi. Which neighbours take part is in the face codes GasMGSetup writes every tick
// (GPU_FLUID_CODE_*, GpuFluidMG.hlsli):
// - FLUID: a fluid cell in the same brick or an allocated neighbour brick: its Phi;
// - DIRICHLET: an unallocated brick (open air inside the domain) or beyond an open domain face: Phi = 0;
// - WALL: beyond a closed domain face: Neumann, the face carries no flow;
// - SOLID: either cell is inside a solid: Neumann, the face keeps the solid's velocity.

/// Phi of the neighbour of cell `local` (0..7) of `slot` along `axis` on `side`, for a FLUID face.
float GpuFluidNeighborPhi(RWStructuredBuffer<float> pressure, uint slot, int3 local, uint axis, int side)
{
  int3 other = local;
  other[axis] += side;

  if (other[axis] >= 0 && other[axis] < GPU_FLUID_BRICK_SIZE)
    return pressure[slot * GPU_FLUID_BRICK_CELLS + GpuFluidCellIndex(uint3(other))];

  int3 d = int3(0, 0, 0);
  d[axis] = side;
  const uint neighbor = GpuFluidNeighbor(slot, d);
  if (neighbor >= GPU_FLUID_NEIGHBOR_OUTSIDE)
    return 0.0; // not a FLUID face; kept for safety

  other[axis] -= side * GPU_FLUID_BRICK_SIZE;
  return pressure[neighbor * GPU_FLUID_BRICK_CELLS + GpuFluidCellIndex(uint3(other))];
}
