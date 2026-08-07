#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Resources/VectorFieldResource.h>

/// Where the vector field data comes from
struct PL_PARTICLEPLUGIN_DLL plParticleVectorFieldSource
{
  using StorageType = plUInt8;

  enum Enum
  {
    CurlNoise, ///< a procedurally baked divergence-free noise field (swirling smoke/dust)
    File,      ///< an .fga file (Fluid Grid ASCII, the industry-standard export format)

    Default = CurlNoise
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleVectorFieldSource);

/// How the sampled vector is applied to the particle
struct PL_PARTICLEPLUGIN_DLL plParticleVectorFieldMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Force,    ///< accelerates the velocity (physical push)
    Velocity, ///< overrides the velocity (pure flow field)

    Default = Force
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleVectorFieldMode);

/// How positions outside the field box map into the grid
struct PL_PARTICLEPLUGIN_DLL plParticleVectorFieldMapping
{
  using StorageType = plUInt8;

  enum Enum
  {
    Tile,  ///< the field repeats infinitely
    Clamp, ///< the border vectors extend outwards

    Default = Tile
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleVectorFieldMapping);

class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_VectorField final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_VectorField, plParticleBehaviorFactory);

public:
  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleVectorFieldSource> m_Source;
  plString m_sFile;

  // curl noise bake parameters
  plUInt8 m_uiResolution = 32;
  plUInt32 m_uiSeed = 0;
  float m_fFrequency = 2.0f;
  plUInt8 m_uiOctaves = 1;

  plEnum<plParticleVectorFieldMode> m_Mode;
  plEnum<plParticleVectorFieldMapping> m_Mapping;
  plVec3 m_vFieldSize = plVec3(4.0f);
  plVec3 m_vPositionOffset = plVec3::MakeZero();
  float m_fStrength = 1.0f;
  plString m_sStrengthParameter;
};

/// Moves particles along a 3D vector field (curl noise or an imported .fga field).
///
/// The field box is anchored at the effect and spans FieldSize meters; outside the box
/// the field tiles or clamps. Force mode accelerates particles, Velocity mode overrides
/// their velocity for pure flow-field motion.
class PL_PARTICLEPLUGIN_DLL plParticleBehavior_VectorField final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_VectorField, plParticleBehavior);

public:
  plVectorFieldResourceHandle m_hField;
  plEnum<plParticleVectorFieldMode> m_Mode;
  plEnum<plParticleVectorFieldMapping> m_Mapping;
  plVec3 m_vFieldSize = plVec3(4.0f);
  plVec3 m_vPositionOffset = plVec3::MakeZero();
  float m_fStrength = 1.0f;
  plTempHashedString m_sStrengthParameter;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
};
