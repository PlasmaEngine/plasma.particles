#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior.h>

/// Where the attractor pulls particles toward
struct PL_PARTICLEPLUGIN_DLL plParticleAttractorTarget
{
  using StorageType = plUInt8;

  enum Enum
  {
    EffectOrigin, ///< Attract toward the effect's world position
    CustomPosition, ///< Attract toward a specified world-space point

    Default = EffectOrigin
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleAttractorTarget);

/// The geometry particles are attracted to
struct PL_PARTICLEPLUGIN_DLL plParticleAttractorShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    Point,  ///< Attract toward the target point
    Line,   ///< Attract toward the closest point on a line through the target along Axis
    Vortex, ///< Swirl around a line through the target along Axis

    Default = Point
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleAttractorShape);

/// Pulls particles toward a point, a line, or swirls them around an axis, with distance-based falloff.
///
/// Can modify velocity (physically correct acceleration) or position directly (snapping).
/// MinDistance prevents singularity at the attractor center. Repel inverts the pull.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_AttractToPosition final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_AttractToPosition, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_AttractToPosition();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleAttractorTarget> m_Target;
  plEnum<plParticleAttractorShape> m_Shape;
  plVec3 m_vCustomPosition = plVec3::MakeZero();
  plVec3 m_vAxis = plVec3(0, 0, 1); // line / vortex axis; relative to the emitter when the target is the effect origin
  float m_fForce = 5.0f;
  float m_fMaxDistance = 10.0f;
  float m_fMinDistance = 0.1f;
  bool m_bAffectVelocity = true;
  bool m_bRepel = false;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_AttractToPosition final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_AttractToPosition, plParticleBehavior);

public:
  plEnum<plParticleAttractorTarget> m_Target;
  plEnum<plParticleAttractorShape> m_Shape;
  plVec3 m_vCustomPosition = plVec3::MakeZero();
  plVec3 m_vAxis = plVec3(0, 0, 1);
  float m_fForce = 5.0f;
  float m_fMaxDistance = 10.0f;
  float m_fMinDistance = 0.1f;
  bool m_bAffectVelocity = true;
  bool m_bRepel = false;

protected:
  virtual void CreateRequiredStreams() override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
};
