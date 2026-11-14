#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Messages/CommonMessages.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidLiquidModifierComponents.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidLiquidVolumeComponent.h>
#include <RendererCore/Debug/DebugRenderer.h>

namespace
{
  /// While editing (the world does not simulate) the modifiers draw what the visualizers cannot show.
  bool IsEditing(const plWorld& world) { return !world.GetWorldSimulationEnabled(); }

  plBoundingBox DomainBox(const plGpuFluidLiquidVolumeComponent& volume)
  {
    const plVec3 vMin = volume.GetDomainOrigin();
    return plBoundingBox::MakeFromMinMax(vMin, vMin + volume.GetDomainSize());
  }

  void DrawOutside(const plComponent& component, const plBoundingBox& box)
  {
    plDebugRenderer::DrawLineBox(component.GetWorld(), box, plColorScheme::LightUI(plColorScheme::Red));
    plDebugRenderer::Draw3DText(component.GetWorld(), "Outside any liquid volume", box.GetCenter() + plVec3(0.0f, 0.0f, box.GetHalfExtents().z), plColorScheme::LightUI(plColorScheme::Red), 14);
  }
} // namespace

const plGpuFluidLiquidVolumeComponent* plGpuFluidLiquidModifiers::FindVolumeAt(const plWorld& world, const plVec3& vPoint)
{
  const plGpuFluidLiquidVolumeComponentManager* pManager = world.GetComponentManager<plGpuFluidLiquidVolumeComponentManager>();
  if (pManager == nullptr)
    return nullptr;

  for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
  {
    if (it->IsActiveAndInitialized() && DomainBox(*it).Contains(vPoint))
      return it;
  }
  return nullptr;
}

bool plGpuFluidLiquidModifiers::OverlapsAnyVolume(const plWorld& world, const plBoundingBox& box)
{
  const plGpuFluidLiquidVolumeComponentManager* pManager = world.GetComponentManager<plGpuFluidLiquidVolumeComponentManager>();
  if (pManager == nullptr)
    return false;

  for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
  {
    if (it->IsActiveAndInitialized() && DomainBox(*it).Overlaps(box))
      return true;
  }
  return false;
}

plBoundingBox plGpuFluidLiquidModifiers::GetWorldBox(const plTransform& transform, const plVec3& vHalfExtents)
{
  plBoundingBox box = plBoundingBox::MakeFromMinMax(-vHalfExtents, vHalfExtents);
  box.TransformFromOrigin(transform.GetAsMat4());
  return box;
}

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plGpuFluidLiquidSourceComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.05f), new plClampValueAttribute(0.001f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Speed", m_fSpeed)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.0f, 30.0f), new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("FlowRate", m_fFlowRate)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" l/s")),
    PL_MEMBER_PROPERTY("Flowing", m_bFlowing)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgSetPlaying, OnMsgSetPlaying),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/GpuFluid"),
    new plCylinderVisualizerAttribute(plBasisAxis::PositiveX, "", "Radius", plColorScheme::LightUI(plColorScheme::Cyan)),
    new plDirectionVisualizerAttribute(plBasisAxis::PositiveX, 0.25f, plColorScheme::LightUI(plColorScheme::Cyan), nullptr, "Speed"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE

PL_BEGIN_COMPONENT_TYPE(plGpuFluidLiquidDrainComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("HalfExtents", m_vHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.2f, 0.2f, 0.05f)), new plClampValueAttribute(plVec3(0.001f), plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Open", m_bOpen)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgSetPlaying, OnMsgSetPlaying),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/GpuFluid"),
    new plBoxVisualizerAttribute("HalfExtents", 2.0f, plColorScheme::LightUI(plColorScheme::Orange)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE

PL_BEGIN_COMPONENT_TYPE(plGpuFluidLiquidFillComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("HalfExtents", m_vHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.5f)), new plClampValueAttribute(plVec3(0.001f), plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Velocity", m_vVelocity)->AddAttributes(new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("Jitter", m_fJitter)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Stratified", m_bStratified)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/GpuFluid"),
    new plBoxVisualizerAttribute("HalfExtents", 2.0f, plColorScheme::LightUI(plColorScheme::Blue)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

//////////////////////////////////////////////////////////////////////////
// Source

plGpuFluidLiquidSourceComponent::plGpuFluidLiquidSourceComponent() = default;
plGpuFluidLiquidSourceComponent::~plGpuFluidLiquidSourceComponent() = default;

void plGpuFluidLiquidSourceComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_fRadius;
  s << m_fSpeed;
  s << m_fFlowRate;
  s << m_bFlowing;
}

void plGpuFluidLiquidSourceComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();

  s >> m_fRadius;
  s >> m_fSpeed;
  s >> m_fFlowRate;
  s >> m_bFlowing;
}

plVec3 plGpuFluidLiquidSourceComponent::GetWorldCenter() const
{
  return GetOwner()->GetGlobalPosition();
}

plVec3 plGpuFluidLiquidSourceComponent::GetWorldVelocity() const
{
  return GetOwner()->GetGlobalRotation() * plVec3(m_fSpeed, 0.0f, 0.0f);
}

float plGpuFluidLiquidSourceComponent::GetWorldRadius() const
{
  const plVec3 vScale = GetOwner()->GetGlobalScaling();
  return m_fRadius * plMath::Max(plMath::Abs(vScale.y), plMath::Abs(vScale.z));
}

void plGpuFluidLiquidSourceComponent::OnMsgSetPlaying(plMsgSetPlaying& ref_msg)
{
  m_bFlowing = ref_msg.m_bPlay;
}

void plGpuFluidLiquidSourceComponent::Update()
{
  if (!IsEditing(*GetWorld()))
    return;

  if (plGpuFluidLiquidModifiers::FindVolumeAt(*GetWorld(), GetWorldCenter()) == nullptr)
  {
    const float fRadius = GetWorldRadius();
    DrawOutside(*this, plBoundingBox::MakeFromCenterAndHalfExtents(GetWorldCenter(), plVec3(fRadius)));
  }
}

//////////////////////////////////////////////////////////////////////////
// Drain

plGpuFluidLiquidDrainComponent::plGpuFluidLiquidDrainComponent() = default;
plGpuFluidLiquidDrainComponent::~plGpuFluidLiquidDrainComponent() = default;

void plGpuFluidLiquidDrainComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_vHalfExtents;
  s << m_bOpen;
}

void plGpuFluidLiquidDrainComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();

  s >> m_vHalfExtents;
  s >> m_bOpen;
}

plBoundingBox plGpuFluidLiquidDrainComponent::GetWorldBox() const
{
  return plGpuFluidLiquidModifiers::GetWorldBox(GetOwner()->GetGlobalTransform(), m_vHalfExtents);
}

void plGpuFluidLiquidDrainComponent::OnMsgSetPlaying(plMsgSetPlaying& ref_msg)
{
  m_bOpen = ref_msg.m_bPlay;
}

void plGpuFluidLiquidDrainComponent::Update()
{
  if (!IsEditing(*GetWorld()))
    return;

  const plBoundingBox box = GetWorldBox();
  if (!plGpuFluidLiquidModifiers::OverlapsAnyVolume(*GetWorld(), box))
  {
    DrawOutside(*this, box);
    return;
  }

  // Open: a dark, filled box (liquid goes in); closed: only the visualizer's outline.
  if (m_bOpen)
    plDebugRenderer::DrawSolidBox(GetWorld(), box, plColor(0.0f, 0.0f, 0.0f, 0.45f));
}

//////////////////////////////////////////////////////////////////////////
// Fill

plGpuFluidLiquidFillComponent::plGpuFluidLiquidFillComponent() = default;
plGpuFluidLiquidFillComponent::~plGpuFluidLiquidFillComponent() = default;

void plGpuFluidLiquidFillComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_vHalfExtents;
  s << m_vVelocity;
  s << m_fJitter;
  s << m_bStratified;
}

void plGpuFluidLiquidFillComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();

  s >> m_vHalfExtents;
  s >> m_vVelocity;
  s >> m_fJitter;
  s >> m_bStratified;
}

plBoundingBox plGpuFluidLiquidFillComponent::GetWorldBox() const
{
  return plGpuFluidLiquidModifiers::GetWorldBox(GetOwner()->GetGlobalTransform(), m_vHalfExtents);
}

plVec3 plGpuFluidLiquidFillComponent::GetWorldVelocity() const
{
  return GetOwner()->GetGlobalRotation() * m_vVelocity;
}

void plGpuFluidLiquidFillComponent::Update()
{
  if (!IsEditing(*GetWorld()))
    return;

  const plBoundingBox box = GetWorldBox();
  if (!plGpuFluidLiquidModifiers::OverlapsAnyVolume(*GetWorld(), box))
    DrawOutside(*this, box);
  else if (!m_vVelocity.IsZero(1e-4f))
  {
    // The starting velocity, from the box's centre.
    const plVec3 vVelocity = GetWorldVelocity();
    const plTransform arrow(box.GetCenter(), plQuat::MakeShortestRotation(plVec3::MakeAxisX(), vVelocity.GetNormalized()));
    plDebugRenderer::DrawArrow(GetWorld(), 0.25f * vVelocity.GetLength(), plColorScheme::LightUI(plColorScheme::Blue), arrow);
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Components_GpuFluidLiquidModifierComponents);
