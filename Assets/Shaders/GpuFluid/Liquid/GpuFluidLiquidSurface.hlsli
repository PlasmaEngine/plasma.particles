#pragma once

// Shared by the kernels of Liquid/Surface: the surface level set on the sim grid (one sample per cell centre, in cell
// units, negative inside the liquid) and the dual grid of surface-nets cubes. Cube m has the samples m .. m + 1 as its
// corners; cubes run from -1 to n - 1 per axis, so the ones on the domain's sides close the mesh against a sample
// outside the domain. Every sample of an inactive brick reads as air (the far value).
//
// Walls mirror the level set: a sample beyond a closed domain side, or inside an analytic solid, takes |phi| of the
// sample on the liquid's side of the wall. Where the liquid touches the wall the surface then crosses exactly half way,
// on the cell face, and its normal is the wall's: a wall face that followed the particles' noise would be a crumpled
// window into the liquid. Beyond an open side the sample is air (the outside value, GpuFluidLiqPassF.x).

#define GPU_FLUID_LIQUID_BRICK_DISPATCH
#define GPU_FLUID_LIQUID_USE_SHAPES
#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidCells.hlsli>

// Read and write views: every kernel of Liquid/Surface reads what an earlier kernel of the same pass wrote, which on
// D3D12 and Vulkan must stay in the unordered-access state.
RWByteAddressBuffer GpuFluidLiqSurfPhi;

float SurfOutsideValue() { return LIQ_CONST(GpuFluidLiqPassF).x; }
float SurfFarValue() { return LIQ_CONST(GpuFluidLiqPassF).y; }

/// The stored level set at sample s inside the domain (cell units); the far value in inactive bricks.
float SurfPhiStored(int3 s)
{
  if (GpuFluidLiqBrickActive.Load(LiqBrickIndex(s) * 4) == 0)
    return SurfFarValue();
  return asfloat(GpuFluidLiqSurfPhi.Load(LiqCellIndex(s) * 4));
}

bool SurfInSolid(int3 s)
{
  return LiqHasSolids() && LiqSolidDistanceAtCell(s) < 0.0;
}

/// The surface level set at sample s (cell units), mirrored at walls and solids.
float SurfPhi(int3 s)
{
  if (!LiqInside(s))
  {
    // Beyond a closed side: the mirror of the sample just inside. Beyond an open one: air.
    const int3 inside = clamp(s, int3(0, 0, 0), int3(LiqCells()) - 1);
    const int3 d = s - inside;
    const uint n = d.x != 0 ? (d.x > 0 ? 1u : 0u) : (d.y != 0 ? (d.y > 0 ? 3u : 2u) : (d.z > 0 ? 5u : 4u));
    if (LiqNeighbourSideOpen(n))
      return SurfOutsideValue();
    return SurfInSolid(inside) ? SurfFarValue() : abs(SurfPhiStored(inside));
  }

  if (SurfInSolid(s))
  {
    // The neighbour across the solid's nearest face, along the axis its normal mostly points to.
    const float3 normal = LiqShapeNormal((float3(s) + 0.5) * LiqDx());
    const float3 a = abs(normal);
    int3 step = int3(0, 0, 0);
    if (a.x >= a.y && a.x >= a.z)
      step.x = normal.x > 0.0 ? 1 : -1;
    else if (a.y >= a.z)
      step.y = normal.y > 0.0 ? 1 : -1;
    else
      step.z = normal.z > 0.0 ? 1 : -1;
    const int3 m = s + step;
    if (!LiqInside(m) || SurfInSolid(m))
      return SurfFarValue();
    return abs(SurfPhiStored(m));
  }

  return SurfPhiStored(s);
}

uint3 SurfCubeDims() { return LiqCells() + 1; }

/// Index of cube m (m from -1 to n - 1 per axis) in the cube-to-vertex map.
uint SurfCubeIndex(int3 m)
{
  const uint3 d = SurfCubeDims();
  const uint3 u = uint3(m + 1);
  return u.x + d.x * (u.y + d.y * u.z);
}

bool SurfCubeExists(int3 m)
{
  return all(m >= -1) && all(m < int3(LiqCells()));
}
