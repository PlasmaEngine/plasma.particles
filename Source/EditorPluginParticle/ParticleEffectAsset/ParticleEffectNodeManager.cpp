#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Emitter/ParticleEmitter.h>
#include <ParticlePlugin/Events/ParticleEventReaction.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>
#include <ParticlePlugin/Type/ParticleType.h>

static plParticleGraphPinCategory::Enum GetPinCategory(const plRTTI* pType)
{
  if (pType->IsDerivedFrom<plParticleEffectNode>())
    return plParticleGraphPinCategory::System;
  if (pType->IsDerivedFrom<plParticleSystemNode>())
    return plParticleGraphPinCategory::System;
  if (pType->IsDerivedFrom<plParticleMixNode>())
    return plParticleGraphPinCategory::Behavior; // actual category read from instance at pin creation
  if (pType->IsDerivedFrom<plParticleEmitterFactory>())
    return plParticleGraphPinCategory::Emitter;
  if (pType->IsDerivedFrom<plParticleInitializerFactory>())
    return plParticleGraphPinCategory::Initializer;
  if (pType->IsDerivedFrom<plParticleBehaviorFactory>())
    return plParticleGraphPinCategory::Behavior;
  if (pType->IsDerivedFrom<plParticleTypeFactory>())
    return plParticleGraphPinCategory::Renderer;
  if (pType->IsDerivedFrom<plParticleEventReactionFactory>())
    return plParticleGraphPinCategory::EventReaction;

  return plParticleGraphPinCategory::System;
}

static plColor GetPinColorForCategory(plParticleGraphPinCategory::Enum cat)
{
  switch (cat)
  {
    case plParticleGraphPinCategory::Initializer:
      return plColorScheme::DarkUI(plColorScheme::Green);
    case plParticleGraphPinCategory::Behavior:
      return plColorScheme::DarkUI(plColorScheme::Orange);
    case plParticleGraphPinCategory::Renderer:
      return plColorScheme::DarkUI(plColorScheme::Grape);
    default:
      return plColorScheme::DarkUI(plColorScheme::Teal);
  }
}

bool plParticleEffectNodeManager::InternalIsNode(const plDocumentObject* pObject) const
{
  auto pType = pObject->GetTypeAccessor().GetType();

  return pType->IsDerivedFrom<plParticleEffectNode>() ||
         pType->IsDerivedFrom<plParticleSystemNode>() ||
         pType->IsDerivedFrom<plParticleMixNode>() ||
         pType->IsDerivedFrom<plParticleEmitterFactory>() ||
         pType->IsDerivedFrom<plParticleInitializerFactory>() ||
         pType->IsDerivedFrom<plParticleBehaviorFactory>() ||
         pType->IsDerivedFrom<plParticleTypeFactory>() ||
         pType->IsDerivedFrom<plParticleEventReactionFactory>();
}

void plParticleEffectNodeManager::InternalCreatePins(const plDocumentObject* pObject, NodeInternal& ref_node)
{
  auto pType = pObject->GetTypeAccessor().GetType();

  if (pType->IsDerivedFrom<plParticleEffectNode>())
  {
    // Effect node has input pins for systems and event reactions
    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, "Systems", plColorScheme::DarkUI(plColorScheme::Blue), pObject);
      pPin->m_Category = plParticleGraphPinCategory::System;
      ref_node.m_Inputs.PushBack(pPin);
    }
    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, "EventReactions", plColorScheme::DarkUI(plColorScheme::Red), pObject);
      pPin->m_Category = plParticleGraphPinCategory::EventReaction;
      ref_node.m_Inputs.PushBack(pPin);
    }
  }
  else if (pType->IsDerivedFrom<plParticleSystemNode>())
  {
    // System node: output connects to effect, inputs for each module category
    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Output, "System", plColorScheme::DarkUI(plColorScheme::Blue), pObject);
      pPin->m_Category = plParticleGraphPinCategory::System;
      ref_node.m_Outputs.PushBack(pPin);
    }
    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, "Emitter", plColorScheme::DarkUI(plColorScheme::Yellow), pObject);
      pPin->m_Category = plParticleGraphPinCategory::Emitter;
      ref_node.m_Inputs.PushBack(pPin);
    }
    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, "Initializers", plColorScheme::DarkUI(plColorScheme::Green), pObject);
      pPin->m_Category = plParticleGraphPinCategory::Initializer;
      ref_node.m_Inputs.PushBack(pPin);
    }
    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, "Behaviors", plColorScheme::DarkUI(plColorScheme::Orange), pObject);
      pPin->m_Category = plParticleGraphPinCategory::Behavior;
      ref_node.m_Inputs.PushBack(pPin);
    }
    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, "Renderers", plColorScheme::DarkUI(plColorScheme::Grape), pObject);
      pPin->m_Category = plParticleGraphPinCategory::Renderer;
      ref_node.m_Inputs.PushBack(pPin);
    }
  }
  else if (pType->IsDerivedFrom<plParticleMixNode>())
  {
    plParticleGraphPinCategory::Enum cat = (plParticleGraphPinCategory::Enum)pObject->GetTypeAccessor().GetValue("Category").ConvertTo<plInt64>();
    plColor pinColor = GetPinColorForCategory(cat);

    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Output, "Output", pinColor, pObject);
      pPin->m_Category = cat;
      ref_node.m_Outputs.PushBack(pPin);
    }

    const char* inputNames[] = {"Input 1", "Input 2", "Input 3", "Input 4"};
    for (int i = 0; i < 4; ++i)
    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, inputNames[i], pinColor, pObject);
      pPin->m_Category = cat;
      ref_node.m_Inputs.PushBack(pPin);
    }
  }
  else if (pType->IsDerivedFrom<plParticleEmitterFactory>())
  {
    auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Output, "Emitter", plColorScheme::DarkUI(plColorScheme::Yellow), pObject);
    pPin->m_Category = plParticleGraphPinCategory::Emitter;
    ref_node.m_Outputs.PushBack(pPin);
  }
  else if (pType->IsDerivedFrom<plParticleInitializerFactory>())
  {
    auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Output, "Initializer", plColorScheme::DarkUI(plColorScheme::Green), pObject);
    pPin->m_Category = plParticleGraphPinCategory::Initializer;
    ref_node.m_Outputs.PushBack(pPin);
  }
  else if (pType->IsDerivedFrom<plParticleBehaviorFactory>())
  {
    auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Output, "Behavior", plColorScheme::DarkUI(plColorScheme::Orange), pObject);
    pPin->m_Category = plParticleGraphPinCategory::Behavior;
    ref_node.m_Outputs.PushBack(pPin);
  }
  else if (pType->IsDerivedFrom<plParticleTypeFactory>())
  {
    auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Output, "Renderer", plColorScheme::DarkUI(plColorScheme::Grape), pObject);
    pPin->m_Category = plParticleGraphPinCategory::Renderer;
    ref_node.m_Outputs.PushBack(pPin);
  }
  else if (pType->IsDerivedFrom<plParticleEventReactionFactory>())
  {
    auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Output, "EventReaction", plColorScheme::DarkUI(plColorScheme::Red), pObject);
    pPin->m_Category = plParticleGraphPinCategory::EventReaction;
    ref_node.m_Outputs.PushBack(pPin);
  }
}

void plParticleEffectNodeManager::GetCreateableTypes(plHybridArray<const plRTTI*, 32>& ref_types) const
{
  ref_types.Clear();

  ref_types.PushBack(plGetStaticRTTI<plParticleSystemNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleMixNode>());

  // Gather all concrete factory types
  plSet<const plRTTI*> typeSet;

  plReflectionUtils::GatherTypesDerivedFromClass(plGetStaticRTTI<plParticleEmitterFactory>(), typeSet);
  plReflectionUtils::GatherTypesDerivedFromClass(plGetStaticRTTI<plParticleInitializerFactory>(), typeSet);
  plReflectionUtils::GatherTypesDerivedFromClass(plGetStaticRTTI<plParticleBehaviorFactory>(), typeSet);
  plReflectionUtils::GatherTypesDerivedFromClass(plGetStaticRTTI<plParticleTypeFactory>(), typeSet);
  plReflectionUtils::GatherTypesDerivedFromClass(plGetStaticRTTI<plParticleEventReactionFactory>(), typeSet);

  for (auto pType : typeSet)
  {
    if (pType->GetTypeFlags().IsAnySet(plTypeFlags::Abstract))
      continue;

    ref_types.PushBack(pType);
  }

  // Note: plParticleEffectNode is NOT in this list — it is auto-created by the document.
}

plStatus plParticleEffectNodeManager::InternalCanConnect(const plVisualGraphPin& source, const plVisualGraphPin& target, CanConnectResult& out_result) const
{
  const auto& srcPin = static_cast<const plParticleGraphPin&>(source);
  const auto& tgtPin = static_cast<const plParticleGraphPin&>(target);

  // Only allow connections between matching categories
  if (srcPin.m_Category != tgtPin.m_Category)
  {
    return plStatus("Pin categories do not match.");
  }

  // Emitter is 1:1 (only one emitter per system)
  if (srcPin.m_Category == plParticleGraphPinCategory::Emitter)
  {
    out_result = CanConnectResult::Connect1to1;
  }
  else
  {
    out_result = CanConnectResult::ConnectNtoN;
  }

  return plStatus(PL_SUCCESS);
}

bool plParticleEffectNodeManager::InternalIsDynamicPinProperty(const plDocumentObject* pObject, const plAbstractProperty* pProp) const
{
  if (pObject->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleMixNode>())
  {
    return plStringUtils::IsEqual(pProp->GetPropertyName(), "Category");
  }

  return false;
}