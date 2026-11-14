#pragma once

#include <Shaders/GpuFluid/GpuFluidConstants.h>

/// Per dispatch data of the analytic debug sources (P0: a pulsing sphere). Push constants, because render-graph
/// passes are recorded before they execute and a shared constant buffer would be last-write-wins.
BEGIN_PUSH_CONSTANTS(plGpuFluidAnalyticConstants)
{
  UINT4(GpuFluidBrickGrid);     // xyz: bricks of the virtual grid, w: pool capacity in bricks
  UINT4(GpuFluidAtlasBricks);   // xyz: atlas bricks per axis, w: unused
  FLOAT4(GpuFluidSphere);       // xyz: centre in voxels of the virtual grid, w: radius in voxels
  FLOAT4(GpuFluidSphereShape);  // x: edge softness in voxels, y: peak density, z: peak temperature (0..1), w: unused
  FLOAT4(GpuFluidBrickRules);   // x: allocation threshold (peak density), y: brick mean floor, zw: unused
}
END_PUSH_CONSTANTS(plGpuFluidAnalyticConstants)
