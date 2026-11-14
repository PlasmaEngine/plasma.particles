#pragma once

// Shared between C++ and HLSL: the flood basin's wave surface (BasinWave, BasinImpulse, BasinMesh).
//
// The surface is a grid of columns over the basin's floor plan, each holding the height of the water above the basin's
// level. The columns' water depth (from the CPU's solid profile) is uploaded when the level moves; a depth of 0 or less
// is a column the water does not reach at this level (a wall, furniture, a raised floor), which reflects waves.

#include <Shaders/Common/ConstantBufferMacros.h>

#define GPU_FLUID_BASIN_GROUP 8

// How far (m) the surface sinks below the level at corners no water column shares (BasinMesh).
#define GPU_FLUID_BASIN_DRY_SINK 0.1

// The mesh the liquid surface pass draws (the liquid's own layout): per vertex float4 position (relative to the
// basin's minimum corner), float4 normal; per quad 4 vertex indices; the draw arguments mono then stereo.
#define GPU_FLUID_BASIN_VERTEX_BYTES 32
#define GPU_FLUID_BASIN_QUAD_BYTES 16

BEGIN_PUSH_CONSTANTS(plGpuFluidBasinConstants)
{
  UINT4(GpuFluidBasinDims);     // xy: columns, zw: first column of the impulse's square (BasinImpulse)
  FLOAT4(GpuFluidBasinGrid);    // x: column size (m), y: 1 / column size, z: step dt (s), w: damping per step
  FLOAT4(GpuFluidBasinLevel);   // x: water level above the basin's floor (m), y: gravity (m/s^2), z: largest wave height (m), w: largest depth the wave speed uses (m)
  FLOAT4(GpuFluidBasinImpulse); // xy: centre (m, from the basin's corner), z: radius (m), w: height added at the centre (m)
}
END_PUSH_CONSTANTS(plGpuFluidBasinConstants)

#define BASIN_CONST(Name) GET_PUSH_CONSTANT(plGpuFluidBasinConstants, Name)
