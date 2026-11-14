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
#define GPU_FLUID_COUNTER_NEW 4     // slots assigned this frame (the new list, cleared by GasClear)
#define GPU_FLUID_COUNTER_FREED 5   // slots freed this frame (idle for GPU_FLUID_FREE_IDLE_STEPS)
#define GPU_FLUID_COUNTER_DENIED_BUCKET 8 // 4 words: denied requests per priority bucket
#define GPU_FLUID_COUNTER_REQUESTS_BUCKET 12 // 4 words: requests for bricks without a slot per priority bucket

// The active bricks split by brick colour (parity of the brick coordinate sum) for the multigrid's block red-black
// launches: colour 0 from the front of the colour list, colour 1 from its back. The colour dispatch arguments are two
// sets of GPU_FLUID_ARGS_WORDS: (x, y, 1, count) each.
#define GPU_FLUID_COUNTER_WORDS 16
#define GPU_FLUID_DISPATCH_FOLD 32768
#define GPU_FLUID_ARGS_WORDS 4 // uint3 groups + the active count

// The dispatch arguments buffer holds four sets of GPU_FLUID_ARGS_WORDS: every active brick (one group each), every
// active brick in GPU_FLUID_SLABS slabs (z = slabs), the bricks assigned this frame (the new list), and the new list in
// slabs.
#define GPU_FLUID_ARGS_ACTIVE 0
#define GPU_FLUID_ARGS_SLABS 4
#define GPU_FLUID_ARGS_NEW 8
#define GPU_FLUID_ARGS_NEW_SLABS 12
#define GPU_FLUID_ARGS_TOTAL_WORDS 16

// Sparse allocation (GasAlloc, GasFinalize; design plan section 3). Requests are one bit per virtual brick in each of
// four priority buckets, bucket b's words at b * (words per bucket). Slots go to requests in bucket order, then virtual
// index, so the pool denies bucket 3 first, then 2, and bucket 0 only once it is exhausted.
#define GPU_FLUID_ALLOC_BUCKETS 4
#define GPU_FLUID_BUCKET_EMITTER 0   // the brick overlaps a source this frame (CPU-binned list)
#define GPU_FLUID_BUCKET_ALIVE 1     // the brick itself holds content
#define GPU_FLUID_BUCKET_FLUX 2      // content of a neighbour moves into it within the lookahead
#define GPU_FLUID_BUCKET_PROXIMITY 3 // content of a neighbour lies within GPU_FLUID_ALLOC_PROXIMITY voxels of the shared boundary
#define GPU_FLUID_ALLOC_PROXIMITY 2
#define GPU_FLUID_FREE_IDLE_STEPS 8  // allocation steps without any request before a slot is freed
#define GPU_FLUID_MAX_EMITTER_BRICKS 8192

// Per-slot meta (uint4, GpuFluidBrickMeta): x allocation steps without a request, y 1 when the last Finalize saw content,
// z the 27-bit mask of neighbours the last Finalize requested (bit = neighbour index), w the buckets that requested
// the brick in the last allocation (bit per bucket; the brick debug view's colour).
#define GPU_FLUID_META_IDLE 0
#define GPU_FLUID_META_ALIVE 1
#define GPU_FLUID_META_MASK 2
#define GPU_FLUID_META_BUCKETS 3

// The grid debug readback (N1 and the brick view): GPU_FLUID_COUNTER_WORDS counters, then per pool slot (packed brick or
// GPU_FLUID_INVALID_SLOT, meta x, y, z, w), then the active list (capacity words), then per virtual brick (page table,
// asuint of the brick table's A) when the virtual part is asked for.
#define GPU_FLUID_DEBUG_SLOT_WORDS 5

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
#define GPU_FLUID_STAT_SOLID_CLEARED 27  // density GasSolids took out of interior solid cells, fixed point GPU_FLUID_MASS_SCALE (summed over the frame's ticks)
#define GPU_FLUID_STAT_EMITTED 28        // density the sources added to interior cells, fixed point GPU_FLUID_MASS_SCALE (summed over the frame's ticks)
#define GPU_FLUID_STAT_PROBE 32          // 16 words of solver probes (GasPoisson): asuint of max |residual| or |error|
#define GPU_FLUID_STAT_PROBE_COUNT 16
#define GPU_FLUID_STAT_ALLOC_NEW 48      // slots assigned this frame
#define GPU_FLUID_STAT_ALLOC_FREED 49    // slots freed this frame
#define GPU_FLUID_STAT_ALLOC_FREE 50     // free slots before this frame's assignment
#define GPU_FLUID_STAT_ALLOC_REQUESTS 51 // requests for bricks without a slot this frame
#define GPU_FLUID_STAT_DENIED_BUCKET 52  // 4 words: denied requests per bucket
#define GPU_FLUID_STAT_ALLOC_ACTIVE 56   // active bricks after this frame's allocation
#define GPU_FLUID_STAT_REQUESTS_BUCKET 59 // 4 words: requests for bricks without a slot per bucket
#define GPU_FLUID_STAT_MASS_ADVECT 64     // 4 words per tick of the frame: interior density before (low, high word) and after (low, high) the tick's advection, 64-bit fixed point GPU_FLUID_MEAN_SCALE (the closed-box mass correction)
#define GPU_FLUID_STAT_MASS_CORRECTION 72 // asuint of the last tick's correction factor (1 without one)
#define GPU_FLUID_STAT_WORD_COUNT 96
#define GPU_FLUID_STATS_BYTES (GPU_FLUID_STAT_WORD_COUNT * 4)

#define GPU_FLUID_MASS_SCALE 256.0
#define GPU_FLUID_DIV_SCALE 16.0

// Fixed point of the per-brick mean density in groupshared memory: 512 cells of density <= 64 stay below 2^32.
#define GPU_FLUID_MEAN_SCALE 65536.0

// GasForces clamps every face velocity to this before the projection, and GasEmit every source velocity, so a blow-up
// stays finite. The projection can push a face past it again (about 60 m/s under Gas Safety's 48 m/s source); the next
// tick's GasForces clamps it back. Backtraces of any length are safe: GpuFluidLocate goes through the page table.
#define GPU_FLUID_MAX_SPEED 32.0
