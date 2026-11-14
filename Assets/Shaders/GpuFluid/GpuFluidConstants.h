#pragma once

// Shared between C++ and HLSL. C++ reaches it as <Shaders/GpuFluid/GpuFluidConstants.h> through the package's
// Assets include root, and finds the engine's macros through the SDK's Data/Base root.
//
// Only defines live here. Push constant blocks are in their own headers (GpuFluidAnalyticConstants.h,
// GpuFluidGasConstants.h), so a shader declares exactly the one block it uses.
#include <Shaders/Common/ConstantBufferMacros.h>

// The VDB brick layout (Shaders/Common/VDBVolume.hlsli): 8 cubed cells, one voxel of apron, stored 10 cubed. The sim's
// atlas is the march's atlas, so these must match VDB_BRICK_*.
#define GPU_FLUID_BRICK_SIZE 8
#define GPU_FLUID_BRICK_APRON 1
#define GPU_FLUID_BRICK_STORE 10
#define GPU_FLUID_BRICK_CELLS 512   // 8 cubed: interior cells of a brick, one thread each
#define GPU_FLUID_BRICK_TEXELS 1000 // 10 cubed: stored texels of a brick, apron included

// The brick table stores an atlas brick as rgb / 255 and the march decodes round(rgb * 255). 2048 texels per axis
// over 10 stored voxels gives 204 bricks per axis.
#define GPU_FLUID_MAX_ATLAS_BRICKS_PER_AXIS 204

// Virtual grid limit per axis, in bricks. The VDB resolution field packs 10 bits per axis (1023 voxels).
#define GPU_FLUID_MAX_BRICK_GRID_PER_AXIS 127

#define GPU_FLUID_INVALID_SLOT 0xFFFFFFFFu

// Neighbour table: 27 entries per pool slot, (dx+1) + 3 (dy+1) + 9 (dz+1), entry 13 being the brick itself. A missing
// neighbour is one of two values, because the boundary rules differ:
#define GPU_FLUID_NEIGHBOR_COUNT 27
#define GPU_FLUID_NEIGHBOR_SELF 13
#define GPU_FLUID_NEIGHBOR_UNALLOCATED 0xFFFFFFFFu // inside the domain, no slot: open air (scalars 0, ambient velocity, p = 0)
#define GPU_FLUID_NEIGHBOR_OUTSIDE 0xFFFFFFFEu     // beyond the domain: the face's open or closed rule applies

// Domain faces, as bits of the open-face mask. A closed face is a wall (no flow through it, Neumann pressure), an open
// face lets gas leave (zero-gradient velocity, scalars 0 outside, p = 0 outside).
#define GPU_FLUID_FACE_NEG_X 1
#define GPU_FLUID_FACE_POS_X 2
#define GPU_FLUID_FACE_NEG_Y 4
#define GPU_FLUID_FACE_POS_Y 8
#define GPU_FLUID_FACE_NEG_Z 16
#define GPU_FLUID_FACE_POS_Z 32

// Counters of the sparse grid (one uint buffer). The dispatch arguments are a separate buffer: (x, y, 1, count), with the
// count folded into 2D above GPU_FLUID_DISPATCH_FOLD groups, so every backend's 65535 limit holds.
#define GPU_FLUID_COUNTER_ACTIVE 0 // pool slots in the active list
#define GPU_FLUID_COUNTER_DENIED 1 // bricks that wanted a slot and found the pool full
#define GPU_FLUID_COUNTER_COLOUR0 2 // active bricks of brick colour 0 (even coordinate sum)
#define GPU_FLUID_COUNTER_COLOUR1 3 // active bricks of brick colour 1

// The active bricks split by brick colour (parity of the brick coordinate sum) for the multigrid's block red-black
// launches: colour 0 from the front of the colour list, colour 1 from its back. The colour dispatch arguments are two
// sets of GPU_FLUID_ARGS_WORDS: (x, y, 1, count) each.
#define GPU_FLUID_COUNTER_WORDS 16
#define GPU_FLUID_DISPATCH_FOLD 32768
#define GPU_FLUID_ARGS_WORDS 4 // uint3 groups + the active count

// Stats: one 256 byte buffer per volume, uint words, cleared at the start of every GPU frame and read back through a
// ring of 3. Counts and bounds are order-independent atomics, so the result does not depend on group order. Float
// maxima are stored as asuint of a non-negative float, which orders like the float.
#define GPU_FLUID_STAT_SLOTS_REQUESTED 0 // bricks that asked for an atlas slot; allocated = min(this, capacity)
#define GPU_FLUID_STAT_DENIED 1          // bricks that found the pool full
#define GPU_FLUID_STAT_NAN 2             // interior values that came out NaN or infinite, counted by the guard that zeroed them
#define GPU_FLUID_STAT_BOUNDS_MIN 3      // 3 words: smallest brick with content per axis (0xFFFFFFFF when none)
#define GPU_FLUID_STAT_BOUNDS_MAX 6      // 3 words: largest brick with content per axis
#define GPU_FLUID_STAT_MASS 9            // sum of interior density, fixed point GPU_FLUID_MASS_SCALE
#define GPU_FLUID_STAT_MAX_DENSITY 10    // asuint of the largest interior density
#define GPU_FLUID_STAT_MAX_TEMPERATURE 11 // asuint of the largest interior temperature (normalised)
#define GPU_FLUID_STAT_MAX_SPEED 12      // asuint of the largest face speed, m/s
#define GPU_FLUID_STAT_DIV_PRE 13        // asuint of max |div u| before the last tick's projection, 1/s
#define GPU_FLUID_STAT_DIV_POST 14       // asuint of max |div u| after it, 1/s
#define GPU_FLUID_STAT_CENTROID 15       // 3 words: sum of density * (cell centre / grid size), fixed point GPU_FLUID_MASS_SCALE
#define GPU_FLUID_STAT_CONTENT_BRICKS 18 // bricks whose peak is above the allocation threshold
#define GPU_FLUID_STAT_ACTIVE_BRICKS 19  // bricks the solver ran on
#define GPU_FLUID_STAT_DIV_PRE_SUM 20    // sum of |div u| before the last projection, fixed point GPU_FLUID_DIV_SCALE
#define GPU_FLUID_STAT_DIV_POST_SUM 21   // sum of |div u| after it, fixed point GPU_FLUID_DIV_SCALE
#define GPU_FLUID_STAT_OVERFLOW 22       // fixed-point contributions that were clamped to their range, or sums that wrapped
#define GPU_FLUID_STAT_SPEED_CLAMPED 23  // face velocities GasForces clamped to GPU_FLUID_MAX_SPEED (summed over the frame's ticks)
#define GPU_FLUID_STAT_SOLID_CELLS 24    // interior cells inside a solid (GasSolids, the frame's last tick)
#define GPU_FLUID_STAT_SOLID_CONTACT 25  // asuint of the largest density in a fluid cell next to a solid (GasSolids, the frame's last tick)
#define GPU_FLUID_STAT_SOLID_DENSITY 26  // asuint of the largest density left in a solid cell at the end of the frame (GasFinalize)
#define GPU_FLUID_STAT_PROBE 32          // 16 words of solver probes (GasPoisson): asuint of max |residual| or |error|
#define GPU_FLUID_STAT_PROBE_COUNT 16
#define GPU_FLUID_STAT_WORD_COUNT 64
#define GPU_FLUID_STATS_BYTES (GPU_FLUID_STAT_WORD_COUNT * 4)

#define GPU_FLUID_MASS_SCALE 256.0
#define GPU_FLUID_DIV_SCALE 16.0

// Fixed point of the per-brick mean density in groupshared memory: 512 cells of density <= 64 stay below 2^32.
#define GPU_FLUID_MEAN_SCALE 65536.0

// GasForces clamps every face velocity to this before the projection, and GasEmit every source velocity, so a blow-up
// stays finite. The projection can push a face past it again (about 60 m/s under Gas Safety's 48 m/s source); the next
// tick's GasForces clamps it back. Backtraces of any length are safe: GpuFluidLocate goes through the page table.
#define GPU_FLUID_MAX_SPEED 32.0
