#pragma once

#include <Foundation/Types/VarianceTypes.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>

class PL_PARTICLEPLUGIN_DLL plParticleInitializerFactory_ConePosition final : public plParticleInitializerFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializerFactory_ConePosition, plParticleInitializerFactory);

public:
  plParticleInitializerFactory_ConePosition();

  virtual const plRTTI* GetInitializerType() const override;
  virtual void CopyInitializerProperties(plParticleInitializer* pInitializer, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

public:
  plVec3 m_vPositionOffset;
  plAngle m_Angle;    // half-angle of the cone
  float m_fHeight;    // distance from the apex to the base
  bool m_bSpawnOnSurface; // spawn on the lateral surface instead of inside the volume
  bool m_bSetVelocity;    // away from the apex, through the spawn position
  plVarianceTypeFloat m_Speed;
  plString m_sScaleHeightParameter;
};


/// Spawns particles inside (or on the lateral surface of) a cone with its apex at the emitter,
/// opening along the emitter's +Z axis.
class PL_PARTICLEPLUGIN_DLL plParticleInitializer_ConePosition final : public plParticleInitializer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializer_ConePosition, plParticleInitializer);

public:
  plVec3 m_vPositionOffset;
  plAngle m_Angle;
  float m_fHeight;
  bool m_bSpawnOnSurface;
  bool m_bSetVelocity;
  plVarianceTypeFloat m_Speed;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
};
