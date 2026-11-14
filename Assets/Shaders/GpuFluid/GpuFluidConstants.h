#pragma once

// Shared between C++ and HLSL. C++ reaches it as <Shaders/GpuFluid/GpuFluidConstants.h> through the package's
// Assets include root, and finds the engine's macros through the SDK's Data/Base root.
#include <Shaders/Common/ConstantBufferMacros.h>

// The VDB brick layout (Shaders/Common/VDBVolume.hlsli): 8 cubed cells, one voxel of apron, stored 10 cubed. The sim's
// atlas is the march's atlas, so these must match VDB_BRICK_*.
#define GPU_FLUID_BRICK_SIZE 8
#define GPU_FLUID_BRICK_APRON 1
#define GPU_FLUID_BRICK_STORE 10

// The brick table stores an atlas brick as rgb / 255 and the march decodes round(rgb * 255). 2048 texels per axis
// over 10 stored voxels gives 204 bricks per axis.
#define GPU_FLUID_MAX_ATLAS_BRICKS_PER_AXIS 204

// Virtual grid limit per axis, in bricks. The VDB resolution field packs 10 bits per axis (1023 voxels).
#define GPU_FLUID_MAX_BRICK_GRID_PER_AXIS 127

#define GPU_FLUID_INVALID_SLOT 0xFFFFFFFFu

// Stats: one 256 byte buffer per volume, uint words, cleared at the start of every GPU frame and read back through a
// ring of 3. Counts and bounds are order-independent atomics, so the result does not depend on group order.
#define GPU_FLUID_STAT_SLOTS_REQUESTED 0 // bricks that asked for an atlas slot; allocated = min(this, capacity)
#define GPU_FLUID_STAT_DENIED 1          // bricks that found the pool full
#define GPU_FLUID_STAT_NAN 2             // texels that came out NaN or infinite and were zeroed
#define GPU_FLUID_STAT_BOUNDS_MIN 3      // 3 words: smallest allocated brick per axis (0xFFFFFFFF when none)
#define GPU_FLUID_STAT_BOUNDS_MAX 6      // 3 words: largest allocated brick per axis
#define GPU_FLUID_STAT_MASS 9            // sum of interior density of allocated bricks, fixed point GPU_FLUID_MASS_SCALE
#define GPU_FLUID_STAT_MAX_DENSITY 10    // asuint of the largest interior density
#define GPU_FLUID_STAT_WORD_COUNT 64
#define GPU_FLUID_STATS_BYTES (GPU_FLUID_STAT_WORD_COUNT * 4)

#define GPU_FLUID_MASS_SCALE 256.0

// Fixed point of the per-brick mean density in groupshared memory: 512 cells of density <= 64 stay below 2^32.
#define GPU_FLUID_MEAN_SCALE 65536.0

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
