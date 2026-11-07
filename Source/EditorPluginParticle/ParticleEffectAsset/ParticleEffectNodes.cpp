#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>

static const plParticleContextInfo s_ContextInfo[plParticleContext::Count] = {
  {plParticleContext::Spawn, "Emitters", "Spawn", []() { return plGetStaticRTTI<plParticleEmitterFactory>(); }},
  {plParticleContext::Initialize, "Initializers", "Initialize", []() { return plGetStaticRTTI<plParticleInitializerFactory>(); }},
  {plParticleContext::Update, "Behaviors", "Update", []() { return plGetStaticRTTI<plParticleBehaviorFactory>(); }},
  {plParticleContext::Output, "Types", "Output", []() { return plGetStaticRTTI<plParticleTypeFactory>(); }},
  {plParticleContext::Events, "EventRaisers", "Events", []() { return plGetStaticRTTI<plParticleEventRaiserBlock>(); }},
};

const plParticleContextInfo& plParticleContextInfo::Get(plParticleContext::Enum context)
{
  PL_ASSERT_DEV(context >= 0 && context < plParticleContext::Count, "Invalid particle context");
  return s_ContextInfo[context];
}

const plRTTI* plParticleContextInfo::GetFactoryBaseType(plParticleContext::Enum context)
{
  return Get(context).m_FactoryBaseType();
}

plParticleContext::Enum plParticleContextInfo::FromFactoryType(const plRTTI* pType)
{
  if (pType != nullptr)
  {
    for (plUInt32 i = 0; i < plParticleContext::Count; ++i)
    {
      if (pType->IsDerivedFrom(s_ContextInfo[i].m_FactoryBaseType()))
        return s_ContextInfo[i].m_Context;
    }
  }

  return plParticleContext::Count;
}

plString plParticlePropertyPin::GetKey(const plDocumentObject* pBlock)
{
  const plVariant pinId = pBlock->GetTypeAccessor().GetValue("PinId");
  if (pinId.IsA<plString>() && !pinId.Get<plString>().IsEmpty())
    return pinId.Get<plString>();

  plStringBuilder sGuid;
  plConversionUtils::ToString(pBlock->GetGuid(), sGuid);
  return sGuid;
}

plString plParticlePropertyPin::Make(const plDocumentObject* pBlock, plStringView sProperty)
{
  plStringBuilder sPin;
  sPin.Set(GetKey(pBlock), "|", sProperty);
  return sPin;
}

bool plParticlePropertyPin::Parse(plStringView sPinName, plStringBuilder& out_sKey, plStringBuilder& out_sProperty)
{
  const char* szSeparator = sPinName.FindSubString("|");
  if (szSeparator == nullptr)
    return false;

  out_sKey = plStringView(sPinName.GetStartPointer(), szSeparator);
  out_sProperty = plStringView(szSeparator + 1, sPinName.GetEndPointer());

  return !out_sKey.IsEmpty() && !out_sProperty.IsEmpty();
}

const plDocumentObject* plParticlePropertyPin::FindBlock(const plDocumentObject* pSystemNode, plStringView sPinName, plStringBuilder& out_sProperty)
{
  plStringBuilder sKey;
  if (pSystemNode == nullptr || !Parse(sPinName, sKey, out_sProperty))
    return nullptr;

  for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
  {
    const char* szProperty = plParticleContextInfo::Get((plParticleContext::Enum)uiContext).m_szProperty;
    const auto& accessor = pSystemNode->GetTypeAccessor();
    const plInt32 iCount = accessor.GetCount(szProperty);

    for (plInt32 i = 0; i < iCount; ++i)
    {
      const plVariant value = accessor.GetValue(szProperty, i);
      if (!value.IsA<plUuid>())
        continue;

      const plDocumentObject* pBlock = pSystemNode->GetDocumentObjectManager()->GetObject(value.Get<plUuid>());
      if (pBlock != nullptr && GetKey(pBlock) == sKey)
        return pBlock;
    }
  }

  return nullptr;
}

void plParticlePropertyPin::StampKeys(plAbstractObjectGraph& ref_graph)
{
  for (auto it = ref_graph.GetAllNodes().GetIterator(); it.IsValid(); ++it)
  {
    plAbstractObjectNode* pNode = it.Value();

    const plRTTI* pType = plRTTI::FindTypeByName(pNode->GetType());
    if (pType == nullptr || plParticleContextInfo::FromFactoryType(pType) == plParticleContext::Count)
      continue;

    plStringBuilder sGuid;
    plConversionUtils::ToString(pNode->GetGuid(), sGuid);

    // default values may be left out of the graph, so the property is not always there
    if (plAbstractObjectNode::Property* pPinId = pNode->FindProperty("PinId"))
    {
      if (!pPinId->m_Value.IsA<plString>() || pPinId->m_Value.Get<plString>().IsEmpty())
        pPinId->m_Value = plString(sGuid);
    }
    else
    {
      pNode->AddProperty("PinId", plVariant(plString(sGuid)));
    }
  }
}

plParticleValueKind::Enum plParticleValueKind::FromProperty(const plAbstractProperty* pProp)
{
  if (pProp->GetCategory() != plPropertyCategory::Member)
    return None;
  if (pProp->GetFlags().IsSet(plPropertyFlags::ReadOnly))
    return None;
  if (pProp->GetAttributeByType<plHiddenAttribute>() != nullptr)
    return None;

  const plRTTI* pType = pProp->GetSpecificType();

  // Settings that pick a code path must stay constant: they decide shader permutations and
  // whether a system can lower to the GPU.
  if (pType->IsDerivedFrom<plEnumBase>() || pType->IsDerivedFrom<plBitflagsBase>())
    return None;

  if (const plAssetBrowserAttribute* pAsset = pProp->GetAttributeByType<plAssetBrowserAttribute>())
  {
    const plStringView sFilter = pAsset->GetTypeFilter();

    if (sFilter.FindSubString("Texture") != nullptr)
      return Texture;
    if (sFilter.FindSubString("Gradient") != nullptr)
      return Gradient;
    if (sFilter.FindSubString("Curve") != nullptr)
      return Curve;

    // Material, mesh and sub-effect slots have no operator that produces them, so giving them a
    // pin would only offer a connection nothing can satisfy. Add the nodes and drop this.
    return None;
  }

  if (pType == plGetStaticRTTI<bool>())
    return Bool;

  if (pType == plGetStaticRTTI<plColor>() || pType == plGetStaticRTTI<plColorGammaUB>())
    return Color;

  if (pType == plGetStaticRTTI<plVec3>() || pType == plGetStaticRTTI<plVec2>())
    return Vector;

  if (pType == plGetStaticRTTI<plString>() || pType == plGetStaticRTTI<plHashedString>())
    return Text;

  if (pType == plGetStaticRTTI<float>() || pType == plGetStaticRTTI<double>() || pType == plGetStaticRTTI<plInt32>() ||
      pType == plGetStaticRTTI<plUInt32>() || pType == plGetStaticRTTI<plInt8>() || pType == plGetStaticRTTI<plUInt8>() ||
      pType == plGetStaticRTTI<plInt16>() || pType == plGetStaticRTTI<plUInt16>() || pType == plGetStaticRTTI<plTime>() ||
      pType == plGetStaticRTTI<plAngle>() || pType == plGetStaticRTTI<plVarianceTypeFloat>() ||
      pType == plGetStaticRTTI<plVarianceTypeTime>() || pType == plGetStaticRTTI<plVarianceTypeAngle>())
    return Number;

  return None;
}

plParticleValueKind::Enum plParticleValueKind::FromNode(const plRTTI* pNodeType, plStringView& out_sValueProperty)
{
  if (pNodeType == nullptr)
    return None;

  if (pNodeType->IsDerivedFrom<plParticleValueNode>())
  {
    out_sValueProperty = "Value";
    return Number;
  }
  if (pNodeType->IsDerivedFrom<plParticleColorNode>())
  {
    out_sValueProperty = "Value";
    return Color;
  }
  if (pNodeType->IsDerivedFrom<plParticleTextureNode>())
  {
    out_sValueProperty = "Texture";
    return Texture;
  }
  if (pNodeType->IsDerivedFrom<plParticleGradientNode>())
  {
    out_sValueProperty = "Gradient";
    return Gradient;
  }
  if (pNodeType->IsDerivedFrom<plParticleCurveNode>())
  {
    out_sValueProperty = "Curve";
    return Curve;
  }

  if (pNodeType->IsDerivedFrom<plParticleBoolNode>())
  {
    out_sValueProperty = "Value";
    return Bool;
  }
  if (pNodeType->IsDerivedFrom<plParticleVectorNode>())
  {
    out_sValueProperty = "Value";
    return Vector;
  }

  // Computed nodes have no stored value; the evaluator produces it from their inputs.
  if (pNodeType->IsDerivedFrom<plParticleMathNode>() || pNodeType->IsDerivedFrom<plParticleBranchNode>() ||
      pNodeType->IsDerivedFrom<plParticleParameterNode>() || pNodeType->IsDerivedFrom<plParticleSystemValueNode>() ||
      pNodeType->IsDerivedFrom<plParticleFunctionNode>() || pNodeType->IsDerivedFrom<plParticleRemapNode>() ||
      pNodeType->IsDerivedFrom<plParticleLerpNode>() || pNodeType->IsDerivedFrom<plParticleClampNode>() ||
      pNodeType->IsDerivedFrom<plParticleVectorReduceNode>() || pNodeType->IsDerivedFrom<plParticleDotNode>() ||
      pNodeType->IsDerivedFrom<plParticleSampleCurveNode>() || pNodeType->IsDerivedFrom<plParticleNoiseNode>() ||
      pNodeType->IsDerivedFrom<plParticleColorChannelNode>() || pNodeType->IsDerivedFrom<plParticleExpressionNode>())
  {
    out_sValueProperty = {};
    return Number;
  }
  if (pNodeType->IsDerivedFrom<plParticleSystemVectorNode>() || pNodeType->IsDerivedFrom<plParticleVectorMathNode>() ||
      pNodeType->IsDerivedFrom<plParticleVectorFunctionNode>() || pNodeType->IsDerivedFrom<plParticleScaleVectorNode>() ||
      pNodeType->IsDerivedFrom<plParticleMakeVectorNode>())
  {
    out_sValueProperty = {};
    return Vector;
  }
  if (pNodeType->IsDerivedFrom<plParticleCompareNode>() || pNodeType->IsDerivedFrom<plParticleToBoolNode>() ||
      pNodeType->IsDerivedFrom<plParticleBoolParameterNode>())
  {
    out_sValueProperty = {};
    return Bool;
  }
  if (pNodeType->IsDerivedFrom<plParticleColorParameterNode>() || pNodeType->IsDerivedFrom<plParticleInputColorNode>() ||
      pNodeType->IsDerivedFrom<plParticleCustomColorNode>() || pNodeType->IsDerivedFrom<plParticleSampleGradientNode>())
  {
    out_sValueProperty = {};
    return Color;
  }

  // per-particle inputs
  if (pNodeType->IsDerivedFrom<plParticleInputValueNode>() || pNodeType->IsDerivedFrom<plParticleRandomNode>() ||
      pNodeType->IsDerivedFrom<plParticleCustomFloatNode>())
  {
    out_sValueProperty = {};
    return Number;
  }
  if (pNodeType->IsDerivedFrom<plParticleInputVectorNode>() || pNodeType->IsDerivedFrom<plParticleCustomVectorNode>() ||
      pNodeType->IsDerivedFrom<plParticleEventInputNode>())
  {
    out_sValueProperty = {};
    return Vector;
  }

  // per-frame game state
  if (pNodeType->IsDerivedFrom<plParticleBlackboardEntryNode>())
  {
    out_sValueProperty = {};
    return Number;
  }
  if (pNodeType->IsDerivedFrom<plParticleBlackboardVectorNode>())
  {
    out_sValueProperty = {};
    return Vector;
  }
  if (pNodeType->IsDerivedFrom<plParticleBlackboardColorNode>())
  {
    out_sValueProperty = {};
    return Color;
  }

  return None;
}

plParticleValueKind::Enum plParticleValueKind::FromNodeInput(const plRTTI* pNodeType, plStringView sPinName)
{
  for (const auto& input : plParticleOperatorInputs::Get(pNodeType))
  {
    if (sPinName == input.m_szPin)
      return input.m_Kind;
  }

  return None;
}

plArrayPtr<const plParticleOperatorInputs::Input> plParticleOperatorInputs::Get(const plRTTI* pNodeType)
{
  constexpr plParticleValueKind::Enum Number = plParticleValueKind::Number;
  constexpr plParticleValueKind::Enum Bool = plParticleValueKind::Bool;
  constexpr plParticleValueKind::Enum Vector = plParticleValueKind::Vector;
  constexpr plParticleValueKind::Enum Color = plParticleValueKind::Color;

  static const Input s_Math[] = {{"A", "A", Number}, {"B", "B", Number}};
  static const Input s_Expression[] = {{"A", "A", Number}, {"B", "B", Number}, {"C", "C", Number}, {"D", "D", Number}};
  static const Input s_ColorSingle[] = {{"Value", "Value", Color}};
  static const Input s_Branch[] = {{"Condition", nullptr, Bool}, {"True", "True", Number}, {"False", "False", Number}};
  static const Input s_Single[] = {{"Value", "Value", Number}};
  static const Input s_Lerp[] = {{"A", "A", Number}, {"B", "B", Number}, {"T", "T", Number}};
  static const Input s_Clamp[] = {{"Value", "Value", Number}, {"Min", "Min", Number}, {"Max", "Max", Number}};
  static const Input s_VectorPair[] = {{"A", "A", Vector}, {"B", "B", Vector}};
  static const Input s_VectorSingle[] = {{"Value", "Value", Vector}};
  static const Input s_Scale[] = {{"Vector", "Vector", Vector}, {"Scale", "Scale", Number}};
  static const Input s_Make[] = {{"X", "X", Number}, {"Y", "Y", Number}, {"Z", "Z", Number}};
  static const Input s_Sample[] = {{"T", "T", Number}};
  static const Input s_Noise[] = {{"Position", "Position", Vector}};

  if (pNodeType == nullptr)
    return {};

  if (pNodeType->IsDerivedFrom<plParticleToBoolNode>() || pNodeType->IsDerivedFrom<plParticleFunctionNode>() || pNodeType->IsDerivedFrom<plParticleRemapNode>())
    return plMakeArrayPtr(s_Single);

  if (pNodeType->IsDerivedFrom<plParticleMathNode>() || pNodeType->IsDerivedFrom<plParticleCompareNode>())
    return plMakeArrayPtr(s_Math);

  if (pNodeType->IsDerivedFrom<plParticleBranchNode>())
    return plMakeArrayPtr(s_Branch);

  if (pNodeType->IsDerivedFrom<plParticleLerpNode>())
    return plMakeArrayPtr(s_Lerp);

  if (pNodeType->IsDerivedFrom<plParticleClampNode>())
    return plMakeArrayPtr(s_Clamp);

  if (pNodeType->IsDerivedFrom<plParticleVectorMathNode>() || pNodeType->IsDerivedFrom<plParticleDotNode>())
    return plMakeArrayPtr(s_VectorPair);

  if (pNodeType->IsDerivedFrom<plParticleVectorFunctionNode>() || pNodeType->IsDerivedFrom<plParticleVectorReduceNode>())
    return plMakeArrayPtr(s_VectorSingle);

  if (pNodeType->IsDerivedFrom<plParticleScaleVectorNode>())
    return plMakeArrayPtr(s_Scale);

  if (pNodeType->IsDerivedFrom<plParticleMakeVectorNode>())
    return plMakeArrayPtr(s_Make);

  if (pNodeType->IsDerivedFrom<plParticleSampleGradientNode>() || pNodeType->IsDerivedFrom<plParticleSampleCurveNode>())
    return plMakeArrayPtr(s_Sample);

  if (pNodeType->IsDerivedFrom<plParticleNoiseNode>())
    return plMakeArrayPtr(s_Noise);

  if (pNodeType->IsDerivedFrom<plParticleExpressionNode>())
    return plMakeArrayPtr(s_Expression);

  if (pNodeType->IsDerivedFrom<plParticleColorChannelNode>())
    return plMakeArrayPtr(s_ColorSingle);

  return {};
}

bool plParticlePropertyPin::IsWireable(const plAbstractProperty* pProp)
{
  return plParticleValueKind::FromProperty(pProp) != plParticleValueKind::None;
}

plParticleEffectNode::~plParticleEffectNode()
{
  for (auto pFactory : m_EventReactions)
    PL_DEFAULT_DELETE(pFactory);
}

plParticleSystemNode::~plParticleSystemNode()
{
  for (auto pFactory : m_EmitterFactories)
    PL_DEFAULT_DELETE(pFactory);
  for (auto pFactory : m_InitializerFactories)
    PL_DEFAULT_DELETE(pFactory);
  for (auto pFactory : m_BehaviorFactories)
    PL_DEFAULT_DELETE(pFactory);
  for (auto pFactory : m_TypeFactories)
    PL_DEFAULT_DELETE(pFactory);
  for (auto pRaiser : m_EventRaisers)
    PL_DEFAULT_DELETE(pRaiser);
}

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleGraphPinCategory, 1)
  PL_ENUM_CONSTANT(plParticleGraphPinCategory::Initializer),
  PL_ENUM_CONSTANT(plParticleGraphPinCategory::Behavior),
  PL_ENUM_CONSTANT(plParticleGraphPinCategory::Renderer),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleGraphPin, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEffectNode, 2, plRTTIDefaultAllocator<plParticleEffectNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("EventReactions", m_EventReactions)->AddFlags(plPropertyFlags::PointerOwner),
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
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effect", plColorScheme::DarkUI(plColorScheme::Blue)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleSystemNode, 2, plRTTIDefaultAllocator<plParticleSystemNode>)
{
  PL_BEGIN_PROPERTIES
  {
    // The contexts, in run order; array order is block order. Hidden from the property grid
    // because the system node draws them itself — the grid edits whichever block is selected.
    PL_ARRAY_MEMBER_PROPERTY("Emitters", m_EmitterFactories)->AddFlags(plPropertyFlags::PointerOwner)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("Initializers", m_InitializerFactories)->AddFlags(plPropertyFlags::PointerOwner)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("Behaviors", m_BehaviorFactories)->AddFlags(plPropertyFlags::PointerOwner)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("Types", m_TypeFactories)->AddFlags(plPropertyFlags::PointerOwner)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("EventRaisers", m_EventRaisers)->AddFlags(plPropertyFlags::PointerOwner)->AddAttributes(new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Visible", m_bVisible)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ENUM_MEMBER_PROPERTY("SimulationTarget", plParticleSimulationTarget, m_SimulationTarget),
    PL_MEMBER_PROPERTY("LifeTime", m_LifeTime)->AddAttributes(new plDefaultValueAttribute(plTime::MakeFromSeconds(2)), new plClampValueAttribute(plTime::MakeFromSeconds(0.0), plVariant())),
    PL_MEMBER_PROPERTY("LifeScaleParam", m_sLifeScaleParameter),
    // before 1.6; a document that sets it gets a Raise on Death block instead when it opens
    PL_MEMBER_PROPERTY("OnDeathEvent", m_sOnDeathEvent)->AddAttributes(new plHiddenAttribute()),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("System", plColorScheme::DarkUI(plColorScheme::Blue)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEventRaiserBlock, 1, plRTTINoAllocator)
{
  PL_BEGIN_PROPERTIES
  {
    // typed: a name here creates the event, which On Event blocks then pick from their list
    PL_MEMBER_PROPERTY("EventName", m_sEventName),
    PL_MEMBER_PROPERTY("Enabled", m_bEnabled)->AddAttributes(new plDefaultValueAttribute(true), new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("PinId", m_sPinId)->AddAttributes(new plHiddenAttribute()),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

plParticleEventRaiserBlock::~plParticleEventRaiserBlock() = default;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleRaiseOnDeathBlock, 1, plRTTIDefaultAllocator<plParticleRaiseOnDeathBlock>)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plTitleAttribute("Raise on Death"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleRaiseOnCollisionBlock, 1, plRTTIDefaultAllocator<plParticleRaiseOnCollisionBlock>)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plTitleAttribute("Raise on Collision"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleValueNode, 1, plRTTIDefaultAllocator<plParticleValueNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Value", m_fValue)->AddAttributes(new plDefaultValueAttribute(1.0)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Value", plColorScheme::DarkUI(plColorScheme::Blue)),
    new plTitleAttribute("Float"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleColorNode, 1, plRTTIDefaultAllocator<plParticleColorNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Value", m_Value)->AddAttributes(new plDefaultValueAttribute(plColor::White), new plExposeColorAlphaAttribute()),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Value", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Color {Name}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_ABSTRACT_DYNAMIC_REFLECTED_TYPE(plParticleAssetNode, 1)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
  }
  PL_END_PROPERTIES;
}
PL_END_ABSTRACT_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTextureNode, 1, plRTTIDefaultAllocator<plParticleTextureNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Texture", m_sTexture)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D")),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Value", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Texture {Name}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleGradientNode, 1, plRTTIDefaultAllocator<plParticleGradientNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Gradient", m_sGradient)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Data_Gradient")),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Value", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Gradient {Name}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleCurveNode, 1, plRTTIDefaultAllocator<plParticleCurveNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Curve", m_sCurve)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Data_Curve")),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Value", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Curve {Name}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleMathOp, 1)
  PL_ENUM_CONSTANT(plParticleMathOp::Add), PL_ENUM_CONSTANT(plParticleMathOp::Subtract),
  PL_ENUM_CONSTANT(plParticleMathOp::Multiply), PL_ENUM_CONSTANT(plParticleMathOp::Divide),
  PL_ENUM_CONSTANT(plParticleMathOp::Min), PL_ENUM_CONSTANT(plParticleMathOp::Max),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleCompareOp, 1)
  PL_ENUM_CONSTANT(plParticleCompareOp::Less), PL_ENUM_CONSTANT(plParticleCompareOp::LessOrEqual),
  PL_ENUM_CONSTANT(plParticleCompareOp::Greater), PL_ENUM_CONSTANT(plParticleCompareOp::GreaterOrEqual),
  PL_ENUM_CONSTANT(plParticleCompareOp::Equal), PL_ENUM_CONSTANT(plParticleCompareOp::NotEqual),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleMathNode, 1, plRTTIDefaultAllocator<plParticleMathNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Operation", plParticleMathOp, m_Operation),
    PL_MEMBER_PROPERTY("A", m_fA)->AddAttributes(new plDefaultValueAttribute(1.0)),
    PL_MEMBER_PROPERTY("B", m_fB)->AddAttributes(new plDefaultValueAttribute(1.0)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Math", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("{Operation}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleCompareNode, 1, plRTTIDefaultAllocator<plParticleCompareNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Operation", plParticleCompareOp, m_Operation),
    PL_MEMBER_PROPERTY("A", m_fA),
    PL_MEMBER_PROPERTY("B", m_fB),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Logic", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Compare {Operation}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBranchNode, 1, plRTTIDefaultAllocator<plParticleBranchNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("True", m_fTrue)->AddAttributes(new plDefaultValueAttribute(1.0)),
    PL_MEMBER_PROPERTY("False", m_fFalse),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Logic", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Branch"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleParameterNode, 1, plRTTIDefaultAllocator<plParticleParameterNode>)
{
  PL_BEGIN_PROPERTIES
  {
    // The document keeps this enum filled from the effect's own parameter maps.
    PL_MEMBER_PROPERTY("Name", m_sName)->AddAttributes(new plDynamicStringEnumAttribute("ParticleParameterNamesEnum")),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Parameters", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Effect Parameter"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBoolNode, 1, plRTTIDefaultAllocator<plParticleBoolNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Value", m_bValue)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Value", plColorScheme::DarkUI(plColorScheme::Red)),
    new plTitleAttribute("Bool"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleToBoolNode, 1, plRTTIDefaultAllocator<plParticleToBoolNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Value", m_fValue),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Convert", plColorScheme::DarkUI(plColorScheme::Red)),
    new plTitleAttribute("To Bool"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleColorParameterNode, 1, plRTTIDefaultAllocator<plParticleColorParameterNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName)->AddAttributes(new plDynamicStringEnumAttribute("ParticleParameterNamesEnum")),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Parameters", plColorScheme::DarkUI(plColorScheme::Yellow)),
    new plTitleAttribute("Effect Color Parameter"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBoolParameterNode, 1, plRTTIDefaultAllocator<plParticleBoolParameterNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName)->AddAttributes(new plDynamicStringEnumAttribute("ParticleParameterNamesEnum")),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Parameters", plColorScheme::DarkUI(plColorScheme::Red)),
    new plTitleAttribute("Effect Bool Parameter"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleSystemValue, 1)
  PL_ENUM_CONSTANT(plParticleSystemValue::Time), PL_ENUM_CONSTANT(plParticleSystemValue::DeltaTime),
  PL_ENUM_CONSTANT(plParticleSystemValue::CameraDistance), PL_ENUM_CONSTANT(plParticleSystemValue::ParticleCount),
  PL_ENUM_CONSTANT(plParticleSystemValue::SpawnScale),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleSystemVector, 1)
  PL_ENUM_CONSTANT(plParticleSystemVector::EffectPosition), PL_ENUM_CONSTANT(plParticleSystemVector::EffectForward),
  PL_ENUM_CONSTANT(plParticleSystemVector::EffectUp), PL_ENUM_CONSTANT(plParticleSystemVector::EffectVelocity),
  PL_ENUM_CONSTANT(plParticleSystemVector::CameraPosition),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleFunctionOp, 1)
  PL_ENUM_CONSTANT(plParticleFunctionOp::Sine), PL_ENUM_CONSTANT(plParticleFunctionOp::Cosine),
  PL_ENUM_CONSTANT(plParticleFunctionOp::Abs), PL_ENUM_CONSTANT(plParticleFunctionOp::Floor),
  PL_ENUM_CONSTANT(plParticleFunctionOp::Ceil), PL_ENUM_CONSTANT(plParticleFunctionOp::Frac),
  PL_ENUM_CONSTANT(plParticleFunctionOp::Sqrt), PL_ENUM_CONSTANT(plParticleFunctionOp::Saturate),
  PL_ENUM_CONSTANT(plParticleFunctionOp::Negate), PL_ENUM_CONSTANT(plParticleFunctionOp::OneMinus),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleVectorMathOp, 1)
  PL_ENUM_CONSTANT(plParticleVectorMathOp::Add), PL_ENUM_CONSTANT(plParticleVectorMathOp::Subtract),
  PL_ENUM_CONSTANT(plParticleVectorMathOp::Multiply), PL_ENUM_CONSTANT(plParticleVectorMathOp::Cross),
  PL_ENUM_CONSTANT(plParticleVectorMathOp::Min), PL_ENUM_CONSTANT(plParticleVectorMathOp::Max),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleVectorFunctionOp, 1)
  PL_ENUM_CONSTANT(plParticleVectorFunctionOp::Normalize), PL_ENUM_CONSTANT(plParticleVectorFunctionOp::Negate),
  PL_ENUM_CONSTANT(plParticleVectorFunctionOp::Abs),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleVectorReduceOp, 1)
  PL_ENUM_CONSTANT(plParticleVectorReduceOp::Length), PL_ENUM_CONSTANT(plParticleVectorReduceOp::X),
  PL_ENUM_CONSTANT(plParticleVectorReduceOp::Y), PL_ENUM_CONSTANT(plParticleVectorReduceOp::Z),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleSystemValueNode, 1, plRTTIDefaultAllocator<plParticleSystemValueNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Input", plParticleSystemValue, m_Input),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("System", plColorScheme::DarkUI(plColorScheme::Blue)),
    new plTitleAttribute("{Input}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleSystemVectorNode, 1, plRTTIDefaultAllocator<plParticleSystemVectorNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Input", plParticleSystemVector, m_Input),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("System", plColorScheme::DarkUI(plColorScheme::Teal)),
    new plTitleAttribute("{Input}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleVectorNode, 1, plRTTIDefaultAllocator<plParticleVectorNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Value", m_vValue)->AddAttributes(new plDefaultValueAttribute(plVec3(0, 0, 1))),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Value", plColorScheme::DarkUI(plColorScheme::Teal)),
    new plTitleAttribute("Vector"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleFunctionNode, 1, plRTTIDefaultAllocator<plParticleFunctionNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Operation", plParticleFunctionOp, m_Operation),
    PL_MEMBER_PROPERTY("Value", m_fValue),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Math", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("{Operation}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleRemapNode, 1, plRTTIDefaultAllocator<plParticleRemapNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Value", m_fValue),
    PL_MEMBER_PROPERTY("InMin", m_fInMin),
    PL_MEMBER_PROPERTY("InMax", m_fInMax)->AddAttributes(new plDefaultValueAttribute(1.0)),
    PL_MEMBER_PROPERTY("OutMin", m_fOutMin),
    PL_MEMBER_PROPERTY("OutMax", m_fOutMax)->AddAttributes(new plDefaultValueAttribute(1.0)),
    PL_MEMBER_PROPERTY("Clamp", m_bClamp)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Math", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Remap"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleLerpNode, 1, plRTTIDefaultAllocator<plParticleLerpNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("A", m_fA),
    PL_MEMBER_PROPERTY("B", m_fB)->AddAttributes(new plDefaultValueAttribute(1.0)),
    PL_MEMBER_PROPERTY("T", m_fT)->AddAttributes(new plDefaultValueAttribute(0.5)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Math", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Lerp"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleClampNode, 1, plRTTIDefaultAllocator<plParticleClampNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Value", m_fValue),
    PL_MEMBER_PROPERTY("Min", m_fMin),
    PL_MEMBER_PROPERTY("Max", m_fMax)->AddAttributes(new plDefaultValueAttribute(1.0)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Math", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Clamp"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleVectorMathNode, 1, plRTTIDefaultAllocator<plParticleVectorMathNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Operation", plParticleVectorMathOp, m_Operation),
    PL_MEMBER_PROPERTY("A", m_vA),
    PL_MEMBER_PROPERTY("B", m_vB),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Vector", plColorScheme::DarkUI(plColorScheme::Teal)),
    new plTitleAttribute("Vector {Operation}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleVectorFunctionNode, 1, plRTTIDefaultAllocator<plParticleVectorFunctionNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Operation", plParticleVectorFunctionOp, m_Operation),
    PL_MEMBER_PROPERTY("Value", m_vValue)->AddAttributes(new plDefaultValueAttribute(plVec3(0, 0, 1))),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Vector", plColorScheme::DarkUI(plColorScheme::Teal)),
    new plTitleAttribute("Vector {Operation}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleVectorReduceNode, 1, plRTTIDefaultAllocator<plParticleVectorReduceNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Operation", plParticleVectorReduceOp, m_Operation),
    PL_MEMBER_PROPERTY("Value", m_vValue),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Vector", plColorScheme::DarkUI(plColorScheme::Blue)),
    new plTitleAttribute("Vector {Operation}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleColorChannelOp, 1)
  PL_ENUM_CONSTANT(plParticleColorChannelOp::R), PL_ENUM_CONSTANT(plParticleColorChannelOp::G),
  PL_ENUM_CONSTANT(plParticleColorChannelOp::B), PL_ENUM_CONSTANT(plParticleColorChannelOp::A),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleColorChannelNode, 1, plRTTIDefaultAllocator<plParticleColorChannelNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Operation", plParticleColorChannelOp, m_Operation),
    PL_MEMBER_PROPERTY("Value", m_Value)->AddAttributes(new plDefaultValueAttribute(plColor::White), new plExposeColorAlphaAttribute()),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Color", plColorScheme::DarkUI(plColorScheme::Blue)),
    new plTitleAttribute("Color {Operation}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleExpressionNode, 1, plRTTIDefaultAllocator<plParticleExpressionNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Expression", m_sExpression)->AddAttributes(new plDefaultValueAttribute(plStringView("a"))),
    PL_MEMBER_PROPERTY("A", m_fA),
    PL_MEMBER_PROPERTY("B", m_fB),
    PL_MEMBER_PROPERTY("C", m_fC),
    PL_MEMBER_PROPERTY("D", m_fD),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Math", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Expression"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleDotNode, 1, plRTTIDefaultAllocator<plParticleDotNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("A", m_vA),
    PL_MEMBER_PROPERTY("B", m_vB),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Vector", plColorScheme::DarkUI(plColorScheme::Blue)),
    new plTitleAttribute("Dot"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleScaleVectorNode, 1, plRTTIDefaultAllocator<plParticleScaleVectorNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Vector", m_vVector)->AddAttributes(new plDefaultValueAttribute(plVec3(0, 0, 1))),
    PL_MEMBER_PROPERTY("Scale", m_fScale)->AddAttributes(new plDefaultValueAttribute(1.0)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Vector", plColorScheme::DarkUI(plColorScheme::Teal)),
    new plTitleAttribute("Scale Vector"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleMakeVectorNode, 1, plRTTIDefaultAllocator<plParticleMakeVectorNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("X", m_fX),
    PL_MEMBER_PROPERTY("Y", m_fY),
    PL_MEMBER_PROPERTY("Z", m_fZ),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Vector", plColorScheme::DarkUI(plColorScheme::Teal)),
    new plTitleAttribute("Make Vector"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleSampleGradientNode, 1, plRTTIDefaultAllocator<plParticleSampleGradientNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Gradient", m_sGradient)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Data_Gradient")),
    PL_MEMBER_PROPERTY("T", m_fT),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Math", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Sample Gradient"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleSampleCurveNode, 1, plRTTIDefaultAllocator<plParticleSampleCurveNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Curve", m_sCurve)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Data_Curve")),
    PL_MEMBER_PROPERTY("T", m_fT),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Math", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Sample Curve"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleNoiseNode, 1, plRTTIDefaultAllocator<plParticleNoiseNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Position", m_vPosition),
    PL_MEMBER_PROPERTY("Frequency", m_fFrequency)->AddAttributes(new plDefaultValueAttribute(1.0)),
    PL_MEMBER_PROPERTY("Octaves", m_uiOctaves)->AddAttributes(new plDefaultValueAttribute(1), new plClampValueAttribute(1, 8)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Math", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Noise"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleInputValue, 1)
  PL_ENUM_CONSTANT(plParticleInputValue::Speed), PL_ENUM_CONSTANT(plParticleInputValue::Age),
  PL_ENUM_CONSTANT(plParticleInputValue::NormalizedAge), PL_ENUM_CONSTANT(plParticleInputValue::Lifetime),
  PL_ENUM_CONSTANT(plParticleInputValue::Size), PL_ENUM_CONSTANT(plParticleInputValue::RotationSpeed),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleInputVector, 1)
  PL_ENUM_CONSTANT(plParticleInputVector::Position), PL_ENUM_CONSTANT(plParticleInputVector::Velocity),
  PL_ENUM_CONSTANT(plParticleInputVector::Axis),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInputValueNode, 1, plRTTIDefaultAllocator<plParticleInputValueNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Input", plParticleInputValue, m_Input),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Particle", plColorScheme::DarkUI(plColorScheme::Lime)),
    new plTitleAttribute("{Input}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInputVectorNode, 1, plRTTIDefaultAllocator<plParticleInputVectorNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Input", plParticleInputVector, m_Input),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Particle", plColorScheme::DarkUI(plColorScheme::Lime)),
    new plTitleAttribute("{Input}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInputColorNode, 1, plRTTIDefaultAllocator<plParticleInputColorNode>)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Particle", plColorScheme::DarkUI(plColorScheme::Lime)),
    new plTitleAttribute("Particle Color"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleRandomNode, 1, plRTTIDefaultAllocator<plParticleRandomNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Seed", m_iSeed),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Particle", plColorScheme::DarkUI(plColorScheme::Lime)),
    new plTitleAttribute("Random"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_ABSTRACT_DYNAMIC_REFLECTED_TYPE(plParticleCustomAttributeNode, 1)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
  }
  PL_END_PROPERTIES;
}
PL_END_ABSTRACT_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleCustomFloatNode, 1, plRTTIDefaultAllocator<plParticleCustomFloatNode>)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Particle", plColorScheme::DarkUI(plColorScheme::Lime)),
    new plTitleAttribute("Custom Float"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleCustomVectorNode, 1, plRTTIDefaultAllocator<plParticleCustomVectorNode>)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Particle", plColorScheme::DarkUI(plColorScheme::Lime)),
    new plTitleAttribute("Custom Vector"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleCustomColorNode, 1, plRTTIDefaultAllocator<plParticleCustomColorNode>)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Particle", plColorScheme::DarkUI(plColorScheme::Lime)),
    new plTitleAttribute("Custom Color"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleEventInput, 1)
  PL_ENUM_CONSTANT(plParticleEventInput::EventPosition), PL_ENUM_CONSTANT(plParticleEventInput::EventVelocity),
  PL_ENUM_CONSTANT(plParticleEventInput::EventNormal),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEventInputNode, 1, plRTTIDefaultAllocator<plParticleEventInputNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Input", plParticleEventInput, m_Input),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Event", plColorScheme::DarkUI(plColorScheme::Orange)),
    new plTitleAttribute("{Input}"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBlackboardNode, 1, plRTTINoAllocator)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Entry", m_sEntry),
    PL_MEMBER_PROPERTY("Blackboard", m_sBlackboard)->AddAttributes(new plDefaultValueAttribute("Global")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBlackboardEntryNode, 1, plRTTIDefaultAllocator<plParticleBlackboardEntryNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Default", m_fDefault),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Parameters", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Blackboard Entry"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBlackboardVectorNode, 1, plRTTIDefaultAllocator<plParticleBlackboardVectorNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Default", m_vDefault),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Parameters", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Blackboard Vector"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBlackboardColorNode, 1, plRTTIDefaultAllocator<plParticleBlackboardColorNode>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Default", m_Default)->AddAttributes(new plDefaultValueAttribute(plColor::White)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Parameters", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Blackboard Color"),
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
    // retired by the context rework: contexts merge same-category blocks by themselves.
    // Still reflected so pre-rework documents load and can be migrated.
    new plHiddenAttribute(),
    new plCategoryAttribute("Mix", plColorScheme::DarkUI(plColorScheme::Teal)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on