#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SceneForces.h>
#include <ParticlePlugin/Components/ParticleForceFieldComponent.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_SceneForces, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_SceneForces>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Influence", m_fInfluence)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("InfluenceParam", m_sInfluenceParameter),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_SceneForces, 1, plRTTIDefaultAllocator<plParticleBehavior_SceneForces>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_SceneForces::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_SceneForces>();
}

void plParticleBehaviorFactory_SceneForces::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_SceneForces* pBehavior = static_cast<plParticleBehavior_SceneForces*>(pObject);

  pBehavior->m_fInfluence = m_fInfluence;
  pBehavior->m_sInfluenceParameter = plTempHashedString(m_sInfluenceParameter.GetData());
}

void plParticleBehaviorFactory_SceneForces::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
}

void plParticleBehaviorFactory_SceneForces::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_fInfluence;
  inout_stream << m_sInfluenceParameter;
}

void plParticleBehaviorFactory_SceneForces::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_fInfluence;
  inout_stream >> m_sInfluenceParameter;
}

//////////////////////////////////////////////////////////////////////////

void plParticleBehavior_SceneForces::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
}

void plParticleBehavior_SceneForces::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: SceneForces");

  const float tDiff = (float)m_TimeDiff.GetSeconds();
  if (tDiff <= 0.0f)
    return;

  // the snapshot is rebuilt on the main thread before the update tasks start, so it is
  // safe to read here without locking
  const plArrayPtr<const plParticleForceFieldData> fields = GetOwnerSystem()->GetOwnerWorldModule()->GetForceFieldData();
  if (fields.IsEmpty())
    return;

  const float fInfluence = m_fInfluence * plMath::Max(GetOwnerEffect()->GetFloatParameter(m_sInfluenceParameter, 1.0f), 0.0f);
  if (fInfluence <= 0.0f)
    return;

  // scene fields are placed in world space; for local-space simulations the particle
  // positions are relative to the effect, so the fields are transformed into effect space
  plTransform invEffect = plTransform::MakeIdentity();
  const bool bLocalSpace = GetOwnerEffect()->IsSimulatedInLocalSpace();
  if (bLocalSpace)
  {
    invEffect = GetOwnerEffect()->GetTransform().GetInverse();
  }

  plProcessingStreamIterator<plSimdVec4f> itPosition(m_pStreamPosition, uiNumElements, 0);
  plProcessingStreamIterator<plVec3> itVelocity(m_pStreamVelocity, uiNumElements, 0);

  while (!itPosition.HasReachedEnd())
  {
    const plSimdVec4f simPos = itPosition.Current();
    const plVec3 pos(simPos.GetComponent<0>(), simPos.GetComponent<1>(), simPos.GetComponent<2>());

    plVec3 vAccel = plVec3::MakeZero();

    for (const plParticleForceFieldData& field : fields)
    {
      const plVec3 vCenter = bLocalSpace ? (invEffect * field.m_vCenter) : field.m_vCenter;
      const plVec3 vAxis = bLocalSpace ? (invEffect.m_qRotation * field.m_vAxis) : field.m_vAxis;

      const plVec3 vRel = pos - vCenter;

      // normalized distance to the field border (>= 1 -> outside)
      float fNormDist;
      if (field.m_uiShape == plParticleForceFieldShape::Sphere)
      {
        fNormDist = vRel.GetLength() / field.m_fRadius;
      }
      else
      {
        const plVec3 vLocal = field.m_qInvRotation * (bLocalSpace ? (GetOwnerEffect()->GetTransform().m_qRotation * vRel) : vRel);
        fNormDist = plMath::Max(plMath::Abs(vLocal.x * field.m_vInvBoxHalfExtents.x), plMath::Abs(vLocal.y * field.m_vInvBoxHalfExtents.y), plMath::Abs(vLocal.z * field.m_vInvBoxHalfExtents.z));
      }

      if (fNormDist >= 1.0f)
        continue;

      float fFalloff = 1.0f;
      if (fNormDist > field.m_fFalloffStart && field.m_fFalloffStart < 1.0f)
      {
        fFalloff = 1.0f - (fNormDist - field.m_fFalloffStart) / (1.0f - field.m_fFalloffStart);
      }

      plVec3 vDir;
      switch (field.m_uiType)
      {
        case plParticleForceFieldType::Point:
          vDir = vRel;
          vDir.NormalizeIfNotZero(vAxis).IgnoreResult();
          break;

        case plParticleForceFieldType::Vortex:
          vDir = vAxis.CrossRH(vRel);
          if (vDir.NormalizeIfNotZero(plVec3::MakeZero()).Failed())
            continue;
          break;

        case plParticleForceFieldType::Directional:
        default:
          vDir = vAxis;
          break;
      }

      vAccel += vDir * (field.m_fStrength * fFalloff);
    }

    itVelocity.Current() += vAccel * (fInfluence * tDiff);

    itPosition.Advance();
    itVelocity.Advance();
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_SceneForces);
