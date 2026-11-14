#pragma once

// Shared between C++ and HLSL: the liquid solver's layouts, fixed-point rules and push constants.
// C++ reaches it as <Shaders/GpuFluid/Liquid/GpuFluidLiquidConstants.h> through the package's Assets include root.

#include <Shaders/Common/ConstantBufferMacros.h>

// Particle kernels: 256 threads, dispatched indirectly from the alive count. Grid kernels: one 8x8x8 group per
// active brick on the fine level, 4x4x4 groups over the dense coarse levels of the multigrid.
#define GPU_FLUID_LIQUID_PARTICLE_GROUP 256
#define GPU_FLUID_LIQUID_BRICK 8
#define GPU_FLUID_LIQUID_LEVEL_GROUP 4
#define GPU_FLUID_LIQUID_COARSEST_GROUP 256

// The coarsest multigrid level is solved by one group in groupshared memory: at most this many cells (16 KB).
#define GPU_FLUID_LIQUID_COARSEST_MAX_CELLS 4096
#define GPU_FLUID_LIQUID_MAX_LEVELS 6

// Cell types (the low two bits of a cell's type word).
#define GPU_FLUID_LIQUID_CELL_AIR 0u
#define GPU_FLUID_LIQUID_CELL_LIQUID 1u
#define GPU_FLUID_LIQUID_CELL_SOLID 2u

// Above the two type bits a liquid cell of the fine level holds, per neighbour n (0..5: -x, +x, -y, +y, -z, +z), five
// bits of the ghost-fluid fraction theta of the free surface towards that neighbour, when it is air (LiquidClassify,
// from the level set: theta = (phi_i - tau) / (phi_i - phi_j), clamped below by the classify dispatch). Code 1 <= q <= 30:
// theta = q / 30; code 31: the constant theta of the push constants (no level set, or an open domain side).
#define GPU_FLUID_LIQUID_THETA_SHIFT 2u
#define GPU_FLUID_LIQUID_THETA_BITS 5u
#define GPU_FLUID_LIQUID_THETA_CONSTANT 31u
#define GPU_FLUID_LIQUID_THETA_STEPS 30.0

// GpuFluidLiqCells.w: bits 0..5 open faces, 8..15 shape count, bit 16: liquid cells and the free surface's theta come
// from the level set (phi < 0 is liquid as well as a cell holding particles), bit 17: the baked static solids
// (GpuFluidLiqSolidSdf) hold solids, bits 18..20: drain count.
#define GPU_FLUID_LIQUID_FLAG_LEVEL_SET (1u << 16)
#define GPU_FLUID_LIQUID_FLAG_SOLID_SDF (1u << 17)
#define GPU_FLUID_LIQUID_DRAIN_SHIFT 18u
#define GPU_FLUID_LIQUID_MAX_DRAINS 4

// Static solids baked from the world's collision meshes (LiquidSolidsBake): one float per cell, the signed distance
// (m, negative inside) of the cell centre, exact within GPU_FLUID_LIQUID_SDF_BAND_CELLS of a triangle and +-band beyond.
// Every surface is at least GPU_FLUID_LIQUID_SDF_MIN_THICKNESS_CELLS thick (half of it on each side), so a single-sided
// wall (an open plane) still blocks the liquid.
#define GPU_FLUID_LIQUID_SDF_BAND_CELLS 3.0
#define GPU_FLUID_LIQUID_SDF_MIN_THICKNESS_CELLS 0.5
#define GPU_FLUID_LIQUID_SDF_UNKNOWN 3.0e38
#define GPU_FLUID_LIQUID_SOLID_TRIANGLE_FLOATS 32

// Sleep: a brick whose particles stay below GPU_FLUID_LIQUID_SLEEP_SPEED (m/s, and its neighbours' too) for
// GPU_FLUID_LIQUID_SLEEP_STEPS substeps, with no wake box (inflow, open drain) over it, freezes its particles.
#define GPU_FLUID_LIQUID_SLEEP_SPEED 0.02
#define GPU_FLUID_LIQUID_SLEEP_STEPS 60u
#define GPU_FLUID_LIQUID_MAX_WAKE_BOXES 8

// Regions (GpuFluidLiqRegions, boxes as plGpuFluidLiquidShape, domain metres): the open drains first, then from
// GPU_FLUID_LIQUID_REGION_WAKE_FIRST the wake boxes. A particle inside a drain is removed and counted as drained, and a
// cell whose centre is inside one is air (the free surface's zero pressure draws the liquid in).
#define GPU_FLUID_LIQUID_REGION_WAKE_FIRST GPU_FLUID_LIQUID_MAX_DRAINS
#define GPU_FLUID_LIQUID_MAX_REGIONS (GPU_FLUID_LIQUID_MAX_DRAINS + GPU_FLUID_LIQUID_MAX_WAKE_BOXES)

// Level probes (the flood-level check): up to four columns whose water level the stats report.
#define GPU_FLUID_LIQUID_MAX_PROBES 4
#define GPU_FLUID_LIQUID_NO_PROBE 0xFFFFFFFFu

// Surface extraction (Liquid/Surface): per vertex two float4 (world position, 1; normal, 0), per quad four vertex
// indices. The counters buffer: vertices, quads, and how many of each did not fit.
#define GPU_FLUID_LIQUID_SURFACE_VERTEX_BYTES 32
#define GPU_FLUID_LIQUID_SURFACE_QUAD_BYTES 16
#define GPU_FLUID_LIQUID_SURFACE_CTR_VERTICES 0
#define GPU_FLUID_LIQUID_SURFACE_CTR_QUADS 1
#define GPU_FLUID_LIQUID_SURFACE_CTR_VERTEX_OVERFLOW 2
#define GPU_FLUID_LIQUID_SURFACE_CTR_QUAD_OVERFLOW 3
#define GPU_FLUID_LIQUID_SURFACE_CTR_WORDS 4
#define GPU_FLUID_LIQUID_SURFACE_NO_VERTEX 0xFFFFFFFFu

// Face validity: one word per padded face index, one bit per MAC component.
#define GPU_FLUID_LIQUID_VALID_U 1u
#define GPU_FLUID_LIQUID_VALID_V 2u
#define GPU_FLUID_LIQUID_VALID_W 4u

// Domain faces that are open (air outside, particles leaving are drained). Closed faces are walls.
#define GPU_FLUID_LIQUID_OPEN_NEG_X 1u
#define GPU_FLUID_LIQUID_OPEN_POS_X 2u
#define GPU_FLUID_LIQUID_OPEN_NEG_Y 4u
#define GPU_FLUID_LIQUID_OPEN_POS_Y 8u
#define GPU_FLUID_LIQUID_OPEN_NEG_Z 16u
#define GPU_FLUID_LIQUID_OPEN_POS_Z 32u

// P2G gathers in int32 registers (no atomics, so the sums do not depend on particle order):
// - momentum at 2^16 per m/s: a particle at the 32 m/s clamp adds at most 2^21, so 1024 full-weight particles fit;
// - weight at 2^20 per unit weight: 2048 full-weight particles fit. The plan's 2^24 fits only 128, too few for the
//   3x3x3-cell gather of a compressed region at 8 particles per cell.
// A sum that would still overflow is clamped and counted in the overflow stat. A face whose total weight is below
// GPU_FLUID_LIQUID_MIN_FACE_WEIGHT is left invalid (the extrapolation fills it): at 2^20 its velocity would be
// quantised in steps of 16 / weight_int m/s, about 0.015 m/s at the threshold.
#define GPU_FLUID_LIQUID_MOMENTUM_SCALE 65536.0
#define GPU_FLUID_LIQUID_WEIGHT_SCALE 1048576.0
#define GPU_FLUID_LIQUID_MIN_FACE_WEIGHT 1e-3
#define GPU_FLUID_LIQUID_MAX_SPEED 32.0
#define GPU_FLUID_LIQUID_MAX_AFFINE 16.0

// Fill volume of a cell in the volume measure: 1/256 of a cell per unit.
#define GPU_FLUID_LIQUID_VOLUME_SCALE 256.0

// Speeds summed into the stats in mm/s.
#define GPU_FLUID_LIQUID_SPEED_SUM_SCALE 1000.0
// Squared speeds summed in (cm/s)^2: a group of 256 at the 32 m/s clamp stays below 2^32.
#define GPU_FLUID_LIQUID_SPEED_SQ_SCALE 10000.0

#define GPU_FLUID_LIQUID_MAX_SHAPES 8
#define GPU_FLUID_LIQUID_SHAPE_BOX 0u
#define GPU_FLUID_LIQUID_SHAPE_SPHERE 1u

// Particle record, structure of arrays (raw buffers): position float3 (domain-relative metres), velocity float3, and
// five words of APIC affine matrix (9 halves) plus a 16 bit flag field.
#define GPU_FLUID_LIQUID_POSITION_BYTES 12
#define GPU_FLUID_LIQUID_VELOCITY_BYTES 12
#define GPU_FLUID_LIQUID_AFFINE_BYTES 20
#define GPU_FLUID_LIQUID_FLAG_ALIVE 1u

// A particle's rank inside its cell after the count pass; this value marks a particle that was removed.
#define GPU_FLUID_LIQUID_REMOVED 0xFFFFFFFFu

// Indirect dispatch arguments: {groupsX, groupsY, 1, count} (plGpuFluidScan's args writer).
#define GPU_FLUID_LIQUID_ARGS_BYTES 16

// Indirect draw arguments (debug points, surface): two records, {vertices, 1, 0, 0} for a mono view at byte 0 and
// {vertices, 2, 0, 0} for a stereo view at byte GPU_FLUID_LIQUID_DRAW_ARGS_STEREO_OFFSET (the eye is SV_InstanceID % 2;
// the engine doubles the instance count of direct draws only).
#define GPU_FLUID_LIQUID_DRAW_ARGS_STEREO_OFFSET 16
#define GPU_FLUID_LIQUID_DRAW_ARGS_BYTES 32

// Counters: persistent and per-substep words the kernels share on the GPU (never read back directly).
#define GPU_FLUID_LIQUID_CTR_SEEDED 0         // particles seeded so far
#define GPU_FLUID_LIQUID_CTR_DRAINED 1        // particles that left through an open face (or a drain, later)
#define GPU_FLUID_LIQUID_CTR_LOST 2           // particles removed for any other reason (NaN, escaped a wall)
#define GPU_FLUID_LIQUID_CTR_SUBSTEPS 3       // substeps run so far
#define GPU_FLUID_LIQUID_CTR_TICKS 4          // ticks (frames that stepped) so far
#define GPU_FLUID_LIQUID_CTR_SIM_TIME 5       // simulated time so far in microseconds (less than wall time in slow motion)
#define GPU_FLUID_LIQUID_CTR_ACTIVE_BRICKS 8  // per substep: active brick list length
#define GPU_FLUID_LIQUID_CTR_VOLUME 9         // per substep: fill volume, GPU_FLUID_LIQUID_VOLUME_SCALE per cell
#define GPU_FLUID_LIQUID_CTR_LIQUID_CELLS 10  // per substep: cells classified liquid
#define GPU_FLUID_LIQUID_CTR_OCCUPIED 11      // per substep: bricks holding particles
#define GPU_FLUID_LIQUID_CTR_RHS_MAX 12       // per substep: asuint of max |rhs| over liquid cells
#define GPU_FLUID_LIQUID_CTR_RESIDUAL_MAX 13  // per substep: asuint of max |residual| after the solve
#define GPU_FLUID_LIQUID_CTR_NAN 14           // per frame: NaN or infinite values met (and repaired)
#define GPU_FLUID_LIQUID_CTR_OVERFLOW 15      // per frame: fixed-point or speed clamps
#define GPU_FLUID_LIQUID_CTR_AWAKE 16         // per substep: occupied bricks that are not asleep (LiquidSleep)
#define GPU_FLUID_LIQUID_CTR_SLEEPING 17      // per substep: occupied bricks that are asleep (LiquidSleep)
#define GPU_FLUID_LIQUID_CTR_IN_SOLID 18      // per frame: particles left deeper than half a cell inside a solid after G2P (N9)
#define GPU_FLUID_LIQUID_CTR_SOLID_CELLS 19   // per bake: cells the static solids bake made solid (kept through resets)
#define GPU_FLUID_LIQUID_CTR_MAX_CELL 20      // per frame: most particles in one cell (bits 20-31, saturating) and that cell (bits 0-19)
#define GPU_FLUID_LIQUID_CTR_MAX_CELL_TYPE 21 // per frame: the classification of that cell (its cell type word) at the last substep
#define GPU_FLUID_LIQUID_CTR_MAX_CELL_PHI 22  // per frame: asuint of its level set (cells)
#define GPU_FLUID_LIQUID_CTR_MAX_CELL_SDF 23  // per frame: asuint of its solids' distance (m)
#define GPU_FLUID_LIQUID_CTR_WORDS 24
#define GPU_FLUID_LIQUID_CTR_FIRST_PER_SUBSTEP 8
#define GPU_FLUID_LIQUID_CTR_LAST_PER_SUBSTEP 13

// MGPCG scalars, kept on the GPU (no readback): float words of the scalar buffer.
#define GPU_FLUID_LIQUID_PCG_RZ 0     // r . z of the current iterate
#define GPU_FLUID_LIQUID_PCG_ALPHA 1  // step length along p
#define GPU_FLUID_LIQUID_PCG_BETA 2   // weight of the old direction in the new p
#define GPU_FLUID_LIQUID_PCG_PQ 3     // p . A p
#define GPU_FLUID_LIQUID_PCG_WORDS 4

// What a reduction of the per-brick dot products sets (GpuFluidLiqPass.y of LiquidPcgReduce).
#define GPU_FLUID_LIQUID_PCG_REDUCE_START 0 // rz = sum, beta = 0
#define GPU_FLUID_LIQUID_PCG_REDUCE_ALPHA 1 // pq = sum, alpha = rz / pq
#define GPU_FLUID_LIQUID_PCG_REDUCE_BETA 2  // beta = sum / rz, rz = sum

// Stats: one buffer per volume, rewritten every frame that steps and read back through a ring of 3.
#define GPU_FLUID_LIQUID_STAT_ALIVE 0
#define GPU_FLUID_LIQUID_STAT_SEEDED 1
#define GPU_FLUID_LIQUID_STAT_DRAINED 2
#define GPU_FLUID_LIQUID_STAT_LOST 3
#define GPU_FLUID_LIQUID_STAT_SUBSTEPS_TOTAL 4
#define GPU_FLUID_LIQUID_STAT_TICKS 5
#define GPU_FLUID_LIQUID_STAT_ACTIVE_BRICKS 6
#define GPU_FLUID_LIQUID_STAT_VOLUME 7
#define GPU_FLUID_LIQUID_STAT_LIQUID_CELLS 8
#define GPU_FLUID_LIQUID_STAT_MAX_SPEED 9          // asuint
#define GPU_FLUID_LIQUID_STAT_NAN 10
#define GPU_FLUID_LIQUID_STAT_OVERFLOW 11
#define GPU_FLUID_LIQUID_STAT_RHS_MAX 12           // asuint, last substep
#define GPU_FLUID_LIQUID_STAT_RESIDUAL_MAX 13      // asuint, last substep
#define GPU_FLUID_LIQUID_STAT_OCCUPIED 14
#define GPU_FLUID_LIQUID_STAT_SUBSTEPS 15          // substeps of this frame
#define GPU_FLUID_LIQUID_STAT_BOUNDS_MIN 16        // 3 words: smallest particle cell per axis
#define GPU_FLUID_LIQUID_STAT_BOUNDS_MAX 19        // 3 words: largest particle cell per axis
#define GPU_FLUID_LIQUID_STAT_SPEED_SUM 22         // sum of speeds, GPU_FLUID_LIQUID_SPEED_SUM_SCALE per m/s
#define GPU_FLUID_LIQUID_STAT_ABOVE_1CM 23         // particles faster than 1 cm/s
#define GPU_FLUID_LIQUID_STAT_ABOVE_1MM 24         // particles faster than 1 mm/s
#define GPU_FLUID_LIQUID_STAT_CENTROID 25          // 3 words: sum of particle positions in cells, 1/16 cell units, low 32 bits
#define GPU_FLUID_LIQUID_STAT_CENTROID_HI 28       // 3 words: the carries out of those sums (high 32 bits)
#define GPU_FLUID_LIQUID_STAT_SIM_TIME 31          // simulated time since the reset, microseconds
#define GPU_FLUID_LIQUID_STAT_HIST_COUNT 32        // bins used by the front histogram
#define GPU_FLUID_LIQUID_STAT_SURFACE_VERTICES 33  // surface vertices written this frame (Liquid/Surface)
#define GPU_FLUID_LIQUID_STAT_SURFACE_QUADS 34     // surface quads written this frame
#define GPU_FLUID_LIQUID_STAT_SURFACE_OVERFLOW 35  // vertices and quads that did not fit
#define GPU_FLUID_LIQUID_STAT_SPEED_SQ_SUM 36      // sum of squared speeds, GPU_FLUID_LIQUID_SPEED_SQ_SCALE per (m/s)^2, low 32 bits
#define GPU_FLUID_LIQUID_STAT_SPEED_SQ_SUM_HI 37   // and the carries out of it (high 32 bits)
#define GPU_FLUID_LIQUID_STAT_AWAKE 38             // occupied bricks awake after the last substep
#define GPU_FLUID_LIQUID_STAT_SLEEPING 39          // occupied bricks asleep after the last substep
#define GPU_FLUID_LIQUID_STAT_PROBES 40            // 4 words: asuint water depth (m) per probe from the particles of its 3 x 3 column
#define GPU_FLUID_LIQUID_STAT_IN_SOLID 44          // particle-substeps that ended deeper than half a cell inside a solid this frame
#define GPU_FLUID_LIQUID_STAT_SOLID_CELLS 45       // cells the last static solids bake made solid
#define GPU_FLUID_LIQUID_STAT_MAX_CELL 46          // the frame's most particles in one cell (bits 20-31) and that cell (bits 0-19)
#define GPU_FLUID_LIQUID_STAT_MAX_CELL_TYPE 47     // its cell type word at the last substep
#define GPU_FLUID_LIQUID_STAT_MAX_CELL_PHI 52      // asuint of its level set (cells)
#define GPU_FLUID_LIQUID_STAT_MAX_CELL_SDF 53      // asuint of its solids' distance (m)
#define GPU_FLUID_LIQUID_STAT_PROBES_PHI 48        // 4 words: asuint water depth (m) per probe from the level set's lowest zero crossing
#define GPU_FLUID_LIQUID_STAT_HIST 64              // front histogram along +x
#define GPU_FLUID_LIQUID_HIST_BINS 1024
#define GPU_FLUID_LIQUID_STAT_WORD_COUNT (GPU_FLUID_LIQUID_STAT_HIST + GPU_FLUID_LIQUID_HIST_BINS)
#define GPU_FLUID_LIQUID_STATS_BYTES (GPU_FLUID_LIQUID_STAT_WORD_COUNT * 4)

// A shader has one push-constant block (one root-constant register on D3D12, one push-constant block on Vulkan): the
// seeding kernel defines GPU_FLUID_LIQUID_SEED_KERNEL and gets the seed block, every other liquid kernel the main one.
// A draw shader that has its own block and only needs the layouts here defines GPU_FLUID_LIQUID_DRAW_SHADER and gets
// neither (Vulkan rejects a second push-constant block even when it is unused).
#if !PL_ENABLED(PLATFORM_SHADER) || (!defined(GPU_FLUID_LIQUID_SEED_KERNEL) && !defined(GPU_FLUID_LIQUID_DRAW_SHADER))

/// Per dispatch data of every liquid kernel. Push constants, because render-graph passes are recorded before they
/// execute and a shared constant buffer would be last-write-wins.
BEGIN_PUSH_CONSTANTS(plGpuFluidLiquidConstants)
{
  UINT4(GpuFluidLiqCells);    // xyz: cells per axis, w: open-face mask | shape count << 8
  UINT4(GpuFluidLiqBricks);   // xyz: bricks per axis, w: particle capacity
  FLOAT4(GpuFluidLiqGrid);    // x: dx (m), y: 1/dx, z: dt of this substep (s), w: FLIP blend
  FLOAT4(GpuFluidLiqGravity); // xyz: gravity (m/s^2), w: particles per cell n0
  FLOAT4(GpuFluidLiqSolve);   // x: 1/theta at the free surface, y: density gain k, z: volume gain c, w: warm-start scale
  FLOAT4(GpuFluidLiqWall);    // x: wall margin (cells), y: wall friction, z: front height (m), w: front bin size (m)
  UINT4(GpuFluidLiqPass);     // kernel specific (cells of a level, the set or colour)
  FLOAT4(GpuFluidLiqPassF);   // kernel specific
}
END_PUSH_CONSTANTS(plGpuFluidLiquidConstants)

#endif

#if !PL_ENABLED(PLATFORM_SHADER) || defined(GPU_FLUID_LIQUID_SEED_KERNEL)

/// Push constants of the seeding kernel: one fill box, snapped to whole cells.
BEGIN_PUSH_CONSTANTS(plGpuFluidLiquidSeedConstants)
{
  UINT4(GpuFluidSeedCellMin);   // xyz: first cell, w: particles per cell
  UINT4(GpuFluidSeedCellCount); // xyz: cells per axis, w: index of the first particle written
  FLOAT4(GpuFluidSeedVelocity); // xyz: initial velocity, w: jitter (0..1 of a sub-cell)
  FLOAT4(GpuFluidSeedGrid);     // x: dx (m), y: filled fraction of the cell height, z: mode (LiquidSeed), w: unused
  UINT4(GpuFluidSeedInfo);      // x: seed of the jitter hash, y: particles to write, zw: unused
}
END_PUSH_CONSTANTS(plGpuFluidLiquidSeedConstants)

#endif

/// An analytic solid in the domain (domain-relative metres).
struct PL_SHADER_STRUCT plGpuFluidLiquidShape
{
  FLOAT4(CenterType); // xyz: centre, w: GPU_FLUID_LIQUID_SHAPE_*
  FLOAT4(Size);       // box: xyz half extents; sphere: x radius
};

#if !PL_ENABLED(PLATFORM_SHADER)
static_assert(sizeof(plGpuFluidLiquidShape) == 32);
#endif
