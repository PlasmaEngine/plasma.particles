#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_KillIf.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_KillIf, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_KillIf>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Condition", m_bCondition),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plTitleAttribute("Kill If"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_KillIf, 1, plRTTIDefaultAllocator<plParticleBehavior_KillIf>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_KillIf::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_KillIf>();
}

void plParticleBehaviorFactory_KillIf::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_KillIf* pBehavior = static_cast<plParticleBehavior_KillIf*>(pObject);
  pBehavior->m_bCondition = m_bCondition;

  // the runner is only configured with the streams; a per-frame change of Condition must keep it
  pBehavior->m_pProgram = &m_Program;
}

void plParticleBehaviorFactory_KillIf::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;
  inout_stream << m_bCondition;
  m_Program.Save(inout_stream);
}

void plParticleBehaviorFactory_KillIf::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);
  inout_stream >> m_bCondition;
  m_Program.Load(inout_stream);
}

//////////////////////////////////////////////////////////////////////////

void plParticleBehavior_KillIf::CreateRequiredStreams()
{
  m_Runner.Configure(m_pProgram);
  m_Runner.CreateRequiredStreams([this](const char* szName, plProcessingStream::DataType type, plProcessingStream** ppStream)
    { CreateStream(szName, type, ppStream, false); });
}

void plParticleBehavior_KillIf::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Kill If");

  if (uiNumElements == 0)
    return;

  plUInt32 uiComponents = 0;
  const float* pCondition = nullptr;
  if (m_Runner.IsActive() && m_Runner.Run(GetOwnerSystem(), 0, uiNumElements, m_TimeDiff))
    pCondition = m_Runner.GetOutput("Condition", uiComponents);

  if (pCondition == nullptr)
  {
    if (!m_bCondition)
      return;

    for (plUInt64 i = 0; i < uiNumElements; ++i)
      m_pStreamGroup->RemoveElement(i);

    return;
  }

  for (plUInt64 i = 0; i < uiNumElements; ++i)
  {
    if (pCondition[i * uiComponents] != 0.0f)
      m_pStreamGroup->RemoveElement(i);
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_KillIf);
