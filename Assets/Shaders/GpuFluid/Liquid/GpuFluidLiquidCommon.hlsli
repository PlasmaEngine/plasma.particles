#pragma once

// Shared helpers of the liquid kernels: grid indexing, the MAC face layout, particle records, analytic solids.
//
// Grid: nx * ny * nz cells of size dx, domain-relative coordinates in [0, n * dx). Cell buffers are dense, x fastest.
// MAC faces: u at (i, j + 1/2, k + 1/2), v at (i + 1/2, j, k + 1/2), w at (i + 1/2, j + 1/2, k), in cell units. All
// three components use one padded index space of (nx + 1) * (ny + 1) * (nz + 1), so the thread of cell (i, j, k) owns
// the low u, v and w faces of its cell, and the last cell on an axis also owns the face on the domain's high side.
// A face velocity set is one raw buffer with the u, v and w regions one after the other.

#include <Shaders/Common/Common.h>
#include <Shaders/GpuFluid/GpuFluidScan.hlsli>
#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidConstants.h>

/// Integer hash (lowbias32), for jitter that depends only on its inputs.
uint LiqHash(uint x)
{
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float LiqHashFloat(uint x) { return float(LiqHash(x) >> 8) * (1.0 / 16777216.0); }

// APIC affine matrix: rows cx, cy, cz (the gradient of u, v and w), nine halves in five words; the high half of the
// fifth word holds the flags.
void LiqPackAffine(float3 cx, float3 cy, float3 cz, uint flags, out uint4 words, out uint word4)
{
  words.x = f32tof16(cx.x) | (f32tof16(cx.y) << 16);
  words.y = f32tof16(cx.z) | (f32tof16(cy.x) << 16);
  words.z = f32tof16(cy.y) | (f32tof16(cy.z) << 16);
  words.w = f32tof16(cz.x) | (f32tof16(cz.y) << 16);
  word4 = f32tof16(cz.z) | (flags << 16);
}

void LiqUnpackAffine(uint4 words, uint word4, out float3 cx, out float3 cy, out float3 cz)
{
  cx = float3(f16tof32(words.x), f16tof32(words.x >> 16), f16tof32(words.y));
  cy = float3(f16tof32(words.y >> 16), f16tof32(words.z), f16tof32(words.z >> 16));
  cz = float3(f16tof32(words.w), f16tof32(words.w >> 16), f16tof32(word4));
}

bool LiqIsFinite3(float3 v) { return !any(isnan(v)) && !any(isinf(v)); }

#if !defined(GPU_FLUID_LIQUID_SEED_KERNEL)

#define LIQ_CONST(Name) GET_PUSH_CONSTANT(plGpuFluidLiquidConstants, Name)

uint3 LiqCells() { return LIQ_CONST(GpuFluidLiqCells).xyz; }
uint LiqOpenMask() { return LIQ_CONST(GpuFluidLiqCells).w & 0xFFu; }
uint LiqShapeCount() { return (LIQ_CONST(GpuFluidLiqCells).w >> 8) & 0xFFu; }
bool LiqSolidSdfEnabled() { return (LIQ_CONST(GpuFluidLiqCells).w & GPU_FLUID_LIQUID_FLAG_SOLID_SDF) != 0; }
uint LiqDrainCount() { return min((LIQ_CONST(GpuFluidLiqCells).w >> GPU_FLUID_LIQUID_DRAIN_SHIFT) & 7u, (uint)GPU_FLUID_LIQUID_MAX_DRAINS); }
/// Any solid inside the domain: an analytic shape or the baked static solids.
bool LiqHasSolids() { return LiqShapeCount() > 0 || LiqSolidSdfEnabled(); }
bool LiqLevelSetEnabled() { return (LIQ_CONST(GpuFluidLiqCells).w & GPU_FLUID_LIQUID_FLAG_LEVEL_SET) != 0; }
uint3 LiqBricks() { return LIQ_CONST(GpuFluidLiqBricks).xyz; }
uint LiqCapacity() { return LIQ_CONST(GpuFluidLiqBricks).w; }
float LiqDx() { return LIQ_CONST(GpuFluidLiqGrid).x; }
float LiqInvDx() { return LIQ_CONST(GpuFluidLiqGrid).y; }
float LiqDt() { return LIQ_CONST(GpuFluidLiqGrid).z; }
float LiqFlipBlend() { return LIQ_CONST(GpuFluidLiqGrid).w; }
float3 LiqGravity() { return LIQ_CONST(GpuFluidLiqGravity).xyz; }
float LiqParticlesPerCell() { return LIQ_CONST(GpuFluidLiqGravity).w; }
float LiqInvTheta() { return LIQ_CONST(GpuFluidLiqSolve).x; }

uint LiqCellIndexIn(int3 c, uint3 dims) { return (uint)c.x + dims.x * ((uint)c.y + dims.y * (uint)c.z); }
uint LiqCellIndex(int3 c) { return LiqCellIndexIn(c, LiqCells()); }
bool LiqInsideDims(int3 c, uint3 dims) { return all(c >= 0) && all(c < int3(dims)); }
bool LiqInside(int3 c) { return LiqInsideDims(c, LiqCells()); }

uint3 LiqFaceDims() { return LiqCells() + 1; }
uint LiqFaceCount() { const uint3 d = LiqFaceDims(); return d.x * d.y * d.z; }
uint LiqFaceIndex(int3 f) { const uint3 d = LiqFaceDims(); return (uint)f.x + d.x * ((uint)f.y + d.y * (uint)f.z); }

/// Byte address of component comp (0 u, 1 v, 2 w) of face f in a velocity set.
uint LiqFaceAddress(uint comp, int3 f) { return (comp * LiqFaceCount() + LiqFaceIndex(f)) * 4; }

int3 LiqAxis(uint comp) { return int3(comp == 0 ? 1 : 0, comp == 1 ? 1 : 0, comp == 2 ? 1 : 0); }

/// Whether face f of component comp exists: comp's own axis runs to n inclusive, the others to n - 1.
bool LiqFaceExists(uint comp, int3 f)
{
  const int3 hi = int3(LiqCells()) - 1 + LiqAxis(comp);
  return all(f >= 0) && all(f <= hi);
}

/// Position of a face in cell units.
float3 LiqFacePosition(uint comp, int3 f)
{
  return float3(f) + 0.5 - 0.5 * float3(LiqAxis(comp));
}

/// The faces a cell's thread owns: slot 0..2 its low u, v, w faces; slot 3..5 the high u, v, w faces, only for the
/// last cell on that axis. Returns false for a slot the cell does not own.
bool LiqOwnedFace(int3 cell, uint slot, out uint comp, out int3 face)
{
  comp = slot % 3;
  face = cell;
  if (slot < 3)
    return true;

  const int3 axis = LiqAxis(comp);
  face = cell + axis;
  return dot(cell, axis) == (int)dot(int3(LiqCells()) - 1, axis);
}

/// Open-face bit of the domain side a face on the boundary lies on (0 for an interior face).
uint LiqBoundarySideBit(uint comp, int3 face)
{
  const int coord = comp == 0 ? face.x : (comp == 1 ? face.y : face.z);
  const int n = comp == 0 ? (int)LiqCells().x : (comp == 1 ? (int)LiqCells().y : (int)LiqCells().z);
  if (coord == 0)
    return 1u << (comp * 2);
  if (coord == n)
    return 2u << (comp * 2);
  return 0u;
}

/// A face on a closed side of the domain is a wall.
bool LiqIsWallFace(uint comp, int3 face)
{
  const uint side = LiqBoundarySideBit(comp, face);
  return side != 0 && (LiqOpenMask() & side) == 0;
}

uint LiqBrickIndex(int3 cell)
{
  const uint3 b = uint3(cell) / GPU_FLUID_LIQUID_BRICK;
  const uint3 bricks = LiqBricks();
  return b.x + bricks.x * (b.y + bricks.y * b.z);
}

/// Grid coordinates of a particle (cell units) and its cell, clamped into the grid.
float3 LiqGridPosition(float3 position) { return position * LiqInvDx(); }

int3 LiqParticleCell(float3 position)
{
  return clamp(int3(floor(LiqGridPosition(position))), int3(0, 0, 0), int3(LiqCells()) - 1);
}

#if defined(GPU_FLUID_LIQUID_USE_SHAPES)

StructuredBuffer<plGpuFluidLiquidShape> GpuFluidLiqShapes;

// The baked static solids (LiquidSolidsBake): the signed distance of every cell centre, metres.
ByteAddressBuffer GpuFluidLiqSolidSdf;

float LiqSolidSdfAtCell(int3 c)
{
  const int3 cc = clamp(c, int3(0, 0, 0), int3(LiqCells()) - 1);
  return asfloat(GpuFluidLiqSolidSdf.Load(LiqCellIndex(cc) * 4));
}

/// The baked solids' distance at a position (m), trilinear between the cell centres.
float LiqSolidSdfSample(float3 position)
{
  const float3 g = clamp(position * LiqInvDx() - 0.5, 0.0, float3(LiqCells()) - 1.0);
  const int3 base = min(int3(floor(g)), int3(LiqCells()) - 2);
  const float3 f = g - float3(base);
  float value = 0.0;
  [unroll] for (uint i = 0; i < 8; ++i)
  {
    const int3 o = int3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
    const float3 w = lerp(1.0 - f, f, float3(o));
    value += w.x * w.y * w.z * LiqSolidSdfAtCell(base + o);
  }
  return value;
}

/// Signed distance (m) to the nearest analytic solid only, negative inside. 1e6 when there is none.
float LiqAnalyticDistance(float3 position)
{
  float distance = 1e6;
  const uint count = min(LiqShapeCount(), (uint)GPU_FLUID_LIQUID_MAX_SHAPES);
  for (uint i = 0; i < count; ++i)
  {
    const plGpuFluidLiquidShape shape = GpuFluidLiqShapes[i];
    const float3 local = position - shape.CenterType.xyz;
    float d;
    if ((uint)shape.CenterType.w == GPU_FLUID_LIQUID_SHAPE_SPHERE)
    {
      d = length(local) - shape.Size.x;
    }
    else
    {
      const float3 q = abs(local) - shape.Size.xyz;
      d = length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
    }
    distance = min(distance, d);
  }
  return distance;
}

/// Signed distance (m) to the nearest solid, analytic or baked, negative inside. 1e6 when there is none.
float LiqShapeDistance(float3 position)
{
  float distance = LiqAnalyticDistance(position);
  if (LiqSolidSdfEnabled())
    distance = min(distance, LiqSolidSdfSample(position));
  return distance;
}

/// The same at the centre of cell c: the baked value directly.
float LiqSolidDistanceAtCell(int3 c)
{
  float distance = LiqShapeCount() > 0 ? LiqAnalyticDistance((float3(c) + 0.5) * LiqDx()) : 1e6;
  if (LiqSolidSdfEnabled())
    distance = min(distance, LiqSolidSdfAtCell(c));
  return distance;
}

float3 LiqShapeNormal(float3 position)
{
  const float h = 0.25 * LiqDx();
  const float3 n = float3(LiqShapeDistance(position + float3(h, 0, 0)) - LiqShapeDistance(position - float3(h, 0, 0)),
    LiqShapeDistance(position + float3(0, h, 0)) - LiqShapeDistance(position - float3(0, h, 0)),
    LiqShapeDistance(position + float3(0, 0, h)) - LiqShapeDistance(position - float3(0, 0, h)));
  const float len = length(n);
  return len > 1e-12 ? n / len : float3(0, 0, 1);
}

#endif

#if defined(GPU_FLUID_LIQUID_USE_REGIONS)

StructuredBuffer<plGpuFluidLiquidShape> GpuFluidLiqRegions;

bool LiqInsideBox(plGpuFluidLiquidShape box, float3 position)
{
  return all(abs(position - box.CenterType.xyz) <= box.Size.xyz);
}

/// Whether a position (domain metres) is inside an open drain.
bool LiqInsideDrain(float3 position)
{
  const uint count = LiqDrainCount();
  for (uint i = 0; i < count; ++i)
  {
    if (LiqInsideBox(GpuFluidLiqRegions[i], position))
      return true;
  }
  return false;
}

#endif

#if defined(GPU_FLUID_LIQUID_BRICK_DISPATCH)

// Grid kernels on the fine level: one 8x8x8 group per entry of the active brick list.
ByteAddressBuffer GpuFluidLiqBrickArgs; // {groupsX, groupsY, 1, count}
ByteAddressBuffer GpuFluidLiqBrickList;

/// The cell of this thread, or false when the group is past the end of the list.
bool LiqBrickCell(uint3 groupId, uint3 groupThreadId, out int3 cell)
{
  const uint4 args = GpuFluidLiqBrickArgs.Load4(0);
  const uint listIndex = GpuFluidLinearGroup(groupId, args.x);
  cell = int3(0, 0, 0);
  if (listIndex >= args.w)
    return false;

  const uint brick = GpuFluidLiqBrickList.Load(listIndex * 4);
  const uint3 bricks = LiqBricks();
  const uint3 b = uint3(brick % bricks.x, (brick / bricks.x) % bricks.y, brick / (bricks.x * bricks.y));
  cell = int3(b * GPU_FLUID_LIQUID_BRICK + groupThreadId);
  return LiqInside(cell);
}

#endif

#if defined(GPU_FLUID_LIQUID_PARTICLE_DISPATCH)

// Particle kernels: 256 threads per group over the particles of a set.
ByteAddressBuffer GpuFluidLiqParticleArgs; // {groupsX, groupsY, 1, count}

/// The particle of this thread, or false past the alive count.
bool LiqParticleIndex(uint3 groupId, uint groupIndex, out uint index)
{
  const uint4 args = GpuFluidLiqParticleArgs.Load4(0);
  index = GpuFluidLinearGroup(groupId, args.x) * GPU_FLUID_LIQUID_PARTICLE_GROUP + groupIndex;
  return index < args.w;
}

#endif

#endif // !GPU_FLUID_LIQUID_SEED_KERNEL
