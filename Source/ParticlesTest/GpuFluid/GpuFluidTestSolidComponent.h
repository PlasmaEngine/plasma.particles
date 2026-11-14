#pragma once

#include <Core/World/World.h>
#include <RendererCore/Meshes/CpuMeshResource.h>

struct plMsgExtractGeometry;
class plGeometry;

using plGpuFluidTestSolidComponentManager = plComponentManager<class plGpuFluidTestSolidComponent, plBlockStorageType::Compact>;

/// \brief Test only: answers plMsgExtractGeometry (ExtractionMode::CollisionMesh) with CPU meshes, the contract a Jolt
/// static actor fulfils in production. Jolt is not loadable in the test on this Mac, and code-built render meshes have
/// no CPU mesh, so the liquid's static solids bake is fed through this.
///
/// Each part is one mesh object (one closed mesh, placed by its own transform relative to the owner); the bake signs
/// the distance per object, so parts may touch or overlap.
class plGpuFluidTestSolidComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidTestSolidComponent, plComponent, plGpuFluidTestSolidComponentManager);

public:
  plGpuFluidTestSolidComponent();
  ~plGpuFluidTestSolidComponent();

  struct Part
  {
    plTransform m_LocalTransform = plTransform::MakeIdentity();
    plCpuMeshResourceHandle m_hMesh;
  };

  plHybridArray<Part, 8> m_Parts;

  /// A CPU mesh (positions and indices) from a geometry, created once per name.
  static plCpuMeshResourceHandle CreateCpuMesh(plStringView sName, plGeometry& ref_geometry);

  /// The unit box (full extents 1) as a CPU mesh.
  static plCpuMeshResourceHandle GetUnitBox();

protected:
  void OnMsgExtractGeometry(plMsgExtractGeometry& ref_msg) const;
};
