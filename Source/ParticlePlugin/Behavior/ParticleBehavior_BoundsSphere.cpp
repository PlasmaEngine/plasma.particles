#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_BoundsSphere.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleSphereOutOfBoundsMode, 1)
  PL_ENUM_CONSTANT(plParticleSphereOutOfBoundsMode::Kill),
  PL_ENUM_CONSTANT(plParticleSphereOutOfBoundsMode::Constrain),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_BoundsSphere, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_BoundsSphere>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("CenterOffset", m_vCenterOffset),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(3.0f), new plClampValueAttribute(0.01f, {})),
    PL_ENUM_MEMBER_PROPERTY("OutOfBoundsMode", plParticleSphereOutOfBoundsMode, m_OutOfBoundsMode),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plSphereVisualizerAttribute("Radius", plColor::LightGreen, nullptr, plVisualizerAnchor::Center, plVec3::MakeZero(), "CenterOffset")
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_BoundsSphere, 1, plRTTIDefaultAllocator<plParticleBehavior_BoundsSphere>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleBehaviorFactory_BoundsSphere::plParticleBehaviorFactory_BoundsSphere() = default;

const plRTTI* plParticleBehaviorFactory_BoundsSphere::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_BoundsSphere>();
}

void plParticleBehaviorFactory_BoundsSphere::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_BoundsSphere* pBehavior = static_cast<plParticleBehavior_BoundsSphere*>(pObject);

  pBehavior->m_vCenterOffset = m_vCenterOffset;
  pBehavior->m_fRadius = m_fRadius;
  pBehavior->m_OutOfBoundsMode = m_OutOfBoundsMode;
}

void plParticleBehaviorFactory_BoundsSphere::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_vCenterOffset;
  inout_stream << m_fRadius;
  inout_stream << m_OutOfBoundsMode;
}

void plParticleBehaviorFactory_BoundsSphere::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_vCenterOffset;
  inout_stream >> m_fRadius;
  inout_stream >> m_OutOfBoundsMode;
}

void plParticleBehavior_BoundsSphere::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
}

void plParticleBehavior_BoundsSphere::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: BoundsSphere");

  const plSimdTransform trans = plSimdConversion::ToTransform(GetOwnerSystem()->GetTransform());
  const plSimdVec4f center = trans.TransformPosition(plSimdConversion::ToVec3(m_vCenterOffset));
  const float fRadiusSqr = m_fRadius * m_fRadius;

  plProcessingStreamIterator<plSimdVec4f> itPosition(m_pStreamPosition, uiNumElements, 0);

  if (m_OutOfBoundsMode == plParticleSphereOutOfBoundsMode::Kill)
  {
    plUInt32 idx = 0;

    while (!itPosition.HasReachedEnd())
    {
      const plSimdVec4f pos = itPosition.Current();
      const plSimdVec4f diff = pos - center;
      const float distSqr = diff.Dot<3>(diff);

      if (distSqr > fRadiusSqr)
      {
        m_pStreamGroup->RemoveElement(idx);
      }

      ++idx;
      itPosition.Advance();
    }
  }
  else // Constrain
  {
    while (!itPosition.HasReachedEnd())
    {
      const plSimdVec4f pos = itPosition.Current();
      const plSimdVec4f diff = pos - center;
      const float distSqr = diff.Dot<3>(diff);

      if (distSqr > fRadiusSqr)
      {
        // Push particle back to the sphere surface
        const float dist = plMath::Sqrt(distSqr);
        const plSimdVec4f normalized = diff / plSimdFloat(dist);
        itPosition.Current() = center + normalized * plSimdFloat(m_fRadius);
      }

      itPosition.Advance();
    }
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_BoundsSphere);