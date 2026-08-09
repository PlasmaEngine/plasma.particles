#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Utils/Blackboard.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SetBlackboardEntry.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_SetBlackboardEntry, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_SetBlackboardEntry>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Blackboard", m_sBlackboard),
    PL_MEMBER_PROPERTY("Entry", m_sEntry),
    PL_MEMBER_PROPERTY("Value", m_fValue),
    PL_MEMBER_PROPERTY("Continuous", m_bContinuous),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plTitleAttribute("{Entry} = {Value}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_SetBlackboardEntry, 1, plRTTIDefaultAllocator<plParticleBehavior_SetBlackboardEntry>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_SetBlackboardEntry::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_SetBlackboardEntry>();
}

void plParticleBehaviorFactory_SetBlackboardEntry::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_SetBlackboardEntry* pBehavior = static_cast<plParticleBehavior_SetBlackboardEntry*>(pObject);

  pBehavior->m_sBlackboard = m_sBlackboard;
  pBehavior->m_sEntry = m_sEntry;
  pBehavior->m_fValue = m_fValue;
  pBehavior->m_bContinuous = m_bContinuous;
}

void plParticleBehaviorFactory_SetBlackboardEntry::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_sBlackboard;
  inout_stream << m_sEntry;
  inout_stream << m_fValue;
  inout_stream << m_bContinuous;
}

void plParticleBehaviorFactory_SetBlackboardEntry::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_sBlackboard;
  inout_stream >> m_sEntry;
  inout_stream >> m_fValue;
  inout_stream >> m_bContinuous;
}

//////////////////////////////////////////////////////////////////////////

void plParticleBehavior_SetBlackboardEntry::Process(plUInt64 uiNumElements)
{
  PL_IGNORE_UNUSED(uiNumElements);
  PL_PROFILE_SCOPE("PFX: Set Blackboard Entry");

  if (!m_bContinuous && m_bWritten)
    return;

  if (m_sEntry.IsEmpty())
    return;

  // Particle systems update on task threads and plBlackboard has no internal locking, so writes
  // from concurrent effects are serialized here. This does NOT synchronize against game code
  // touching the same blackboard from other threads mid-frame — same caveat as any cross-thread
  // blackboard use in the engine.
  static plMutex s_WriteMutex;
  PL_LOCK(s_WriteMutex);

  plHashedString sBlackboardName;
  sBlackboardName.Assign(m_sBlackboard.IsEmpty() ? "Global" : m_sBlackboard.GetData());

  if (plSharedPtr<plBlackboard> pBlackboard = plBlackboard::GetOrCreateGlobal(sBlackboardName))
  {
    pBlackboard->SetEntryValue(m_sEntry, m_fValue);
    m_bWritten = true;
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_SetBlackboardEntry);
