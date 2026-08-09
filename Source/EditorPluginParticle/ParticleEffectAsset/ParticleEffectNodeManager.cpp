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

/// True for any operator node, i.e. anything that produces a value a property pin can take.
static bool IsValueNodeType(const plRTTI* pType)
{
  plStringView sValueProperty;
  return plParticleValueKind::FromNode(pType, sValueProperty) != plParticleValueKind::None;
}

/// Value pins are coloured by what flows through them, the way the material graph colours its
/// data pins, so a wire's type is readable without selecting anything.
static plColor GetValueKindColor(plParticleValueKind::Enum kind)
{
  switch (kind)
  {
    case plParticleValueKind::Number:
      return plColorScheme::DarkUI(plColorScheme::Blue);
    case plParticleValueKind::Bool:
      return plColorScheme::DarkUI(plColorScheme::Red);
    case plParticleValueKind::Color:
      return plColorScheme::DarkUI(plColorScheme::Yellow);
    case plParticleValueKind::Vector:
      return plColorScheme::DarkUI(plColorScheme::Teal);
    case plParticleValueKind::Texture:
      return plColorScheme::DarkUI(plColorScheme::Grape);
    case plParticleValueKind::Gradient:
      return plColorScheme::DarkUI(plColorScheme::Orange);
    case plParticleValueKind::Curve:
      return plColorScheme::DarkUI(plColorScheme::Green);
    default:
      return plColorScheme::DarkUI(plColorScheme::Gray);
  }
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

  // IsValueNodeType covers every operator, so adding one never needs editing this list again.
  if (pType->IsDerivedFrom<plParticleEffectNode>() || pType->IsDerivedFrom<plParticleSystemNode>() || IsValueNodeType(pType))
    return true;

  // Blocks live inside a system's context arrays and are not graph nodes. Pre-rework documents
  // store them as root-level nodes wired by pins, so those still need pins and connections in
  // order to be readable by the migration in InitializeAfterLoading.
  if (pObject->GetParent() != GetRootObject())
    return false;

  return pType->IsDerivedFrom<plParticleMixNode>() ||
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

    // One input per wireable property of every block the system owns. The pin name carries the
    // block guid and property name, which is all a connection needs to survive a save.
    for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
    {
      const char* szProperty = plParticleContextInfo::Get((plParticleContext::Enum)uiContext).m_szProperty;
      const auto& accessor = pObject->GetTypeAccessor();
      const plInt32 iCount = accessor.GetCount(szProperty);

      for (plInt32 i = 0; i < iCount; ++i)
      {
        const plVariant value = accessor.GetValue(szProperty, i);
        if (!value.IsA<plUuid>())
          continue;

        const plDocumentObject* pBlock = GetObject(value.Get<plUuid>());
        if (pBlock == nullptr)
          continue;

        plHybridArray<const plAbstractProperty*, 32> properties;
        pBlock->GetTypeAccessor().GetType()->GetAllProperties(properties);

        for (const plAbstractProperty* pProp : properties)
        {
          const plParticleValueKind::Enum kind = plParticleValueKind::FromProperty(pProp);
          if (kind == plParticleValueKind::None)
            continue;

          const plString sPinName = plParticlePropertyPin::Make(pBlock->GetGuid(), pProp->GetPropertyName());

          auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, sPinName, GetValueKindColor(kind), pObject);
          pPin->m_Category = plParticleGraphPinCategory::Value;
          ref_node.m_Inputs.PushBack(pPin);
        }
      }
    }
  }
  else if (IsValueNodeType(pType))
  {
    plStringView sValueProperty;
    const plParticleValueKind::Enum outputKind = plParticleValueKind::FromNode(pType, sValueProperty);

    for (const auto& input : plParticleOperatorInputs::Get(pType))
    {
      const plParticleValueKind::Enum inputKind = plParticleValueKind::FromNodeInput(pType, input.m_szPin);

      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, input.m_szPin, GetValueKindColor(inputKind), pObject);
      pPin->m_Category = plParticleGraphPinCategory::Value;
      ref_node.m_Inputs.PushBack(pPin);
    }

    auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Output, "Value", GetValueKindColor(outputKind), pObject);
    pPin->m_Category = plParticleGraphPinCategory::Value;
    ref_node.m_Outputs.PushBack(pPin);
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

  // Only systems are graph nodes now. Blocks are added into a system's contexts through the
  // block palette on the system node itself, and event reactions through the effect node.
  ref_types.PushBack(plGetStaticRTTI<plParticleSystemNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleValueNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleColorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleTextureNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleGradientNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleCurveNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleBoolNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleParameterNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleColorParameterNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleBoolParameterNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleToBoolNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleMathNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleCompareNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleBranchNode>());

  // Note: plParticleEffectNode is NOT in this list — it is auto-created by the document.
}

void plParticleEffectNodeManager::GetBlockTypes(plParticleContext::Enum context, plDynamicArray<const plRTTI*>& out_types)
{
  out_types.Clear();

  plSet<const plRTTI*> typeSet;
  plReflectionUtils::GatherTypesDerivedFromClass(plParticleContextInfo::GetFactoryBaseType(context), typeSet);

  for (auto pType : typeSet)
  {
    if (pType->GetTypeFlags().IsAnySet(plTypeFlags::Abstract))
      continue;

    // retired factories stay reflected so existing assets still load, but are not offered
    if (pType->GetAttributeByType<plHiddenAttribute>() != nullptr)
      continue;

    out_types.PushBack(pType);
  }

  struct TypeNameLess
  {
    PL_ALWAYS_INLINE bool Less(const plRTTI* a, const plRTTI* b) const { return a->GetTypeName().Compare(b->GetTypeName()) < 0; }
    PL_ALWAYS_INLINE bool Equal(const plRTTI* a, const plRTTI* b) const { return a == b; }
  };

  out_types.Sort(TypeNameLess());
}

plString plParticleEffectNodeManager::FriendlyTypeName(const plRTTI* pType)
{
  // A plTitleAttribute with no {Property} tokens is already the display name the author chose.
  if (const plTitleAttribute* pTitle = pType->GetAttributeByType<plTitleAttribute>())
  {
    const plStringView sTitle = pTitle->GetTitle();

    if (sTitle.FindSubString("{") == nullptr)
      return sTitle;
  }

  // Deliberately not plTranslate: with no translation entry the editor's make-readable translator
  // expands "plParticleCurveNode" into "Particle Curve Node", which is the decoration we are here
  // to remove. The raw type names are predictable, so trim them directly.
  plStringBuilder sName = pType->GetTypeName();

  if (const char* szSuffix = sName.FindLastSubString("Factory_"))
  {
    const plString sTail = szSuffix + 8; // strlen("Factory_")
    sName = sTail;
  }
  else
  {
    sName.TrimWordStart("plParticleType");
    sName.TrimWordEnd("Factory");
  }

  sName.TrimWordStart("plParticle");
  sName.TrimWordEnd("Node");

  return sName;
}

void plParticleEffectNodeManager::GetNodeCreationTemplates(plDynamicArray<plVisualGraphNodeDesc>& out_templates) const
{
  plHybridArray<const plRTTI*, 32> types;
  GetCreateableTypes(types);

  m_CreationNames.Clear();
  m_CreationNames.Reserve(types.GetCount());
  out_templates.Clear();

  for (const plRTTI* pType : types)
  {
    m_CreationNames.PushBack(FriendlyTypeName(pType));

    auto& desc = out_templates.ExpandAndGetRef();
    desc.m_pType = pType;
    desc.m_sTypeName = m_CreationNames.PeekBack();

    if (const plCategoryAttribute* pCategory = pType->GetAttributeByType<plCategoryAttribute>())
      desc.m_sCategory.Assign(pCategory->GetCategory());
  }
}

void plParticleEffectNodeManager::RecreateAllSystemPins()
{
  for (const plDocumentObject* pChild : GetRootObject()->GetChildren())
  {
    if (pChild->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
      TryRecreatePins(pChild);
  }
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

  // A property takes one value, but one value can drive many properties. The kinds have to agree,
  // otherwise a texture could be dropped into a numeric port and silently fold to nothing.
  if (srcPin.m_Category == plParticleGraphPinCategory::Value)
  {
    plStringView sValueProperty;
    const plParticleValueKind::Enum sourceKind = plParticleValueKind::FromNode(srcPin.GetParent()->GetTypeAccessor().GetType(), sValueProperty);

    plParticleValueKind::Enum targetKind = plParticleValueKind::None;

    plUuid block;
    plStringBuilder sProperty;

    if (plParticlePropertyPin::Parse(tgtPin.GetName(), block, sProperty))
    {
      const plDocumentObject* pBlock = GetObject(block);
      if (pBlock == nullptr)
        return plStatus("The block this pin belongs to no longer exists.");

      const plAbstractProperty* pProp = pBlock->GetTypeAccessor().GetType()->FindPropertyByName(sProperty);
      if (pProp == nullptr)
        return plStatus("The target property no longer exists.");

      targetKind = plParticleValueKind::FromProperty(pProp);
    }
    else
    {
      // feeding another operator's input
      targetKind = plParticleValueKind::FromNodeInput(tgtPin.GetParent()->GetTypeAccessor().GetType(), tgtPin.GetName());
    }

    if (targetKind == plParticleValueKind::None)
      return plStatus("That pin does not take a value.");

    // Numbers and bools convert implicitly in both directions, the way the other graphs allow an
    // int into a bool. Asset kinds never do: a texture is not a gradient.
    const bool bScalar = (sourceKind == plParticleValueKind::Number || sourceKind == plParticleValueKind::Bool) &&
                         (targetKind == plParticleValueKind::Number || targetKind == plParticleValueKind::Bool);

    if (sourceKind != targetKind && !bScalar)
      return plStatus("This value does not fit that property.");

    // N outgoing, 1 incoming: one value can drive many properties, but a property takes exactly
    // one value. Connect1toN is the mirror image of that and was the wrong way round.
    out_result = CanConnectResult::ConnectNto1;
    return plStatus(PL_SUCCESS);
  }

  // The block categories only still exist so pre-rework documents can be read and migrated;
  // new connections of those kinds must not be authorable.
  if (srcPin.m_Category != plParticleGraphPinCategory::System)
  {
    return plStatus("Blocks live inside a system's contexts, not on a wire.");
  }

  out_result = CanConnectResult::ConnectNtoN;

  return plStatus(PL_SUCCESS);
}

bool plParticleEffectNodeManager::InternalIsDynamicPinProperty(const plDocumentObject* pObject, const plAbstractProperty* pProp) const
{
  const plRTTI* pType = pObject->GetTypeAccessor().GetType();

  if (pType->IsDerivedFrom<plParticleMixNode>())
  {
    return plStringUtils::IsEqual(pProp->GetPropertyName(), "Category");
  }

  // Adding or removing a block changes which property pins the system offers, so the four
  // context arrays have to trigger a pin rebuild.
  if (pType->IsDerivedFrom<plParticleSystemNode>())
  {
    for (plUInt32 i = 0; i < plParticleContext::Count; ++i)
    {
      if (plStringUtils::IsEqual(pProp->GetPropertyName(), plParticleContextInfo::Get((plParticleContext::Enum)i).m_szProperty))
        return true;
    }
  }

  return false;
}