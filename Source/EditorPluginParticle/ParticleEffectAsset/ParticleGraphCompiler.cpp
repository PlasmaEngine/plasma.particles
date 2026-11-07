#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphCompiler.h>
#include <Foundation/CodeUtils/Expression/ExpressionCompiler.h>
#include <Foundation/CodeUtils/Expression/ExpressionParser.h>
#include <ParticlePlugin/System/ParticleFrameProgram.h>

using NodeType = plExpressionAST::NodeType;
using DataType = plExpressionAST::DataType;

namespace
{
  /// Keeps the first error a parse reports, without the "(line,column): " the parser puts in front.
  class FirstErrorLog : public plLogInterface
  {
  public:
    virtual void HandleLogMessage(const plLoggingEventData& le) override
    {
      if (le.m_EventType != plLogMsgType::ErrorMsg || !m_sError.IsEmpty())
        return;

      const char* szMessage = le.m_sText.FindSubString("): ");
      m_sError = szMessage != nullptr ? plStringView(szMessage + 3, le.m_sText.GetEndPointer()) : le.m_sText;
    }

    plString m_sError;
  };

  constexpr plExpressionAST::VectorComponent::Enum s_ColorChannels[] = {
    plExpressionAST::VectorComponent::R, plExpressionAST::VectorComponent::G, plExpressionAST::VectorComponent::B, plExpressionAST::VectorComponent::A};

  plExpressionAST::VectorComponent::Enum GetColorChannel(plInt64 iOperation)
  {
    return (iOperation >= 0 && iOperation < 4) ? s_ColorChannels[iOperation] : plExpressionAST::VectorComponent::A;
  }

  plString FormatNumber(double fValue)
  {
    plStringBuilder s;
    s.SetFormat("{}", plArgF(fValue, 2));

    // "60.00" reads as "60", "0.50" as "0.5"
    if (s.FindSubString(".") != nullptr)
    {
      while (s.EndsWith("0"))
        s.Shrink(0, 1);
      if (s.EndsWith("."))
        s.Shrink(0, 1);
    }

    return s;
  }

  const char* GetMathSymbol(plInt64 iOperation)
  {
    switch (iOperation)
    {
      case plParticleMathOp::Add:
        return "+";
      case plParticleMathOp::Subtract:
        return "-";
      case plParticleMathOp::Divide:
        return "/";
      case plParticleMathOp::Min:
        return "min";
      case plParticleMathOp::Max:
        return "max";
      default:
        return "x";
    }
  }

  plString GetEnumName(const plRTTI* pEnumType, plInt64 iValue)
  {
    plStringBuilder s;
    plReflectionUtils::EnumerationToString(pEnumType, iValue, s, plReflectionUtils::EnumConversionMode::ValueNameOnly);
    return s;
  }

  /// What a per-frame input starts at, which is also what a constant fold of it gives.
  plVariant GetSystemInputStartValue(const plRTTI* pType, plInt64 iInput)
  {
    if (pType->IsDerivedFrom<plParticleSystemValueNode>())
    {
      switch (iInput)
      {
        case plParticleSystemValue::DeltaTime:
          return plVariant(1.0 / 60.0);
        case plParticleSystemValue::SpawnScale:
          return plVariant(1.0);
        default:
          return plVariant(0.0);
      }
    }

    switch (iInput)
    {
      case plParticleSystemVector::EffectForward:
        return plVariant(plVec3(1, 0, 0));
      case plParticleSystemVector::EffectUp:
        return plVariant(plVec3(0, 0, 1));
      default:
        return plVariant(plVec3::MakeZero());
    }
  }

  const char* GetSystemInputName(const plRTTI* pType, plInt64 iInput)
  {
    if (pType->IsDerivedFrom<plParticleSystemValueNode>())
    {
      switch (iInput)
      {
        case plParticleSystemValue::DeltaTime:
          return plParticleFrameInput::DeltaTime;
        case plParticleSystemValue::CameraDistance:
          return plParticleFrameInput::CameraDistance;
        case plParticleSystemValue::ParticleCount:
          return plParticleFrameInput::ParticleCount;
        case plParticleSystemValue::SpawnScale:
          return plParticleFrameInput::SpawnScale;
        default:
          return plParticleFrameInput::Time;
      }
    }

    switch (iInput)
    {
      case plParticleSystemVector::EffectForward:
        return plParticleFrameInput::EffectForward;
      case plParticleSystemVector::EffectUp:
        return plParticleFrameInput::EffectUp;
      case plParticleSystemVector::EffectVelocity:
        return plParticleFrameInput::EffectVelocity;
      case plParticleSystemVector::CameraPosition:
        return plParticleFrameInput::CameraPosition;
      default:
        return plParticleFrameInput::EffectPosition;
    }
  }

  bool IsParameterNode(const plRTTI* pType)
  {
    return pType->IsDerivedFrom<plParticleParameterNode>() || pType->IsDerivedFrom<plParticleColorParameterNode>() || pType->IsDerivedFrom<plParticleBoolParameterNode>();
  }

  double ToDouble(const plVariant& value)
  {
    return value.CanConvertTo<double>() ? value.ConvertTo<double>() : 0.0;
  }

  plVec3 ToVec3(const plVariant& value)
  {
    return value.IsA<plVec3>() ? value.Get<plVec3>() : plVec3::MakeZero();
  }
} // namespace

plParticleGraphCompiler::plParticleGraphCompiler(const plVisualGraphObjectManager* pManager, const plDocumentObject* pEffectNode)
  : m_pManager(pManager)
  , m_pEffectNode(pEffectNode)
{
}

plParticleGraphCompiler::~plParticleGraphCompiler() = default;

const plDocumentObject* plParticleGraphCompiler::GetInputSource(const plDocumentObject* pNode, plStringView sPinName) const
{
  const plVisualGraphPin* pPin = m_pManager->GetInputPinByName(pNode, sPinName);
  if (pPin == nullptr)
    return nullptr;

  for (const plVisualGraphConnection* pConnection : m_pManager->GetConnections(*pPin))
    return pConnection->GetSourcePin().GetParent();

  return nullptr;
}

plParticleTier plParticleGraphCompiler::GetTier(const plDocumentObject* pNode)
{
  if (pNode == nullptr)
    return plParticleTier::Constant;

  if (const plParticleTier* pTier = m_Tiers.GetValue(pNode))
    return *pTier;

  // a cycle reads as constant while it is being resolved; it folds to nothing anyway
  m_Tiers[pNode] = plParticleTier::Constant;

  const plRTTI* pType = pNode->GetTypeAccessor().GetType();
  plParticleTier tier = plParticleTier::Constant;

  if (IsParticleInputNode(pType))
  {
    tier = plParticleTier::Particle;
  }
  else if (IsParameterNode(pType) || pType->IsDerivedFrom<plParticleSystemValueNode>() || pType->IsDerivedFrom<plParticleSystemVectorNode>() ||
           pType->IsDerivedFrom<plParticleBlackboardNode>())
  {
    tier = plParticleTier::Frame;
  }
  else
  {
    if (IsRuntimeSampledNode(pType))
      tier = plParticleTier::Frame;

    for (const auto& input : plParticleOperatorInputs::Get(pType))
    {
      const plParticleTier inputTier = GetTier(GetInputSource(pNode, input.m_szPin));
      tier = plMath::Max(tier, inputTier);
    }
  }

  m_Tiers[pNode] = tier;
  return tier;
}

plVariant plParticleGraphCompiler::Fold(const plDocumentObject* pNode)
{
  plSet<const plDocumentObject*> visited;
  return FoldInternal(pNode, visited);
}

plVariant plParticleGraphCompiler::FoldInput(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plSet<const plDocumentObject*>& ref_visited)
{
  if (const plDocumentObject* pSource = GetInputSource(pNode, szPin))
  {
    const plVariant value = FoldInternal(pSource, ref_visited);
    if (value.IsValid())
      return value;
  }

  return szLiteral != nullptr ? pNode->GetTypeAccessor().GetValue(szLiteral) : plVariant();
}

plVariant plParticleGraphCompiler::FoldInternal(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited)
{
  if (pNode == nullptr || ref_visited.Contains(pNode))
    return plVariant();

  ref_visited.Insert(pNode);
  PL_SCOPE_EXIT(ref_visited.Remove(pNode));

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  // a stored constant: texture, gradient, curve, colour, vector or plain number
  plStringView sValueProperty;
  if (plParticleValueKind::FromNode(pType, sValueProperty) == plParticleValueKind::None)
    return plVariant();

  if (!sValueProperty.IsEmpty())
    return accessor.GetValue(sValueProperty);

  auto number = [&](const char* szPin) -> double
  { return ToDouble(FoldInput(pNode, szPin, szPin, ref_visited)); };

  auto vector = [&](const char* szPin) -> plVec3
  { return ToVec3(FoldInput(pNode, szPin, szPin, ref_visited)); };

  const plInt64 iOperation = accessor.GetValue("Operation").IsValid() ? accessor.GetValue("Operation").ConvertTo<plInt64>() : 0;

  if (pType->IsDerivedFrom<plParticleToBoolNode>())
    return plVariant(number("Value") != 0.0);

  // the game sets the entry; its stand-in is the node's default
  if (pType->IsDerivedFrom<plParticleBlackboardNode>())
    return accessor.GetValue("Default");

  if (IsParameterNode(pType))
  {
    // the parameter's default, which lives on the effect node
    if (m_pEffectNode == nullptr)
      return plVariant();

    const plString sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      return plVariant();

    if (pType->IsDerivedFrom<plParticleColorParameterNode>())
      return m_pEffectNode->GetTypeAccessor().GetValue("ColorParameters", plVariant(sName));

    const plVariant value = m_pEffectNode->GetTypeAccessor().GetValue("FloatParameters", plVariant(sName));
    if (!value.IsValid())
      return plVariant();

    if (pType->IsDerivedFrom<plParticleBoolParameterNode>())
      return plVariant(value.ConvertTo<double>() != 0.0);

    return plVariant(value.ConvertTo<double>());
  }

  if (pType->IsDerivedFrom<plParticleSystemValueNode>() || pType->IsDerivedFrom<plParticleSystemVectorNode>())
    return GetSystemInputStartValue(pType, accessor.GetValue("Input").ConvertTo<plInt64>());

  // per-particle inputs have no single value; folding gives a neutral one, which only shows where
  // a per-particle wire is not evaluated per particle
  if (pType->IsDerivedFrom<plParticleInputVectorNode>() || pType->IsDerivedFrom<plParticleCustomVectorNode>() || pType->IsDerivedFrom<plParticleEventInputNode>())
    return plVariant(plVec3::MakeZero());
  if (pType->IsDerivedFrom<plParticleInputColorNode>() || pType->IsDerivedFrom<plParticleCustomColorNode>())
    return plVariant(plColor::White);
  if (pType->IsDerivedFrom<plParticleRandomNode>())
    return plVariant(0.5);
  if (pType->IsDerivedFrom<plParticleInputValueNode>())
  {
    const plInt64 iInput = accessor.GetValue("Input").ConvertTo<plInt64>();
    return plVariant(iInput == plParticleInputValue::Lifetime ? m_fSystemLifetime : (iInput == plParticleInputValue::Size ? 1.0 : 0.0));
  }
  if (pType->IsDerivedFrom<plParticleCustomFloatNode>())
    return plVariant(0.0);

  // sampled by the runtime; the editor only has a neutral start value for them
  if (pType->IsDerivedFrom<plParticleSampleGradientNode>())
    return plVariant(plColor::White);
  if (pType->IsDerivedFrom<plParticleSampleCurveNode>())
    return plVariant(0.0);
  if (pType->IsDerivedFrom<plParticleNoiseNode>())
    return plVariant(0.5);

  if (pType->IsDerivedFrom<plParticleMathNode>())
  {
    const double a = number("A");
    const double b = number("B");

    switch (iOperation)
    {
      case plParticleMathOp::Add:
        return plVariant(a + b);
      case plParticleMathOp::Subtract:
        return plVariant(a - b);
      case plParticleMathOp::Divide:
        return plVariant(b == 0.0 ? 0.0 : a / b);
      case plParticleMathOp::Min:
        return plVariant(plMath::Min(a, b));
      case plParticleMathOp::Max:
        return plVariant(plMath::Max(a, b));
      default:
        return plVariant(a * b);
    }
  }

  if (pType->IsDerivedFrom<plParticleCompareNode>())
  {
    const double a = number("A");
    const double b = number("B");

    switch (iOperation)
    {
      case plParticleCompareOp::Less:
        return plVariant(a < b);
      case plParticleCompareOp::LessOrEqual:
        return plVariant(a <= b);
      case plParticleCompareOp::GreaterOrEqual:
        return plVariant(a >= b);
      case plParticleCompareOp::Equal:
        return plVariant(a == b);
      case plParticleCompareOp::NotEqual:
        return plVariant(a != b);
      default:
        return plVariant(a > b);
    }
  }

  if (pType->IsDerivedFrom<plParticleBranchNode>())
  {
    const plVariant condition = FoldInput(pNode, "Condition", nullptr, ref_visited);
    const bool bTaken = condition.IsValid() && condition.ConvertTo<bool>();

    return bTaken ? FoldInput(pNode, "True", "True", ref_visited) : FoldInput(pNode, "False", "False", ref_visited);
  }

  if (pType->IsDerivedFrom<plParticleFunctionNode>())
  {
    const double x = number("Value");

    switch (iOperation)
    {
      case plParticleFunctionOp::Cosine:
        return plVariant(plMath::Cos(plAngle::MakeFromRadian((float)x)));
      case plParticleFunctionOp::Abs:
        return plVariant(plMath::Abs(x));
      case plParticleFunctionOp::Floor:
        return plVariant(plMath::Floor(x));
      case plParticleFunctionOp::Ceil:
        return plVariant(plMath::Ceil(x));
      case plParticleFunctionOp::Frac:
        return plVariant(x - plMath::Floor(x));
      case plParticleFunctionOp::Sqrt:
        return plVariant(plMath::Sqrt(plMath::Max(x, 0.0)));
      case plParticleFunctionOp::Saturate:
        return plVariant(plMath::Saturate(x));
      case plParticleFunctionOp::Negate:
        return plVariant(-x);
      case plParticleFunctionOp::OneMinus:
        return plVariant(1.0 - x);
      default:
        return plVariant(plMath::Sin(plAngle::MakeFromRadian((float)x)));
    }
  }

  if (pType->IsDerivedFrom<plParticleRemapNode>())
  {
    const double x = number("Value");
    const double inMin = accessor.GetValue("InMin").ConvertTo<double>();
    const double inMax = accessor.GetValue("InMax").ConvertTo<double>();
    const double outMin = accessor.GetValue("OutMin").ConvertTo<double>();
    const double outMax = accessor.GetValue("OutMax").ConvertTo<double>();

    double t = (inMax != inMin) ? (x - inMin) / (inMax - inMin) : 0.0;
    if (accessor.GetValue("Clamp").ConvertTo<bool>())
      t = plMath::Saturate(t);

    return plVariant(outMin + (outMax - outMin) * t);
  }

  if (pType->IsDerivedFrom<plParticleLerpNode>())
    return plVariant(plMath::Lerp(number("A"), number("B"), number("T")));

  if (pType->IsDerivedFrom<plParticleClampNode>())
    return plVariant(plMath::Clamp(number("Value"), number("Min"), number("Max")));

  if (pType->IsDerivedFrom<plParticleVectorMathNode>())
  {
    const plVec3 a = vector("A");
    const plVec3 b = vector("B");

    switch (iOperation)
    {
      case plParticleVectorMathOp::Subtract:
        return plVariant(a - b);
      case plParticleVectorMathOp::Multiply:
        return plVariant(a.CompMul(b));
      case plParticleVectorMathOp::Cross:
        return plVariant(a.CrossRH(b));
      case plParticleVectorMathOp::Min:
        return plVariant(a.CompMin(b));
      case plParticleVectorMathOp::Max:
        return plVariant(a.CompMax(b));
      default:
        return plVariant(a + b);
    }
  }

  if (pType->IsDerivedFrom<plParticleVectorFunctionNode>())
  {
    plVec3 v = vector("Value");

    switch (iOperation)
    {
      case plParticleVectorFunctionOp::Negate:
        return plVariant(-v);
      case plParticleVectorFunctionOp::Abs:
        return plVariant(v.Abs());
      default:
        v.NormalizeIfNotZero(plVec3::MakeZero()).IgnoreResult();
        return plVariant(v);
    }
  }

  if (pType->IsDerivedFrom<plParticleVectorReduceNode>())
  {
    const plVec3 v = vector("Value");

    switch (iOperation)
    {
      case plParticleVectorReduceOp::X:
        return plVariant((double)v.x);
      case plParticleVectorReduceOp::Y:
        return plVariant((double)v.y);
      case plParticleVectorReduceOp::Z:
        return plVariant((double)v.z);
      default:
        return plVariant((double)v.GetLength());
    }
  }

  if (pType->IsDerivedFrom<plParticleDotNode>())
    return plVariant((double)vector("A").Dot(vector("B")));

  if (pType->IsDerivedFrom<plParticleScaleVectorNode>())
    return plVariant(vector("Vector") * (float)number("Scale"));

  if (pType->IsDerivedFrom<plParticleMakeVectorNode>())
    return plVariant(plVec3((float)number("X"), (float)number("Y"), (float)number("Z")));

  if (pType->IsDerivedFrom<plParticleColorChannelNode>())
  {
    const plVariant color = FoldInput(pNode, "Value", "Value", ref_visited);
    if (!color.IsA<plColor>())
      return plVariant();

    const plColor c = color.Get<plColor>();
    const float channels[] = {c.r, c.g, c.b, c.a};
    return plVariant((double)channels[GetColorChannel(iOperation)]);
  }

  if (pType->IsDerivedFrom<plParticleExpressionNode>())
    return FoldExpression(pNode, ref_visited);

  return plVariant();
}

plString plParticleGraphCompiler::Describe(const plDocumentObject* pNode, plUInt32 uiDepth) const
{
  if (pNode == nullptr)
    return "?";

  if (uiDepth == 0)
    return "...";

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  auto input = [&](const char* szPin, const char* szLiteral) -> plString
  {
    if (const plDocumentObject* pSource = GetInputSource(pNode, szPin))
      return Describe(pSource, uiDepth - 1);

    if (szLiteral == nullptr)
      return "?";

    const plVariant literal = accessor.GetValue(szLiteral);
    if (literal.IsA<plVec3>())
    {
      const plVec3 v = literal.Get<plVec3>();
      plStringBuilder s;
      s.SetFormat("({}, {}, {})", FormatNumber(v.x), FormatNumber(v.y), FormatNumber(v.z));
      return s;
    }

    return FormatNumber(ToDouble(literal));
  };

  plStringBuilder s;

  // named constants read better by name; unnamed ones by value
  const plVariant name = accessor.GetValue("Name");
  if (name.IsA<plString>() && !name.Get<plString>().IsEmpty())
    return name.Get<plString>();

  if (pType->IsDerivedFrom<plParticleValueNode>())
    return FormatNumber(accessor.GetValue("Value").ConvertTo<double>());

  if (pType->IsDerivedFrom<plParticleSystemValueNode>())
    return GetEnumName(plGetStaticRTTI<plParticleSystemValue>(), accessor.GetValue("Input").ConvertTo<plInt64>());

  if (pType->IsDerivedFrom<plParticleSystemVectorNode>())
    return GetEnumName(plGetStaticRTTI<plParticleSystemVector>(), accessor.GetValue("Input").ConvertTo<plInt64>());

  if (pType->IsDerivedFrom<plParticleInputValueNode>())
    return GetEnumName(plGetStaticRTTI<plParticleInputValue>(), accessor.GetValue("Input").ConvertTo<plInt64>());

  if (pType->IsDerivedFrom<plParticleInputVectorNode>())
    return GetEnumName(plGetStaticRTTI<plParticleInputVector>(), accessor.GetValue("Input").ConvertTo<plInt64>());

  if (pType->IsDerivedFrom<plParticleEventInputNode>())
    return GetEnumName(plGetStaticRTTI<plParticleEventInput>(), accessor.GetValue("Input").ConvertTo<plInt64>());

  if (pType->IsDerivedFrom<plParticleBlackboardNode>())
    return accessor.GetValue("Entry").ConvertTo<plString>();

  if (pType->IsDerivedFrom<plParticleInputColorNode>())
    return "Color";

  if (pType->IsDerivedFrom<plParticleRandomNode>())
  {
    s.SetFormat("Random({})", accessor.GetValue("Seed").ConvertTo<plInt32>());
    return s;
  }

  if (pType->IsDerivedFrom<plParticleMathNode>())
  {
    s.SetFormat("{} {} {}", input("A", "A"), GetMathSymbol(accessor.GetValue("Operation").ConvertTo<plInt64>()), input("B", "B"));
    return s;
  }

  if (pType->IsDerivedFrom<plParticleFunctionNode>())
  {
    s.SetFormat("{}({})", GetEnumName(plGetStaticRTTI<plParticleFunctionOp>(), accessor.GetValue("Operation").ConvertTo<plInt64>()), input("Value", "Value"));
    return s;
  }

  if (pType->IsDerivedFrom<plParticleRemapNode>())
  {
    s.SetFormat("Remap({})", input("Value", "Value"));
    return s;
  }

  if (pType->IsDerivedFrom<plParticleScaleVectorNode>())
  {
    s.SetFormat("{} x {}", input("Vector", "Vector"), input("Scale", "Scale"));
    return s;
  }

  if (pType->IsDerivedFrom<plParticleColorChannelNode>())
  {
    // "Color.A"; an unwired colour reads as its channel name only
    const plString sChannel = GetEnumName(plGetStaticRTTI<plParticleColorChannelOp>(), accessor.GetValue("Operation").ConvertTo<plInt64>());
    if (const plDocumentObject* pSource = GetInputSource(pNode, "Value"))
      s.SetFormat("{}.{}", Describe(pSource, uiDepth - 1), sChannel);
    else
      s.SetFormat("Color.{}", sChannel);
    return s;
  }

  // everything else: its title, and its first wired input
  s = plParticleEffectNodeManager::FriendlyTypeName(pType);
  for (const auto& in : plParticleOperatorInputs::Get(pType))
  {
    if (const plDocumentObject* pSource = GetInputSource(pNode, in.m_szPin))
    {
      s.AppendFormat("({})", Describe(pSource, uiDepth - 1));
      break;
    }
  }

  return s;
}

plString plParticleGraphCompiler::GetIssue(const plDocumentObject* pNode) const
{
  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  if (pType->IsDerivedFrom<plParticleCustomAttributeNode>() && accessor.GetValue("Name").ConvertTo<plString>().IsEmpty())
    return "Name the custom attribute to read, as a Set Attribute block names it.";

  if (pType->IsDerivedFrom<plParticleSampleGradientNode>() && accessor.GetValue("Gradient").ConvertTo<plString>().IsEmpty())
    return "Pick a gradient to sample.";

  if (pType->IsDerivedFrom<plParticleSampleCurveNode>() && accessor.GetValue("Curve").ConvertTo<plString>().IsEmpty())
    return "Pick a curve to sample.";

  if (pType->IsDerivedFrom<plParticleExpressionNode>())
  {
    const plString sCode = accessor.GetValue("Expression").ConvertTo<plString>();
    if (sCode.IsEmpty())
      return "Type the expression, using a, b, c and d for the inputs.";

    plExpressionAST ast;
    plString sError;
    bool bFailed = ParseExpression(sCode, ast, &sError).Failed();

    if (!bFailed)
    {
      // parsing passes code whose result is not a number; compiling catches that
      FirstErrorLog log;
      plLogSystemScope logScope(&log);

      plExpressionByteCode byteCode;
      plExpressionCompiler compiler;
      bFailed = compiler.Compile(ast, byteCode).Failed();
      sError = log.m_sError;
    }

    if (bFailed)
    {
      plStringBuilder s;
      s.SetFormat("The expression does not compile: {}", sError.IsEmpty() ? plStringView("unknown error") : sError.GetView());
      return s;
    }
  }

  if (pType->IsDerivedFrom<plParticleBlackboardNode>() && accessor.GetValue("Entry").ConvertTo<plString>().IsEmpty())
    return "Name the blackboard entry to read.";

  if (IsParameterNode(pType))
  {
    const plString sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      return "Pick a parameter name.";

    if (m_pEffectNode == nullptr)
      return {};

    const char* szMap = pType->IsDerivedFrom<plParticleColorParameterNode>() ? "ColorParameters" : "FloatParameters";
    if (!m_pEffectNode->GetTypeAccessor().GetValue(szMap, plVariant(sName)).IsValid())
    {
      plStringBuilder s;
      s.SetFormat("No parameter '{}' on this effect. Add it on the effect node, or pick an existing name.", sName);
      return s;
    }
  }

  return {};
}

//////////////////////////////////////////////////////////////////////////
// Per-frame program

plExpressionAST::Node* plParticleGraphCompiler::Number(double fValue)
{
  return m_Ast.CreateConstant(plVariant((float)fValue), DataType::Float);
}

plExpressionAST::Node* plParticleGraphCompiler::Vector(const plVec3& v)
{
  plExpressionAST::Node* components[] = {Number(v.x), Number(v.y), Number(v.z)};
  return m_Ast.CreateConstructorCall(DataType::Float3, components);
}

plExpressionAST::Node* plParticleGraphCompiler::Color(const plColor& c)
{
  plExpressionAST::Node* components[] = {Number(c.r), Number(c.g), Number(c.b), Number(c.a)};
  return m_Ast.CreateConstructorCall(DataType::Float4, components);
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::Constant(const plVariant& value, plParticleValueKind::Enum kind)
{
  switch (kind)
  {
    case plParticleValueKind::Vector:
      return {Vector(ToVec3(value)), kind};
    case plParticleValueKind::Color:
      return {Color(value.IsA<plColor>() ? value.Get<plColor>() : plColor::White), kind};
    case plParticleValueKind::Bool:
      return {m_Ast.CreateConstant(plVariant(value.IsValid() && value.ConvertTo<bool>()), DataType::Bool), kind};
    default:
      return {Number(ToDouble(value)), kind};
  }
}

plExpressionAST::Node* plParticleGraphCompiler::Input(plStringView sName, plProcessingStream::DataType type)
{
  const plString sKey = sName;

  if (plExpressionAST::Node** ppNode = m_Inputs.GetValue(sKey))
    return *ppNode;

  plHashedString sHashed;
  sHashed.Assign(sName);

  plExpressionAST::Input* pInput = m_Ast.CreateInput({sHashed, type});
  m_Ast.m_InputNodes.PushBack(pInput);
  m_Inputs[sKey] = pInput;
  return pInput;
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::Convert(const AstValue& value, plParticleValueKind::Enum kind)
{
  if (value.m_pNode == nullptr || value.m_Kind == kind)
    return value;

  if (value.m_Kind == plParticleValueKind::Bool && kind == plParticleValueKind::Number)
    return {m_Ast.CreateTernaryOperator(NodeType::Select, value.m_pNode, Number(1.0), Number(0.0)), kind};

  if (value.m_Kind == plParticleValueKind::Number && kind == plParticleValueKind::Bool)
    return {m_Ast.CreateBinaryOperator(NodeType::NotEqual, value.m_pNode, Number(0.0)), kind};

  return {};
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::BuildInput(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plParticleValueKind::Enum kind, plSet<const plDocumentObject*>& ref_visited)
{
  if (const plDocumentObject* pSource = GetInputSource(pNode, szPin))
  {
    // a constant subgraph needs no instructions: it folds right here
    if (GetTier(pSource) == plParticleTier::Constant)
      return Constant(FoldInternal(pSource, ref_visited), kind);

    return Convert(Build(pSource, ref_visited), kind);
  }

  if (szLiteral == nullptr)
    return {};

  return Constant(pNode->GetTypeAccessor().GetValue(szLiteral), kind);
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::Build(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited)
{
  if (pNode == nullptr || ref_visited.Contains(pNode))
    return {};

  if (const AstValue* pBuilt = m_Built.GetValue(pNode))
    return *pBuilt;

  ref_visited.Insert(pNode);
  PL_SCOPE_EXIT(ref_visited.Remove(pNode));

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();
  const plInt64 iOperation = accessor.GetValue("Operation").IsValid() ? accessor.GetValue("Operation").ConvertTo<plInt64>() : 0;

  constexpr plParticleValueKind::Enum kNumber = plParticleValueKind::Number;
  constexpr plParticleValueKind::Enum kVector = plParticleValueKind::Vector;

  auto number = [&](const char* szPin) -> plExpressionAST::Node*
  { return BuildInput(pNode, szPin, szPin, kNumber, ref_visited).m_pNode; };

  auto vector = [&](const char* szPin) -> plExpressionAST::Node*
  { return BuildInput(pNode, szPin, szPin, kVector, ref_visited).m_pNode; };

  auto unary = [&](NodeType::Enum type, plExpressionAST::Node* pOperand) -> plExpressionAST::Node*
  { return pOperand != nullptr ? m_Ast.CreateUnaryOperator(type, pOperand) : nullptr; };

  auto binary = [&](NodeType::Enum type, plExpressionAST::Node* a, plExpressionAST::Node* b) -> plExpressionAST::Node*
  { return (a != nullptr && b != nullptr) ? m_Ast.CreateBinaryOperator(type, a, b) : nullptr; };

  auto ternary = [&](NodeType::Enum type, plExpressionAST::Node* a, plExpressionAST::Node* b, plExpressionAST::Node* c) -> plExpressionAST::Node*
  { return (a != nullptr && b != nullptr && c != nullptr) ? m_Ast.CreateTernaryOperator(type, a, b, c) : nullptr; };

  AstValue result;

  if (IsParticleInputNode(pType))
  {
    result = BuildParticleInput(pNode);
  }
  else if (IsRuntimeSampledNode(pType))
  {
    result = BuildSampled(pNode, ref_visited);
  }
  else if (pType->IsDerivedFrom<plParticleSystemValueNode>())
  {
    result = {Input(GetSystemInputName(pType, accessor.GetValue("Input").ConvertTo<plInt64>()), plProcessingStream::DataType::Float), kNumber};
  }
  else if (pType->IsDerivedFrom<plParticleSystemVectorNode>())
  {
    result = {Input(GetSystemInputName(pType, accessor.GetValue("Input").ConvertTo<plInt64>()), plProcessingStream::DataType::Float3), kVector};
  }
  else if (pType->IsDerivedFrom<plParticleBlackboardNode>())
  {
    result = BuildBlackboardEntry(pNode);
  }
  else if (IsParameterNode(pType))
  {
    const plString sName = accessor.GetValue("Name").ConvertTo<plString>();

    if (!sName.IsEmpty())
    {
      plStringBuilder sInput;

      if (pType->IsDerivedFrom<plParticleColorParameterNode>())
      {
        sInput.Set(plParticleFrameInput::ColorParameterPrefix, sName);
        result = {Input(sInput, plProcessingStream::DataType::Float4), plParticleValueKind::Color};
      }
      else
      {
        sInput.Set(plParticleFrameInput::FloatParameterPrefix, sName);
        result = {Input(sInput, plProcessingStream::DataType::Float), kNumber};

        if (pType->IsDerivedFrom<plParticleBoolParameterNode>())
          result = Convert(result, plParticleValueKind::Bool);
      }
    }
  }
  else if (pType->IsDerivedFrom<plParticleToBoolNode>())
  {
    result = Convert({number("Value"), kNumber}, plParticleValueKind::Bool);
  }
  else if (pType->IsDerivedFrom<plParticleMathNode>())
  {
    plExpressionAST::Node* a = number("A");
    plExpressionAST::Node* b = number("B");

    switch (iOperation)
    {
      case plParticleMathOp::Add:
        result = {binary(NodeType::Add, a, b), kNumber};
        break;
      case plParticleMathOp::Subtract:
        result = {binary(NodeType::Subtract, a, b), kNumber};
        break;
      case plParticleMathOp::Divide:
        // a zero divisor gives zero, as the folded value does
        result = {ternary(NodeType::Select, binary(NodeType::Equal, b, Number(0.0)), Number(0.0), binary(NodeType::Divide, a, b)), kNumber};
        break;
      case plParticleMathOp::Min:
        result = {binary(NodeType::Min, a, b), kNumber};
        break;
      case plParticleMathOp::Max:
        result = {binary(NodeType::Max, a, b), kNumber};
        break;
      default:
        result = {binary(NodeType::Multiply, a, b), kNumber};
        break;
    }
  }
  else if (pType->IsDerivedFrom<plParticleCompareNode>())
  {
    static const NodeType::Enum s_Compare[] = {NodeType::Less, NodeType::LessEqual, NodeType::Greater, NodeType::GreaterEqual, NodeType::Equal, NodeType::NotEqual};
    const NodeType::Enum type = (iOperation >= 0 && iOperation < (plInt64)PL_ARRAY_SIZE(s_Compare)) ? s_Compare[iOperation] : NodeType::Greater;
    result = {binary(type, number("A"), number("B")), plParticleValueKind::Bool};
  }
  else if (pType->IsDerivedFrom<plParticleBranchNode>())
  {
    const AstValue condition = BuildInput(pNode, "Condition", nullptr, plParticleValueKind::Bool, ref_visited);
    result = {ternary(NodeType::Select, condition.m_pNode, number("True"), number("False")), kNumber};
  }
  else if (pType->IsDerivedFrom<plParticleFunctionNode>())
  {
    plExpressionAST::Node* x = number("Value");

    switch (iOperation)
    {
      case plParticleFunctionOp::Cosine:
        result = {unary(NodeType::Cos, x), kNumber};
        break;
      case plParticleFunctionOp::Abs:
        result = {unary(NodeType::Absolute, x), kNumber};
        break;
      case plParticleFunctionOp::Floor:
        result = {unary(NodeType::Floor, x), kNumber};
        break;
      case plParticleFunctionOp::Ceil:
        result = {unary(NodeType::Ceil, x), kNumber};
        break;
      case plParticleFunctionOp::Frac:
        result = {unary(NodeType::Frac, x), kNumber};
        break;
      case plParticleFunctionOp::Sqrt:
        result = {unary(NodeType::Sqrt, binary(NodeType::Max, x, Number(0.0))), kNumber};
        break;
      case plParticleFunctionOp::Saturate:
        result = {unary(NodeType::Saturate, x), kNumber};
        break;
      case plParticleFunctionOp::Negate:
        result = {unary(NodeType::Negate, x), kNumber};
        break;
      case plParticleFunctionOp::OneMinus:
        result = {binary(NodeType::Subtract, Number(1.0), x), kNumber};
        break;
      default:
        result = {unary(NodeType::Sin, x), kNumber};
        break;
    }
  }
  else if (pType->IsDerivedFrom<plParticleRemapNode>())
  {
    const double inMin = accessor.GetValue("InMin").ConvertTo<double>();
    const double inMax = accessor.GetValue("InMax").ConvertTo<double>();
    const double outMin = accessor.GetValue("OutMin").ConvertTo<double>();
    const double outMax = accessor.GetValue("OutMax").ConvertTo<double>();

    // t = (x - inMin) / (inMax - inMin), with the range folded into one multiply
    const double fInvRange = (inMax != inMin) ? 1.0 / (inMax - inMin) : 0.0;
    plExpressionAST::Node* t = binary(NodeType::Multiply, binary(NodeType::Subtract, number("Value"), Number(inMin)), Number(fInvRange));

    if (accessor.GetValue("Clamp").ConvertTo<bool>())
      t = unary(NodeType::Saturate, t);

    result = {binary(NodeType::Add, Number(outMin), binary(NodeType::Multiply, t, Number(outMax - outMin))), kNumber};
  }
  else if (pType->IsDerivedFrom<plParticleLerpNode>())
  {
    result = {ternary(NodeType::Lerp, number("A"), number("B"), number("T")), kNumber};
  }
  else if (pType->IsDerivedFrom<plParticleClampNode>())
  {
    result = {ternary(NodeType::Clamp, number("Value"), number("Min"), number("Max")), kNumber};
  }
  else if (pType->IsDerivedFrom<plParticleVectorMathNode>())
  {
    plExpressionAST::Node* a = vector("A");
    plExpressionAST::Node* b = vector("B");

    switch (iOperation)
    {
      case plParticleVectorMathOp::Subtract:
        result = {binary(NodeType::Subtract, a, b), kVector};
        break;
      case plParticleVectorMathOp::Multiply:
        result = {binary(NodeType::Multiply, a, b), kVector};
        break;
      case plParticleVectorMathOp::Cross:
        result = {binary(NodeType::Cross, a, b), kVector};
        break;
      case plParticleVectorMathOp::Min:
        result = {binary(NodeType::Min, a, b), kVector};
        break;
      case plParticleVectorMathOp::Max:
        result = {binary(NodeType::Max, a, b), kVector};
        break;
      default:
        result = {binary(NodeType::Add, a, b), kVector};
        break;
    }
  }
  else if (pType->IsDerivedFrom<plParticleVectorFunctionNode>())
  {
    plExpressionAST::Node* v = vector("Value");

    switch (iOperation)
    {
      case plParticleVectorFunctionOp::Negate:
        result = {unary(NodeType::Negate, v), kVector};
        break;
      case plParticleVectorFunctionOp::Abs:
        result = {unary(NodeType::Absolute, v), kVector};
        break;
      default:
        result = {unary(NodeType::Normalize, v), kVector};
        break;
    }
  }
  else if (pType->IsDerivedFrom<plParticleVectorReduceNode>())
  {
    plExpressionAST::Node* v = vector("Value");

    switch (iOperation)
    {
      case plParticleVectorReduceOp::X:
        result = {v != nullptr ? m_Ast.CreateSwizzle(plExpressionAST::VectorComponent::X, v) : nullptr, kNumber};
        break;
      case plParticleVectorReduceOp::Y:
        result = {v != nullptr ? m_Ast.CreateSwizzle(plExpressionAST::VectorComponent::Y, v) : nullptr, kNumber};
        break;
      case plParticleVectorReduceOp::Z:
        result = {v != nullptr ? m_Ast.CreateSwizzle(plExpressionAST::VectorComponent::Z, v) : nullptr, kNumber};
        break;
      default:
        result = {unary(NodeType::Length, v), kNumber};
        break;
    }
  }
  else if (pType->IsDerivedFrom<plParticleDotNode>())
  {
    result = {binary(NodeType::Dot, vector("A"), vector("B")), kNumber};
  }
  else if (pType->IsDerivedFrom<plParticleScaleVectorNode>())
  {
    plExpressionAST::Node* s = number("Scale");
    plExpressionAST::Node* broadcast[] = {s, s, s};
    result = {binary(NodeType::Multiply, vector("Vector"), s != nullptr ? m_Ast.CreateConstructorCall(DataType::Float3, broadcast) : nullptr), kVector};
  }
  else if (pType->IsDerivedFrom<plParticleMakeVectorNode>())
  {
    plExpressionAST::Node* components[] = {number("X"), number("Y"), number("Z")};
    if (components[0] != nullptr && components[1] != nullptr && components[2] != nullptr)
      result = {m_Ast.CreateConstructorCall(DataType::Float3, components), kVector};
  }
  else if (pType->IsDerivedFrom<plParticleColorChannelNode>())
  {
    plExpressionAST::Node* c = BuildInput(pNode, "Value", "Value", plParticleValueKind::Color, ref_visited).m_pNode;
    result = {c != nullptr ? m_Ast.CreateSwizzle(GetColorChannel(iOperation), c) : nullptr, kNumber};
  }
  else if (pType->IsDerivedFrom<plParticleExpressionNode>())
  {
    result = BuildExpression(pNode, ref_visited);
  }
  else
  {
    // constants, colours and assets: whatever folding gives
    const plVariant value = Fold(pNode);

    if (value.IsA<plVec3>())
      result = {Vector(value.Get<plVec3>()), kVector};
    else if (value.IsA<plColor>())
    {
      const plColor c = value.Get<plColor>();
      plExpressionAST::Node* components[] = {Number(c.r), Number(c.g), Number(c.b), Number(c.a)};
      result = {m_Ast.CreateConstructorCall(DataType::Float4, components), plParticleValueKind::Color};
    }
    else if (value.IsA<bool>())
      result = {m_Ast.CreateConstant(value, DataType::Bool), plParticleValueKind::Bool};
    else if (value.CanConvertTo<double>())
      result = {Number(value.ConvertTo<double>()), kNumber};
  }

  if (result.m_pNode == nullptr)
    result = {};

  m_Built[pNode] = result;
  return result;
}

void plParticleGraphCompiler::SetParticleContext(bool bInitialize, double fSystemLifetime)
{
  m_bInitializeContext = bInitialize;
  m_fSystemLifetime = fSystemLifetime;
}

bool plParticleGraphCompiler::IsParticleInputNode(const plRTTI* pType)
{
  return pType->IsDerivedFrom<plParticleInputValueNode>() || pType->IsDerivedFrom<plParticleInputVectorNode>() ||
         pType->IsDerivedFrom<plParticleInputColorNode>() || pType->IsDerivedFrom<plParticleRandomNode>() ||
         pType->IsDerivedFrom<plParticleCustomAttributeNode>() || pType->IsDerivedFrom<plParticleEventInputNode>();
}

bool plParticleGraphCompiler::IsRuntimeSampledNode(const plRTTI* pType)
{
  return pType->IsDerivedFrom<plParticleSampleGradientNode>() || pType->IsDerivedFrom<plParticleSampleCurveNode>() || pType->IsDerivedFrom<plParticleNoiseNode>();
}

plInt32 plParticleGraphCompiler::GetAssetSlot(plHybridArray<plString, 2>& ref_assets, const plString& sAsset)
{
  plUInt32 uiSlot = ref_assets.IndexOf(sAsset);
  if (uiSlot == plInvalidIndex)
  {
    uiSlot = ref_assets.GetCount();
    ref_assets.PushBack(sAsset);
  }

  return (plInt32)uiSlot;
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::BuildSampled(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited)
{
  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  auto intConstant = [&](plInt32 iValue)
  { return m_Ast.CreateConstant(plVariant(iValue), DataType::Int); };

  if (pType->IsDerivedFrom<plParticleNoiseNode>())
  {
    plExpressionAST::Node* pPosition = BuildInput(pNode, "Position", "Position", plParticleValueKind::Vector, ref_visited).m_pNode;
    if (pPosition == nullptr)
      return {};

    const float fFrequency = accessor.GetValue("Frequency").ConvertTo<float>();
    const plInt32 iOctaves = plMath::Clamp(accessor.GetValue("Octaves").ConvertTo<plInt32>(), 1, 8);

    plExpressionAST::Node* pScaled = m_Ast.CreateBinaryOperator(NodeType::Multiply, pPosition, Vector(plVec3(fFrequency)));
    plExpressionAST::Node* arguments[] = {
      m_Ast.CreateSwizzle(plExpressionAST::VectorComponent::X, pScaled),
      m_Ast.CreateSwizzle(plExpressionAST::VectorComponent::Y, pScaled),
      m_Ast.CreateSwizzle(plExpressionAST::VectorComponent::Z, pScaled),
      intConstant(iOctaves),
    };
    return {m_Ast.CreateFunctionCall(plDefaultExpressionFunctions::s_PerlinNoiseFunc.m_Desc, arguments), plParticleValueKind::Number};
  }

  plExpressionAST::Node* pT = BuildInput(pNode, "T", "T", plParticleValueKind::Number, ref_visited).m_pNode;
  if (pT == nullptr)
    return {};

  if (pType->IsDerivedFrom<plParticleSampleCurveNode>())
  {
    const plString sCurve = accessor.GetValue("Curve").ConvertTo<plString>();
    if (sCurve.IsEmpty())
      return {};

    plExpressionAST::Node* arguments[] = {pT, intConstant(GetAssetSlot(m_Assets.m_Curves, sCurve))};
    return {m_Ast.CreateFunctionCall(plParticleProgramTables::s_CurveFunc.m_Desc, arguments), plParticleValueKind::Number};
  }

  // plParticleSampleGradientNode: one table read per component
  const plString sGradient = accessor.GetValue("Gradient").ConvertTo<plString>();
  if (sGradient.IsEmpty())
    return {};

  const plInt32 iSlot = GetAssetSlot(m_Assets.m_Gradients, sGradient);

  plExpressionAST::Node* components[4];
  for (plInt32 c = 0; c < 4; ++c)
  {
    plExpressionAST::Node* arguments[] = {pT, intConstant(iSlot), intConstant(c)};
    components[c] = m_Ast.CreateFunctionCall(plParticleProgramTables::s_GradientFunc.m_Desc, arguments);
  }

  return {m_Ast.CreateConstructorCall(DataType::Float4, components), plParticleValueKind::Color};
}

namespace
{
  constexpr const char* s_szExpressionPins[] = {"A", "B", "C", "D"};
}

plResult plParticleGraphCompiler::ParseExpression(plStringView sCode, plExpressionAST& out_ast, plString* out_pError)
{
  if (sCode.IsEmpty())
  {
    if (out_pError != nullptr)
      *out_pError = "The expression is empty.";
    return PL_FAILURE;
  }

  plExpression::StreamDesc inputs[] = {
    {plMakeHashedString("a"), plProcessingStream::DataType::Float},
    {plMakeHashedString("b"), plProcessingStream::DataType::Float},
    {plMakeHashedString("c"), plProcessingStream::DataType::Float},
    {plMakeHashedString("d"), plProcessingStream::DataType::Float},
  };
  plExpression::StreamDesc outputs[] = {{plMakeHashedString("out"), plProcessingStream::DataType::Float}};

  plStringBuilder sFullCode("out = ", sCode);

  FirstErrorLog log;
  plLogSystemScope logScope(&log);

  plExpressionParser parser;
  plExpressionParser::Options options;
  if (parser.Parse(sFullCode, inputs, outputs, options, out_ast).Failed())
  {
    if (out_pError != nullptr)
      *out_pError = log.m_sError;
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::BuildExpression(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited)
{
  const plString sCode = pNode->GetTypeAccessor().GetValue("Expression").ConvertTo<plString>();

  plUniquePtr<plExpressionAST> pAst = PL_DEFAULT_NEW(plExpressionAST);
  if (ParseExpression(sCode, *pAst).Failed() || pAst->m_OutputNodes.GetCount() != 1 || pAst->m_OutputNodes[0]->m_pExpression == nullptr)
    return {};

  // the variables a to d become whatever feeds the pins A to D
  plExpressionAST::Node* arguments[4] = {};
  for (plUInt32 i = 0; i < 4; ++i)
  {
    arguments[i] = BuildInput(pNode, s_szExpressionPins[i], s_szExpressionPins[i], plParticleValueKind::Number, ref_visited).m_pNode;
    if (arguments[i] == nullptr)
      return {};
  }

  auto substitute = [&](plExpressionAST::Node* pCodeNode) -> plExpressionAST::Node*
  {
    if (pCodeNode->m_Type != NodeType::Input)
      return nullptr;

    const plStringView sName = static_cast<plExpressionAST::Input*>(pCodeNode)->m_Desc.m_sName.GetView();
    for (plUInt32 i = 0; i < 4; ++i)
    {
      if (sName.GetElementCount() == 1 && sName.GetStartPointer()[0] == 'a' + (char)i)
        return arguments[i];
    }

    return nullptr;
  };

  plExpressionAST::Node* pRoot = pAst->m_OutputNodes[0]->m_pExpression;
  if (plExpressionAST::Node* pArgument = substitute(pRoot))
    return {pArgument, plParticleValueKind::Number};

  // the code can share sub-expressions through variables, so each of its nodes is visited once
  plSet<plExpressionAST::Node*> visited;
  plHybridArray<plExpressionAST::Node*, 32> pending;
  pending.PushBack(pRoot);

  while (!pending.IsEmpty())
  {
    plExpressionAST::Node* pCodeNode = pending.PeekBack();
    pending.PopBack();

    if (visited.Contains(pCodeNode))
      continue;
    visited.Insert(pCodeNode);

    for (plExpressionAST::Node*& pChild : plExpressionAST::GetChildren(pCodeNode))
    {
      if (pChild == nullptr)
        continue;

      if (plExpressionAST::Node* pArgument = substitute(pChild))
        pChild = pArgument;
      else
        pending.PushBack(pChild);
    }
  }

  m_ExpressionAsts.PushBack(std::move(pAst));
  return {pRoot, plParticleValueKind::Number};
}

plVariant plParticleGraphCompiler::FoldExpression(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited)
{
  plExpressionAST ast;
  if (ParseExpression(pNode->GetTypeAccessor().GetValue("Expression").ConvertTo<plString>(), ast).Failed())
    return plVariant();

  plExpressionByteCode byteCode;
  {
    plMuteLog mute;
    plLogSystemScope muteScope(&mute);

    plExpressionCompiler compiler;
    if (compiler.Compile(ast, byteCode).Failed())
      return plVariant();
  }

  // one instance, in arrays as wide as the VM's batch
  float values[4][4] = {};
  float result[4] = {};

  for (plUInt32 i = 0; i < 4; ++i)
  {
    const float fValue = (float)ToDouble(FoldInput(pNode, s_szExpressionPins[i], s_szExpressionPins[i], ref_visited));
    for (float& f : values[i])
      f = fValue;
  }

  plProcessingStream inputs[] = {
    plProcessingStream(plMakeHashedString("a"), plMakeArrayPtr(values[0]).ToByteArray(), plProcessingStream::DataType::Float),
    plProcessingStream(plMakeHashedString("b"), plMakeArrayPtr(values[1]).ToByteArray(), plProcessingStream::DataType::Float),
    plProcessingStream(plMakeHashedString("c"), plMakeArrayPtr(values[2]).ToByteArray(), plProcessingStream::DataType::Float),
    plProcessingStream(plMakeHashedString("d"), plMakeArrayPtr(values[3]).ToByteArray(), plProcessingStream::DataType::Float),
  };
  plProcessingStream outputs[] = {plProcessingStream(plMakeHashedString("out"), plMakeArrayPtr(result).ToByteArray(), plProcessingStream::DataType::Float)};

  plExpressionVM vm;
  if (vm.Execute(byteCode, inputs, outputs, 1).Failed())
    return plVariant();

  return plVariant((double)result[0]);
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::BuildBlackboardEntry(const plDocumentObject* pNode)
{
  using DT = plProcessingStream::DataType;

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  const plString sEntry = accessor.GetValue("Entry").ConvertTo<plString>();
  if (sEntry.IsEmpty())
    return {};

  const plString sBlackboard = accessor.GetValue("Blackboard").ConvertTo<plString>();

  plStringBuilder sValue, sFound;
  plParticleMakeBlackboardInput(sBlackboard, sEntry, false, sValue);
  plParticleMakeBlackboardInput(sBlackboard, sEntry, true, sFound);

  // default + (entry - default) * found: the entry when it exists, the node's default when not
  plExpressionAST::Node* pFound = Input(sFound, DT::Float);
  const plVariant def = accessor.GetValue("Default");

  auto Pick = [&](plExpressionAST::Node* pValue, plExpressionAST::Node* pDefault, plExpressionAST::Node* pFoundBroadcast)
  {
    plExpressionAST::Node* pDelta = m_Ast.CreateBinaryOperator(plExpressionAST::NodeType::Subtract, pValue, pDefault);
    return m_Ast.CreateBinaryOperator(plExpressionAST::NodeType::Add, pDefault, m_Ast.CreateBinaryOperator(plExpressionAST::NodeType::Multiply, pDelta, pFoundBroadcast));
  };

  if (pType->IsDerivedFrom<plParticleBlackboardVectorNode>())
  {
    plExpressionAST::Node* broadcast[] = {pFound, pFound, pFound};
    return {Pick(Input(sValue, DT::Float3), Vector(def.ConvertTo<plVec3>()), m_Ast.CreateConstructorCall(DataType::Float3, broadcast)), plParticleValueKind::Vector};
  }

  if (pType->IsDerivedFrom<plParticleBlackboardColorNode>())
  {
    plExpressionAST::Node* broadcast[] = {pFound, pFound, pFound, pFound};
    return {Pick(Input(sValue, DT::Float4), Color(def.ConvertTo<plColor>()), m_Ast.CreateConstructorCall(DataType::Float4, broadcast)), plParticleValueKind::Color};
  }

  return {Pick(Input(sValue, DT::Float), Number(def.ConvertTo<double>()), pFound), plParticleValueKind::Number};
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::BuildParticleInput(const plDocumentObject* pNode)
{
  // particle inputs read the system's streams under their own names and types, so the runtime can
  // point the program straight at them
  using DT = plProcessingStream::DataType;

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  auto lifeTime = [&](plExpressionAST::VectorComponent::Enum component) -> plExpressionAST::Node*
  { return m_Ast.CreateSwizzle(component, Input("LifeTime", DT::Float2)); };

  if (pType->IsDerivedFrom<plParticleInputVectorNode>())
  {
    switch (accessor.GetValue("Input").ConvertTo<plInt64>())
    {
      case plParticleInputVector::Velocity:
        return {Input("Velocity", DT::Float3), plParticleValueKind::Vector};
      case plParticleInputVector::Axis:
        return {Input("Axis", DT::Float3), plParticleValueKind::Vector};
      default:
        return {m_Ast.CreateSwizzle("xyz", Input("Position", DT::Float4)), plParticleValueKind::Vector};
    }
  }

  if (pType->IsDerivedFrom<plParticleInputColorNode>())
    return {Input("Color", DT::Half4), plParticleValueKind::Color};

  // the spawning event's vectors, kept per particle by the system
  if (pType->IsDerivedFrom<plParticleEventInputNode>())
  {
    switch (accessor.GetValue("Input").ConvertTo<plInt64>())
    {
      case plParticleEventInput::EventVelocity:
        return {Input("EventVelocity", DT::Float3), plParticleValueKind::Vector};
      case plParticleEventInput::EventNormal:
        return {Input("EventNormal", DT::Float3), plParticleValueKind::Vector};
      default:
        return {Input("EventPosition", DT::Float3), plParticleValueKind::Vector};
    }
  }

  if (pType->IsDerivedFrom<plParticleRandomNode>())
  {
    // the VM's Random hashes a per-particle seed with the node's own seed
    plExpressionAST::Node* arguments[] = {Input("RandomSeed", DT::Int), m_Ast.CreateConstant(plVariant(accessor.GetValue("Seed").ConvertTo<plInt32>()), DataType::Int)};
    return {m_Ast.CreateFunctionCall(plDefaultExpressionFunctions::s_RandomFunc.m_Desc, arguments), plParticleValueKind::Number};
  }

  if (pType->IsDerivedFrom<plParticleCustomAttributeNode>())
  {
    const plString sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      return {};

    plStringBuilder sStream("Custom.", sName);

    if (pType->IsDerivedFrom<plParticleCustomVectorNode>())
      return {Input(sStream, DT::Float3), plParticleValueKind::Vector};
    if (pType->IsDerivedFrom<plParticleCustomColorNode>())
      return {Input(sStream, DT::Half4), plParticleValueKind::Color};

    return {Input(sStream, DT::Float), plParticleValueKind::Number};
  }

  // plParticleInputValueNode
  switch (accessor.GetValue("Input").ConvertTo<plInt64>())
  {
    case plParticleInputValue::Age:
      // LifeTime holds (remaining seconds, 1 / total seconds)
      if (m_bInitializeContext)
        return {Number(0.0), plParticleValueKind::Number};
      return {m_Ast.CreateBinaryOperator(NodeType::Subtract, m_Ast.CreateBinaryOperator(NodeType::Divide, Number(1.0), lifeTime(plExpressionAST::VectorComponent::Y)), lifeTime(plExpressionAST::VectorComponent::X)), plParticleValueKind::Number};

    case plParticleInputValue::NormalizedAge:
      if (m_bInitializeContext)
        return {Number(0.0), plParticleValueKind::Number};
      return {m_Ast.CreateBinaryOperator(NodeType::Subtract, Number(1.0), m_Ast.CreateBinaryOperator(NodeType::Multiply, lifeTime(plExpressionAST::VectorComponent::X), lifeTime(plExpressionAST::VectorComponent::Y))), plParticleValueKind::Number};

    case plParticleInputValue::Lifetime:
      if (m_bInitializeContext)
        return {Number(m_fSystemLifetime), plParticleValueKind::Number};
      return {m_Ast.CreateBinaryOperator(NodeType::Divide, Number(1.0), lifeTime(plExpressionAST::VectorComponent::Y)), plParticleValueKind::Number};

    case plParticleInputValue::Size:
      return {Input("Size", DT::Half), plParticleValueKind::Number};

    case plParticleInputValue::RotationSpeed:
      return {Input("RotationSpeed", DT::Half), plParticleValueKind::Number};

    default:
      return {m_Ast.CreateUnaryOperator(NodeType::Length, Input("Velocity", DT::Float3)), plParticleValueKind::Number};
  }
}

plUInt32 plParticleGraphCompiler::AddOutput(const plDocumentObject* pSource, plParticleValueKind::Enum targetKind)
{
  plSet<const plDocumentObject*> visited;
  AstValue value = Build(pSource, visited);

  // a bool property takes 0 or 1 like a number; the runtime turns non-zero into true
  const plParticleValueKind::Enum outputKind = (targetKind == plParticleValueKind::Bool) ? plParticleValueKind::Number : targetKind;
  value = Convert(value, outputKind);

  if (value.m_pNode == nullptr || value.m_Kind != outputKind)
    return plInvalidIndex;

  plProcessingStream::DataType type = plProcessingStream::DataType::Float;
  if (outputKind == plParticleValueKind::Vector)
    type = plProcessingStream::DataType::Float3;
  else if (outputKind == plParticleValueKind::Color)
    type = plProcessingStream::DataType::Float4;

  const plUInt32 uiIndex = m_Ast.m_OutputNodes.GetCount();

  plStringBuilder sName;
  sName.SetFormat("Out{}", uiIndex);
  plHashedString sHashed;
  sHashed.Assign(sName);

  m_Ast.m_OutputNodes.PushBack(m_Ast.CreateOutput({sHashed, type}, value.m_pNode));
  return uiIndex;
}

plUInt32 plParticleGraphCompiler::AddSpyOutput(const plDocumentObject* pSource)
{
  plSet<const plDocumentObject*> visited;
  AstValue value = Build(pSource, visited);

  if (value.m_Kind == plParticleValueKind::Bool)
  {
    value = Convert(value, plParticleValueKind::Number);
  }
  else if (value.m_Kind == plParticleValueKind::Vector && value.m_pNode != nullptr)
  {
    value = {m_Ast.CreateUnaryOperator(NodeType::Length, value.m_pNode), plParticleValueKind::Number};
  }
  else if (value.m_Kind == plParticleValueKind::Color && value.m_pNode != nullptr)
  {
    plExpressionAST::Node* weights[] = {Number(0.2126), Number(0.7152), Number(0.0722)};
    plExpressionAST::Node* pRgb = m_Ast.CreateSwizzle("xyz", value.m_pNode);
    value = {m_Ast.CreateBinaryOperator(NodeType::Dot, pRgb, m_Ast.CreateConstructorCall(DataType::Float3, weights)), plParticleValueKind::Number};
  }

  if (value.m_pNode == nullptr || value.m_Kind != plParticleValueKind::Number)
    return plInvalidIndex;

  const plUInt32 uiIndex = m_Ast.m_OutputNodes.GetCount();

  plStringBuilder sName;
  sName.SetFormat("Out{}", uiIndex);
  plHashedString sHashed;
  sHashed.Assign(sName);

  m_Ast.m_OutputNodes.PushBack(m_Ast.CreateOutput({sHashed, plProcessingStream::DataType::Float}, value.m_pNode));
  return uiIndex;
}

plResult plParticleGraphCompiler::Compile(plExpressionByteCode& out_byteCode)
{
  plExpressionCompiler compiler;
  return compiler.Compile(m_Ast, out_byteCode);
}
