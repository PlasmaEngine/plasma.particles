#pragma once

#include <Foundation/SimdMath/SimdNoise.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>

/// Applies a turbulent force to particles using 3D curl noise.
///
/// Curl noise produces divergence-free flow fields that look natural and avoid
/// compression artifacts. Useful for smoke, fire, and magical effects.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_Turbulence final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_Turbulence, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_Turbulence();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  float m_fStrength = 2.0f;
  float m_fFrequency = 1.0f;
  plVec3 m_vScrollSpeed = plVec3::MakeZero();
  plUInt8 m_uiOctaves = 1;
  bool m_bAffectVelocity = true;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_Turbulence final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_Turbulence, plParticleBehavior);

public:
  float m_fStrength = 2.0f;
  float m_fFrequency = 1.0f;
  plVec3 m_vScrollSpeed = plVec3::MakeZero();
  plUInt8 m_uiOctaves = 1;
  bool m_bAffectVelocity = true;

protected:
  virtual void CreateRequiredStreams() override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;

  plSimdPerlinNoise m_Noise{12345};
  float m_fTotalTime = 0.0f;
};