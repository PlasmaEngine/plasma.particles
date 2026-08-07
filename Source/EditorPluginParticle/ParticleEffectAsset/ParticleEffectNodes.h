#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Types/VarianceTypes.h>
#include <ParticlePlugin/Declarations.h>
#include <ToolsFoundation/VisualGraph/VisualGraphObjectManager.h>

struct plParticleGraphPinCategory
{
  using StorageType = plUInt8;

  enum Enum
  {
    System,
    Emitter,
    Initializer,
    Behavior,
    Renderer,
    EventReaction,

    Default = System
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleGraphPinCategory);

/// Pin type for the particle effect visual graph.
///
/// Carries a category that determines which pin types can connect to each other.
class plParticleGraphPin : public plVisualGraphPin
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleGraphPin, plVisualGraphPin);

public:
  using plVisualGraphPin::plVisualGraphPin;

  plEnum<plParticleGraphPinCategory> m_Category;
};

/// Root node of a particle effect graph. One per document.
///
/// Holds effect-level settings. Systems and event reactions connect to this node's input pins.
class plParticleEffectNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEffectNode, plReflectedClass);

public:
  plEnum<plEffectInvisibleUpdateRate> m_InvisibleUpdateRate;
  bool m_bAlwaysShared = false;
  bool m_bSimulateInLocalSpace = false;
  float m_fApplyInstanceVelocity = 0.0f;
  plTime m_PreSimulateDuration;
  plVec3U32 m_vNumWindSamples = plVec3U32(1);
  float m_fFadeOutStartDistance = 0.0f;
  float m_fFadeOutEndDistance = 0.0f;
  plEnum<plParticleEffectImportance> m_Importance;
  float m_fFixedTickHz = 0.0f;
  plUInt8 m_uiMaxTicksPerFrame = 4;
  plMap<plString, float> m_FloatParameters;
  plMap<plString, plColor> m_ColorParameters;
};

/// Represents a particle system/layer within the effect graph.
///
/// Each system node has input pins for its emitter, initializers, behaviors, and renderers,
/// and an output pin that connects to the effect node.
class plParticleSystemNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSystemNode, plReflectedClass);

public:
  plString m_sName;
  bool m_bVisible = true;
  plEnum<plParticleSimulationTarget> m_SimulationTarget;
  plVarianceTypeTime m_LifeTime;
  plString m_sOnDeathEvent;
  plString m_sLifeScaleParameter;
};

/// Passthrough grouping node that merges multiple inputs of the same category into one output.
///
/// The category property determines which pin types (Initializer, Behavior, or Renderer) the
/// mix node operates on. At export time the node is expanded: all leaf factory nodes feeding
/// into it are collected and added to the system descriptor individually.
class plParticleMixNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleMixNode, plReflectedClass);

public:
  plEnum<plParticleGraphPinCategory> m_Category = plParticleGraphPinCategory::Behavior;
};