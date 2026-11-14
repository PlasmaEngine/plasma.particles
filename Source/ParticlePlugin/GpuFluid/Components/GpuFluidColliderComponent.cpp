#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidColliderComponent.h>
#include <RendererCore/Meshes/CpuMeshResource.h>
#include <RendererCore/Utils/WorldGeoExtractionUtil.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidColliderShape, 1)
  PL_ENUM_CONSTANT(plGpuFluidColliderShape::Sphere),
  PL_ENUM_CONSTANT(plGpuFluidColliderShape::Box),
  PL_ENUM_CONSTANT(plGpuFluidColliderShape::Capsule),
  PL_ENUM_CONSTANT(plGpuFluidColliderShape::Plane),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_COMPONENT_TYPE(plGpuFluidColliderComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Shape", plGpuFluidColliderShape, m_Shape),
    PL_MEMBER_PROPERTY("HalfExtents", m_vHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.5f)), new plClampValueAttribute(plVec3(0.001f), plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.001f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("HalfHeight", m_fHalfHeight)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Static", m_bStatic),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgExtractGeometry, OnMsgExtractGeometry),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/GpuFluid"),
    new plBoxVisualizerAttribute("HalfExtents", 2.0f, plColorScheme::LightUI(plColorScheme::Grape)),
    new plSphereVisualizerAttribute("Radius", plColorScheme::LightUI(plColorScheme::Grape)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

plGpuFluidColliderComponent::plGpuFluidColliderComponent() = default;
plGpuFluidColliderComponent::~plGpuFluidColliderComponent() = default;

void plGpuFluidColliderComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_Shape;
  s << m_vHalfExtents;
  s << m_fRadius;
  s << m_fHalfHeight;
  s << m_bStatic;
}

void plGpuFluidColliderComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();

  s >> m_Shape;
  s >> m_vHalfExtents;
  s >> m_fRadius;
  s >> m_fHalfHeight;
  s >> m_bStatic;
}

plUInt64 plGpuFluidColliderComponent::GetBakeHash() const
{
  if (!m_bStatic || !IsActiveAndInitialized())
    return 0;

  const plTransform t = GetOwner()->GetGlobalTransform();
  struct
  {
    plVec3 m_vPosition;
    plQuat m_qRotation;
    plVec3 m_vScale;
    plVec3 m_vHalfExtents;
    float m_fRadius;
    float m_fHalfHeight;
    plUInt32 m_uiShape;
  } key = {t.m_vPosition, t.m_qRotation, t.m_vScale, m_vHalfExtents, m_fRadius, m_fHalfHeight, static_cast<plUInt32>(m_Shape.GetValue())};
  return plHashingUtils::xxHash64(&key, sizeof(key)) | 1u;
}

void plGpuFluidColliderComponent::OnMsgExtractGeometry(plMsgExtractGeometry& ref_msg) const
{
  // Only static colliders, and only as collision: moving ones do not take part in the liquid's static bake.
  if (!m_bStatic || ref_msg.m_Mode != plWorldGeoExtractionUtil::ExtractionMode::CollisionMesh)
    return;

  // The shape in the object's space, one resource per shape and size.
  plStringBuilder sName;
  plGeometry geom;
  switch (m_Shape)
  {
    case plGpuFluidColliderShape::Sphere:
      sName.SetFormat("GpuFluidCollider_Sphere_{}", m_fRadius);
      geom.AddGeodesicSphere(m_fRadius, 2);
      break;

    case plGpuFluidColliderShape::Capsule:
      sName.SetFormat("GpuFluidCollider_Capsule_{}_{}", m_fRadius, m_fHalfHeight);
      geom.AddCapsule(m_fRadius, 2.0f * m_fHalfHeight, 16, 4);
      break;

    case plGpuFluidColliderShape::Plane:
    {
      // Solid below the plane: a slab 200 m across and 50 m deep under it (the bake keeps only what is in the domain).
      sName = "GpuFluidCollider_Plane";
      plGeometry::GeoOptions opt;
      opt.m_Transform = plMat4::MakeTranslation(plVec3(0.0f, 0.0f, -25.0f));
      geom.AddBox(plVec3(200.0f, 200.0f, 50.0f), false, opt);
      break;
    }

    default:
      sName.SetFormat("GpuFluidCollider_Box_{}_{}_{}", m_vHalfExtents.x, m_vHalfExtents.y, m_vHalfExtents.z);
      geom.AddBox(m_vHalfExtents * 2.0f, false);
      break;
  }

  plCpuMeshResourceHandle hMesh = plResourceManager::GetExistingResource<plCpuMeshResource>(sName);
  if (!hMesh.IsValid())
  {
    geom.TriangulatePolygons();
    geom.ComputeTangents();

    plMeshResourceDescriptor desc;
    desc.MeshBufferDesc().AddCommonStreams();
    desc.MeshBufferDesc().AllocateStreamsFromGeometry(geom, plGALPrimitiveTopology::Triangles);
    desc.AddSubMesh(desc.MeshBufferDesc().GetPrimitiveCount(), 0, 0);
    desc.ComputeBounds();
    hMesh = plResourceManager::GetOrCreateResource<plCpuMeshResource>(sName, std::move(desc), sName);
  }

  ref_msg.AddMeshObject(GetOwner()->GetGlobalTransform(), hMesh);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Components_GpuFluidColliderComponent);
