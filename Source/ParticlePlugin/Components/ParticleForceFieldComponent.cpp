#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <ParticlePlugin/Components/ParticleForceFieldComponent.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleForceFieldType, 1)
  PL_ENUM_CONSTANT(plParticleForceFieldType::Directional),
  PL_ENUM_CONSTANT(plParticleForceFieldType::Point),
  PL_ENUM_CONSTANT(plParticleForceFieldType::Vortex),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleForceFieldShape, 1)
  PL_ENUM_CONSTANT(plParticleForceFieldShape::Sphere),
  PL_ENUM_CONSTANT(plParticleForceFieldShape::Box),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_COMPONENT_TYPE(plParticleForceFieldComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Type", plParticleForceFieldType, m_Type),
    PL_ENUM_MEMBER_PROPERTY("Shape", plParticleForceFieldShape, m_Shape),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(5.0f), new plClampValueAttribute(0.1f, 1000.0f)),
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(10.0f)), new plClampValueAttribute(plVec3(0.1f), plVec3(1000.0f))),
    PL_MEMBER_PROPERTY("Strength", m_fStrength)->AddAttributes(new plDefaultValueAttribute(5.0f)),
    PL_MEMBER_PROPERTY("FalloffStart", m_fFalloffStart)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects"),
    new plSphereVisualizerAttribute("Radius", plColorScheme::LightUI(plColorScheme::Orange)),
    new plBoxVisualizerAttribute("Extents", 1.0f, plColorScheme::LightUI(plColorScheme::Orange)),
    new plDirectionVisualizerAttribute(plBasisAxis::PositiveZ, 1.0f, plColorScheme::LightUI(plColorScheme::Orange)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

plParticleForceFieldComponent::plParticleForceFieldComponent() = default;
plParticleForceFieldComponent::~plParticleForceFieldComponent() = default;

void plParticleForceFieldComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);

  auto& s = inout_stream.GetStream();

  s << m_Type;
  s << m_Shape;
  s << m_fRadius;
  s << m_vExtents;
  s << m_fStrength;
  s << m_fFalloffStart;
}

void plParticleForceFieldComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);

  auto& s = inout_stream.GetStream();

  s >> m_Type;
  s >> m_Shape;
  s >> m_fRadius;
  s >> m_vExtents;
  s >> m_fStrength;
  s >> m_fFalloffStart;
}

void plParticleForceFieldComponent::OnActivated()
{
  SUPER::OnActivated();

  GetWorld()->GetOrCreateModule<plParticleWorldModule>()->RegisterForceField(this);
}

void plParticleForceFieldComponent::OnDeactivated()
{
  if (plParticleWorldModule* pModule = GetWorld()->GetModule<plParticleWorldModule>())
  {
    pModule->UnregisterForceField(this);
  }

  SUPER::OnDeactivated();
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Components_ParticleForceFieldComponent);
