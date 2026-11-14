#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Time/Stopwatch.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidBasinColumns.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidBasinSolver.h>

namespace
{
  struct Crossing
  {
    PL_DECLARE_POD_TYPE();

    plUInt32 m_uiColumn;
    float m_fZ;
  };
} // namespace

void plGpuFluidBasinColumns::Build(const plDynamicArray<plGpuFluidLiquidSolidBake::SourceMesh>& meshes, const plVec3& vOrigin, const plVec2U32& vColumns, float fColumnSize, float fHeight)
{
  plStopwatch timer;

  m_vColumns = vColumns;
  m_fColumnSize = plMath::Max(fColumnSize, 1e-3f);
  m_fHeight = plMath::Max(fHeight, m_fSlice);
  m_vOrigin = vOrigin;
  m_uiTriangles = 0;

  const plUInt32 uiColumns = vColumns.x * vColumns.y;
  const float fDx = m_fColumnSize;
  // The sample point of a column sits a little off its centre, so lines through grid-aligned edges and corners of
  // box-shaped geometry (shared by two triangles) are not counted twice or missed.
  const plVec2 vJitter(0.0137f * fDx, 0.0291f * fDx);

  // Every crossing of a column's vertical line with a triangle, in metres above the basin's floor.
  plDynamicArray<Crossing> crossings;
  for (const plGpuFluidLiquidSolidBake::SourceMesh& mesh : meshes)
  {
    for (plUInt32 t = 0; t + 2 < mesh.m_Indices.GetCount(); t += 3)
    {
      const plVec3 a = mesh.m_Positions[mesh.m_Indices[t]] - vOrigin;
      const plVec3 b = mesh.m_Positions[mesh.m_Indices[t + 1]] - vOrigin;
      const plVec3 c = mesh.m_Positions[mesh.m_Indices[t + 2]] - vOrigin;
      ++m_uiTriangles;

      // Vertical triangles (walls, sides) are never crossed by a vertical line.
      const float fArea2 = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
      if (plMath::Abs(fArea2) < 1e-10f)
        continue;

      const float fMinX = plMath::Min(a.x, b.x, c.x), fMaxX = plMath::Max(a.x, b.x, c.x);
      const float fMinY = plMath::Min(a.y, b.y, c.y), fMaxY = plMath::Max(a.y, b.y, c.y);
      const plInt32 iX0 = plMath::Max(static_cast<plInt32>(plMath::Floor((fMinX - vJitter.x) / fDx - 0.5f)), 0);
      const plInt32 iX1 = plMath::Min(static_cast<plInt32>(plMath::Ceil((fMaxX - vJitter.x) / fDx - 0.5f)), static_cast<plInt32>(vColumns.x) - 1);
      const plInt32 iY0 = plMath::Max(static_cast<plInt32>(plMath::Floor((fMinY - vJitter.y) / fDx - 0.5f)), 0);
      const plInt32 iY1 = plMath::Min(static_cast<plInt32>(plMath::Ceil((fMaxY - vJitter.y) / fDx - 0.5f)), static_cast<plInt32>(vColumns.y) - 1);

      const float fInvArea2 = 1.0f / fArea2;
      for (plInt32 y = iY0; y <= iY1; ++y)
      {
        for (plInt32 x = iX0; x <= iX1; ++x)
        {
          const float px = (static_cast<float>(x) + 0.5f) * fDx + vJitter.x;
          const float py = (static_cast<float>(y) + 0.5f) * fDx + vJitter.y;
          // Barycentric coordinates in the xy plane.
          const float w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) * fInvArea2;
          const float w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) * fInvArea2;
          const float w2 = 1.0f - w0 - w1;
          if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
            continue;
          crossings.PushBack({static_cast<plUInt32>(x) + vColumns.x * static_cast<plUInt32>(y), w0 * a.z + w1 * b.z + w2 * c.z});
        }
      }
    }
  }

  crossings.Sort([](const Crossing& l, const Crossing& r) { return l.m_uiColumn != r.m_uiColumn ? l.m_uiColumn < r.m_uiColumn : l.m_fZ > r.m_fZ; });

  // From the top down the crossings alternate: into a solid, out of it. An odd count (a mesh open at the bottom, a floor
  // slab reaching below the box) leaves the last solid open downwards, to the floor.
  m_First.SetCount(uiColumns);
  m_Count.SetCount(uiColumns);
  m_Intervals.Clear();
  plUInt32 uiNext = 0;
  for (plUInt32 col = 0; col < uiColumns; ++col)
  {
    // This column's crossings: [uiBegin, uiEnd), highest first.
    const plUInt32 uiBegin = uiNext;
    while (uiNext < crossings.GetCount() && crossings[uiNext].m_uiColumn == col)
      ++uiNext;
    const plUInt32 uiEnd = uiNext;

    m_First[col] = m_Intervals.GetCount();
    plUInt32 uiCount = 0;
    for (plUInt32 j = uiBegin; j < uiEnd; j += 2)
    {
      const float fTop = crossings[j].m_fZ;
      const float fBottom = j + 1 < uiEnd ? crossings[j + 1].m_fZ : -1.0e6f;
      const float fLo = plMath::Max(fBottom, 0.0f), fHi = plMath::Min(fTop, m_fHeight);
      if (fHi > fLo && uiCount < 0xFFFF)
      {
        m_Intervals.PushBack(plVec2(fLo, fHi));
        ++uiCount;
      }
    }
    m_Count[col] = static_cast<plUInt16>(uiCount);
  }

  // Volume table: the solid in each 1 cm slice, summed over the columns.
  const plUInt32 uiSlices = static_cast<plUInt32>(plMath::Ceil(m_fHeight / m_fSlice));
  plDynamicArray<double> solid;
  solid.SetCount(uiSlices);
  for (const plVec2& interval : m_Intervals)
  {
    const plUInt32 k0 = plMath::Min(static_cast<plUInt32>(interval.x / m_fSlice), uiSlices - 1);
    const plUInt32 k1 = plMath::Min(static_cast<plUInt32>(interval.y / m_fSlice), uiSlices - 1);
    for (plUInt32 k = k0; k <= k1; ++k)
    {
      const float fLo = plMath::Max(interval.x, k * m_fSlice);
      const float fHi = plMath::Min(interval.y, (k + 1) * m_fSlice);
      if (fHi > fLo)
        solid[k] += fHi - fLo;
    }
  }

  const double fColumnArea = static_cast<double>(fDx) * fDx;
  m_VolumeBelow.SetCount(uiSlices + 1);
  m_VolumeBelow[0] = 0.0;
  for (plUInt32 k = 0; k < uiSlices; ++k)
    m_VolumeBelow[k + 1] = m_VolumeBelow[k] + (static_cast<double>(uiColumns) * m_fSlice - solid[k]) * fColumnArea;

  m_fBuildMs = timer.GetRunningTotal().GetMilliseconds();
}

double plGpuFluidBasinColumns::GetVolumeBelow(float h) const
{
  if (m_VolumeBelow.GetCount() < 2 || h <= 0.0f)
    return 0.0;
  const float fK = h / m_fSlice;
  const plUInt32 k = plMath::Min(static_cast<plUInt32>(fK), m_VolumeBelow.GetCount() - 2);
  const double t = plMath::Clamp(static_cast<double>(fK) - k, 0.0, 1.0);
  return plMath::Lerp(m_VolumeBelow[k], m_VolumeBelow[k + 1], t);
}

float plGpuFluidBasinColumns::GetLevelForVolume(double fVolume) const
{
  if (m_VolumeBelow.GetCount() < 2 || fVolume <= 0.0)
    return 0.0f;
  if (fVolume >= m_VolumeBelow.PeekBack())
    return m_fHeight;

  // The table rises monotonically: bisect for the slice, then interpolate within it.
  plUInt32 lo = 0, hi = m_VolumeBelow.GetCount() - 1;
  while (hi - lo > 1)
  {
    const plUInt32 mid = (lo + hi) / 2;
    if (m_VolumeBelow[mid] <= fVolume)
      lo = mid;
    else
      hi = mid;
  }
  const double fSpan = m_VolumeBelow[hi] - m_VolumeBelow[lo];
  const double t = fSpan > 0.0 ? (fVolume - m_VolumeBelow[lo]) / fSpan : 0.0;
  return static_cast<float>((lo + t) * m_fSlice);
}

float plGpuFluidBasinColumns::GetFreeArea(float h) const
{
  if (m_VolumeBelow.GetCount() < 2)
    return 0.0f;
  const plUInt32 k = plMath::Min(static_cast<plUInt32>(plMath::Max(h, 0.0f) / m_fSlice), m_VolumeBelow.GetCount() - 2);
  return static_cast<float>((m_VolumeBelow[k + 1] - m_VolumeBelow[k]) / m_fSlice);
}

void plGpuFluidBasinColumns::ComputeDepths(float h, plGpuFluidBasinDepths& out_depths) const
{
  const plUInt32 uiColumns = m_vColumns.x * m_vColumns.y;
  out_depths.m_Depths.SetCountUninitialized(uiColumns);
  for (plUInt32 col = 0; col < uiColumns; ++col)
  {
    // The water stands on the highest solid top below the level; a solid around the level keeps it out.
    float fGround = 0.0f;
    bool bInside = false;
    const plUInt32 uiFirst = m_First[col];
    for (plUInt32 k = 0; k < m_Count[col]; ++k)
    {
      const plVec2& interval = m_Intervals[uiFirst + k];
      if (h >= interval.x && h <= interval.y)
        bInside = true;
      else if (interval.y < h)
        fGround = plMath::Max(fGround, interval.y);
    }
    out_depths.m_Depths[col] = bInside ? -1.0f : h - fGround;
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidBasinColumns);
