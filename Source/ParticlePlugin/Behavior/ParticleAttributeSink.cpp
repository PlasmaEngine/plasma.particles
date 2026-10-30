#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Math/Float16.h>
#include <ParticlePlugin/Behavior/ParticleAttributeSink.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleSinkTarget, 1)
  PL_ENUM_CONSTANT(plParticleSinkTarget::Position),
  PL_ENUM_CONSTANT(plParticleSinkTarget::Velocity),
  PL_ENUM_CONSTANT(plParticleSinkTarget::Size),
  PL_ENUM_CONSTANT(plParticleSinkTarget::Color),
  PL_ENUM_CONSTANT(plParticleSinkTarget::RotationSpeed),
  PL_ENUM_CONSTANT(plParticleSinkTarget::Custom),
  PL_ENUM_CONSTANT(plParticleSinkTarget::Rotation),
  PL_ENUM_CONSTANT(plParticleSinkTarget::Facing),
  PL_ENUM_CONSTANT(plParticleSinkTarget::SizeScale),
  PL_ENUM_CONSTANT(plParticleSinkTarget::Emissive),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleSinkComponent, 1)
  PL_ENUM_CONSTANT(plParticleSinkComponent::All),
  PL_ENUM_CONSTANT(plParticleSinkComponent::X),
  PL_ENUM_CONSTANT(plParticleSinkComponent::Y),
  PL_ENUM_CONSTANT(plParticleSinkComponent::Z),
  PL_ENUM_CONSTANT(plParticleSinkComponent::W),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleSinkMode, 1)
  PL_ENUM_CONSTANT(plParticleSinkMode::Replace),
  PL_ENUM_CONSTANT(plParticleSinkMode::Add),
  PL_ENUM_CONSTANT(plParticleSinkMode::Multiply),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleCustomAttributeType, 1)
  PL_ENUM_CONSTANT(plParticleCustomAttributeType::Float),
  PL_ENUM_CONSTANT(plParticleCustomAttributeType::Vector),
  PL_ENUM_CONSTANT(plParticleCustomAttributeType::Color),
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

plProcessingStream::DataType plParticleCustomAttributeType::GetStreamType(Enum type)
{
  switch (type)
  {
    case Vector:
      return plProcessingStream::DataType::Float3;
    case Color:
      return plProcessingStream::DataType::Half4;
    default:
      return plProcessingStream::DataType::Float;
  }
}

const char* plParticleAttributeSinkSettings::GetValuePropertyName() const
{
  if (GetWrittenComponent() >= 0)
    return "Number";

  switch (m_Target)
  {
    case plParticleSinkTarget::Position:
    case plParticleSinkTarget::Velocity:
    case plParticleSinkTarget::Facing:
    case plParticleSinkTarget::SizeScale:
      return "Vector";
    case plParticleSinkTarget::Color:
      return "Color";
    case plParticleSinkTarget::Custom:
      return m_CustomType == plParticleCustomAttributeType::Vector ? "Vector" : (m_CustomType == plParticleCustomAttributeType::Color ? "Color" : "Number");
    default:
      return "Number";
  }
}

plUInt32 plParticleAttributeSinkSettings::GetValueComponents() const
{
  const plStringView sProperty = GetValuePropertyName();
  return sProperty == "Vector" ? 3 : (sProperty == "Color" ? 4 : 1);
}

plUInt32 plParticleAttributeSinkSettings::GetTargetComponents() const
{
  switch (m_Target)
  {
    case plParticleSinkTarget::Position:
    case plParticleSinkTarget::Velocity:
    case plParticleSinkTarget::Facing:
    case plParticleSinkTarget::SizeScale:
      return 3;
    case plParticleSinkTarget::Color:
      return 4;
    case plParticleSinkTarget::Custom:
      return m_CustomType == plParticleCustomAttributeType::Vector ? 3 : (m_CustomType == plParticleCustomAttributeType::Color ? 4 : 1);
    default:
      return 1;
  }
}

plInt32 plParticleAttributeSinkSettings::GetWrittenComponent() const
{
  // a number target has only the one component, whatever the setting says
  const plInt32 iComponent = (plInt32)m_Component.GetValue() - 1;
  if (iComponent < 0 || GetTargetComponents() == 1)
    return -1;

  return iComponent;
}

void plParticleAttributeSinkSettings::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 2;
  inout_stream << uiVersion;

  inout_stream << m_Target;
  inout_stream << m_Component;
  inout_stream << m_Mode;
  inout_stream << m_sCustomAttribute;
  inout_stream << m_CustomType;
  inout_stream << m_fNumber;
  inout_stream << m_vVector;
  inout_stream << m_Color;
  m_Program.Save(inout_stream);
}

void plParticleAttributeSinkSettings::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  PL_ASSERT_DEV(uiVersion <= 2, "Invalid Set Attribute version {0}", uiVersion);

  inout_stream >> m_Target;
  if (uiVersion >= 2)
    inout_stream >> m_Component;
  inout_stream >> m_Mode;
  inout_stream >> m_sCustomAttribute;
  inout_stream >> m_CustomType;
  inout_stream >> m_fNumber;
  inout_stream >> m_vVector;
  inout_stream >> m_Color;
  m_Program.Load(inout_stream);
}

//////////////////////////////////////////////////////////////////////////

const char* plParticleAttributeSink::GetTargetStream(plStringBuilder& ref_sCustom) const
{
  switch (m_pSettings->m_Target)
  {
    case plParticleSinkTarget::Position:
      return "Position";
    case plParticleSinkTarget::Velocity:
      return "Velocity";
    case plParticleSinkTarget::Size:
      return "Size";
    case plParticleSinkTarget::Color:
      return "Color";
    case plParticleSinkTarget::RotationSpeed:
      return "RotationSpeed";
    case plParticleSinkTarget::Rotation:
      return "RotationOffset";
    case plParticleSinkTarget::Facing:
      return "Axis";
    case plParticleSinkTarget::SizeScale:
      return "SizeScale";
    case plParticleSinkTarget::Emissive:
      return "Emissive";
    case plParticleSinkTarget::Custom:
      if (m_pSettings->m_sCustomAttribute.IsEmpty())
        return nullptr;
      ref_sCustom.Set("Custom.", m_pSettings->m_sCustomAttribute);
      return ref_sCustom.GetData();
  }

  return nullptr;
}

plProcessingStream::DataType plParticleAttributeSink::GetTargetType() const
{
  switch (m_pSettings->m_Target)
  {
    case plParticleSinkTarget::Position:
      return plProcessingStream::DataType::Float4;
    case plParticleSinkTarget::Velocity:
    case plParticleSinkTarget::Facing:
      return plProcessingStream::DataType::Float3;
    case plParticleSinkTarget::SizeScale:
      return plProcessingStream::DataType::Half3;
    case plParticleSinkTarget::Color:
      return plProcessingStream::DataType::Half4;
    case plParticleSinkTarget::Custom:
      return plParticleCustomAttributeType::GetStreamType(m_pSettings->m_CustomType);
    default:
      return plProcessingStream::DataType::Half;
  }
}

void plParticleAttributeSink::Apply(const plParticleSystemInstance* pSystem, plUInt64 uiStart, plUInt64 uiCount, const plTime& diff, float fRateScale)
{
  if (m_pTarget == nullptr || uiCount == 0)
    return;

  const plParticleAttributeSinkSettings& settings = *m_pSettings;
  const plUInt32 uiValueComponents = settings.GetValueComponents();

  // the authored value, unless a per-particle wire computes it
  plVec4 vLiteral(settings.m_fNumber, 0.0f, 0.0f, 0.0f);
  if (uiValueComponents == 3)
    vLiteral = settings.m_vVector.GetAsVec4(0.0f);
  else if (uiValueComponents == 4)
    vLiteral.Set(settings.m_Color.r, settings.m_Color.g, settings.m_Color.b, settings.m_Color.a);

  plUInt32 uiOutputComponents = 0;
  const float* pOutput = nullptr;
  if (m_Runner.IsActive() && m_Runner.Run(pSystem, uiStart, uiCount, diff))
    pOutput = m_Runner.GetOutput(settings.GetValuePropertyName(), uiOutputComponents);

  const plProcessingStream::DataType type = m_pTarget->GetDataType();
  const plUInt64 uiStride = m_pTarget->GetElementStride();
  plUInt8* pBase = static_cast<plUInt8*>(m_pTarget->GetWritableData()) + uiStart * uiStride;

  const bool bHalf = type == plProcessingStream::DataType::Half || type == plProcessingStream::DataType::Half3 || type == plProcessingStream::DataType::Half4;
  plUInt32 uiTargetComponents = 1;
  if (type == plProcessingStream::DataType::Float3 || type == plProcessingStream::DataType::Half3)
    uiTargetComponents = 3;
  else if (type == plProcessingStream::DataType::Float4 || type == plProcessingStream::DataType::Half4)
    uiTargetComponents = (type == plProcessingStream::DataType::Float4) ? 3 : 4; // Position keeps its w

  // a single component is the only one written; one the target does not have writes nothing
  const plInt32 iWritten = settings.GetWrittenComponent();
  const plUInt32 uiFirst = iWritten >= 0 ? (plUInt32)iWritten : 0;
  const plUInt32 uiEnd = iWritten >= 0 ? plMath::Min((plUInt32)iWritten + 1, uiTargetComponents) : uiTargetComponents;

  // Rotation is authored in degrees but the stream holds radians; Multiply takes a plain factor
  const float fUnitScale = (settings.m_Target == plParticleSinkTarget::Rotation && settings.m_Mode != plParticleSinkMode::Multiply) ? plMath::Pi<float>() / 180.0f : 1.0f;

  for (plUInt64 i = 0; i < uiCount; ++i)
  {
    plVec4 vValue = vLiteral;
    if (pOutput != nullptr)
    {
      for (plUInt32 c = 0; c < uiOutputComponents && c < 4; ++c)
        vValue.GetData()[c] = pOutput[i * uiOutputComponents + c];
    }

    plUInt8* pElement = pBase + i * uiStride;

    for (plUInt32 c = uiFirst; c < uiEnd; ++c)
    {
      float fCurrent = bHalf ? (float)reinterpret_cast<plFloat16*>(pElement)[c] : reinterpret_cast<float*>(pElement)[c];

      // a number written to a vector or colour affects every component
      const float fValue = ((uiValueComponents == 1) ? vValue.x : vValue.GetData()[c]) * fUnitScale;

      switch (settings.m_Mode)
      {
        case plParticleSinkMode::Add:
          fCurrent += fValue * fRateScale;
          break;
        case plParticleSinkMode::Multiply:
          fCurrent *= fValue;
          break;
        default:
          fCurrent = fValue;
          break;
      }

      if (bHalf)
        reinterpret_cast<plFloat16*>(pElement)[c] = fCurrent;
      else
        reinterpret_cast<float*>(pElement)[c] = fCurrent;
    }
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleAttributeSink);
