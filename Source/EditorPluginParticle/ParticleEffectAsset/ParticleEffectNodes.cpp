#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>

static const plParticleContextInfo s_ContextInfo[plParticleContext::Count] = {
  {plParticleContext::Spawn, "Emitters", "Spawn", []() { return plGetStaticRTTI<plParticleEmitterFactory>(); }},
  {plParticleContext::Initialize, "Initializers", "Initialize", []() { return plGetStaticRTTI<plParticleInitializerFactory>(); }},
  {plParticleContext::Update, "Behaviors", "Update", []() { return plGetStaticRTTI<plParticleBehaviorFactory>(); }},
  {plParticleContext::Output, "Types", "Output", []() { return plGetStaticRTTI<plParticleTypeFactory>(); }},
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

plString plParticlePropertyPin::Make(const plUuid& block, plStringView sProperty)
{
  plStringBuilder sGuid, sPin;
  plConversionUtils::ToString(block, sGuid);
  sPin.Set(sGuid, "|", sProperty);
  return sPin;
}

bool plParticlePropertyPin::Parse(plStringView sPinName, plUuid& out_block, plStringBuilder& out_sProperty)
{
  const char* szSeparator = sPinName.FindSubString("|");
  if (szSeparator == nullptr)
    return false;

  const plStringBuilder sGuid(plStringView(sPinName.GetStartPointer(), szSeparator));
  out_block = plConversionUtils::ConvertStringToUuid(sGuid);
  out_sProperty = plStringView(szSeparator + 1, sPinName.GetEndPointer());

  return out_block.IsValid() && !out_sProperty.IsEmpty();
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

  // Computed nodes have no stored value; the evaluator produces it from their inputs.
  if (pNodeType->IsDerivedFrom<plParticleMathNode>() || pNodeType->IsDerivedFrom<plParticleBranchNode>() ||
      pNodeType->IsDerivedFrom<plParticleParameterNode>())
  {
    out_sValueProperty = {};
    return Number;
  }
  if (pNodeType->IsDerivedFrom<plParticleCompareNode>() || pNodeType->IsDerivedFrom<plParticleToBoolNode>() ||
      pNodeType->IsDerivedFrom<plParticleBoolParameterNode>())
  {
    out_sValueProperty = {};
    return Bool;
  }
  if (pNodeType->IsDerivedFrom<plParticleColorParameterNode>())
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
    {
      // only Branch takes a bool, and only on its condition
      return (sPinName == "Condition") ? Bool : Number;
    }
  }

  return None;
}

plArrayPtr<const plParticleOperatorInputs::Input> plParticleOperatorInputs::Get(const plRTTI* pNodeType)
{
  static const Input s_Math[] = {{"A", "A"}, {"B", "B"}};
  static const Input s_Branch[] = {{"Condition", nullptr}, {"True", "True"}, {"False", "False"}};
  static const Input s_Single[] = {{"Value", "Value"}};

  if (pNodeType == nullptr)
    return {};

  if (pNodeType->IsDerivedFrom<plParticleToBoolNode>())
    return plMakeArrayPtr(s_Single);

  if (pNodeType->IsDerivedFrom<plParticleMathNode>() || pNodeType->IsDerivedFrom<plParticleCompareNode>())
    return plMakeArrayPtr(s_Math);

  if (pNodeType->IsDerivedFrom<plParticleBranchNode>())
    return plMakeArrayPtr(s_Branch);

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
    // The four contexts, in run order; array order is block order. Hidden from the property grid
    // because the system node draws them itself — the grid edits whichever block is selected.
    PL_ARRAY_MEMBER_PROPERTY("Emitters", m_EmitterFactories)->AddFlags(plPropertyFlags::PointerOwner)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("Initializers", m_InitializerFactories)->AddFlags(plPropertyFlags::PointerOwner)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("Behaviors", m_BehaviorFactories)->AddFlags(plPropertyFlags::PointerOwner)->AddAttributes(new plHiddenAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("Types", m_TypeFactories)->AddFlags(plPropertyFlags::PointerOwner)->AddAttributes(new plHiddenAttribute()),
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Visible", m_bVisible)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ENUM_MEMBER_PROPERTY("SimulationTarget", plParticleSimulationTarget, m_SimulationTarget),
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
    new plCategoryAttribute("Blackboard", plColorScheme::DarkUI(plColorScheme::Gray)),
    new plTitleAttribute("Blackboard Float"),
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
    new plCategoryAttribute("Blackboard", plColorScheme::DarkUI(plColorScheme::Yellow)),
    new plTitleAttribute("Blackboard Color"),
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
    new plCategoryAttribute("Blackboard", plColorScheme::DarkUI(plColorScheme::Red)),
    new plTitleAttribute("Blackboard Bool"),
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