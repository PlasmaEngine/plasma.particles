#pragma once

#include <Foundation/Strings/HashedString.h>
#include <Foundation/Types/VarianceTypes.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>
#include <ParticlePlugin/System/ParticleProgram.h>

class PL_PARTICLEPLUGIN_DLL plParticleInitializerFactory_VelocityCone final : public plParticleInitializerFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializerFactory_VelocityCone, plParticleInitializerFactory);

public:
  plParticleInitializerFactory_VelocityCone();

  virtual const plRTTI* GetInitializerType() const override;
  virtual void CopyInitializerProperties(plParticleInitializer* pInitializer, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual plParticleProgramData* GetParticleProgram() override { return &m_Program; }

public:
  plAngle m_Angle;
  plVarianceTypeFloat m_Speed;
  plHashedString m_sSpeedScaleParameter; ///< effect float parameter the speed is multiplied by when an instance starts

  /// The cone's direction in the effect's space (+Z, the effect's up, by default). A per-particle wire gives
  /// it in simulation space, the space of the particle and Event inputs.
  plVec3 m_vAxis = plVec3(0, 0, 1);

  /// The per-particle wire into Axis.
  plParticleProgramData m_Program;
};


class PL_PARTICLEPLUGIN_DLL plParticleInitializer_VelocityCone final : public plParticleInitializer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializer_VelocityCone, plParticleInitializer);

public:
  plAngle m_Angle;
  plVarianceTypeFloat m_Speed;
  float m_fSpeedScale = 1.0f;
  plVec3 m_vAxis = plVec3(0, 0, 1);
  const plParticleProgramData* m_pProgram = nullptr;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamVelocity;
  plParticleProgramRunner m_Runner;
};
