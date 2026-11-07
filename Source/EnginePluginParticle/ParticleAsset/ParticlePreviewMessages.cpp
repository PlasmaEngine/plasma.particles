#include <EnginePluginParticle/EnginePluginParticlePCH.h>

#include <EnginePluginParticle/ParticleAsset/ParticlePreviewMessages.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticlePreviewMotion, 1)
  PL_ENUM_CONSTANT(plParticlePreviewMotion::Static),
  PL_ENUM_CONSTANT(plParticlePreviewMotion::Orbit),
  PL_ENUM_CONSTANT(plParticlePreviewMotion::Line),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticlePreviewSettingsMsgToEngine, 1, plRTTIDefaultAllocator<plParticlePreviewSettingsMsgToEngine>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("ShowStats", m_bShowStats),
    PL_MEMBER_PROPERTY("ShowEvents", m_bShowEvents),
    PL_ENUM_MEMBER_PROPERTY("Motion", plParticlePreviewMotion, m_Motion),
    PL_ARRAY_MEMBER_PROPERTY("SystemNames", m_SystemNames),
    PL_ARRAY_MEMBER_PROPERTY("BlackboardEntries", m_BlackboardEntries),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleSetPreviewTimeMsgToEngine, 1, plRTTIDefaultAllocator<plParticleSetPreviewTimeMsgToEngine>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Time", m_fTime),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleSpyMsgToEngine, 1, plRTTIDefaultAllocator<plParticleSpyMsgToEngine>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Node", m_Node),
    PL_MEMBER_PROPERTY("System", m_uiSystem),
    PL_MEMBER_PROPERTY("PerFrame", m_bPerFrame),
    PL_MEMBER_PROPERTY("Program", m_Program),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticlePreviewStatusMsgToEditor, 1, plRTTIDefaultAllocator<plParticlePreviewStatusMsgToEditor>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("EffectTime", m_fEffectTime),
    PL_MEMBER_PROPERTY("EffectAlive", m_bEffectAlive),
    PL_MEMBER_PROPERTY("SpyNode", m_SpyNode),
    PL_MEMBER_PROPERTY("SpyCount", m_uiSpyCount),
    PL_MEMBER_PROPERTY("SpyMin", m_fSpyMin),
    PL_MEMBER_PROPERTY("SpyAvg", m_fSpyAvg),
    PL_MEMBER_PROPERTY("SpyMax", m_fSpyMax),
    PL_ARRAY_MEMBER_PROPERTY("SpyBins", m_SpyBins),
    PL_MEMBER_PROPERTY("SpyOnGPU", m_bSpyOnGPU),
    PL_ARRAY_MEMBER_PROPERTY("MissingBlackboardEntries", m_MissingBlackboardEntries),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on
