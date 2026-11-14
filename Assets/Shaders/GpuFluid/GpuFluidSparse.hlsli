#pragma once

// Sparse brick grid of a GpuFluid gas volume: page lookup, neighbour table, atlas addressing and sampling.
//
// Index spaces:
// - Virtual brick b (uint3, < grid): the brick of the volume's fixed grid. Cell (i,j,k) of the grid spans [i, i+1) in
//   voxel units, so its centre is at i + 0.5.
// - Pool slot s: the atlas brick the page table maps b to, at (s % Ax, (s / Ax) % Ay, s / (Ax Ay)).
// - Stored texel: 10 cubed per slot, local cell l in [-1, 8] at stored index l + 1. Index -1 and 8 are the apron.
//
// The MAC velocity is staggered inside the texel: x holds u on the cell's -x face (position (i, j+.5, k+.5)), y holds v
// on its -y face, z holds w on its -z face. The +x face of cell 7 is therefore the apron texel 8.
//
// A shader that includes this declares the read-only grid buffers below; the allocation kernels, which write them,
// define GPU_FLUID_SPARSE_WRITABLE first and declare their own.

#include <Shaders/GpuFluid/GpuFluidGasConstants.h>

#if !defined(GPU_FLUID_SPARSE_WRITABLE)
StructuredBuffer<uint> GpuFluidPageTable;    // virtual brick (linear) -> pool slot or GPU_FLUID_INVALID_SLOT
StructuredBuffer<uint> GpuFluidActiveList;   // active index -> pool slot
StructuredBuffer<uint> GpuFluidBrickCoords;  // pool slot -> packed virtual brick
StructuredBuffer<uint> GpuFluidNeighbors;    // pool slot * 27 + neighbour -> pool slot or GPU_FLUID_NEIGHBOR_*
StructuredBuffer<uint> GpuFluidDispatchArgs; // x, y, z groups and the active count
#endif

uint3 GpuFluidGrid()
{
  return GET_PUSH_CONSTANT(plGpuFluidGasConstants, GpuFluidGasGrid).xyz;
}

uint3 GpuFluidAtlasBricks()
{
  return GET_PUSH_CONSTANT(plGpuFluidGasConstants, GpuFluidGasAtlas).xyz;
}

uint GpuFluidOpenFaces()
{
  return GET_PUSH_CONSTANT(plGpuFluidGasConstants, GpuFluidGasAtlas).w;
}

uint GpuFluidPackBrick(uint3 brick)
{
  return brick.x | (brick.y << 10) | (brick.z << 20);
}

uint3 GpuFluidUnpackBrick(uint packed)
{
  return uint3(packed & 1023u, (packed >> 10) & 1023u, packed >> 20);
}

uint GpuFluidBrickLinear(uint3 brick, uint3 grid)
{
  return brick.x + grid.x * (brick.y + grid.y * brick.z);
}

uint3 GpuFluidSlotToAtlasBrick(uint slot, uint3 atlasBricks)
{
  return uint3(slot % atlasBricks.x, (slot / atlasBricks.x) % atlasBricks.y, slot / (atlasBricks.x * atlasBricks.y));
}

/// First stored texel (the apron corner) of a pool slot in the atlas.
int3 GpuFluidSlotBase(uint slot)
{
  return int3(GpuFluidSlotToAtlasBrick(slot, GpuFluidAtlasBricks()) * GPU_FLUID_BRICK_STORE);
}

/// Atlas texel of local cell l (-1..8) of a pool slot.
int3 GpuFluidTexel(uint slot, int3 local)
{
  return GpuFluidSlotBase(slot) + local + GPU_FLUID_BRICK_APRON;
}

uint GpuFluidNeighborIndex(int3 d)
{
  return uint((d.x + 1) + 3 * (d.y + 1) + 9 * (d.z + 1));
}

/// Interior cell of a brick (0..7 per axis) to its index in the per-slot 512 entry buffers.
uint GpuFluidCellIndex(uint3 local)
{
  return local.x + GPU_FLUID_BRICK_SIZE * (local.y + GPU_FLUID_BRICK_SIZE * local.z);
}

/// Whether the domain face a missing neighbour lies beyond is open. axis 0..2, side -1 or +1.
bool GpuFluidFaceOpen(uint axis, int side)
{
  const uint bit = 1u << (axis * 2 + (side > 0 ? 1 : 0));
  return (GpuFluidOpenFaces() & bit) != 0;
}

/// NaN or infinite, by the exponent bits, so no compiler float mode can fold the test away.
bool GpuFluidIsNonFinite(float x)
{
  return (asuint(x) & 0x7F800000u) == 0x7F800000u;
}

bool GpuFluidIsFinite(float4 v)
{
  return !GpuFluidIsNonFinite(v.x) && !GpuFluidIsNonFinite(v.y) && !GpuFluidIsNonFinite(v.z) && !GpuFluidIsNonFinite(v.w);
}

float GpuFluidNaN()
{
  return asfloat(0x7FC00000u);
}

/// result, except that every component which is NaN or infinite in any of the sources stays NaN. min, max and clamp
/// drop a NaN operand; this keeps a non-finite value alive past them, so the guard that zeroes it also counts it.
float4 GpuFluidKeepNonFinite(float4 result, float4 a, float4 b, float4 c)
{
  [unroll] for (uint i = 0; i < 4; ++i)
  {
    if (GpuFluidIsNonFinite(a[i]) || GpuFluidIsNonFinite(b[i]) || GpuFluidIsNonFinite(c[i]))
      result[i] = GpuFluidNaN();
  }
  return result;
}

/// InterlockedAdd into a stats word that counts one more overflow when the sum wraps. The first wrap is always seen,
/// which is all the overflow check needs.
void GpuFluidStatAdd(RWByteAddressBuffer stats, uint word, uint value, inout uint io_overflow)
{
  uint original;
  stats.InterlockedAdd(word * 4, value, original);
  if (original + value < original)
    io_overflow += 1;
}

#if !defined(GPU_FLUID_SPARSE_WRITABLE)

/// Pool slot an indirect group works on, or GPU_FLUID_INVALID_SLOT for the padding groups of the 2D fold.
uint GpuFluidGroupSlot(uint3 groupId)
{
  const uint activeIndex = groupId.y * GPU_FLUID_DISPATCH_FOLD + groupId.x;
  if (activeIndex >= GpuFluidDispatchArgs[3])
    return GPU_FLUID_INVALID_SLOT;
  return GpuFluidActiveList[activeIndex];
}

/// Pool slot of a virtual brick, GPU_FLUID_NEIGHBOR_OUTSIDE beyond the grid, GPU_FLUID_NEIGHBOR_UNALLOCATED without one.
uint GpuFluidLookupBrick(int3 brick)
{
  const int3 grid = int3(GpuFluidGrid());
  if (any(brick < 0) || any(brick >= grid))
    return GPU_FLUID_NEIGHBOR_OUTSIDE;

  const uint slot = GpuFluidPageTable[GpuFluidBrickLinear(uint3(brick), uint3(grid))];
  return slot == GPU_FLUID_INVALID_SLOT ? GPU_FLUID_NEIGHBOR_UNALLOCATED : slot;
}

/// Neighbour of a pool slot, d in {-1, 0, 1} cubed.
uint GpuFluidNeighbor(uint slot, int3 d)
{
  return GpuFluidNeighbors[slot * GPU_FLUID_NEIGHBOR_COUNT + GpuFluidNeighborIndex(d)];
}

/// Where an atlas field's sample at texel-index position q lives in the atlas. The field's sample n sits at q = n (q is
/// the position in voxels minus the field's offset: 0.5 for cell centred values, 0 on the staggered axis of a MAC
/// component). out_texel is continuous: atlas texel i holds the value at out_texel = i.
///
/// The 8 texels a sample needs are always inside one stored brick: the brick of floor(q) on each axis, whose apron
/// holds the +1 texel. Positions are clamped to the apron band of the domain, so a trace that leaves the grid reads
/// the boundary values the halo wrote. False for a brick without a slot.
bool GpuFluidLocate(float3 q, out float3 out_texel)
{
  const int3 grid = int3(GpuFluidGrid());
  const float3 gridVoxels = float3(grid * GPU_FLUID_BRICK_SIZE);

  // Keep the upper tap's weight strictly below 1, so the texel beyond the apron is never read at all.
  q = clamp(q, -1.0, gridVoxels - (1.0 / 256.0));

  const int3 cell = int3(floor(q));
  const int3 brick = clamp(int3(floor(float3(cell) / GPU_FLUID_BRICK_SIZE)), 0, grid - 1);

  out_texel = float3(0.0, 0.0, 0.0);
  const uint slot = GpuFluidPageTable[GpuFluidBrickLinear(uint3(brick), uint3(grid))];
  if (slot == GPU_FLUID_INVALID_SLOT)
    return false;

  out_texel = float3(GpuFluidSlotBase(slot)) + GPU_FLUID_BRICK_APRON + (q - float3(brick * GPU_FLUID_BRICK_SIZE));
  return true;
}

/// Hardware trilinear sample of an atlas field at texel-index position q (see GpuFluidLocate). A brick without a slot
/// reads fUnallocated.
float4 GpuFluidSampleAtlas(Texture3D<float4> atlas, SamplerState linearClamp, float3 q, float4 fUnallocated)
{
  float3 texel;
  if (!GpuFluidLocate(q, texel))
    return fUnallocated;

  const float3 atlasDim = float3(GpuFluidAtlasBricks() * GPU_FLUID_BRICK_STORE);
  return atlas.SampleLevel(linearClamp, (texel + 0.5) / atlasDim, 0.0);
}

/// The same sample with eight loads and full float weights, for an atlas bound read-write in the same pass.
float4 GpuFluidLoadTrilinear(RWTexture3D<float4> atlas, float3 q, float4 fUnallocated)
{
  float3 texel;
  if (!GpuFluidLocate(q, texel))
    return fUnallocated;

  const float3 t0 = floor(texel);
  const float3 f = texel - t0;
  const int3 i0 = int3(t0);

  const float4 c000 = atlas[i0 + int3(0, 0, 0)];
  const float4 c100 = atlas[i0 + int3(1, 0, 0)];
  const float4 c010 = atlas[i0 + int3(0, 1, 0)];
  const float4 c110 = atlas[i0 + int3(1, 1, 0)];
  const float4 c001 = atlas[i0 + int3(0, 0, 1)];
  const float4 c101 = atlas[i0 + int3(1, 0, 1)];
  const float4 c011 = atlas[i0 + int3(0, 1, 1)];
  const float4 c111 = atlas[i0 + int3(1, 1, 1)];

  const float4 x00 = lerp(c000, c100, f.x);
  const float4 x10 = lerp(c010, c110, f.x);
  const float4 x01 = lerp(c001, c101, f.x);
  const float4 x11 = lerp(c011, c111, f.x);
  return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}

/// Smallest and largest value of the eight texels a sample at q interpolates between (MacCormack's limiter).
void GpuFluidLoadRange(Texture3D<float4> atlas, float3 q, out float4 out_min, out float4 out_max)
{
  float3 texel;
  if (!GpuFluidLocate(q, texel))
  {
    out_min = float4(0.0, 0.0, 0.0, 0.0);
    out_max = float4(0.0, 0.0, 0.0, 0.0);
    return;
  }

  const int3 i0 = int3(floor(texel));
  out_min = atlas.Load(int4(i0, 0));
  out_max = out_min;

  [unroll] for (uint c = 1; c < 8; ++c)
  {
    const float4 v = atlas.Load(int4(i0 + int3(c & 1, (c >> 1) & 1, c >> 2), 0));
    out_min = min(out_min, v);
    out_max = max(out_max, v);
  }
}

/// MAC velocity (m/s) at a position in voxels, each component from its own staggered grid.
float3 GpuFluidSampleVelocity(Texture3D<float4> velocity, SamplerState linearClamp, float3 p, float3 vAmbient)
{
  const float4 fallback = float4(vAmbient, 0.0);
  return float3(
    GpuFluidSampleAtlas(velocity, linearClamp, p - float3(0.0, 0.5, 0.5), fallback).x,
    GpuFluidSampleAtlas(velocity, linearClamp, p - float3(0.5, 0.0, 0.5), fallback).y,
    GpuFluidSampleAtlas(velocity, linearClamp, p - float3(0.5, 0.5, 0.0), fallback).z);
}

#endif
