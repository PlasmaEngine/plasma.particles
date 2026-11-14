#pragma once

#include <Shaders/GpuFluid/GpuFluidConstants.h>

// Kernel modes (GpuFluidGasPass.x).
#define GPU_FLUID_HALO_SCALAR 0          // scalars: neighbour interiors, 0 in open air and beyond open faces, zero gradient at closed faces
#define GPU_FLUID_HALO_VELOCITY 1        // MAC velocity: neighbour interiors, ambient in open air, zero gradient beyond open faces, no flow through closed ones
#define GPU_FLUID_HALO_VELOCITY_KEEP 2   // after the projection: neighbour interiors only, boundary faces keep what the projection wrote

#define GPU_FLUID_EMITTER_SPHERE 0
#define GPU_FLUID_EMITTER_BOX 1

/// Per dispatch data of every gas solver kernel. Push constants, because render-graph passes are recorded before they
/// execute and a shared constant buffer would be last-write-wins. 128 bytes, the Metal limit.
///
/// Units: positions in voxels of the virtual grid (cell (i,j,k) spans [i, i+1)), velocity in m/s, time in seconds.
/// Temperature is normalised to [0, 1] of the preset's maximum.
BEGIN_PUSH_CONSTANTS(plGpuFluidGasConstants)
{
  UINT4(GpuFluidGasGrid);    // xyz: bricks of the virtual grid, w: pool capacity in bricks
  UINT4(GpuFluidGasAtlas);   // xyz: atlas bricks per axis, w: open-face mask (GPU_FLUID_FACE_*)
  UINT4(GpuFluidGasPass);    // x: mode, y: red-black colour or emitter shape, z: 1 on the frame's last tick, w: unused
  FLOAT4(GpuFluidGasStep);   // x: dt, y: voxel size in metres, z: 1 / voxel size, w: simulation time
  FLOAT4(GpuFluidGasParams0); // kernel specific, see the kernel
  FLOAT4(GpuFluidGasParams1);
  FLOAT4(GpuFluidGasParams2);
  FLOAT4(GpuFluidGasParams3);
}
END_PUSH_CONSTANTS(plGpuFluidGasConstants)

// What the kernels put in Params0..3:
//
// GasEmit:     0: centre xyz (voxels), radius (voxels, sphere)     1: box half extents xyz (voxels), edge softness (voxels)
//              2: density rate (1/s), temperature, fuel, noise strength (0..1)
//              3: velocity xyz (m/s), noise frequency (1/voxel)
// GasForces:   0: buoyancy direction xyz (unit, against gravity), buoyancy per unit temperature (m/s^2)
//              1: x weight per unit density (m/s^2), y ambient temperature, z cooling rate (1/s), w density dissipation (1/s)
//              2: x density clamp, y velocity dissipation (1/s), z fuel dissipation (1/s), w edge fade width (voxels)
//              3: x edge fade rate at an open face (1/s)
// GasPressure: 0: x over-relaxation
// GasFinalize: 0: x allocation threshold, y brick mean floor
