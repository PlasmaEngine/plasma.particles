#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleGraphPinCategory, 1)
  PL_ENUM_CONSTANT(plParticleGraphPinCategory::Initializer),
  PL_ENUM_CONSTANT(plParticleGraphPinCategory::Behavior),
  PL_ENUM_CONSTANT(plParticleGraphPinCategory::Renderer),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleGraphPin, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEffectNode, 1, plRTTIDefaultAllocator<plParticleEffectNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("WhenInvisible", plEffectInvisibleUpdateRate, m_InvisibleUpdateRate),
    PL_MEMBER_PROPERTY("AlwaysShared", m_bAlwaysShared),
    PL_MEMBER_PROPERTY("SimulateInLocalSpace", m_bSimulateInLocalSpace),
    PL_MEMBER_PROPERTY("ApplyOwnerVelocity", m_fApplyInstanceVelocity)->AddAttributes(new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("PreSimulateDuration", m_PreSimulateDuration),
    PL_MEMBER_PROPERTY("NumWindSamples", m_vNumWindSamples)->AddAttributes(new plDefaultValueAttribute(plVec3U32(1)), new plClampValueAttribute(plVec3U32(1), plVec3U32(8))),
    PL_MAP_MEMBER_PROPERTY("FloatParameters", m_FloatParameters),
    PL_MAP_MEMBER_PROPERTY("ColorParameters", m_ColorParameters)->AddAttributes(new plExposeColorAlphaAttribute),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effect", plColorScheme::DarkUI(plColorScheme::Blue)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleSystemNode, 1, plRTTIDefaultAllocator<plParticleSystemNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Visible", m_bVisible)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("LifeTime", m_LifeTime)->AddAttributes(new plDefaultValueAttribute(plTime::MakeFromSeconds(2)), new plClampValueAttribute(plTime::MakeFromSeconds(0.0), plVariant())),
    PL_MEMBER_PROPERTY("LifeScaleParam", m_sLifeScaleParameter),
    PL_MEMBER_PROPERTY("OnDeathEvent", m_sOnDeathEvent)->AddAttributes(new plDynamicStringEnumAttribute("ParticleEventNamesEnum")),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("System", plColorScheme::DarkUI(plColorScheme::Blue)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleMixNode, 1, plRTTIDefaultAllocator<plParticleMixNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Category", plParticleGraphPinCategory, m_Category)->AddAttributes(new plDefaultValueAttribute((plInt32)plParticleGraphPinCategory::Behavior)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Mix", plColorScheme::DarkUI(plColorScheme::Teal)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on