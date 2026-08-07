#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/SimdMath/SimdConversion.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_AttractToPosition.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleAttractorTarget, 1)
  PL_ENUM_CONSTANT(plParticleAttractorTarget::EffectOrigin),
  PL_ENUM_CONSTANT(plParticleAttractorTarget::CustomPosition),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleAttractorShape, 1)
  PL_ENUM_CONSTANT(plParticleAttractorShape::Point),
  PL_ENUM_CONSTANT(plParticleAttractorShape::Line),
  PL_ENUM_CONSTANT(plParticleAttractorShape::Vortex),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_AttractToPosition, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_AttractToPosition>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Target", plParticleAttractorTarget, m_Target),
    PL_ENUM_MEMBER_PROPERTY("Shape", plParticleAttractorShape, m_Shape),
    PL_MEMBER_PROPERTY("CustomPosition", m_vCustomPosition),
    PL_MEMBER_PROPERTY("Axis", m_vAxis)->AddAttributes(new plDefaultValueAttribute(plVec3(0, 0, 1))),
    PL_MEMBER_PROPERTY("Force", m_fForce)->AddAttributes(new plDefaultValueAttribute(5.0f)),
    PL_MEMBER_PROPERTY("MaxDistance", m_fMaxDistance)->AddAttributes(new plDefaultValueAttribute(10.0f), new plClampValueAttribute(0.0f, {})),
    PL_MEMBER_PROPERTY("MinDistance", m_fMinDistance)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.001f, {})),
    PL_MEMBER_PROPERTY("AffectVelocity", m_bAffectVelocity)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("Repel", m_bRepel),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_AttractToPosition, 1, plRTTIDefaultAllocator<plParticleBehavior_AttractToPosition>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleBehaviorFactory_AttractToPosition::plParticleBehaviorFactory_AttractToPosition() = default;

const plRTTI* plParticleBehaviorFactory_AttractToPosition::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_AttractToPosition>();
}

void plParticleBehaviorFactory_AttractToPosition::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_AttractToPosition* pBehavior = static_cast<plParticleBehavior_AttractToPosition*>(pObject);

  pBehavior->m_Target = m_Target;
  pBehavior->m_Shape = m_Shape;
  pBehavior->m_vCustomPosition = m_vCustomPosition;
  pBehavior->m_vAxis = m_vAxis.GetLength() > 0.001f ? m_vAxis.GetNormalized() : plVec3(0, 0, 1);
  pBehavior->m_fForce = m_fForce;
  pBehavior->m_fMaxDistance = m_fMaxDistance;
  pBehavior->m_fMinDistance = m_fMinDistance;
  pBehavior->m_bAffectVelocity = m_bAffectVelocity;
  pBehavior->m_bRepel = m_bRepel;
}

void plParticleBehaviorFactory_AttractToPosition::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  if (m_bAffectVelocity)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
  }
}

void plParticleBehaviorFactory_AttractToPosition::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 2;
  inout_stream << uiVersion;

  inout_stream << m_Target;
  inout_stream << m_vCustomPosition;
  inout_stream << m_fForce;
  inout_stream << m_fMaxDistance;
  inout_stream << m_fMinDistance;
  inout_stream << m_bAffectVelocity;

  // version 2
  inout_stream << m_Shape;
  inout_stream << m_vAxis;
  inout_stream << m_bRepel;
}

void plParticleBehaviorFactory_AttractToPosition::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 2, "Invalid version {0}", uiVersion);

  inout_stream >> m_Target;
  inout_stream >> m_vCustomPosition;
  inout_stream >> m_fForce;
  inout_stream >> m_fMaxDistance;
  inout_stream >> m_fMinDistance;
  inout_stream >> m_bAffectVelocity;

  if (uiVersion >= 2)
  {
    inout_stream >> m_Shape;
    inout_stream >> m_vAxis;
    inout_stream >> m_bRepel;
  }
}

void plParticleBehavior_AttractToPosition::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
}

void plParticleBehavior_AttractToPosition::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: AttractToPosition");

  const float tDiff = (float)m_TimeDiff.GetSeconds();
  if (tDiff <= 0.0f)
    return;

  // Determine attractor world position and axis
  plVec3 vTarget;
  plVec3 vAxis = m_vAxis;

  if (m_Target == plParticleAttractorTarget::EffectOrigin)
  {
    vTarget = GetOwnerEffect()->GetTransform().m_vPosition;
    vAxis = GetOwnerEffect()->GetTransform().m_qRotation * m_vAxis;
  }
  else
  {
    vTarget = m_vCustomPosition;
  }

  const float fMaxDistSqr = m_fMaxDistance * m_fMaxDistance;
  const float fMinDistSqr = m_fMinDistance * m_fMinDistance;
  const float fForce = m_bRepel ? -m_fForce : m_fForce;

  // vector from the particle to the closest point of the attractor geometry
  auto GetToTarget = [&](const plVec3& pos) -> plVec3
  {
    if (m_Shape == plParticleAttractorShape::Point)
      return vTarget - pos;

    // Line / Vortex: closest point on the line through vTarget along vAxis
    const plVec3 toPos = pos - vTarget;
    return (vTarget + vAxis * toPos.Dot(vAxis)) - pos;
  };

  plProcessingStreamIterator<plSimdVec4f> itPosition(m_pStreamPosition, uiNumElements, 0);
  plProcessingStreamIterator<plVec3> itVelocity(m_pStreamVelocity, uiNumElements, 0);

  while (!itPosition.HasReachedEnd())
  {
    const plSimdVec4f simPos = itPosition.Current();
    const plVec3 pos(simPos.GetComponent<0>(), simPos.GetComponent<1>(), simPos.GetComponent<2>());

    const plVec3 toTarget = GetToTarget(pos);
    const float distSqr = toTarget.GetLengthSquared();

    if (distSqr < fMaxDistSqr && distSqr > fMinDistSqr)
    {
      const float dist = plMath::Sqrt(distSqr);
      const plVec3 dirToTarget = toTarget / dist;

      // Linear falloff: full force at minDist, zero at maxDist
      const float falloff = 1.0f - (dist - m_fMinDistance) / (m_fMaxDistance - m_fMinDistance);
      const float amount = fForce * falloff * tDiff;

      plVec3 vDir = dirToTarget;

      if (m_Shape == plParticleAttractorShape::Vortex)
      {
        // swirl around the axis; repel spins the other way
        vDir = vAxis.CrossRH(-dirToTarget);
        vDir.NormalizeIfNotZero(dirToTarget).IgnoreResult();
      }

      if (m_bAffectVelocity)
      {
        itVelocity.Current() += vDir * amount;
      }
      else if (m_Shape == plParticleAttractorShape::Vortex)
      {
        itPosition.Current() = plSimdConversion::ToVec3(pos + vDir * amount);
      }
      else
      {
        // don't overshoot past the min distance when snapping positions
        itPosition.Current() = plSimdConversion::ToVec3(pos + vDir * plMath::Min(amount, dist - m_fMinDistance));
      }
    }

    itPosition.Advance();
    itVelocity.Advance();
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_AttractToPosition);
