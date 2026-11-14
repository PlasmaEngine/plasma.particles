#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidColliderComponent.h>

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

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Components_GpuFluidColliderComponent);
