#pragma once

#include <Shaders/GpuFluid/GpuFluidSparse.hlsli>

// Multigrid of the gas pressure: the operator, its levels and the transfers between them.
//
// Every level solves A x = b with
//   (A x)_i = sum over faces f of w_f (x_n(f) - x_i) - s_i x_i
// where w_f couples a cell to a fluid neighbour (1 on the fine level when both are fluid, the open fraction of the face
// on coarser levels), and s_i is the Dirichlet sink: the coupling to open air (Phi = 0) beyond unallocated bricks and
// open domain faces. Neumann faces (closed domain faces, solids) have w = 0 and add nothing. diag = sum w + s.
//
// Scaling: b of level l is H_l^2 times the right-hand side in 1/s (H_l = 2^l dx); on the fine level b = dx^2 div. A
// coarse level's b is 4 times the mean of its 8 children's residuals, i.e. 0.5 times their sum.
//
// Coefficients of a coarse level come from its children (GasMGSetup inside a brick, GasMGDense above):
// - w of a coarse face = the mean of the 4 child faces across it (so a fully open face is 1, a thin wall along it 0);
// - s = the sum of the children's s / 4, times GpuFluidSinkFactor: open air sits at the centre of the first air cell,
//   half a fine cell beyond the face, which a coarse cell sees closer than its own spacing. The Dirichlet weight of a
//   fully open face at level l is 2^(l+1) / (2^l + 1): 1, 4/3, 8/5, 16/9, ... for F, M, C, D0, ...
// A coarse cell whose children are all solid has diag = 0 and takes no part, like a solid fine cell.
//
// Prolongation is cell-centred trilinear (3/4, 1/4 per axis) from the 8 nearest coarse cells. A coarse sample that is
// open air reads 0, one behind a Neumann boundary (closed face, solid) reads the parent's value, so the correction is
// flat towards walls and falls to zero towards open air.

#define GPU_FLUID_KIND_FLUID 0
#define GPU_FLUID_KIND_DIRICHLET 1
#define GPU_FLUID_KIND_NEUMANN 2

uint GpuFluidFaceCode(uint codes, uint face)
{
  return (codes >> (face * 2)) & 3u;
}

bool GpuFluidCodeIsSolid(uint codes)
{
  return ((codes >> GPU_FLUID_CODE_SOLID_BIT) & 1u) != 0;
}

/// Weight of a fully open Dirichlet face at level l (0 = F): 2^(l+1) / (2^l + 1).
float GpuFluidDirichletWeight(uint level)
{
  const float p = float(1u << min(level, 20u));
  return 2.0 * p / (p + 1.0);
}

/// The factor a sink gains from level l to level l + 1.
float GpuFluidSinkFactor(uint level)
{
  return GpuFluidDirichletWeight(level + 1) / GpuFluidDirichletWeight(level);
}

/// Level of the intra-brick hierarchy: 0 = F, 1 = M, 2 = C; the dense level k is 3 + k.
#define GPU_FLUID_MG_LEVEL_M 1
#define GPU_FLUID_MG_LEVEL_C 2
#define GPU_FLUID_MG_LEVEL_D0 3

/// Kind of a missing neighbour cell beyond the domain: Neumann if any of the faces it lies beyond is closed.
uint GpuFluidOutsideKind(int3 brickOffset, int3 brick)
{
  const int3 grid = int3(GpuFluidGrid());
  const int3 other = brick + brickOffset;
  [unroll] for (uint axis = 0; axis < 3; ++axis)
  {
    if (other[axis] < 0 && !GpuFluidFaceOpen(axis, -1))
      return GPU_FLUID_KIND_NEUMANN;
    if (other[axis] >= grid[axis] && !GpuFluidFaceOpen(axis, 1))
      return GPU_FLUID_KIND_NEUMANN;
  }
  return GPU_FLUID_KIND_DIRICHLET;
}

/// floor(l / 2) for l >= -2.
int GpuFluidHalfFloor(int l)
{
  return (l + 2) / 2 - 1;
}

int3 GpuFluidHalfFloor3(int3 l)
{
  return (l + 2) / 2 - 1;
}

/// The value a coarse sample contributes to a prolongation.
float GpuFluidProlongSample(uint kind, float value, float parent)
{
  return kind == GPU_FLUID_KIND_FLUID ? value : (kind == GPU_FLUID_KIND_DIRICHLET ? 0.0 : parent);
}

// Dense levels: D0 is the virtual brick grid, each next level ceil(n / 2) per axis, until a level has at most
// GPU_FLUID_MG_COARSEST_CELLS cells (or GPU_FLUID_MG_MAX_DENSE_LEVELS levels). Levels are stored one after another.
struct GpuFluidDenseLevel
{
  uint3 m_Dims;
  uint m_uiOffset;
};

uint GpuFluidDenseLevelCount(uint3 grid)
{
  uint count = 1;
  uint3 dims = grid;
  while (dims.x * dims.y * dims.z > GPU_FLUID_MG_COARSEST_CELLS && count < GPU_FLUID_MG_MAX_DENSE_LEVELS)
  {
    dims = (dims + 1) / 2;
    ++count;
  }
  return count;
}

GpuFluidDenseLevel GpuFluidGetDenseLevel(uint3 grid, uint level)
{
  GpuFluidDenseLevel result;
  result.m_Dims = grid;
  result.m_uiOffset = 0;
  for (uint i = 0; i < level; ++i)
  {
    result.m_uiOffset += result.m_Dims.x * result.m_Dims.y * result.m_Dims.z;
    result.m_Dims = (result.m_Dims + 1) / 2;
  }
  return result;
}

uint GpuFluidDenseIndex(GpuFluidDenseLevel level, uint3 c)
{
  return level.m_uiOffset + c.x + level.m_Dims.x * (c.y + level.m_Dims.y * c.z);
}

uint3 GpuFluidDenseCoord(GpuFluidDenseLevel level, uint i)
{
  return uint3(i % level.m_Dims.x, (i / level.m_Dims.x) % level.m_Dims.y, i / (level.m_Dims.x * level.m_Dims.y));
}
