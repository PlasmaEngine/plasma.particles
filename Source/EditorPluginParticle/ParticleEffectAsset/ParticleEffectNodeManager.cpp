#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphCompiler.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_KillIf.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SetAttribute.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Strings/TranslationLookup.h>
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
    case plParticleValueKind::Adaptive:
      return plColorScheme::LightUI(plColorScheme::Gray);
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

          const plString sPinName = plParticlePropertyPin::Make(pBlock, pProp->GetPropertyName());

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
    const plParticleValueKind::Enum nodeKind = plParticleValueKind::FromNode(pType, sValueProperty);

    for (const auto& input : plParticleOperatorInputs::Get(pType))
    {
      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Input, input.m_szPin, GetValueKindColor(input.m_Kind), pObject);
      pPin->m_Category = plParticleGraphPinCategory::Value;
      ref_node.m_Inputs.PushBack(pPin);
    }

    // one output per component for the Split nodes, else the one value
    for (const auto& output : plParticleOperatorOutputs::Get(pType))
    {
      const plParticleValueKind::Enum outputKind = output.m_Kind != plParticleValueKind::None ? output.m_Kind : nodeKind;

      auto pPin = PL_DEFAULT_NEW(plParticleGraphPin, plVisualGraphPin::Type::Output, output.m_szPin, GetValueKindColor(outputKind), pObject);
      pPin->m_Category = plParticleGraphPinCategory::Value;
      ref_node.m_Outputs.PushBack(pPin);
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
  ref_types.PushBack(plGetStaticRTTI<plParticleBlackboardEntryNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleBlackboardVectorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleBlackboardColorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleToBoolNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleMathNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleCompareNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleBranchNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleSystemValueNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleSystemVectorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleVectorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleFunctionNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleRemapNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleLerpNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleClampNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleVectorFunctionNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleVectorReduceNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleExpressionNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleDotNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleMakeVectorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleSampleGradientNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleSampleCurveNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleNoiseNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleInputValueNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleInputVectorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleInputColorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleRandomNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleCustomFloatNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleCustomVectorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleCustomColorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleEventInputNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleSplitVectorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleSplitColorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleMakeColorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleSwizzleNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleDistanceNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleCrossNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleReflectNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleProjectNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleRotateAboutAxisNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticlePowerNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleStepNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleSmoothStepNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleInverseLerpNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleATan2Node>());
  ref_types.PushBack(plGetStaticRTTI<plParticleMathConstantNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleLogicNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleNotNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleRandomRangeNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleRandomIntegerNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleRandomVectorNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleVectorNoiseNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleTransformNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleCurveOverLifeNode>());
  ref_types.PushBack(plGetStaticRTTI<plParticleGradientOverLifeNode>());

  // Vector Math, Scale Vector and Color Channel still load; adaptive maths and Split Colour replace them.
  // plParticleEffectNode is not in this list either: the document creates it.
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

  return SplitWords(sName);
}

plString plParticleEffectNodeManager::SplitWords(plStringView sName)
{
  // "VectorMath" reads as "Vector Math"
  plStringBuilder sResult;
  bool bPreviousLower = false;

  for (plUInt32 c : sName)
  {
    if (bPreviousLower && c >= 'A' && c <= 'Z')
      sResult.Append(" ");

    sResult.Append(c);
    bPreviousLower = c >= 'a' && c <= 'z';
  }

  return sResult;
}

namespace
{
  /// One preset property of a palette entry.
  struct PaletteProperty
  {
    const char* m_szName = nullptr;
    plVariant m_Value;
  };
} // namespace

void plParticleEffectNodeManager::GetNodeCreationTemplates(plDynamicArray<plVisualGraphNodeDesc>& out_templates) const
{
  m_CreationNames.Clear();
  m_CreationProperties.Clear();
  out_templates.Clear();

  auto add = [&](const plRTTI* pType, plStringView sName, const char* szCategory, std::initializer_list<PaletteProperty> properties = {})
  {
    m_CreationNames.PushBack(sName);

    auto& desc = out_templates.ExpandAndGetRef();
    desc.m_pType = pType;
    desc.m_sTypeName = m_CreationNames.PeekBack();
    desc.m_sCategory.Assign(szCategory);

    if (properties.size() == 0)
      return;

    // filled completely before the view is taken, so the inline storage never moves
    auto& values = m_CreationProperties.ExpandAndGetRef();
    for (const PaletteProperty& property : properties)
    {
      plVisualGraphNodeProperty& value = values.ExpandAndGetRef();
      value.m_sPropertyName.Assign(property.m_szName);
      value.m_Value = property.m_Value;
    }

    desc.m_PropertyValues = values.GetArrayPtr();
  };

  auto addType = [&](const plRTTI* pType, const char* szCategory)
  { add(pType, FriendlyTypeName(pType), szCategory); };

  // a node that reads one of several inputs gets one entry per input, named after it ("Speed", "Time")
  auto addPerInput = [&](const plRTTI* pType, const plRTTI* pInputEnum, const char* szCategory)
  {
    plHybridArray<plReflectionUtils::EnumKeyValuePair, 16> entries;
    plReflectionUtils::GetEnumKeysAndValues(pInputEnum, entries, plReflectionUtils::EnumConversionMode::ValueNameOnly);

    for (const auto& entry : entries)
      add(pType, SplitWords(entry.m_sKey), szCategory, {{"Input", (plInt64)entry.m_iValue}});
  };

  auto op = [](plInt64 iOperation) -> PaletteProperty
  { return {"Operation", iOperation}; };

  const char* szValue = "Value";
  const char* szVector = "Vector & Colour";
  const char* szMaths = "Maths";
  const char* szFunctions = "Functions";
  const char* szLogic = "Logic";
  const char* szRandom = "Random";
  const char* szNoise = "Noise";
  const char* szCurves = "Curves";
  const char* szParticle = "Particle";
  const char* szEffect = "Effect & Camera";
  const char* szCode = "Code";

  // systems and constants
  add(plGetStaticRTTI<plParticleSystemNode>(), FriendlyTypeName(plGetStaticRTTI<plParticleSystemNode>()), "System");
  addType(plGetStaticRTTI<plParticleValueNode>(), szValue);
  addType(plGetStaticRTTI<plParticleColorNode>(), szValue);
  addType(plGetStaticRTTI<plParticleVectorNode>(), szValue);
  addType(plGetStaticRTTI<plParticleBoolNode>(), szValue);
  addType(plGetStaticRTTI<plParticleTextureNode>(), szValue);
  addType(plGetStaticRTTI<plParticleGradientNode>(), szValue);
  addType(plGetStaticRTTI<plParticleCurveNode>(), szValue);

  // vector and colour
  const plRTTI* pVectorFunction = plGetStaticRTTI<plParticleVectorFunctionNode>();
  const plRTTI* pVectorReduce = plGetStaticRTTI<plParticleVectorReduceNode>();
  add(plGetStaticRTTI<plParticleSplitVectorNode>(), "Split Vector", szVector);
  add(plGetStaticRTTI<plParticleSplitColorNode>(), "Split Colour", szVector);
  add(plGetStaticRTTI<plParticleMakeVectorNode>(), "Make Vector", szVector);
  add(plGetStaticRTTI<plParticleMakeColorNode>(), "Make Colour", szVector);
  add(plGetStaticRTTI<plParticleSwizzleNode>(), "Swizzle", szVector);
  add(plGetStaticRTTI<plParticleDistanceNode>(), "Distance", szVector);
  add(pVectorFunction, "Normalize", szVector, {op(plParticleVectorFunctionOp::Normalize)});
  add(pVectorReduce, "Length", szVector, {op(plParticleVectorReduceOp::Length)});
  add(plGetStaticRTTI<plParticleDotNode>(), "Dot", szVector);
  add(plGetStaticRTTI<plParticleCrossNode>(), "Cross", szVector);
  add(plGetStaticRTTI<plParticleReflectNode>(), "Reflect", szVector);
  add(plGetStaticRTTI<plParticleProjectNode>(), "Project On", szVector);
  add(plGetStaticRTTI<plParticleRotateAboutAxisNode>(), "Rotate About Axis", szVector);

  // maths, for numbers, vectors and colours alike
  const plRTTI* pMath = plGetStaticRTTI<plParticleMathNode>();
  const plRTTI* pFunction = plGetStaticRTTI<plParticleFunctionNode>();
  add(pMath, "Add", szMaths, {op(plParticleMathOp::Add)});
  add(pMath, "Subtract", szMaths, {op(plParticleMathOp::Subtract)});
  add(pMath, "Multiply", szMaths, {op(plParticleMathOp::Multiply)});
  add(pMath, "Divide", szMaths, {op(plParticleMathOp::Divide)});
  add(pMath, "Min", szMaths, {op(plParticleMathOp::Min)});
  add(pMath, "Max", szMaths, {op(plParticleMathOp::Max)});
  add(plGetStaticRTTI<plParticleLerpNode>(), "Lerp", szMaths);
  add(plGetStaticRTTI<plParticleClampNode>(), "Clamp", szMaths);
  add(plGetStaticRTTI<plParticlePowerNode>(), "Power", szMaths);
  add(pMath, "Modulo", szMaths, {op(plParticleMathOp::Modulo)});
  add(plGetStaticRTTI<plParticleStepNode>(), "Step", szMaths);
  add(plGetStaticRTTI<plParticleSmoothStepNode>(), "Smooth Step", szMaths);
  add(plGetStaticRTTI<plParticleInverseLerpNode>(), "Inverse Lerp", szMaths);
  add(plGetStaticRTTI<plParticleRemapNode>(), "Remap", szMaths);
  add(pFunction, "One Minus", szMaths, {op(plParticleFunctionOp::OneMinus)});
  add(pFunction, "Negate", szMaths, {op(plParticleFunctionOp::Negate)});
  add(pFunction, "Abs", szMaths, {op(plParticleFunctionOp::Abs)});
  add(pFunction, "Saturate", szMaths, {op(plParticleFunctionOp::Saturate)});

  // functions; angles are degrees
  add(pFunction, "Sin", szFunctions, {op(plParticleFunctionOp::Sine)});
  add(pFunction, "Cos", szFunctions, {op(plParticleFunctionOp::Cosine)});
  add(pFunction, "Tan", szFunctions, {op(plParticleFunctionOp::Tan)});
  add(pFunction, "ASin", szFunctions, {op(plParticleFunctionOp::ASin)});
  add(pFunction, "ACos", szFunctions, {op(plParticleFunctionOp::ACos)});
  add(pFunction, "ATan", szFunctions, {op(plParticleFunctionOp::ATan)});
  add(plGetStaticRTTI<plParticleATan2Node>(), "ATan2", szFunctions);
  add(pFunction, "Exp", szFunctions, {op(plParticleFunctionOp::Exp)});
  add(pFunction, "Log", szFunctions, {op(plParticleFunctionOp::Log)});
  add(pFunction, "Log2", szFunctions, {op(plParticleFunctionOp::Log2)});
  add(pFunction, "Floor", szFunctions, {op(plParticleFunctionOp::Floor)});
  add(pFunction, "Ceil", szFunctions, {op(plParticleFunctionOp::Ceil)});
  add(pFunction, "Round", szFunctions, {op(plParticleFunctionOp::Round)});
  add(pFunction, "Trunc", szFunctions, {op(plParticleFunctionOp::Trunc)});
  add(pFunction, "Frac", szFunctions, {op(plParticleFunctionOp::Frac)});
  add(pFunction, "Sign", szFunctions, {op(plParticleFunctionOp::Sign)});
  add(pFunction, "Reciprocal", szFunctions, {op(plParticleFunctionOp::Reciprocal)});
  add(pFunction, "Square", szFunctions, {op(plParticleFunctionOp::Square)});
  add(pFunction, "Sqrt", szFunctions, {op(plParticleFunctionOp::Sqrt)});
  add(pFunction, "Degrees To Radians", szFunctions, {op(plParticleFunctionOp::DegreesToRadians)});
  add(pFunction, "Radians To Degrees", szFunctions, {op(plParticleFunctionOp::RadiansToDegrees)});

  const plRTTI* pConstant = plGetStaticRTTI<plParticleMathConstantNode>();
  add(pConstant, "Pi", szFunctions, {{"Constant", (plInt64)plParticleMathConstant::Pi}});
  add(pConstant, "Tau", szFunctions, {{"Constant", (plInt64)plParticleMathConstant::Tau}});
  add(pConstant, "E", szFunctions, {{"Constant", (plInt64)plParticleMathConstant::E}});

  // logic
  const plRTTI* pLogic = plGetStaticRTTI<plParticleLogicNode>();
  add(plGetStaticRTTI<plParticleCompareNode>(), "Compare", szLogic);
  add(pLogic, "And", szLogic, {op(plParticleLogicOp::And)});
  add(pLogic, "Or", szLogic, {op(plParticleLogicOp::Or)});
  add(pLogic, "Xor", szLogic, {op(plParticleLogicOp::Xor)});
  add(plGetStaticRTTI<plParticleNotNode>(), "Not", szLogic);
  add(plGetStaticRTTI<plParticleBranchNode>(), "Branch", szLogic);
  add(plGetStaticRTTI<plParticleToBoolNode>(), "To Bool", szLogic);

  // random and noise
  add(plGetStaticRTTI<plParticleRandomNode>(), "Random", szRandom);
  add(plGetStaticRTTI<plParticleRandomRangeNode>(), "Random Range", szRandom);
  add(plGetStaticRTTI<plParticleRandomIntegerNode>(), "Random Integer", szRandom);
  add(plGetStaticRTTI<plParticleRandomVectorNode>(), "Random Vector", szRandom);
  add(plGetStaticRTTI<plParticleNoiseNode>(), "Noise", szNoise);
  add(plGetStaticRTTI<plParticleVectorNoiseNode>(), "Vector Noise", szNoise);

  // curves; the Over Life presets also add the Set Attribute they drive (plParticleCurvePreset)
  const plRTTI* pCurveOverLife = plGetStaticRTTI<plParticleCurveOverLifeNode>();
  add(pCurveOverLife, "Curve Over Life", szCurves);
  add(pCurveOverLife, "Size Over Life", szCurves, {{"Preset", (plInt64)plParticleCurvePreset::SizeOverLife}});
  add(pCurveOverLife, "Opacity Over Life", szCurves, {{"Preset", (plInt64)plParticleCurvePreset::OpacityOverLife}});
  add(pCurveOverLife, "Emissive Over Life", szCurves, {{"Preset", (plInt64)plParticleCurvePreset::EmissiveOverLife}});
  add(plGetStaticRTTI<plParticleGradientOverLifeNode>(), "Gradient Over Life", szCurves);
  add(plGetStaticRTTI<plParticleSampleCurveNode>(), "Sample Curve", szCurves);
  add(plGetStaticRTTI<plParticleSampleGradientNode>(), "Sample Gradient", szCurves);

  // particle data
  addPerInput(plGetStaticRTTI<plParticleInputValueNode>(), plGetStaticRTTI<plParticleInputValue>(), szParticle);
  addPerInput(plGetStaticRTTI<plParticleInputVectorNode>(), plGetStaticRTTI<plParticleInputVector>(), szParticle);
  addType(plGetStaticRTTI<plParticleInputColorNode>(), szParticle);
  addType(plGetStaticRTTI<plParticleCustomFloatNode>(), szParticle);
  addType(plGetStaticRTTI<plParticleCustomVectorNode>(), szParticle);
  addType(plGetStaticRTTI<plParticleCustomColorNode>(), szParticle);
  addPerInput(plGetStaticRTTI<plParticleEventInputNode>(), plGetStaticRTTI<plParticleEventInput>(), "Event");

  // the effect, the camera and gameplay
  addPerInput(plGetStaticRTTI<plParticleSystemValueNode>(), plGetStaticRTTI<plParticleSystemValue>(), szEffect);
  addPerInput(plGetStaticRTTI<plParticleSystemVectorNode>(), plGetStaticRTTI<plParticleSystemVector>(), szEffect);

  const plRTTI* pTransform = plGetStaticRTTI<plParticleTransformNode>();
  add(pTransform, "Transform Position", szEffect, {{"Kind", (plInt64)plParticleTransformKind::Position}});
  add(pTransform, "Transform Direction", szEffect, {{"Kind", (plInt64)plParticleTransformKind::Direction}});

  addType(plGetStaticRTTI<plParticleParameterNode>(), szEffect);
  addType(plGetStaticRTTI<plParticleColorParameterNode>(), szEffect);
  addType(plGetStaticRTTI<plParticleBoolParameterNode>(), szEffect);
  addType(plGetStaticRTTI<plParticleBlackboardEntryNode>(), szEffect);
  addType(plGetStaticRTTI<plParticleBlackboardVectorNode>(), szEffect);
  addType(plGetStaticRTTI<plParticleBlackboardColorNode>(), szEffect);

  add(plGetStaticRTTI<plParticleExpressionNode>(), "Expression", szCode);
}

plString plParticleEffectNodeManager::GetPinTypeName(const plVisualGraphPin& pin) const
{
  const auto& particlePin = static_cast<const plParticleGraphPin&>(pin);
  if (particlePin.m_Category != plParticleGraphPinCategory::Value)
    return {};

  plParticleValueKind::Enum kind = plParticleValueKind::None;

  if (pin.GetType() == plVisualGraphPin::Type::Output)
  {
    plParticleGraphCompiler compiler(this, FindEffectNode());
    kind = compiler.GetOutputKind(pin.GetParent(), plParticleOperatorOutputs::FindIndex(pin.GetParent()->GetTypeAccessor().GetType(), pin.GetName()));
  }
  else
  {
    plStringBuilder sProperty;
    if (const plDocumentObject* pBlock = plParticlePropertyPin::FindBlock(pin.GetParent(), pin.GetName(), sProperty))
    {
      if (const plAbstractProperty* pProp = pBlock->GetTypeAccessor().GetType()->FindPropertyByName(sProperty))
        kind = plParticleValueKind::FromProperty(pProp);
    }
    else
    {
      kind = plParticleValueKind::FromNodeInput(pin.GetParent()->GetTypeAccessor().GetType(), pin.GetName());
    }
  }

  if (kind == plParticleValueKind::None)
    return {};

  if (kind == plParticleValueKind::Adaptive)
    return "Number, Vector or Colour";

  // capitalised as the palette writes types; the names are plain ASCII
  plStringBuilder sName = plParticleValueKind::GetName(kind);
  plStringBuilder sRest = sName.GetView().GetSubString(1, plInvalidIndex);
  plStringBuilder sFirst = sName.GetView().GetSubString(0, 1);
  sFirst.ToUpper();
  sName.Set(sFirst, sRest);
  return sName;
}

void plParticleEffectNodeManager::RecreateAllSystemPins()
{
  // TryRecreatePins adds and removes connection objects, which are children of the root object,
  // so the nodes have to be collected before the child list is modified.
  plHybridArray<const plDocumentObject*, 16> systemNodes;
  for (const plDocumentObject* pChild : GetRootObject()->GetChildren())
  {
    if (pChild->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
      systemNodes.PushBack(pChild);
  }

  for (const plDocumentObject* pNode : systemNodes)
  {
    TryRecreatePins(pNode);
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
    plParticleGraphCompiler compiler(this, FindEffectNode());

    const plDocumentObject* pSourceNode = srcPin.GetParent();
    const plParticleValueKind::Enum sourceKind = compiler.GetOutputKind(pSourceNode, plParticleOperatorOutputs::FindIndex(pSourceNode->GetTypeAccessor().GetType(), srcPin.GetName()));

    plParticleValueKind::Enum targetKind = plParticleValueKind::None;

    plStringBuilder sKey;
    plStringBuilder sProperty;

    if (plParticlePropertyPin::Parse(tgtPin.GetName(), sKey, sProperty))
    {
      const plDocumentObject* pBlock = plParticlePropertyPin::FindBlock(tgtPin.GetParent(), tgtPin.GetName(), sProperty);
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

    // Numbers and bools convert into each other, and vectors and colours do, the way the other graphs
    // allow an int into a bool. Adaptive inputs take any of them. Asset kinds never convert: a texture is
    // not a gradient.
    if (!plParticleValueKind::CanConvert(sourceKind, targetKind))
      return plStatus(plFmt("A {} does not fit a pin that takes a {}.", plParticleValueKind::GetName(sourceKind), plParticleValueKind::GetName(targetKind)));

    // a wider value makes an adaptive node wider, which everything it feeds has to take
    if (targetKind == plParticleValueKind::Adaptive)
    {
      const plDocumentObject* pTargetNode = tgtPin.GetParent();
      plParticleValueKind::Enum newKind = plParticleValueKind::Widen(plParticleValueKind::Number, sourceKind);

      // the other wired inputs keep their kinds; this pin's current wire is replaced
      for (const auto& input : plParticleOperatorInputs::Get(pTargetNode->GetTypeAccessor().GetType()))
      {
        if (input.m_Kind != plParticleValueKind::Adaptive || tgtPin.GetName() == input.m_szPin)
          continue;

        plUInt32 uiOutput = 0;
        if (const plDocumentObject* pOther = compiler.GetInputSource(pTargetNode, input.m_szPin, &uiOutput))
          newKind = plParticleValueKind::Widen(newKind, compiler.GetOutputKind(pOther, uiOutput));
      }

      if (newKind != compiler.GetOutputKind(pTargetNode, 0))
      {
        plSet<const plDocumentObject*> visited;
        PL_SUCCEED_OR_RETURN(CheckWidening(pTargetNode, newKind, compiler, visited));
      }
    }

    // a per-particle value may only end up where something runs per particle
    if (compiler.GetTier(srcPin.GetParent()) == plParticleTier::Particle)
    {
      PL_SUCCEED_OR_RETURN(CheckParticleTarget(tgtPin));
    }

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

const plDocumentObject* plParticleEffectNodeManager::FindEffectNode() const
{
  for (const plDocumentObject* pChild : GetRootObject()->GetChildren())
  {
    if (pChild->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleEffectNode>())
      return pChild;
  }

  return nullptr;
}

plStatus plParticleEffectNodeManager::CheckParticleTarget(const plVisualGraphPin& targetPin) const
{
  plStringBuilder sProperty;
  if (const plDocumentObject* pBlock = plParticlePropertyPin::FindBlock(targetPin.GetParent(), targetPin.GetName(), sProperty))
  {
    const plRTTI* pBlockType = pBlock->GetTypeAccessor().GetType();
    const plParticleContext::Enum context = plParticleContextInfo::FromFactoryType(pBlockType);

    if (context == plParticleContext::Spawn)
      return plStatus("Per-particle values can't drive Spawn: the emitter runs once per step, not per particle. Use Set Attribute in Update.");

    if (context == plParticleContext::Output)
      return plStatus("Per-particle values can't drive Output blocks. Write Color or Size with Set Attribute in Update; the render types read them per particle.");

    const bool bSink = pBlockType->IsDerivedFrom<plParticleInitializerFactory_SetAttribute>() || pBlockType->IsDerivedFrom<plParticleBehaviorFactory_Attribute>() ||
                       pBlockType->IsDerivedFrom<plParticleBehaviorFactory_KillIf>();
    if (bSink)
      return plStatus(PL_SUCCESS);

    // the short list of block properties that read their value per particle
    const plAbstractProperty* pProp = pBlockType->FindPropertyByName(sProperty);
    if (pProp != nullptr && pProp->GetAttributeByType<plParticlePerParticleAttribute>() != nullptr)
      return plStatus(PL_SUCCESS);

    const char* szContext = (context == plParticleContext::Initialize) ? "Initialize" : "Update";
    return plStatus(plFmt("{} takes constant and per-frame values. Set the value per particle with Set Attribute in {} instead.", plTranslate(sProperty), szContext));
  }

  // some operator inputs pick a code path once per program rather than per particle
  auto checkUniform = [](const plVisualGraphPin& pin) -> plStatus
  {
    const plParticleOperatorInputs::Input* pInput = plParticleOperatorInputs::Find(pin.GetParent()->GetTypeAccessor().GetType(), pin.GetName());
    if (pInput != nullptr && pInput->m_bUniform)
      return plStatus(plFmt("{} takes constant and per-frame values only.", pInput->m_szPin));

    return plStatus(PL_SUCCESS);
  };

  PL_SUCCEED_OR_RETURN(checkUniform(targetPin));

  // into another operator: everything that operator feeds must take per-particle values as well
  plSet<const plDocumentObject*> visited;
  plHybridArray<const plDocumentObject*, 8> pending;
  pending.PushBack(targetPin.GetParent());

  while (!pending.IsEmpty())
  {
    const plDocumentObject* pNode = pending.PeekBack();
    pending.PopBack();

    if (visited.Contains(pNode))
      continue;
    visited.Insert(pNode);

    for (const auto& pOutput : GetOutputPins(pNode))
    {
      for (const plVisualGraphConnection* pConnection : GetConnections(*pOutput))
      {
        const plVisualGraphPin& downstream = pConnection->GetTargetPin();

        plStringBuilder sDownstreamProperty;
        if (plParticlePropertyPin::FindBlock(downstream.GetParent(), downstream.GetName(), sDownstreamProperty) != nullptr)
        {
          PL_SUCCEED_OR_RETURN(CheckParticleTarget(downstream));
        }
        else
        {
          PL_SUCCEED_OR_RETURN(checkUniform(downstream));
          pending.PushBack(downstream.GetParent());
        }
      }
    }
  }

  return plStatus(PL_SUCCESS);
}

plStatus plParticleEffectNodeManager::CheckWidening(const plDocumentObject* pNode, plParticleValueKind::Enum newKind, plParticleGraphCompiler& ref_compiler, plSet<const plDocumentObject*>& ref_visited) const
{
  if (ref_visited.Contains(pNode))
    return plStatus(PL_SUCCESS);

  ref_visited.Insert(pNode);

  for (const auto& pOutput : GetOutputPins(pNode))
  {
    for (const plVisualGraphConnection* pConnection : GetConnections(*pOutput))
    {
      const plVisualGraphPin& downstream = pConnection->GetTargetPin();
      const plDocumentObject* pTarget = downstream.GetParent();

      plStringBuilder sProperty;
      if (const plDocumentObject* pBlock = plParticlePropertyPin::FindBlock(pTarget, downstream.GetName(), sProperty))
      {
        const plAbstractProperty* pProp = pBlock->GetTypeAccessor().GetType()->FindPropertyByName(sProperty);
        const plParticleValueKind::Enum propertyKind = pProp != nullptr ? plParticleValueKind::FromProperty(pProp) : plParticleValueKind::None;

        if (!plParticleValueKind::CanConvert(newKind, propertyKind))
          return plStatus(plFmt("That makes the result a {}, but {} takes a {}.", plParticleValueKind::GetName(newKind), plTranslate(sProperty), plParticleValueKind::GetName(propertyKind)));

        continue;
      }

      const plParticleValueKind::Enum inputKind = plParticleValueKind::FromNodeInput(pTarget->GetTypeAccessor().GetType(), downstream.GetName());

      if (inputKind != plParticleValueKind::Adaptive)
      {
        if (!plParticleValueKind::CanConvert(newKind, inputKind))
          return plStatus(plFmt("That makes the result a {}, but it feeds a {} input that takes a {}.", plParticleValueKind::GetName(newKind), downstream.GetName(), plParticleValueKind::GetName(inputKind)));

        continue;
      }

      // an adaptive node downstream widens in turn, which its own consumers have to take
      const plParticleValueKind::Enum downstreamKind = plParticleValueKind::Widen(ref_compiler.GetOutputKind(pTarget, 0), newKind);
      if (downstreamKind != ref_compiler.GetOutputKind(pTarget, 0))
      {
        PL_SUCCEED_OR_RETURN(CheckWidening(pTarget, downstreamKind, ref_compiler, ref_visited));
      }
    }
  }

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