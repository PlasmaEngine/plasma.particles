#pragma once

#include <Shaders/GpuFluid/GpuFluidMG.hlsli>

// The exact MAC divergence of an interior cell, (u[i+1] - u[i] + v[j+1] - v[j] + w[k+1] - w[k]) / dx, the +1 faces of a
// brick's last cells coming from the apron, and its stats. Shared by GasDivergence and the multigrid's first fine
// launches (GasMGFine), which take it on the way instead of a launch of its own.
//
// On the frame's last tick the largest and the summed |div| go to the stats, the "before" half of the projection check.
// Every tick, a non-finite divergence is counted in GPU_FLUID_STAT_NAN before it is replaced by 0, and a fixed-point
// contribution that had to be clamped, or a sum that wrapped, in GPU_FLUID_STAT_OVERFLOW. A solid cell gets 0.
//
// Use: GpuFluidDivergenceBegin, a group barrier, GpuFluidDivergenceOfCell per cell, a group barrier, GpuFluidDivergenceEnd.

groupshared uint gs_uiDivMax;
groupshared uint gs_uiDivSum;
groupshared uint gs_uiDivNaN;
groupshared uint gs_uiDivOverflow;

void GpuFluidDivergenceBegin(uint groupIndex)
{
  if (groupIndex == 0)
  {
    gs_uiDivMax = 0;
    gs_uiDivSum = 0;
    gs_uiDivNaN = 0;
    gs_uiDivOverflow = 0;
  }
}

float GpuFluidDivergenceOfCell(RWTexture3D<float4> velocity, int3 texel, float invDx, bool bSolid, bool bLastTick)
{
  const float3 minus = velocity[texel].xyz;
  const float plusX = velocity[texel + int3(1, 0, 0)].x;
  const float plusY = velocity[texel + int3(0, 1, 0)].y;
  const float plusZ = velocity[texel + int3(0, 0, 1)].z;

  float divergence = (plusX - minus.x + plusY - minus.y + plusZ - minus.z) * invDx;
  if (bSolid)
    divergence = 0.0; // inside a solid: not part of the solve, and its faces move with the solid
  if (GpuFluidIsNonFinite(divergence))
  {
    InterlockedAdd(gs_uiDivNaN, 1u); // counted before it is dropped, like every other guard
    divergence = 0.0;
  }

  if (bLastTick)
  {
    // 512 cells of at most 2^20 each stay below 2^32 in groupshared memory.
    const float scaled = round(abs(divergence) * GPU_FLUID_DIV_SCALE);
    if (scaled > 1048576.0)
      InterlockedAdd(gs_uiDivOverflow, 1u);
    InterlockedMax(gs_uiDivMax, asuint(abs(divergence)));
    InterlockedAdd(gs_uiDivSum, (uint)min(scaled, 1048576.0));
  }
  return divergence;
}

void GpuFluidDivergenceEnd(RWByteAddressBuffer stats, uint groupIndex, bool bLastTick)
{
  if (groupIndex != 0)
    return;

  uint overflow = gs_uiDivOverflow;
  if (bLastTick)
  {
    stats.InterlockedMax(GPU_FLUID_STAT_DIV_PRE * 4, gs_uiDivMax);
    GpuFluidStatAdd(stats, GPU_FLUID_STAT_DIV_PRE_SUM, gs_uiDivSum, overflow);
  }
  if (gs_uiDivNaN > 0)
    stats.InterlockedAdd(GPU_FLUID_STAT_NAN * 4, gs_uiDivNaN);
  if (overflow > 0)
    stats.InterlockedAdd(GPU_FLUID_STAT_OVERFLOW * 4, overflow);
}
