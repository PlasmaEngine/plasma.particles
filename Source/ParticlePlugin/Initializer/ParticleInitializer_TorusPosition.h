#pragma once

#include <Foundation/Types/VarianceTypes.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>

class PL_PARTICLEPLUGIN_DLL plParticleInitializerFactory_TorusPosition final : public plParticleInitializerFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializerFactory_TorusPosition, plParticleInitializerFactory);

public:
  plParticleInitializerFactory_TorusPosition();

  virtual const plRTTI* GetInitializerType() const override;
  virtual void CopyInitializerProperties(plParticleInitializer* pInitializer, bool bFirstTime) const override;
  virtual float GetSpawnCountMultiplier(const plParticleEffectInstance* pEffect) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

public:
  plVec3 m_vPositionOffset;
  float m_fRadius;     // major radius: distance from the center to the middle of the ring
  float m_fRingRadius; // minor radius: thickness of the ring
  bool m_bSpawnOnSurface;
  bool m_bSetVelocity; // outward from the ring center-line
  plVarianceTypeFloat m_Speed;
  plString m_sScaleRadiusParameter;
};


/// Spawns particles inside (or on the surface of) a torus lying in the emitter's XY plane.
class PL_PARTICLEPLUGIN_DLL plParticleInitializer_TorusPosition final : public plParticleInitializer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializer_TorusPosition, plParticleInitializer);

public:
  plVec3 m_vPositionOffset;
  float m_fRadius;
  float m_fRingRadius;
  bool m_bSpawnOnSurface;
  bool m_bSetVelocity;
  plVarianceTypeFloat m_Speed;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
};
