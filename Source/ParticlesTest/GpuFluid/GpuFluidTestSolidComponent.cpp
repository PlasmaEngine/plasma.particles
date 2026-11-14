#include <ParticlesTest/ParticlesTestPCH.h>

#include <Core/Graphics/Geometry.h>
#include <ParticlesTest/GpuFluid/GpuFluidTestSolidComponent.h>
#include <RendererCore/Meshes/MeshBufferUtils.h>
#include <RendererCore/Utils/WorldGeoExtractionUtil.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plGpuFluidTestSolidComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgExtractGeometry, OnMsgExtractGeometry),
  }
  PL_END_MESSAGEHANDLERS;
}
PL_END_COMPONENT_TYPE
// clang-format on

plGpuFluidTestSolidComponent::plGpuFluidTestSolidComponent() = default;
plGpuFluidTestSolidComponent::~plGpuFluidTestSolidComponent() = default;

plCpuMeshResourceHandle plGpuFluidTestSolidComponent::CreateCpuMesh(plStringView sName, plGeometry& ref_geometry)
{
  plStringBuilder sResource("GpuFluidTest/CpuMesh/", sName);
  plCpuMeshResourceHandle hMesh = plResourceManager::GetExistingResource<plCpuMeshResource>(sResource);
  if (hMesh.IsValid())
    return hMesh;

  ref_geometry.TriangulatePolygons();
  ref_geometry.ComputeTangents();

  plMeshResourceDescriptor desc;
  desc.SetMaterial(0, "{ 7c71fc2d-2a1a-4938-9305-f3a0f26d73cc }"); // LitStandard (never drawn)
  desc.MeshBufferDesc().AddCommonStreams();
  desc.MeshBufferDesc().AllocateStreamsFromGeometry(ref_geometry, plGALPrimitiveTopology::Triangles);
  desc.AddSubMesh(desc.MeshBufferDesc().GetPrimitiveCount(), 0, 0);
  desc.ComputeBounds();
  return plResourceManager::GetOrCreateResource<plCpuMeshResource>(sResource, std::move(desc), sResource);
}

plCpuMeshResourceHandle plGpuFluidTestSolidComponent::GetUnitBox()
{
  plGeometry geometry;
  geometry.AddBox(plVec3(1.0f), false);
  return CreateCpuMesh("UnitBox", geometry);
}

void plGpuFluidTestSolidComponent::OnMsgExtractGeometry(plMsgExtractGeometry& ref_msg) const
{
  if (ref_msg.m_Mode != plWorldGeoExtractionUtil::ExtractionMode::CollisionMesh)
    return;

  const plTransform owner = GetOwner()->GetGlobalTransform();
  for (const Part& part : m_Parts)
    ref_msg.AddMeshObject(plTransform::MakeGlobalTransform(owner, part.m_LocalTransform), part.m_hMesh);
}
