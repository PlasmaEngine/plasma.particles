#pragma once

#include <Foundation/DataProcessing/Stream/ProcessingStream.h>
#include <Foundation/Reflection/Reflection.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <ParticlePlugin/System/ParticleProgram.h>

class plParticleSystemInstance;

/// The particle data a Set Attribute block writes.
struct PL_PARTICLEPLUGIN_DLL plParticleSinkTarget
{
  using StorageType = plUInt8;

  enum Enum
  {
    Position,      ///< vector
    Velocity,      ///< vector
    Size,          ///< number
    Color,         ///< colour
    RotationSpeed, ///< number, radians per second
    Custom,        ///< a custom attribute of the system, named on the block
    Rotation,      ///< number, degrees (the RotationOffset stream); Add in Update is degrees per second
    Facing,        ///< vector, the direction a Custom Facing quad faces (the Axis stream)
    SizeScale,     ///< vector, per-axis multipliers of Size (X/Y on quads, X/Y/Z on meshes)
    Emissive,      ///< number, per-particle multiplier of the type's emissive map

    Default = Color
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleSinkTarget);

struct PL_PARTICLEPLUGIN_DLL plParticleSinkMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Replace,
    Add,      ///< in Update, the value is a rate: it is scaled by the step length
    Multiply, ///< per component

    Default = Replace
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleSinkMode);

/// Which part of a vector or colour target a Set Attribute block writes. A single component takes a
/// number and leaves the other components as they are.
struct PL_PARTICLEPLUGIN_DLL plParticleSinkComponent
{
  using StorageType = plUInt8;

  enum Enum
  {
    All,
    X, ///< R of a colour
    Y, ///< G of a colour
    Z, ///< B of a colour
    W, ///< A of a colour

    Default = All
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleSinkComponent);

/// The value type of a custom attribute. It decides the stream: Float, Float3 or Half4.
struct PL_PARTICLEPLUGIN_DLL plParticleCustomAttributeType
{
  using StorageType = plUInt8;

  enum Enum
  {
    Float,
    Vector,
    Color,

    Default = Float
  };

  static plProcessingStream::DataType GetStreamType(Enum type);
};
PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleCustomAttributeType);

/// The authored settings of a Set Attribute block, shared by the Initialize and Update variants.
struct PL_PARTICLEPLUGIN_DLL plParticleAttributeSinkSettings
{
  plEnum<plParticleSinkTarget> m_Target;
  plEnum<plParticleSinkComponent> m_Component;
  plEnum<plParticleSinkMode> m_Mode;
  plString m_sCustomAttribute;
  plEnum<plParticleCustomAttributeType> m_CustomType;

  // the value used when no per-particle wire drives it; only the one the target needs is used
  float m_fNumber = 1.0f;
  plVec3 m_vVector = plVec3::MakeZero();
  plColor m_Color = plColor::White;

  /// The compiled per-particle wires into the value properties ("Number", "Vector", "Color").
  plParticleProgramData m_Program;

  /// The value property the target takes: a single component takes a number.
  const char* GetValuePropertyName() const;
  plUInt32 GetValueComponents() const;

  /// The number of components the target stream has that a block may write (Position keeps its w).
  plUInt32 GetTargetComponents() const;

  /// The one component written, or -1 when the whole target is.
  plInt32 GetWrittenComponent() const;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// Runtime half of a Set Attribute block: runs its program and writes the target stream.
class PL_PARTICLEPLUGIN_DLL plParticleAttributeSink
{
public:
  /// Remembers the settings; the program is only (re)configured by CreateRequiredStreams.
  void SetSettings(const plParticleAttributeSinkSettings* pSettings) { m_pSettings = pSettings; }

  template <typename CREATE>
  void CreateRequiredStreams(CREATE fnCreate)
  {
    m_Runner.Configure(&m_pSettings->m_Program);
    m_Runner.CreateRequiredStreams(fnCreate);

    m_pTarget = nullptr;
    plStringBuilder sCustom;
    const char* szName = GetTargetStream(sCustom);
    if (szName != nullptr)
      fnCreate(szName, GetTargetType(), &m_pTarget);
  }

  /// Writes the particles [uiStart, uiStart + uiCount). fRateScale scales Add (the step length in Update).
  void Apply(const plParticleSystemInstance* pSystem, plUInt64 uiStart, plUInt64 uiCount, const plTime& diff, float fRateScale);

private:
  const char* GetTargetStream(plStringBuilder& ref_sCustom) const;
  plProcessingStream::DataType GetTargetType() const;

  const plParticleAttributeSinkSettings* m_pSettings = nullptr;
  plParticleProgramRunner m_Runner;
  plProcessingStream* m_pTarget = nullptr;
};

/// Accessors for the shared settings, declared in a factory that holds them as m_Settings. Reflection
/// cannot point at a member of a member, so the properties go through these.
#define PL_PARTICLE_ATTRIBUTE_SINK_ACCESSORS                                                                         \
  plParticleSinkTarget::Enum GetTarget() const { return m_Settings.m_Target; }                                      \
  void SetTarget(plParticleSinkTarget::Enum value) { m_Settings.m_Target = value; }                                 \
  plParticleSinkComponent::Enum GetComponent() const { return m_Settings.m_Component; }                             \
  void SetComponent(plParticleSinkComponent::Enum value) { m_Settings.m_Component = value; }                        \
  plParticleSinkMode::Enum GetMode() const { return m_Settings.m_Mode; }                                            \
  void SetMode(plParticleSinkMode::Enum value) { m_Settings.m_Mode = value; }                                       \
  const char* GetCustomAttribute() const { return m_Settings.m_sCustomAttribute; }                                  \
  void SetCustomAttribute(const char* szValue) { m_Settings.m_sCustomAttribute = szValue; }                        \
  plParticleCustomAttributeType::Enum GetCustomType() const { return m_Settings.m_CustomType; }                    \
  void SetCustomType(plParticleCustomAttributeType::Enum value) { m_Settings.m_CustomType = value; }               \
  float GetNumber() const { return m_Settings.m_fNumber; }                                                          \
  void SetNumber(float fValue) { m_Settings.m_fNumber = fValue; }                                                   \
  const plVec3& GetVector() const { return m_Settings.m_vVector; }                                                  \
  void SetVector(const plVec3& vValue) { m_Settings.m_vVector = vValue; }                                           \
  const plColor& GetColor() const { return m_Settings.m_Color; }                                                    \
  void SetColor(const plColor& value) { m_Settings.m_Color = value; }

/// Reflection of the shared settings, for a factory that uses PL_PARTICLE_ATTRIBUTE_SINK_ACCESSORS.
#define PL_PARTICLE_ATTRIBUTE_SINK_PROPERTIES                                                                                \
  PL_ENUM_ACCESSOR_PROPERTY("Target", plParticleSinkTarget, GetTarget, SetTarget),                                           \
    PL_ENUM_ACCESSOR_PROPERTY("Component", plParticleSinkComponent, GetComponent, SetComponent),                               \
    PL_ACCESSOR_PROPERTY("CustomAttribute", GetCustomAttribute, SetCustomAttribute),                                          \
    PL_ENUM_ACCESSOR_PROPERTY("CustomType", plParticleCustomAttributeType, GetCustomType, SetCustomType),                     \
    PL_ENUM_ACCESSOR_PROPERTY("Mode", plParticleSinkMode, GetMode, SetMode),                                                  \
    PL_ACCESSOR_PROPERTY("Number", GetNumber, SetNumber)->AddAttributes(new plDefaultValueAttribute(1.0f)),                  \
    PL_ACCESSOR_PROPERTY("Vector", GetVector, SetVector),                                                                     \
    PL_ACCESSOR_PROPERTY("Color", GetColor, SetColor)->AddAttributes(new plDefaultValueAttribute(plColor::White), new plExposeColorAlphaAttribute())
