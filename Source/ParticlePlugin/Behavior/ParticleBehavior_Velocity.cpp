#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Interfaces/WindWorldModule.h>
#include <Core/World/World.h>
#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Time/Clock.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Velocity.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_Velocity, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_Velocity>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("RiseSpeed", m_fRiseSpeed),
    PL_MEMBER_PROPERTY("Friction", m_fFriction)->AddAttributes(new plClampValueAttribute(0.0f, 100.0f)),
    PL_MEMBER_PROPERTY("WindInfluence", m_fWindInfluence)->AddAttributes(new plClampValueAttribute(0.0f, 1.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_Velocity, 1, plRTTIDefaultAllocator<plParticleBehavior_Velocity>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleBehaviorFactory_Velocity::plParticleBehaviorFactory_Velocity() = default;
plParticleBehaviorFactory_Velocity::~plParticleBehaviorFactory_Velocity() = default;

const plRTTI* plParticleBehaviorFactory_Velocity::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_Velocity>();
}

void plParticleBehaviorFactory_Velocity::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_Velocity* pBehavior = static_cast<plParticleBehavior_Velocity*>(pObject);

  pBehavior->m_fRiseSpeed = m_fRiseSpeed;
  pBehavior->m_fFriction = m_fFriction;
  pBehavior->m_fWindInfluence = m_fWindInfluence;


  pBehavior->m_pPhysicsModule = (plPhysicsWorldModuleInterface*)pBehavior->GetOwnerSystem()->GetOwnerWorldModule()->GetCachedWorldModule(plGetStaticRTTI<plPhysicsWorldModuleInterface>());
}

enum class BehaviorVelocityVersion
{
  Version_0 = 0,
  Version_1,
  Version_2, // added rise speed and acceleration
  Version_3, // added wind influence

  // insert new version numbers above
  Version_Count,
  Version_Current = Version_Count - 1
};

void plParticleBehaviorFactory_Velocity::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = (int)BehaviorVelocityVersion::Version_Current;
  inout_stream << uiVersion;

  inout_stream << m_fRiseSpeed;
  inout_stream << m_fFriction;

  // Version 3
  inout_stream << m_fWindInfluence;
}

void plParticleBehaviorFactory_Velocity::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= (int)BehaviorVelocityVersion::Version_Current, "Invalid version {0}", uiVersion);

  inout_stream >> m_fRiseSpeed;
  inout_stream >> m_fFriction;

  if (uiVersion >= 3)
  {
    inout_stream >> m_fWindInfluence;
  }
}

void plParticleBehaviorFactory_Velocity::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
}

void plParticleBehavior_Velocity::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);

  if (m_fWindInfluence > 0)
  {
    GetOwnerEffect()->RequestWindSamples();
  }
}

void plParticleBehavior_Velocity::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Velocity");

  auto pOwner = GetOwnerEffect();

  const float tDiff = (float)m_TimeDiff.GetSeconds();
  const plVec3 vDown = m_pPhysicsModule != nullptr ? m_pPhysicsModule->GetGravity().GetNormalized() : plVec3(0.0f, 0.0f, -1.0f);
  const plSimdVec4f vRise = plSimdConversion::ToVec3(vDown * tDiff * -m_fRiseSpeed);

  const float fFriction = plMath::Clamp(m_fFriction, 0.0f, 100.0f);
  const float fFrictionFactor = plMath::Pow(0.5f, tDiff * fFriction);

  plProcessingStreamIterator<plSimdVec4f> itPosition(m_pStreamPosition, uiNumElements, 0);
  plProcessingStreamIterator<plVec3> itVelocity(m_pStreamVelocity, uiNumElements, 0);

  if (m_fWindInfluence > 0)
  {
    const plSimdFloat fWindFactor = m_fWindInfluence * tDiff;

    while (!itPosition.HasReachedEnd())
    {
      plSimdVec4f addPos = vRise + pOwner->GetWindAt(itPosition.Current()) * fWindFactor;

      itPosition.Current() += addPos;
      itVelocity.Current() *= fFrictionFactor;

      itPosition.Advance();
      itVelocity.Advance();
    }
  }
  else
  {
    while (!itPosition.HasReachedEnd())
    {
      itPosition.Current() += vRise;
      itVelocity.Current() *= fFrictionFactor;

      itPosition.Advance();
      itVelocity.Advance();
    }
  }
}

void plParticleBehavior_Velocity::RequestRequiredWorldModulesForCache(plParticleWorldModule* pParticleModule)
{
  pParticleModule->CacheWorldModule<plPhysicsWorldModuleInterface>();
  pParticleModule->CacheWorldModule<plWindWorldModuleInterface>();
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_Velocity);
