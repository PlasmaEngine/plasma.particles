#pragma once

#include <Foundation/Types/RefCounted.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidLiquidSolids.h>

class plGpuFluidBasinDepths;

/// \brief The solid profile of a flood basin's floor plan: per column of the basin, the heights the world's collision
/// geometry fills (a table top, a cabinet, a raised floor), and from that how much water the basin holds below any
/// level.
///
/// Built on the CPU from the same collision meshes the liquid's solid bake reads. A vertical line through each column's
/// centre is crossed with the triangles; the crossings, from the top down, alternate between entering and leaving solid
/// (closed meshes). Heights are metres above the basin's floor (the bottom of its box).
class PL_PARTICLEPLUGIN_DLL plGpuFluidBasinColumns : public plRefCounted
{
public:
  plVec2U32 m_vColumns = plVec2U32::MakeZero();
  float m_fColumnSize = 0.05f;
  float m_fHeight = 0.0f;                 ///< the box's height: no level above it
  plVec3 m_vOrigin = plVec3::MakeZero();  ///< world position of the box's minimum corner

  /// Solid intervals (min, max height) of each column, column after column.
  plDynamicArray<plUInt32> m_First;
  plDynamicArray<plUInt16> m_Count;
  plDynamicArray<plVec2> m_Intervals;

  /// Free volume (m^3) below each slice boundary k * m_fSlice.
  float m_fSlice = 0.01f;
  plDynamicArray<double> m_VolumeBelow;

  plUInt32 m_uiTriangles = 0;
  double m_fBuildMs = 0.0;

  /// Reads the meshes and builds the profile and the volume table.
  void Build(const plDynamicArray<plGpuFluidLiquidSolidBake::SourceMesh>& meshes, const plVec3& vOrigin, const plVec2U32& vColumns, float fColumnSize, float fHeight);

  /// Water the basin holds below level h (m above its floor).
  double GetVolumeBelow(float h) const;

  /// The level (m above the floor) a volume of water stands at; the box's height when it holds more.
  float GetLevelForVolume(double fVolume) const;

  /// Free (water) area of the floor plan at level h (m^2): how much a litre raises the level there.
  float GetFreeArea(float h) const;

  /// Water depth of each column at level h: from the level down to the solid it stands on (or the floor). 0 or less
  /// where the level lies inside a solid (the water does not reach the column there).
  void ComputeDepths(float h, plGpuFluidBasinDepths& out_depths) const;
};
