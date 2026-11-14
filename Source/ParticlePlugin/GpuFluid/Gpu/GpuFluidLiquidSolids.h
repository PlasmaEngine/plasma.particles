#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererCore/Utils/WorldGeoExtractionUtil.h>

/// \brief The static solids of a liquid domain, binned per brick on the CPU for the GPU bake (plan section 7).
///
/// Built from the world's collision geometry (plWorldGeoExtractionUtil, ExtractionMode::CollisionMesh): every
/// triangle within the bake band of the domain, in domain-relative metres, with the angle-weighted pseudonormals of its
/// face, edges and vertices (Baerentzen & Aanaes), which give the sign of the distance. The GPU pass LiquidSolidsBake
/// computes the exact unsigned distance to the triangles binned to a brick (every triangle whose box, grown by the band,
/// touches the brick), and LiquidSolidsSign carries the sign up each column of cells to the cells farther than the band
/// from every triangle (the inside of a thick solid).
///
/// Immutable once built; shared between the component, the frame inputs and the solver.
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidSolidBake : public plRefCounted
{
public:
  /// Words per triangle record: the corners a, b, c; the face normal; the edge pseudonormals of ab, bc, ca; the vertex
  /// pseudonormals of a, b, c (ten float3); the index of the mesh object it belongs to (a uint) and a pad word
  /// (LiquidSolidsBake.plShader reads the same layout). The bake signs the distance per object and keeps the smallest,
  /// so touching or overlapping objects (a table's legs under its top) form their union.
  static constexpr plUInt32 s_uiFloatsPerTriangle = 32;

  plVec3U32 m_vCells = plVec3U32::MakeZero();
  plVec3U32 m_vBricks = plVec3U32::MakeZero();
  float m_fCellSize = 0.0f;
  float m_fBand = 0.0f; ///< metres: distances are exact up to this, the sign carries beyond

  plDynamicArray<float> m_Triangles;     ///< s_uiFloatsPerTriangle per triangle
  plDynamicArray<plUInt32> m_BrickBins;  ///< per brick: first index into m_Indices, count
  plDynamicArray<plUInt32> m_Indices;    ///< triangle indices, brick after brick
  plDynamicArray<plUInt32> m_BakeBricks; ///< the bricks with at least one triangle

  plUInt32 m_uiMeshObjects = 0;      ///< mesh objects the extraction returned
  plUInt32 m_uiSourceTriangles = 0;  ///< their triangles before the domain cull
  plUInt32 m_uiMaxBinCount = 0;      ///< most triangles in one brick
  double m_fBuildMs = 0.0;           ///< CPU time of Build()

  plUInt32 GetTriangleCount() const { return m_Triangles.GetCount() / s_uiFloatsPerTriangle; }

  /// World-space triangles of the extracted mesh objects, read on the thread that owns the resources (the CPU meshes
  /// are locked), so the binning can run as a task.
  struct SourceMesh
  {
    plDynamicArray<plVec3> m_Positions; ///< world space
    plDynamicArray<plUInt32> m_Indices; ///< three per triangle, counter-clockwise seen from outside
  };

  /// Reads the triangles of the mesh objects that can touch the domain (world box vDomainMin..vDomainMax grown by
  /// fMargin). Objects whose CPU mesh is not loaded or has no positions are skipped.
  static void GatherSourceMeshes(const plWorldGeoExtractionUtil::MeshObjectList& objects, const plVec3& vDomainMin, const plVec3& vDomainMax, float fMargin, plDynamicArray<SourceMesh>& out_meshes);

  /// Welds each mesh's vertices, computes the pseudonormals, culls to the domain and bins per brick.
  void Build(const plDynamicArray<SourceMesh>& meshes, const plVec3& vDomainOrigin, const plVec3U32& vCells, float fCellSize, float fBandCells);
};

using plGpuFluidLiquidSolidBakePtr = plSharedPtr<const plGpuFluidLiquidSolidBake>;

/// \brief One static solids bake in flight: the meshes read on the update thread, binned on a task (Run()).
class PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidSolidBakeJob : public plRefCounted
{
public:
  plDynamicArray<plGpuFluidLiquidSolidBake::SourceMesh> m_Meshes;
  plVec3 m_vDomainOrigin = plVec3::MakeZero();
  plVec3U32 m_vCells = plVec3U32::MakeZero();
  float m_fCellSize = 0.0f;
  plSharedPtr<plGpuFluidLiquidSolidBake> m_pResult;

  void Run();
};
