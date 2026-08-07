#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Types/ScopeExit.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>
#include <ParticlePlugin/Events/ParticleEventReaction.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEffectDescriptor, 2, plRTTIDefaultAllocator<plParticleEffectDescriptor>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("WhenInvisible", plEffectInvisibleUpdateRate, m_InvisibleUpdateRate),
    PL_MEMBER_PROPERTY("AlwaysShared", m_bAlwaysShared),
    PL_MEMBER_PROPERTY("SimulateInLocalSpace", m_bSimulateInLocalSpace),
    PL_MEMBER_PROPERTY("ApplyOwnerVelocity", m_fApplyInstanceVelocity)->AddAttributes(new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("PreSimulateDuration", m_PreSimulateDuration),
    PL_MEMBER_PROPERTY("NumWindSamples", m_vNumWindSamples)->AddAttributes(new plDefaultValueAttribute(plVec3U32(1)), new plClampValueAttribute(plVec3U32(1), plVec3U32(8))),
    PL_MEMBER_PROPERTY("FadeOutStartDistance", m_fFadeOutStartDistance)->AddAttributes(new plClampValueAttribute(0.0f, {})),
    PL_MEMBER_PROPERTY("FadeOutEndDistance", m_fFadeOutEndDistance)->AddAttributes(new plClampValueAttribute(0.0f, {})),
    PL_ENUM_MEMBER_PROPERTY("Importance", plParticleEffectImportance, m_Importance),
    PL_MEMBER_PROPERTY("FixedTickHz", m_fFixedTickHz)->AddAttributes(new plClampValueAttribute(0.0f, 120.0f)),
    PL_MEMBER_PROPERTY("MaxTicksPerFrame", m_uiMaxTicksPerFrame)->AddAttributes(new plDefaultValueAttribute(4), new plClampValueAttribute(1, 16)),
    PL_MAP_MEMBER_PROPERTY("FloatParameters", m_FloatParameters),
    PL_MAP_MEMBER_PROPERTY("ColorParameters", m_ColorParameters)->AddAttributes(new plExposeColorAlphaAttribute),
    PL_SET_ACCESSOR_PROPERTY("ParticleSystems", GetParticleSystems, AddParticleSystem, RemoveParticleSystem)->AddFlags(plPropertyFlags::PointerOwner),
    PL_SET_ACCESSOR_PROPERTY("EventReactions", GetEventReactions, AddEventReaction, RemoveEventReaction)->AddFlags(plPropertyFlags::PointerOwner),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleEffectDescriptor::plParticleEffectDescriptor() = default;

plParticleEffectDescriptor::~plParticleEffectDescriptor()
{
  ClearSystems();
  ClearEventReactions();
}

void plParticleEffectDescriptor::ClearSystems()
{
  for (auto pSystem : m_ParticleSystems)
  {
    pSystem->GetDynamicRTTI()->GetAllocator()->Deallocate(pSystem);
  }

  m_ParticleSystems.Clear();
}


void plParticleEffectDescriptor::ClearEventReactions()
{
  for (auto pReaction : m_EventReactions)
  {
    pReaction->GetDynamicRTTI()->GetAllocator()->Deallocate(pReaction);
  }

  m_EventReactions.Clear();
}

enum class ParticleEffectVersion
{
  Version_0 = 0,
  Version_1,
  Version_2,
  Version_3,
  Version_4,
  Version_5,  // m_bAlwaysShared
  Version_6,  // added parameters
  Version_7,  // added instance velocity
  Version_8,  // added event reactions
  Version_9,  // breaking change
  Version_10, // added wind samples
  Version_11, // added distance fade
  Version_12, // added importance
  Version_13, // added fixed tick

  // insert new version numbers above
  Version_Count,
  Version_Current = Version_Count - 1
};

void plParticleEffectDescriptor::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = (int)ParticleEffectVersion::Version_Current;

  inout_stream << uiVersion;

  const plUInt32 uiNumSystems = m_ParticleSystems.GetCount();

  inout_stream << uiNumSystems;

  // Version 3
  inout_stream << m_bSimulateInLocalSpace;
  inout_stream << m_PreSimulateDuration;
  // Version 4
  inout_stream << m_InvisibleUpdateRate;
  // Version 5
  inout_stream << m_bAlwaysShared;

  // Version 3
  for (auto pSystem : m_ParticleSystems)
  {
    inout_stream << pSystem->GetDynamicRTTI()->GetTypeName();

    pSystem->Save(inout_stream);
  }

  // Version 6
  {
    plUInt8 paramCol = static_cast<plUInt8>(m_ColorParameters.GetCount());
    inout_stream << paramCol;
    for (auto it = m_ColorParameters.GetIterator(); it.IsValid(); ++it)
    {
      inout_stream << it.Key();
      inout_stream << it.Value();
    }

    plUInt8 paramFloat = static_cast<plUInt8>(m_FloatParameters.GetCount());
    inout_stream << paramFloat;
    for (auto it = m_FloatParameters.GetIterator(); it.IsValid(); ++it)
    {
      inout_stream << it.Key();
      inout_stream << it.Value();
    }
  }

  // Version 7
  inout_stream << m_fApplyInstanceVelocity;

  // Version 8
  {
    const plUInt32 uiNumReactions = m_EventReactions.GetCount();
    inout_stream << uiNumReactions;

    for (auto pReaction : m_EventReactions)
    {
      inout_stream << pReaction->GetDynamicRTTI()->GetTypeName();

      pReaction->Save(inout_stream);
    }
  }

  // Version 10
  inout_stream << m_vNumWindSamples;

  // Version 11
  inout_stream << m_fFadeOutStartDistance;
  inout_stream << m_fFadeOutEndDistance;

  // Version 12
  inout_stream << m_Importance;

  // Version 13
  inout_stream << m_fFixedTickHz;
  inout_stream << m_uiMaxTicksPerFrame;
}


void plParticleEffectDescriptor::Load(plStreamReader& inout_stream)
{
  ClearSystems();
  ClearEventReactions();

  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  PL_ASSERT_DEV(uiVersion <= (int)ParticleEffectVersion::Version_Current, "Unknown particle effect template version {0}", uiVersion);

  if (uiVersion < (int)ParticleEffectVersion::Version_9)
  {
    plLog::SeriousWarning("Unsupported old particle effect version");
    return;
  }

  plUInt32 uiNumSystems = 0;
  inout_stream >> uiNumSystems;

  inout_stream >> m_bSimulateInLocalSpace;
  inout_stream >> m_PreSimulateDuration;
  inout_stream >> m_InvisibleUpdateRate;
  inout_stream >> m_bAlwaysShared;

  m_ParticleSystems.SetCountUninitialized(uiNumSystems);

  plStringBuilder sType;

  for (auto& pSystem : m_ParticleSystems)
  {
    inout_stream >> sType;

    const plRTTI* pRtti = plRTTI::FindTypeByName(sType);
    PL_ASSERT_DEBUG(pRtti != nullptr, "Unknown particle effect type '{0}'", sType);

    pSystem = pRtti->GetAllocator()->Allocate<plParticleSystemDescriptor>();

    pSystem->Load(inout_stream);
  }

  plStringBuilder key;
  m_ColorParameters.Clear();
  m_FloatParameters.Clear();

  plUInt8 paramCol;
  inout_stream >> paramCol;
  for (plUInt32 i = 0; i < paramCol; ++i)
  {
    plColor val;
    inout_stream >> key;
    inout_stream >> val;
    m_ColorParameters[key] = val;
  }

  plUInt8 paramFloat;
  inout_stream >> paramFloat;
  for (plUInt32 i = 0; i < paramFloat; ++i)
  {
    float val;
    inout_stream >> key;
    inout_stream >> val;
    m_FloatParameters[key] = val;
  }

  inout_stream >> m_fApplyInstanceVelocity;

  plUInt32 uiNumReactions = 0;
  inout_stream >> uiNumReactions;

  m_EventReactions.SetCountUninitialized(uiNumReactions);

  for (auto& pReaction : m_EventReactions)
  {
    inout_stream >> sType;

    const plRTTI* pRtti = plRTTI::FindTypeByName(sType);
    PL_ASSERT_DEBUG(pRtti != nullptr, "Unknown particle effect event reaction type '{0}'", sType);

    pReaction = pRtti->GetAllocator()->Allocate<plParticleEventReactionFactory>();

    pReaction->Load(inout_stream);
  }

  if (uiVersion >= (int)ParticleEffectVersion::Version_10)
  {
    inout_stream >> m_vNumWindSamples;
  }

  if (uiVersion >= (int)ParticleEffectVersion::Version_11)
  {
    inout_stream >> m_fFadeOutStartDistance;
    inout_stream >> m_fFadeOutEndDistance;
  }

  if (uiVersion >= (int)ParticleEffectVersion::Version_12)
  {
    inout_stream >> m_Importance;
  }

  if (uiVersion >= (int)ParticleEffectVersion::Version_13)
  {
    inout_stream >> m_fFixedTickHz;
    inout_stream >> m_uiMaxTicksPerFrame;
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Effect_ParticleEffectDescriptor);
