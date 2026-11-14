#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Messages/CommonMessages.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasEmitterComponent.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasVolumeComponent.h>
#include <RendererCore/Debug/DebugRenderer.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plGpuFluidGasEmitterComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Shape", plGpuFluidEmitterShape, m_Shape),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("HalfExtents", m_vHalfExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.4f, 0.4f, 0.2f)), new plClampValueAttribute(plVec3(0.0f), plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Softness", m_fSoftness)->AddAttributes(new plDefaultValueAttribute(0.2f), new plClampValueAttribute(0.001f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Rate", m_fRate)->AddAttributes(new plDefaultValueAttribute(5.0f), new plClampValueAttribute(0.0f, 1000.0f), new plSuffixAttribute(" 1/s")),
    PL_MEMBER_PROPERTY("Temperature", m_fTemperature)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Velocity", m_vVelocity)->AddAttributes(new plDefaultValueAttribute(plVec3(0.0f, 0.0f, 2.0f)), new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("Noise", m_fNoise)->AddAttributes(new plDefaultValueAttribute(0.9f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("NoiseScale", m_fNoiseScale)->AddAttributes(new plDefaultValueAttribute(0.3f), new plClampValueAttribute(0.01f, plVariant()), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Emitting", m_bEmitting)->AddAttributes(new plDefaultValueAttribute(true)),
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
    new plSphereVisualizerAttribute("Radius", plColorScheme::LightUI(plColorScheme::Orange)),
    new plBoxVisualizerAttribute("HalfExtents", 2.0f, plColorScheme::LightUI(plColorScheme::Orange)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

plGpuFluidGasEmitterComponent::plGpuFluidGasEmitterComponent() = default;
plGpuFluidGasEmitterComponent::~plGpuFluidGasEmitterComponent() = default;

void plGpuFluidGasEmitterComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_Shape;
  s << m_fRadius;
  s << m_vHalfExtents;
  s << m_fSoftness;
  s << m_fRate;
  s << m_fTemperature;
  s << m_vVelocity;
  s << m_fNoise;
  s << m_fNoiseScale;
  s << m_bEmitting;
}

void plGpuFluidGasEmitterComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();

  s >> m_Shape;
  s >> m_fRadius;
  s >> m_vHalfExtents;
  s >> m_fSoftness;
  s >> m_fRate;
  s >> m_fTemperature;
  s >> m_vVelocity;
  s >> m_fNoise;
  s >> m_fNoiseScale;
  s >> m_bEmitting;
}

void plGpuFluidGasEmitterComponent::OnMsgSetPlaying(plMsgSetPlaying& ref_msg)
{
  m_bEmitting = ref_msg.m_bPlay;
}

void plGpuFluidGasEmitterComponent::Update()
{
  // While editing, an emitter no gas volume holds is drawn red.
  if (GetWorld()->GetWorldSimulationEnabled())
    return;

  const plVec3 vPosition = GetOwner()->GetGlobalPosition();
  if (const plGpuFluidGasVolumeComponentManager* pManager = GetWorld()->GetComponentManager<plGpuFluidGasVolumeComponentManager>())
  {
    for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
    {
      if (it->IsActiveAndInitialized() && it->ContainsPoint(vPosition))
        return;
    }
  }

  const float fSize = m_Shape == plGpuFluidEmitterShape::Box ? m_vHalfExtents.GetLength() : m_fRadius;
  const plColor red = plColorScheme::LightUI(plColorScheme::Red);
  plDebugRenderer::DrawLineSphere(GetWorld(), plBoundingSphere::MakeFromCenterAndRadius(vPosition, plMath::Max(fSize, 0.05f)), red);
  plDebugRenderer::Draw3DText(GetWorld(), "Outside any gas volume", vPosition + plVec3(0.0f, 0.0f, fSize), red, 14);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Components_GpuFluidGasEmitterComponent);
