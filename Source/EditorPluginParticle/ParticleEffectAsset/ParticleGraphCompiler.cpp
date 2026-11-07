#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphCompiler.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphEditing.h>
#include <Foundation/CodeUtils/Expression/ExpressionCompiler.h>
#include <Foundation/CodeUtils/Expression/ExpressionParser.h>
#include <ParticlePlugin/Behavior/ParticleAttributeSink.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SetAttribute.h>
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
      case plParticleSystemVector::CameraForward:
        return plVariant(plVec3(1, 0, 0));
      case plParticleSystemVector::EffectRight:
      case plParticleSystemVector::CameraRight:
        return plVariant(plVec3(0, 1, 0));
      case plParticleSystemVector::EffectUp:
      case plParticleSystemVector::CameraUp:
        return plVariant(plVec3(0, 0, 1));
      case plParticleSystemVector::EffectScale:
        return plVariant(plVec3(1, 1, 1));
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
      case plParticleSystemVector::EffectRight:
        return plParticleFrameInput::EffectRight;
      case plParticleSystemVector::EffectScale:
        return plParticleFrameInput::EffectScale;
      case plParticleSystemVector::CameraForward:
        return plParticleFrameInput::CameraForward;
      case plParticleSystemVector::CameraRight:
        return plParticleFrameInput::CameraRight;
      case plParticleSystemVector::CameraUp:
        return plParticleFrameInput::CameraUp;
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

  /// A folded value as four lanes of the kind it is used as: a number or bool fills all four, a vector is
  /// xyz with w = 0 (a vector used as a colour gets A = 1), a colour is rgba.
  plVec4 ToLanes(const plVariant& value, plParticleValueKind::Enum kind)
  {
    if (value.IsA<plColor>())
    {
      const plColor c = value.Get<plColor>();
      return plVec4(c.r, c.g, c.b, kind == plParticleValueKind::Vector ? 0.0f : c.a);
    }

    if (value.IsA<plVec3>())
      return value.Get<plVec3>().GetAsVec4(kind == plParticleValueKind::Color ? 1.0f : 0.0f);

    if (value.IsA<bool>())
      return plVec4(value.Get<bool>() ? 1.0f : 0.0f);

    return plVec4((float)ToDouble(value));
  }

  /// Four lanes back as a value of the kind.
  plVariant FromLanes(const plVec4& v, plParticleValueKind::Enum kind)
  {
    switch (kind)
    {
      case plParticleValueKind::Vector:
        return plVariant(v.GetAsVec3());
      case plParticleValueKind::Color:
        return plVariant(plColor(v.x, v.y, v.z, v.w));
      case plParticleValueKind::Bool:
        return plVariant(v.x != 0.0f);
      default:
        return plVariant((double)v.x);
    }
  }

  /// Applies fn to each of the four lanes.
  template <typename FN>
  plVec4 PerLane(const plVec4& a, FN fn)
  {
    return plVec4(fn(a.x), fn(a.y), fn(a.z), fn(a.w));
  }

  template <typename FN>
  plVec4 PerLane(const plVec4& a, const plVec4& b, FN fn)
  {
    return plVec4(fn(a.x, b.x), fn(a.y, b.y), fn(a.z, b.z), fn(a.w, b.w));
  }

  template <typename FN>
  plVec4 PerLane(const plVec4& a, const plVec4& b, const plVec4& c, FN fn)
  {
    return plVec4(fn(a.x, b.x, c.x), fn(a.y, b.y, c.y), fn(a.z, b.z, c.z), fn(a.w, b.w, c.w));
  }

  float SafeDivide(float a, float b)
  {
    return b == 0.0f ? 0.0f : a / b;
  }

  /// The remainder of a / b with the sign of a, as the VM computes it; 0 for b = 0.
  float Modulo(float a, float b)
  {
    return b == 0.0f ? 0.0f : a - b * plMath::Trunc(a / b);
  }

  float SmoothStepLane(float fEdge0, float fEdge1, float x, bool bSmoother)
  {
    const float t = (fEdge1 == fEdge0) ? (x >= fEdge0 ? 1.0f : 0.0f) : plMath::Saturate((x - fEdge0) / (fEdge1 - fEdge0));
    return bSmoother ? t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f) : t * t * (3.0f - 2.0f * t);
  }

  float SignLane(float x)
  {
    return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f);
  }

  /// Samples a table of plParticleProgramTables::s_uiTableSize entries the way the VM function does.
  template <typename T>
  T SampleTable(plArrayPtr<const T> table, float t)
  {
    const plUInt32 n = table.GetCount();
    const float fPos = plMath::Saturate(t) * (n - 1);
    const plUInt32 i0 = plMath::Min((plUInt32)fPos, n - 2);
    return plMath::Lerp(table[i0], table[i0 + 1], fPos - (float)i0);
  }

  // The functions the particle graph adds to Expression code. The parser knows them by these names; the graph
  // replaces each call with what it stands for.
  const plEnum<plExpression::RegisterType> s_OneFloat[] = {plExpression::RegisterType::Float};
  const plEnum<plExpression::RegisterType> s_TwoFloats[] = {plExpression::RegisterType::Float, plExpression::RegisterType::Float};
  const plEnum<plExpression::RegisterType> s_NoiseArguments[] = {plExpression::RegisterType::Float, plExpression::RegisterType::Float, plExpression::RegisterType::Float, plExpression::RegisterType::Float};

  enum class ExpressionCall : plUInt8
  {
    None,
    Random, ///< random(seed): 0..1 per particle
    Noise,  ///< noise(x, y, z[, octaves]): 0..1
    Curve,  ///< curve(t): the node's Curve
    Step,   ///< step(edge, x)
    Sign,   ///< sign(x)
    ATan2,  ///< atan2(y, x), radians
  };

  const plExpression::FunctionDesc& GetExpressionCallDesc(ExpressionCall call)
  {
    static const plExpression::FunctionDesc s_Random = {plMakeHashedString("random"), plExpression::FunctionDesc::TypeList(s_OneFloat), 1, plExpression::RegisterType::Float};
    static const plExpression::FunctionDesc s_Noise = {plMakeHashedString("noise"), plExpression::FunctionDesc::TypeList(s_NoiseArguments), 3, plExpression::RegisterType::Float};
    static const plExpression::FunctionDesc s_Curve = {plMakeHashedString("curve"), plExpression::FunctionDesc::TypeList(s_OneFloat), 1, plExpression::RegisterType::Float};
    static const plExpression::FunctionDesc s_Step = {plMakeHashedString("step"), plExpression::FunctionDesc::TypeList(s_TwoFloats), 2, plExpression::RegisterType::Float};
    static const plExpression::FunctionDesc s_Sign = {plMakeHashedString("sign"), plExpression::FunctionDesc::TypeList(s_OneFloat), 1, plExpression::RegisterType::Float};
    static const plExpression::FunctionDesc s_ATan2 = {plMakeHashedString("atan2"), plExpression::FunctionDesc::TypeList(s_TwoFloats), 2, plExpression::RegisterType::Float};

    switch (call)
    {
      case ExpressionCall::Random:
        return s_Random;
      case ExpressionCall::Noise:
        return s_Noise;
      case ExpressionCall::Curve:
        return s_Curve;
      case ExpressionCall::Step:
        return s_Step;
      case ExpressionCall::Sign:
        return s_Sign;
      default:
        return s_ATan2;
    }
  }

  ExpressionCall GetExpressionCall(const plExpressionAST::Node* pNode)
  {
    if (pNode->m_Type != plExpressionAST::NodeType::FunctionCall)
      return ExpressionCall::None;

    const auto* pCall = static_cast<const plExpressionAST::FunctionCall*>(pNode);
    if (pCall->m_Descs.IsEmpty())
      return ExpressionCall::None;

    const plHashedString& sName = pCall->m_Descs[0]->m_sName;
    for (ExpressionCall call : {ExpressionCall::Random, ExpressionCall::Noise, ExpressionCall::Curve, ExpressionCall::Step, ExpressionCall::Sign, ExpressionCall::ATan2})
    {
      if (sName == GetExpressionCallDesc(call).m_sName)
        return call;
    }

    return ExpressionCall::None;
  }

  /// Rewrites the code's nodes bottom up: fnReplace returns a node to put in place of one, or null to keep it. A
  /// node the code shares through a variable is rewritten once.
  template <typename FN>
  plExpressionAST::Node* RewriteExpression(plExpressionAST::Node* pNode, plHashTable<plExpressionAST::Node*, plExpressionAST::Node*>& ref_done, FN& fnReplace)
  {
    plExpressionAST::Node* pDone = nullptr;
    if (ref_done.TryGetValue(pNode, pDone))
      return pDone;

    for (plExpressionAST::Node*& pChild : plExpressionAST::GetChildren(pNode))
    {
      if (pChild != nullptr)
        pChild = RewriteExpression(pChild, ref_done, fnReplace);
    }

    plExpressionAST::Node* pReplacement = fnReplace(pNode);
    pDone = pReplacement != nullptr ? pReplacement : pNode;
    ref_done[pNode] = pDone;
    return pDone;
  }

  /// The single-letter input of Expression code (a to d) as its index, or -1.
  plInt32 GetExpressionInputIndex(const plExpressionAST::Node* pNode)
  {
    if (pNode->m_Type != plExpressionAST::NodeType::Input)
      return -1;

    const plStringView sName = static_cast<const plExpressionAST::Input*>(pNode)->m_Desc.m_sName.GetView();
    if (sName.GetElementCount() != 1)
      return -1;

    const char c = sName.GetStartPointer()[0];
    return (c >= 'a' && c <= 'd') ? (c - 'a') : -1;
  }
} // namespace

plParticleGraphCompiler::plParticleGraphCompiler(const plVisualGraphObjectManager* pManager, const plDocumentObject* pEffectNode)
  : m_pManager(pManager)
  , m_pEffectNode(pEffectNode)
{
}

plParticleGraphCompiler::~plParticleGraphCompiler() = default;

const plDocumentObject* plParticleGraphCompiler::GetInputSource(const plDocumentObject* pNode, plStringView sPinName, plUInt32* out_pOutput) const
{
  if (out_pOutput != nullptr)
    *out_pOutput = 0;

  const plVisualGraphPin* pPin = m_pManager->GetInputPinByName(pNode, sPinName);
  if (pPin == nullptr)
    return nullptr;

  for (const plVisualGraphConnection* pConnection : m_pManager->GetConnections(*pPin))
  {
    if (out_pOutput != nullptr)
      *out_pOutput = GetOutputIndex(pConnection->GetSourcePin());

    return pConnection->GetSourcePin().GetParent();
  }

  return nullptr;
}

plUInt32 plParticleGraphCompiler::GetOutputIndex(const plVisualGraphPin& sourcePin)
{
  return plParticleOperatorOutputs::FindIndex(sourcePin.GetParent()->GetTypeAccessor().GetType(), sourcePin.GetName());
}

plParticleTier plParticleGraphCompiler::GetTier(const plDocumentObject* pNode)
{
  if (pNode == nullptr)
    return plParticleTier::Constant;

  if (const plParticleTier* pTier = m_Tiers.GetValue(pNode))
    return *pTier;

  // a cycle reads as constant while it is being resolved; it folds to nothing anyway
  m_Tiers[pNode] = plParticleTier::Constant;

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();
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
    // the effect transform changes every step
    if (IsRuntimeSampledNode(pType) || pType->IsDerivedFrom<plParticleTransformNode>())
      tier = plParticleTier::Frame;

    // random values are drawn per particle
    if (pType->IsDerivedFrom<plParticleRandomRangeNode>() || pType->IsDerivedFrom<plParticleRandomIntegerNode>() || pType->IsDerivedFrom<plParticleRandomVectorNode>())
      tier = plParticleTier::Particle;

    if (pType->IsDerivedFrom<plParticleExpressionNode>())
      tier = GetExpressionCallTier(accessor.GetValue("Expression").ConvertTo<plString>());

    // without a wire into T, the Over Life nodes read the particle; an asset is only sampled while the effect runs
    const bool bCurve = pType->IsDerivedFrom<plParticleCurveOverLifeNode>();
    if (bCurve || pType->IsDerivedFrom<plParticleGradientOverLifeNode>())
    {
      if (GetInputSource(pNode, "T") == nullptr)
        tier = plParticleTier::Particle;

      plSet<const plDocumentObject*> visited;
      const plString sAsset = FoldInput(pNode, bCurve ? "Curve Asset" : "Gradient Asset", bCurve ? "CurveAsset" : "GradientAsset", visited).ConvertTo<plString>();
      if (!sAsset.IsEmpty())
        tier = plMath::Max(tier, plParticleTier::Frame);
    }

    for (const auto& input : plParticleOperatorInputs::Get(pType))
    {
      const plParticleTier inputTier = GetTier(GetInputSource(pNode, input.m_szPin));
      tier = plMath::Max(tier, inputTier);
    }
  }

  m_Tiers[pNode] = tier;
  return tier;
}

plParticleValueKind::Enum plParticleGraphCompiler::GetOutputKind(const plDocumentObject* pNode, plUInt32 uiOutput)
{
  if (pNode == nullptr)
    return plParticleValueKind::None;

  const plRTTI* pType = pNode->GetTypeAccessor().GetType();

  const auto outputs = plParticleOperatorOutputs::Get(pType);
  if (uiOutput < outputs.GetCount() && outputs[uiOutput].m_Kind != plParticleValueKind::None)
    return outputs[uiOutput].m_Kind;

  plStringView sValueProperty;
  const plParticleValueKind::Enum staticKind = plParticleValueKind::FromNode(pType, sValueProperty);
  if (!plParticleValueKind::IsAdaptiveNode(pType))
    return staticKind;

  if (const plParticleValueKind::Enum* pKind = m_Kinds.GetValue(pNode))
    return *pKind;

  // a cycle reads as a number while it is being resolved
  m_Kinds[pNode] = plParticleValueKind::Number;

  plParticleValueKind::Enum kind = plParticleValueKind::Number;
  for (const auto& input : plParticleOperatorInputs::Get(pType))
  {
    if (input.m_Kind != plParticleValueKind::Adaptive)
      continue;

    plUInt32 uiSourceOutput = 0;
    if (const plDocumentObject* pSource = GetInputSource(pNode, input.m_szPin, &uiSourceOutput))
      kind = plParticleValueKind::Widen(kind, GetOutputKind(pSource, uiSourceOutput));
  }

  m_Kinds[pNode] = kind;
  return kind;
}

plVariant plParticleGraphCompiler::Fold(const plDocumentObject* pNode, plUInt32 uiOutput)
{
  plSet<const plDocumentObject*> visited;
  return FoldInternal(pNode, visited, uiOutput);
}

plVariant plParticleGraphCompiler::FoldInput(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plSet<const plDocumentObject*>& ref_visited)
{
  plUInt32 uiOutput = 0;
  if (const plDocumentObject* pSource = GetInputSource(pNode, szPin, &uiOutput))
  {
    const plVariant value = FoldInternal(pSource, ref_visited, uiOutput);
    if (value.IsValid())
      return value;
  }

  return szLiteral != nullptr ? pNode->GetTypeAccessor().GetValue(szLiteral) : plVariant();
}

plVec4 plParticleGraphCompiler::FoldLanes(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plParticleValueKind::Enum kind, plSet<const plDocumentObject*>& ref_visited)
{
  return ToLanes(FoldInput(pNode, szPin, szLiteral, ref_visited), kind);
}

plVariant plParticleGraphCompiler::FoldInternal(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, plUInt32 uiOutput)
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

  // the adaptive maths, and the vector, logic, random and curve nodes
  bool bHandled = false;
  const plVariant newValue = FoldNewNode(pNode, ref_visited, uiOutput, bHandled);
  if (bHandled)
    return newValue;

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
    const bool bLifetime = iInput == plParticleInputValue::Lifetime || iInput == plParticleInputValue::RemainingLife;
    return plVariant(bLifetime ? m_fSystemLifetime : (iInput == plParticleInputValue::Size ? 1.0 : 0.0));
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

plVariant plParticleGraphCompiler::FoldNewNode(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, plUInt32 uiOutput, bool& out_bHandled)
{
  out_bHandled = true;

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();
  const plInt64 iOperation = accessor.GetValue("Operation").IsValid() ? accessor.GetValue("Operation").ConvertTo<plInt64>() : 0;

  auto number = [&](const char* szPin, const char* szLiteral) -> float
  { return (float)ToDouble(FoldInput(pNode, szPin, szLiteral, ref_visited)); };

  auto vector = [&](const char* szPin) -> plVec3
  { return FoldLanes(pNode, szPin, szPin, plParticleValueKind::Vector, ref_visited).GetAsVec3(); };

  auto condition = [&](const char* szPin, const char* szLiteral) -> bool
  {
    const plVariant value = FoldInput(pNode, szPin, szLiteral, ref_visited);
    return value.IsValid() && value.ConvertTo<bool>();
  };

  // adaptive nodes work on four lanes and give the result in the widest kind wired into them
  if (plParticleValueKind::IsAdaptiveNode(pType))
  {
    const plParticleValueKind::Enum kind = GetOutputKind(pNode);
    auto lanes = [&](const char* szPin, const char* szLiteral) -> plVec4
    { return FoldLanes(pNode, szPin, szLiteral, kind, ref_visited); };

    plVec4 result = plVec4::MakeZero();

    if (pType->IsDerivedFrom<plParticleMathNode>())
    {
      const plVec4 a = lanes("A", "A");
      const plVec4 b = lanes("B", "B");

      switch (iOperation)
      {
        case plParticleMathOp::Add:
          result = a + b;
          break;
        case plParticleMathOp::Subtract:
          result = a - b;
          break;
        case plParticleMathOp::Divide:
          result = PerLane(a, b, [](float x, float y)
            { return SafeDivide(x, y); });
          break;
        case plParticleMathOp::Min:
          result = a.CompMin(b);
          break;
        case plParticleMathOp::Max:
          result = a.CompMax(b);
          break;
        case plParticleMathOp::Modulo:
          result = PerLane(a, b, [](float x, float y)
            { return Modulo(x, y); });
          break;
        default:
          result = a.CompMul(b);
          break;
      }
    }
    else if (pType->IsDerivedFrom<plParticleLerpNode>())
    {
      const plVec4 a = lanes("A", "A");
      result = a + (lanes("B", "B") - a).CompMul(lanes("T", "T"));
    }
    else if (pType->IsDerivedFrom<plParticleClampNode>())
    {
      // max(min, min(max, value)), the order the program computes it in
      result = lanes("Min", "Min").CompMax(lanes("Max", "Max").CompMin(lanes("Value", "Value")));
    }
    else if (pType->IsDerivedFrom<plParticleBranchNode>())
    {
      result = condition("Condition", nullptr) ? lanes("True", "True") : lanes("False", "False");
    }
    else if (pType->IsDerivedFrom<plParticleRemapNode>())
    {
      const plVec4 inMin = lanes("In Min", "InMin");
      const plVec4 outMin = lanes("Out Min", "OutMin");
      const bool bClamp = accessor.GetValue("Clamp").ConvertTo<bool>();

      plVec4 t = PerLane(lanes("Value", "Value") - inMin, lanes("In Max", "InMax") - inMin, [](float x, float y)
        { return SafeDivide(x, y); });
      if (bClamp)
        t = PerLane(t, [](float x)
          { return plMath::Saturate(x); });

      result = outMin + (lanes("Out Max", "OutMax") - outMin).CompMul(t);
    }
    else if (pType->IsDerivedFrom<plParticlePowerNode>())
    {
      result = PerLane(lanes("Base", "Base"), lanes("Exponent", "Exponent"), [](float x, float y)
        { return plMath::Pow(x, y); });
    }
    else if (pType->IsDerivedFrom<plParticleStepNode>())
    {
      result = PerLane(lanes("Edge", "Edge"), lanes("X", "X"), [](float fEdge, float x)
        { return x >= fEdge ? 1.0f : 0.0f; });
    }
    else if (pType->IsDerivedFrom<plParticleSmoothStepNode>())
    {
      const bool bSmoother = accessor.GetValue("Smoother").ConvertTo<bool>();
      result = PerLane(lanes("Edge 0", "Edge0"), lanes("Edge 1", "Edge1"), lanes("X", "X"), [bSmoother](float e0, float e1, float x)
        { return SmoothStepLane(e0, e1, x, bSmoother); });
    }
    else if (pType->IsDerivedFrom<plParticleRandomRangeNode>())
    {
      // a random value stands in at the middle of its range
      const plVec4 fMin = lanes("Min", "Min");
      result = fMin + (lanes("Max", "Max") - fMin) * 0.5f;
    }
    else if (pType->IsDerivedFrom<plParticleFunctionNode>())
    {
      const bool bRadians = accessor.GetValue("Radians").ConvertTo<bool>();
      const float fToRadians = bRadians ? 1.0f : plAngle::DegToRadMultiplier<float>();
      const float fFromRadians = bRadians ? 1.0f : plAngle::RadToDegMultiplier<float>();

      result = PerLane(lanes("Value", "Value"), [&](float x) -> float
        {
          switch (iOperation)
          {
            case plParticleFunctionOp::Cosine:
              return plMath::Cos(plAngle::MakeFromRadian(x * fToRadians));
            case plParticleFunctionOp::Tan:
              return plMath::Tan(plAngle::MakeFromRadian(x * fToRadians));
            case plParticleFunctionOp::ASin:
              return plMath::ASin(x).GetRadian() * fFromRadians;
            case plParticleFunctionOp::ACos:
              return plMath::ACos(x).GetRadian() * fFromRadians;
            case plParticleFunctionOp::ATan:
              return plMath::ATan(x).GetRadian() * fFromRadians;
            case plParticleFunctionOp::Abs:
              return plMath::Abs(x);
            case plParticleFunctionOp::Floor:
              return plMath::Floor(x);
            case plParticleFunctionOp::Ceil:
              return plMath::Ceil(x);
            case plParticleFunctionOp::Frac:
              return x - plMath::Floor(x);
            case plParticleFunctionOp::Sqrt:
              return plMath::Sqrt(plMath::Max(x, 0.0f));
            case plParticleFunctionOp::Saturate:
              return plMath::Saturate(x);
            case plParticleFunctionOp::Negate:
              return -x;
            case plParticleFunctionOp::OneMinus:
              return 1.0f - x;
            case plParticleFunctionOp::Exp:
              return plMath::Exp(x);
            case plParticleFunctionOp::Log:
              return plMath::Ln(x);
            case plParticleFunctionOp::Log2:
              return plMath::Log2(x);
            case plParticleFunctionOp::Round:
              return plMath::Round(x);
            case plParticleFunctionOp::Trunc:
              return plMath::Trunc(x);
            case plParticleFunctionOp::Sign:
              return SignLane(x);
            case plParticleFunctionOp::Reciprocal:
              return SafeDivide(1.0f, x);
            case plParticleFunctionOp::Square:
              return x * x;
            case plParticleFunctionOp::DegreesToRadians:
              return x * plAngle::DegToRadMultiplier<float>();
            case plParticleFunctionOp::RadiansToDegrees:
              return x * plAngle::RadToDegMultiplier<float>();
            default:
              return plMath::Sin(plAngle::MakeFromRadian(x * fToRadians));
          }
        });
    }

    return FromLanes(result, kind);
  }

  if (pType->IsDerivedFrom<plParticleSplitVectorNode>())
    return plVariant((double)FoldLanes(pNode, "Vector", "Vector", plParticleValueKind::Vector, ref_visited).GetData()[plMath::Min(uiOutput, 2u)]);

  if (pType->IsDerivedFrom<plParticleSplitColorNode>())
    return plVariant((double)FoldLanes(pNode, "Colour", "Colour", plParticleValueKind::Color, ref_visited).GetData()[plMath::Min(uiOutput, 3u)]);

  if (pType->IsDerivedFrom<plParticleMakeColorNode>())
    return plVariant(plColor(number("R", "R"), number("G", "G"), number("B", "B"), number("A", "A")));

  if (pType->IsDerivedFrom<plParticleSwizzleNode>())
  {
    plUInt32 components[3] = {};
    if (!plParticleSwizzleNode::ParsePattern(accessor.GetValue("Pattern").ConvertTo<plString>(), components))
      return plVariant();

    const plVec3 v = vector("Vector");
    return plVariant(plVec3(v.GetData()[components[0]], v.GetData()[components[1]], v.GetData()[components[2]]));
  }

  if (pType->IsDerivedFrom<plParticleDistanceNode>())
    return plVariant((double)(vector("A") - vector("B")).GetLength());

  if (pType->IsDerivedFrom<plParticleCrossNode>())
    return plVariant(vector("A").CrossRH(vector("B")));

  if (pType->IsDerivedFrom<plParticleReflectNode>())
  {
    const plVec3 d = vector("Direction");
    const plVec3 n = vector("Normal");
    return plVariant(d - n * (2.0f * d.Dot(n)));
  }

  if (pType->IsDerivedFrom<plParticleProjectNode>())
  {
    const plVec3 v = vector("Vector");
    const plVec3 onto = vector("Onto");
    return plVariant(onto * SafeDivide(v.Dot(onto), onto.Dot(onto)));
  }

  if (pType->IsDerivedFrom<plParticleRotateAboutAxisNode>())
  {
    const plVec3 v = vector("Vector");
    const plVec3 axis = vector("Axis");
    const float fLength = axis.GetLength();
    if (fLength <= 0.0f)
      return plVariant(v);

    const plVec3 k = axis / fLength;
    const plAngle angle = plAngle::MakeFromDegree(number("Degrees", "Degrees"));
    const float c = plMath::Cos(angle);
    const float s = plMath::Sin(angle);
    return plVariant(v * c + k.CrossRH(v) * s + k * (k.Dot(v) * (1.0f - c)));
  }

  if (pType->IsDerivedFrom<plParticleInverseLerpNode>())
  {
    const float a = number("A", "A");
    float t = SafeDivide(number("Value", "Value") - a, number("B", "B") - a);
    if (accessor.GetValue("Clamp").ConvertTo<bool>())
      t = plMath::Saturate(t);
    return plVariant((double)t);
  }

  if (pType->IsDerivedFrom<plParticleATan2Node>())
  {
    const float y = number("Y", "Y");
    const float x = number("X", "X");
    return plVariant((x == 0.0f && y == 0.0f) ? 0.0 : (double)plMath::ATan2(y, x).GetDegree());
  }

  if (pType->IsDerivedFrom<plParticleMathConstantNode>())
    return plVariant(plParticleMathConstant::GetValue(accessor.GetValue("Constant").ConvertTo<plInt64>()));

  if (pType->IsDerivedFrom<plParticleLogicNode>())
  {
    const bool a = condition("A", "A");
    const bool b = condition("B", "B");

    switch (iOperation)
    {
      case plParticleLogicOp::Or:
        return plVariant(a || b);
      case plParticleLogicOp::Xor:
        return plVariant(a != b);
      default:
        return plVariant(a && b);
    }
  }

  if (pType->IsDerivedFrom<plParticleNotNode>())
    return plVariant(!condition("Value", "Value"));

  // random values stand in at the middle of their range
  if (pType->IsDerivedFrom<plParticleRandomIntegerNode>())
  {
    const float fMin = number("Min", "Min");
    const float fMax = number("Max", "Max");
    const float fLow = plMath::Round(plMath::Min(fMin, fMax));
    const float fHigh = plMath::Round(plMath::Max(fMin, fMax));
    return plVariant((double)plMath::Min(plMath::Floor(fLow + 0.5f * (fHigh - fLow + 1.0f)), fHigh));
  }

  if (pType->IsDerivedFrom<plParticleRandomVectorNode>() || pType->IsDerivedFrom<plParticleVectorNoiseNode>())
    return plVariant(plVec3::MakeZero());

  // the effect frame starts out as the identity
  if (pType->IsDerivedFrom<plParticleTransformNode>())
    return plVariant(vector("Vector"));

  if (pType->IsDerivedFrom<plParticleCurveOverLifeNode>())
  {
    const float fMin = number("Output Min", "OutputMin");
    const float fMax = number("Output Max", "OutputMax");

    // an asset is only known to the runtime; it starts at Output Min
    if (!FoldInput(pNode, "Curve Asset", "CurveAsset", ref_visited).ConvertTo<plString>().IsEmpty())
      return plVariant((double)fMin);

    constexpr plUInt32 n = plParticleProgramTables::s_uiTableSize;
    float table[n];
    BakeInlineCurve(pNode, plArrayPtr<float>(table, n));

    const float t = GetInputSource(pNode, "T") != nullptr ? number("T", nullptr) : 0.0f;
    return plVariant((double)plMath::Lerp(fMin, fMax, SampleTable(plArrayPtr<const float>(table, n), t)));
  }

  if (pType->IsDerivedFrom<plParticleGradientOverLifeNode>())
  {
    if (!FoldInput(pNode, "Gradient Asset", "GradientAsset", ref_visited).ConvertTo<plString>().IsEmpty())
      return plVariant(plColor::White);

    constexpr plUInt32 n = plParticleProgramTables::s_uiTableSize;
    plVec4 table[n];
    BakeInlineGradient(pNode, plArrayPtr<plVec4>(table, n));

    const float t = GetInputSource(pNode, "T") != nullptr ? number("T", nullptr) : 0.0f;
    const plVec4 c = SampleTable(plArrayPtr<const plVec4>(table, n), t);
    return plVariant(plColor(c.x, c.y, c.z, c.w));
  }

  out_bHandled = false;
  return plVariant();
}

plString plParticleGraphCompiler::Describe(const plDocumentObject* pNode, plUInt32 uiDepth, plUInt32 uiOutput) const
{
  if (pNode == nullptr)
    return "?";

  if (uiDepth == 0)
    return "...";

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  auto input = [&](const char* szPin, const char* szLiteral) -> plString
  {
    plUInt32 uiSourceOutput = 0;
    if (const plDocumentObject* pSource = GetInputSource(pNode, szPin, &uiSourceOutput))
      return Describe(pSource, uiDepth - 1, uiSourceOutput);

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
    plUInt32 uiSourceOutput = 0;
    if (const plDocumentObject* pSource = GetInputSource(pNode, "Value", &uiSourceOutput))
      s.SetFormat("{}.{}", Describe(pSource, uiDepth - 1, uiSourceOutput), sChannel);
    else
      s.SetFormat("Color.{}", sChannel);
    return s;
  }

  // "Velocity.Z", "Colour.A": the input and the component the output takes
  const bool bSplitVector = pType->IsDerivedFrom<plParticleSplitVectorNode>();
  if (bSplitVector || pType->IsDerivedFrom<plParticleSplitColorNode>())
  {
    const auto outputs = plParticleOperatorOutputs::Get(pType);
    const char* szComponent = outputs[plMath::Min(uiOutput, outputs.GetCount() - 1)].m_szPin;
    const char* szPin = bSplitVector ? "Vector" : "Colour";

    plUInt32 uiSourceOutput = 0;
    if (const plDocumentObject* pSource = GetInputSource(pNode, szPin, &uiSourceOutput))
      s.SetFormat("{}.{}", Describe(pSource, uiDepth - 1, uiSourceOutput), szComponent);
    else
      s.SetFormat("{}.{}", szPin, szComponent);
    return s;
  }

  if (pType->IsDerivedFrom<plParticleSwizzleNode>())
  {
    s.SetFormat("{}.{}", input("Vector", "Vector"), accessor.GetValue("Pattern").ConvertTo<plString>());
    return s;
  }

  if (pType->IsDerivedFrom<plParticleMathConstantNode>())
    return GetEnumName(plGetStaticRTTI<plParticleMathConstant>(), accessor.GetValue("Constant").ConvertTo<plInt64>());

  if (pType->IsDerivedFrom<plParticleCurveOverLifeNode>() || pType->IsDerivedFrom<plParticleGradientOverLifeNode>())
  {
    // "Curve(Normalized Age)", "Gradient(Speed)"
    const char* szKind = pType->IsDerivedFrom<plParticleCurveOverLifeNode>() ? "Curve" : "Gradient";
    plUInt32 uiSourceOutput = 0;
    if (const plDocumentObject* pSource = GetInputSource(pNode, "T", &uiSourceOutput))
      s.SetFormat("{}({})", szKind, Describe(pSource, uiDepth - 1, uiSourceOutput));
    else
      s.SetFormat("{}({})", szKind, plParticleEffectNodeManager::SplitWords(GetEnumName(plGetStaticRTTI<plParticleCurveInput>(), accessor.GetValue("Input").ConvertTo<plInt64>())));
    return s;
  }

  // everything else: its title, and its first wired input
  s = plParticleGraphEditing::GetOperatorTitle(pNode);
  for (const auto& in : plParticleOperatorInputs::Get(pType))
  {
    plUInt32 uiSourceOutput = 0;
    if (const plDocumentObject* pSource = GetInputSource(pNode, in.m_szPin, &uiSourceOutput))
    {
      s.AppendFormat("({})", Describe(pSource, uiDepth - 1, uiSourceOutput));
      break;
    }
  }

  return s;
}

plString plParticleGraphCompiler::GetIssue(const plDocumentObject* pNode)
{
  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  plSet<const plDocumentObject*> visited;

  if (pType->IsDerivedFrom<plParticleCustomAttributeNode>() && accessor.GetValue("Name").ConvertTo<plString>().IsEmpty())
    return "Name the custom attribute to read, as a Set Attribute block names it.";

  // the asset can also come from a Gradient or Curve node on the pin
  if (pType->IsDerivedFrom<plParticleSampleGradientNode>() && FoldInput(pNode, "Gradient", "Gradient", visited).ConvertTo<plString>().IsEmpty())
    return "Pick a gradient to sample, or wire a Gradient node into it.";

  if (pType->IsDerivedFrom<plParticleSampleCurveNode>() && FoldInput(pNode, "Curve", "Curve", visited).ConvertTo<plString>().IsEmpty())
    return "Pick a curve to sample, or wire a Curve node into it.";

  if (pType->IsDerivedFrom<plParticleSwizzleNode>())
  {
    plUInt32 components[3] = {};
    if (!plParticleSwizzleNode::ParsePattern(accessor.GetValue("Pattern").ConvertTo<plString>(), components))
      return "The pattern needs three of x, y and z, such as zyx or xxz.";
  }

  if (pType->IsDerivedFrom<plParticleExpressionNode>())
  {
    const plString sCode = accessor.GetValue("Expression").ConvertTo<plString>();
    if (sCode.IsEmpty())
      return "Type the expression, using a, b, c and d for the inputs.";

    plExpressionAST ast;
    plString sError;
    bool bFailed = ParseExpression(sCode, ast, &sError).Failed();
    bool bCallsCurve = false;

    if (!bFailed && ast.m_OutputNodes.GetCount() == 1 && ast.m_OutputNodes[0]->m_pExpression != nullptr)
    {
      // the particle functions become what the program runs, so the compile below checks them too
      plExpressionAST::Node* pRandomSeed = ast.CreateInput({plMakeHashedString("RandomSeed"), plProcessingStream::DataType::Int});
      ast.m_InputNodes.PushBack(static_cast<plExpressionAST::Input*>(pRandomSeed));

      plHashTable<plExpressionAST::Node*, plExpressionAST::Node*> done;
      auto replace = [&](plExpressionAST::Node* pCodeNode) -> plExpressionAST::Node*
      {
        const ExpressionCall call = GetExpressionCall(pCodeNode);
        if (call == ExpressionCall::None)
          return nullptr;

        auto arguments = static_cast<plExpressionAST::FunctionCall*>(pCodeNode)->m_Arguments.GetArrayPtr();
        switch (call)
        {
          case ExpressionCall::Random:
          {
            plExpressionAST::Node* callArguments[] = {pRandomSeed, ast.CreateUnaryOperator(plExpressionAST::NodeType::TypeConversion, arguments[0], DataType::Int)};
            return ast.CreateFunctionCall(plDefaultExpressionFunctions::s_RandomFunc.m_Desc, callArguments);
          }
          case ExpressionCall::Noise:
            return plParticleProgramOps::Noise(ast, ast.CreateConstructorCall(DataType::Float3, arguments.GetSubArray(0, 3)), plParticleProgramOps::Number(ast, 1.0f), arguments.GetCount() > 3 ? arguments[3] : plParticleProgramOps::Number(ast, 1.0f));
          case ExpressionCall::Curve:
            bCallsCurve = true;
            return plParticleProgramOps::SampleCurve(ast, arguments[0], ast.CreateConstant(plVariant(0), DataType::Int));
          case ExpressionCall::Step:
            return plParticleProgramOps::Step(ast, arguments[0], arguments[1]);
          case ExpressionCall::Sign:
            return plParticleProgramOps::Sign(ast, arguments[0]);
          default:
            return plParticleProgramOps::ATan2(ast, arguments[0], arguments[1]);
        }
      };

      ast.m_OutputNodes[0]->m_pExpression = RewriteExpression(ast.m_OutputNodes[0]->m_pExpression, done, replace);

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

    if (bCallsCurve && FoldInput(pNode, "Curve", "Curve", visited).ConvertTo<plString>().IsEmpty())
      return "curve() samples the node's Curve: pick one, or wire a Curve node into it.";
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

  // an input that gets a kind it cannot take, which a wider adaptive node upstream can come to produce
  for (const auto& input : plParticleOperatorInputs::Get(pType))
  {
    plUInt32 uiSourceOutput = 0;
    const plDocumentObject* pSource = GetInputSource(pNode, input.m_szPin, &uiSourceOutput);
    if (pSource == nullptr)
      continue;

    const plParticleValueKind::Enum sourceKind = GetOutputKind(pSource, uiSourceOutput);
    if (!plParticleValueKind::CanConvert(sourceKind, input.m_Kind))
    {
      plStringBuilder s;
      s.SetFormat("Its {} input gets a {}, which it cannot take.", input.m_szPin, plParticleValueKind::GetName(sourceKind));
      return s;
    }
  }

  // the wires into blocks: a property that takes another kind, or a Set Attribute value its target does not read
  const auto outputs = m_pManager->GetOutputPins(pNode);
  for (plUInt32 uiOutput = 0; uiOutput < outputs.GetCount(); ++uiOutput)
  {
    for (const plVisualGraphConnection* pConnection : m_pManager->GetConnections(*outputs[uiOutput]))
    {
      const plVisualGraphPin& target = pConnection->GetTargetPin();

      plStringBuilder sProperty;
      const plDocumentObject* pBlock = plParticlePropertyPin::FindBlock(target.GetParent(), target.GetName(), sProperty);
      if (pBlock == nullptr)
        continue;

      const auto& blockAccessor = pBlock->GetTypeAccessor();
      const plAbstractProperty* pProp = blockAccessor.GetType()->FindPropertyByName(sProperty);
      const plParticleValueKind::Enum kind = GetOutputKind(pNode, GetOutputIndex(*outputs[uiOutput]));

      if (pProp != nullptr && !plParticleValueKind::CanConvert(kind, plParticleValueKind::FromProperty(pProp)))
      {
        plStringBuilder s;
        s.SetFormat("It gives a {}, but {} takes a {}.", plParticleValueKind::GetName(kind), plTranslate(sProperty), plParticleValueKind::GetName(plParticleValueKind::FromProperty(pProp)));
        return s;
      }

      const plRTTI* pBlockType = blockAccessor.GetType();
      if (!pBlockType->IsDerivedFrom<plParticleInitializerFactory_SetAttribute>() && !pBlockType->IsDerivedFrom<plParticleBehaviorFactory_Attribute>())
        continue;

      plParticleAttributeSinkSettings settings;
      settings.m_Target = (plParticleSinkTarget::Enum)blockAccessor.GetValue("Target").ConvertTo<plInt64>();
      settings.m_Component = (plParticleSinkComponent::Enum)blockAccessor.GetValue("Component").ConvertTo<plInt64>();
      settings.m_CustomType = (plParticleCustomAttributeType::Enum)blockAccessor.GetValue("CustomType").ConvertTo<plInt64>();

      const plStringView sValue = settings.GetValuePropertyName();
      const bool bValueProperty = sProperty == "Number" || sProperty == "Vector" || sProperty == "Color";
      if (bValueProperty && sProperty != sValue)
      {
        plStringBuilder s;
        s.SetFormat("This wire goes into Set Attribute's {}, but its target {} reads {}, so the wire is ignored. Wire it into {} or change the target.", sProperty,
          plParticleGraphEditing::GetSinkTargetLabel(pBlock), sValue, sValue);
        return s;
      }
    }
  }

  return {};
}

void plParticleGraphCompiler::CollectSinkTargets(const plDocumentObject* pNode, plDynamicArray<SinkTarget>& out_targets) const
{
  plSet<const plDocumentObject*> visited;
  plHybridArray<const plDocumentObject*, 8> pending;
  pending.PushBack(pNode);

  while (!pending.IsEmpty())
  {
    const plDocumentObject* pCurrent = pending.PeekBack();
    pending.PopBack();

    if (visited.Contains(pCurrent))
      continue;
    visited.Insert(pCurrent);

    for (const auto& pOutput : m_pManager->GetOutputPins(pCurrent))
    {
      for (const plVisualGraphConnection* pConnection : m_pManager->GetConnections(*pOutput))
      {
        const plVisualGraphPin& target = pConnection->GetTargetPin();

        plStringBuilder sProperty;
        const plDocumentObject* pBlock = plParticlePropertyPin::FindBlock(target.GetParent(), target.GetName(), sProperty);
        if (pBlock == nullptr)
        {
          pending.PushBack(target.GetParent());
          continue;
        }

        const plRTTI* pBlockType = pBlock->GetTypeAccessor().GetType();
        const bool bInitialize = pBlockType->IsDerivedFrom<plParticleInitializerFactory_SetAttribute>();
        if (!bInitialize && !pBlockType->IsDerivedFrom<plParticleBehaviorFactory_Attribute>())
          continue;

        const plInt64 iSimulation = target.GetParent()->GetTypeAccessor().GetValue("SimulationTarget").ConvertTo<plInt64>();

        SinkTarget& sink = out_targets.ExpandAndGetRef();
        sink.m_iTarget = pBlock->GetTypeAccessor().GetValue("Target").ConvertTo<plInt64>();
        sink.m_bInitialize = bInitialize;
        sink.m_bGpuSystem = iSimulation != plParticleSimulationTarget::CPU;
      }
    }
  }
}

plString plParticleGraphCompiler::GetNote(const plDocumentObject* pNode)
{
  const plRTTI* pType = pNode->GetTypeAccessor().GetType();
  if (!pType->IsDerivedFrom<plParticleCurveOverLifeNode>() && !pType->IsDerivedFrom<plParticleGradientOverLifeNode>())
    return {};

  plDynamicArray<SinkTarget> targets;
  CollectSinkTargets(pNode, targets);

  auto targetName = [](plInt64 iTarget) -> plString
  { return plParticleEffectNodeManager::SplitWords(GetEnumName(plGetStaticRTTI<plParticleSinkTarget>(), iTarget)); };

  // the targets GPU systems keep no data for say so instead of the system quietly staying on the CPU
  for (const SinkTarget& target : targets)
  {
    const bool bCpuOnly = target.m_iTarget == plParticleSinkTarget::Rotation || target.m_iTarget == plParticleSinkTarget::Facing ||
                          target.m_iTarget == plParticleSinkTarget::SizeScale || target.m_iTarget == plParticleSinkTarget::Emissive ||
                          target.m_iTarget == plParticleSinkTarget::Custom;

    if (target.m_bGpuSystem && bCpuOnly)
    {
      plStringBuilder s;
      s.SetFormat("{} keeps this system on the CPU: GPU systems have no {}. Size, Opacity, Colour, Rotation Speed, Position and Velocity run on the GPU.", targetName(target.m_iTarget), targetName(target.m_iTarget));
      return s;
    }
  }

  for (const SinkTarget& target : targets)
  {
    if (target.m_bGpuSystem)
    {
      plStringBuilder s;
      s.SetFormat("{} runs on the GPU, from the same table as on the CPU.", targetName(target.m_iTarget));
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
      return {Vector(ToLanes(value, kind).GetAsVec3()), kind};
    case plParticleValueKind::Color:
    {
      // an invalid value is the white a colour pin starts at
      const plVec4 c = value.IsValid() ? ToLanes(value, kind) : plVec4(1.0f);
      return {Color(plColor(c.x, c.y, c.z, c.w)), kind};
    }
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

  // a number fills every component of a vector or colour
  if ((value.m_Kind == plParticleValueKind::Number || value.m_Kind == plParticleValueKind::Bool) && (kind == plParticleValueKind::Vector || kind == plParticleValueKind::Color))
  {
    const AstValue number = Convert(value, plParticleValueKind::Number);
    return {plParticleProgramOps::Broadcast(m_Ast, number.m_pNode, kind == plParticleValueKind::Color ? 4 : 3), kind};
  }

  // a vector becomes an opaque colour, a colour a vector without its alpha
  if (value.m_Kind == plParticleValueKind::Vector && kind == plParticleValueKind::Color)
  {
    plExpressionAST::Node* components[] = {m_Ast.CreateSwizzle(plExpressionAST::VectorComponent::X, value.m_pNode), m_Ast.CreateSwizzle(plExpressionAST::VectorComponent::Y, value.m_pNode),
      m_Ast.CreateSwizzle(plExpressionAST::VectorComponent::Z, value.m_pNode), Number(1.0)};
    return {m_Ast.CreateConstructorCall(DataType::Float4, components), kind};
  }

  if (value.m_Kind == plParticleValueKind::Color && kind == plParticleValueKind::Vector)
    return {m_Ast.CreateSwizzle("xyz", value.m_pNode), kind};

  return {};
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::BuildInput(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plParticleValueKind::Enum kind, plSet<const plDocumentObject*>& ref_visited)
{
  plUInt32 uiOutput = 0;
  if (const plDocumentObject* pSource = GetInputSource(pNode, szPin, &uiOutput))
  {
    // a constant subgraph needs no instructions: it folds right here
    if (GetTier(pSource) == plParticleTier::Constant)
      return Constant(FoldInternal(pSource, ref_visited, uiOutput), kind);

    return Convert(Build(pSource, ref_visited, uiOutput), kind);
  }

  if (szLiteral == nullptr)
    return {};

  return Constant(pNode->GetTypeAccessor().GetValue(szLiteral), kind);
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::Build(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, plUInt32 uiOutput)
{
  if (pNode == nullptr || ref_visited.Contains(pNode) || uiOutput >= PL_ARRAY_SIZE(BuiltNode::m_Outputs))
    return {};

  if (const BuiltNode* pBuilt = m_Built.GetValue(pNode))
    return pBuilt->m_Outputs[uiOutput];

  ref_visited.Insert(pNode);
  PL_SCOPE_EXIT(ref_visited.Remove(pNode));

  BuiltNode built;
  BuildNode(pNode, ref_visited, built);

  for (AstValue& value : built.m_Outputs)
  {
    if (value.m_pNode == nullptr)
      value = {};
  }

  m_Built[pNode] = built;
  return built.m_Outputs[uiOutput];
}

void plParticleGraphCompiler::BuildNode(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, BuiltNode& out_built)
{
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

  AstValue result;

  // the adaptive maths, and the vector, logic, random and curve nodes
  bool bHandled = false;
  const AstValue newValue = BuildNewNode(pNode, ref_visited, out_built, bHandled);

  if (bHandled)
  {
    result = newValue;
  }
  else if (IsParticleInputNode(pType))
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
  else if (pType->IsDerivedFrom<plParticleCompareNode>())
  {
    static const NodeType::Enum s_Compare[] = {NodeType::Less, NodeType::LessEqual, NodeType::Greater, NodeType::GreaterEqual, NodeType::Equal, NodeType::NotEqual};
    const NodeType::Enum type = (iOperation >= 0 && iOperation < (plInt64)PL_ARRAY_SIZE(s_Compare)) ? s_Compare[iOperation] : NodeType::Greater;
    result = {binary(type, number("A"), number("B")), plParticleValueKind::Bool};
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

  // the Split nodes fill their outputs themselves
  if (out_built.m_Outputs[0].m_pNode == nullptr)
    out_built.m_Outputs[0] = result;
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::BuildNewNode(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, BuiltNode& out_built, bool& out_bHandled)
{
  namespace Ops = plParticleProgramOps;

  out_bHandled = true;

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();
  const plInt64 iOperation = accessor.GetValue("Operation").IsValid() ? accessor.GetValue("Operation").ConvertTo<plInt64>() : 0;

  constexpr plParticleValueKind::Enum kNumber = plParticleValueKind::Number;
  constexpr plParticleValueKind::Enum kVector = plParticleValueKind::Vector;
  constexpr plParticleValueKind::Enum kColor = plParticleValueKind::Color;

  auto input = [&](const char* szPin, const char* szLiteral, plParticleValueKind::Enum kind) -> plExpressionAST::Node*
  { return BuildInput(pNode, szPin, szLiteral, kind, ref_visited).m_pNode; };

  auto valid = [](std::initializer_list<plExpressionAST::Node*> nodes) -> bool
  {
    for (plExpressionAST::Node* pArgument : nodes)
    {
      if (pArgument == nullptr)
        return false;
    }
    return true;
  };

  auto binary = [&](NodeType::Enum type, plExpressionAST::Node* a, plExpressionAST::Node* b) -> plExpressionAST::Node*
  { return m_Ast.CreateBinaryOperator(type, a, b); };

  auto unary = [&](NodeType::Enum type, plExpressionAST::Node* a) -> plExpressionAST::Node*
  { return m_Ast.CreateUnaryOperator(type, a); };

  // the adaptive nodes compute in the widest kind wired into them; their inputs are converted to it
  if (plParticleValueKind::IsAdaptiveNode(pType))
  {
    const plParticleValueKind::Enum kind = GetOutputKind(pNode);
    auto any = [&](const char* szPin, const char* szLiteral) -> plExpressionAST::Node*
    { return input(szPin, szLiteral, kind); };

    plExpressionAST::Node* pResult = nullptr;

    if (pType->IsDerivedFrom<plParticleMathNode>())
    {
      plExpressionAST::Node* a = any("A", "A");
      plExpressionAST::Node* b = any("B", "B");
      if (!valid({a, b}))
        return {};

      switch (iOperation)
      {
        case plParticleMathOp::Add:
          pResult = binary(NodeType::Add, a, b);
          break;
        case plParticleMathOp::Subtract:
          pResult = binary(NodeType::Subtract, a, b);
          break;
        case plParticleMathOp::Divide:
          // a zero divisor gives zero, as the folded value does
          pResult = Ops::SafeDivide(m_Ast, a, b);
          break;
        case plParticleMathOp::Min:
          pResult = binary(NodeType::Min, a, b);
          break;
        case plParticleMathOp::Max:
          pResult = binary(NodeType::Max, a, b);
          break;
        case plParticleMathOp::Modulo:
          pResult = m_Ast.CreateTernaryOperator(NodeType::Select, binary(NodeType::Equal, b, Number(0.0)), Number(0.0), binary(NodeType::Modulo, a, b));
          break;
        default:
          pResult = binary(NodeType::Multiply, a, b);
          break;
      }
    }
    else if (pType->IsDerivedFrom<plParticleLerpNode>())
    {
      plExpressionAST::Node* a = any("A", "A");
      plExpressionAST::Node* b = any("B", "B");
      plExpressionAST::Node* t = any("T", "T");
      if (!valid({a, b, t}))
        return {};

      pResult = m_Ast.CreateTernaryOperator(NodeType::Lerp, a, b, t);
    }
    else if (pType->IsDerivedFrom<plParticleClampNode>())
    {
      plExpressionAST::Node* v = any("Value", "Value");
      plExpressionAST::Node* fMin = any("Min", "Min");
      plExpressionAST::Node* fMax = any("Max", "Max");
      if (!valid({v, fMin, fMax}))
        return {};

      pResult = m_Ast.CreateTernaryOperator(NodeType::Clamp, v, fMin, fMax);
    }
    else if (pType->IsDerivedFrom<plParticleBranchNode>())
    {
      plExpressionAST::Node* pCondition = input("Condition", nullptr, plParticleValueKind::Bool);
      plExpressionAST::Node* pTrue = any("True", "True");
      plExpressionAST::Node* pFalse = any("False", "False");
      if (!valid({pCondition, pTrue, pFalse}))
        return {};

      pResult = m_Ast.CreateTernaryOperator(NodeType::Select, pCondition, pTrue, pFalse);
    }
    else if (pType->IsDerivedFrom<plParticleRemapNode>())
    {
      plExpressionAST::Node* v = any("Value", "Value");
      plExpressionAST::Node* inMin = any("In Min", "InMin");
      plExpressionAST::Node* inMax = any("In Max", "InMax");
      plExpressionAST::Node* outMin = any("Out Min", "OutMin");
      plExpressionAST::Node* outMax = any("Out Max", "OutMax");
      if (!valid({v, inMin, inMax, outMin, outMax}))
        return {};

      pResult = Ops::Remap(m_Ast, v, inMin, inMax, outMin, outMax, accessor.GetValue("Clamp").ConvertTo<bool>());
    }
    else if (pType->IsDerivedFrom<plParticlePowerNode>())
    {
      plExpressionAST::Node* pBase = any("Base", "Base");
      plExpressionAST::Node* pExponent = any("Exponent", "Exponent");
      if (!valid({pBase, pExponent}))
        return {};

      pResult = binary(NodeType::Pow, pBase, pExponent);
    }
    else if (pType->IsDerivedFrom<plParticleStepNode>())
    {
      plExpressionAST::Node* pEdge = any("Edge", "Edge");
      plExpressionAST::Node* x = any("X", "X");
      if (!valid({pEdge, x}))
        return {};

      pResult = Ops::Step(m_Ast, pEdge, x);
    }
    else if (pType->IsDerivedFrom<plParticleSmoothStepNode>())
    {
      plExpressionAST::Node* e0 = any("Edge 0", "Edge0");
      plExpressionAST::Node* e1 = any("Edge 1", "Edge1");
      plExpressionAST::Node* x = any("X", "X");
      if (!valid({e0, e1, x}))
        return {};

      pResult = Ops::SmoothStep(m_Ast, e0, e1, x, accessor.GetValue("Smoother").ConvertTo<bool>());
    }
    else if (pType->IsDerivedFrom<plParticleRandomRangeNode>())
    {
      plExpressionAST::Node* fMin = any("Min", "Min");
      plExpressionAST::Node* fMax = any("Max", "Max");
      if (!valid({fMin, fMax}))
        return {};

      const plUInt32 uiComponents = kind == kColor ? 4 : (kind == kVector ? 3 : 1);
      pResult = Ops::RandomRange(m_Ast, Input("RandomSeed", plProcessingStream::DataType::Int), accessor.GetValue("Seed").ConvertTo<plInt32>(), fMin, fMax, uiComponents);
    }
    else if (pType->IsDerivedFrom<plParticleFunctionNode>())
    {
      plExpressionAST::Node* x = any("Value", "Value");
      if (x == nullptr)
        return {};

      // the graph's angles are degrees; nodes of type version 1 keep radians
      const bool bRadians = accessor.GetValue("Radians").ConvertTo<bool>();
      auto toRadians = [&](plExpressionAST::Node* a)
      { return bRadians ? a : unary(NodeType::DegToRad, a); };
      auto fromRadians = [&](plExpressionAST::Node* a)
      { return bRadians ? a : unary(NodeType::RadToDeg, a); };

      switch (iOperation)
      {
        case plParticleFunctionOp::Cosine:
          pResult = unary(NodeType::Cos, toRadians(x));
          break;
        case plParticleFunctionOp::Tan:
          pResult = unary(NodeType::Tan, toRadians(x));
          break;
        case plParticleFunctionOp::ASin:
          pResult = fromRadians(unary(NodeType::ASin, x));
          break;
        case plParticleFunctionOp::ACos:
          pResult = fromRadians(unary(NodeType::ACos, x));
          break;
        case plParticleFunctionOp::ATan:
          pResult = fromRadians(unary(NodeType::ATan, x));
          break;
        case plParticleFunctionOp::Abs:
          pResult = unary(NodeType::Absolute, x);
          break;
        case plParticleFunctionOp::Floor:
          pResult = unary(NodeType::Floor, x);
          break;
        case plParticleFunctionOp::Ceil:
          pResult = unary(NodeType::Ceil, x);
          break;
        case plParticleFunctionOp::Frac:
          pResult = unary(NodeType::Frac, x);
          break;
        case plParticleFunctionOp::Sqrt:
          pResult = unary(NodeType::Sqrt, binary(NodeType::Max, x, Number(0.0)));
          break;
        case plParticleFunctionOp::Saturate:
          pResult = unary(NodeType::Saturate, x);
          break;
        case plParticleFunctionOp::Negate:
          pResult = unary(NodeType::Negate, x);
          break;
        case plParticleFunctionOp::OneMinus:
          pResult = binary(NodeType::Subtract, Number(1.0), x);
          break;
        case plParticleFunctionOp::Exp:
          pResult = unary(NodeType::Exp, x);
          break;
        case plParticleFunctionOp::Log:
          pResult = unary(NodeType::Ln, x);
          break;
        case plParticleFunctionOp::Log2:
          pResult = unary(NodeType::Log2, x);
          break;
        case plParticleFunctionOp::Round:
          pResult = unary(NodeType::Round, x);
          break;
        case plParticleFunctionOp::Trunc:
          pResult = unary(NodeType::Trunc, x);
          break;
        case plParticleFunctionOp::Sign:
          pResult = Ops::Sign(m_Ast, x);
          break;
        case plParticleFunctionOp::Reciprocal:
          pResult = Ops::SafeDivide(m_Ast, Number(1.0), x);
          break;
        case plParticleFunctionOp::Square:
          pResult = binary(NodeType::Multiply, x, x);
          break;
        case plParticleFunctionOp::DegreesToRadians:
          pResult = unary(NodeType::DegToRad, x);
          break;
        case plParticleFunctionOp::RadiansToDegrees:
          pResult = unary(NodeType::RadToDeg, x);
          break;
        default:
          pResult = unary(NodeType::Sin, toRadians(x));
          break;
      }
    }

    return {pResult, kind};
  }

  if (pType->IsDerivedFrom<plParticleSplitVectorNode>() || pType->IsDerivedFrom<plParticleSplitColorNode>())
  {
    const bool bVector = pType->IsDerivedFrom<plParticleSplitVectorNode>();
    plExpressionAST::Node* v = bVector ? input("Vector", "Vector", kVector) : input("Colour", "Colour", kColor);
    if (v == nullptr)
      return {};

    const plUInt32 uiComponents = bVector ? 3 : 4;
    for (plUInt32 c = 0; c < uiComponents; ++c)
      out_built.m_Outputs[c] = {m_Ast.CreateSwizzle(static_cast<plExpressionAST::VectorComponent::Enum>(c), v), kNumber};

    return out_built.m_Outputs[0];
  }

  if (pType->IsDerivedFrom<plParticleMakeColorNode>())
  {
    plExpressionAST::Node* components[] = {input("R", "R", kNumber), input("G", "G", kNumber), input("B", "B", kNumber), input("A", "A", kNumber)};
    if (!valid({components[0], components[1], components[2], components[3]}))
      return {};

    return {m_Ast.CreateConstructorCall(DataType::Float4, components), kColor};
  }

  if (pType->IsDerivedFrom<plParticleSwizzleNode>())
  {
    plUInt32 pattern[3] = {};
    plExpressionAST::Node* v = input("Vector", "Vector", kVector);
    if (v == nullptr || !plParticleSwizzleNode::ParsePattern(accessor.GetValue("Pattern").ConvertTo<plString>(), pattern))
      return {};

    plEnum<plExpressionAST::VectorComponent> components[3];
    for (plUInt32 c = 0; c < 3; ++c)
      components[c] = static_cast<plExpressionAST::VectorComponent::Enum>(pattern[c]);

    return {m_Ast.CreateSwizzle(plMakeArrayPtr(components), v), kVector};
  }

  if (pType->IsDerivedFrom<plParticleDistanceNode>() || pType->IsDerivedFrom<plParticleCrossNode>())
  {
    plExpressionAST::Node* a = input("A", "A", kVector);
    plExpressionAST::Node* b = input("B", "B", kVector);
    if (!valid({a, b}))
      return {};

    if (pType->IsDerivedFrom<plParticleCrossNode>())
      return {binary(NodeType::Cross, a, b), kVector};

    return {Ops::Distance(m_Ast, a, b), kNumber};
  }

  if (pType->IsDerivedFrom<plParticleReflectNode>())
  {
    plExpressionAST::Node* d = input("Direction", "Direction", kVector);
    plExpressionAST::Node* n = input("Normal", "Normal", kVector);
    if (!valid({d, n}))
      return {};

    return {binary(NodeType::Reflect, d, n), kVector};
  }

  if (pType->IsDerivedFrom<plParticleProjectNode>())
  {
    plExpressionAST::Node* v = input("Vector", "Vector", kVector);
    plExpressionAST::Node* onto = input("Onto", "Onto", kVector);
    if (!valid({v, onto}))
      return {};

    return {Ops::ProjectOnto(m_Ast, v, onto), kVector};
  }

  if (pType->IsDerivedFrom<plParticleRotateAboutAxisNode>())
  {
    plExpressionAST::Node* v = input("Vector", "Vector", kVector);
    plExpressionAST::Node* axis = input("Axis", "Axis", kVector);
    plExpressionAST::Node* degrees = input("Degrees", "Degrees", kNumber);
    if (!valid({v, axis, degrees}))
      return {};

    return {Ops::RotateAboutAxis(m_Ast, v, axis, unary(NodeType::DegToRad, degrees)), kVector};
  }

  if (pType->IsDerivedFrom<plParticleInverseLerpNode>())
  {
    plExpressionAST::Node* a = input("A", "A", kNumber);
    plExpressionAST::Node* b = input("B", "B", kNumber);
    plExpressionAST::Node* v = input("Value", "Value", kNumber);
    if (!valid({a, b, v}))
      return {};

    return {Ops::InverseLerp(m_Ast, a, b, v, accessor.GetValue("Clamp").ConvertTo<bool>()), kNumber};
  }

  if (pType->IsDerivedFrom<plParticleATan2Node>())
  {
    plExpressionAST::Node* y = input("Y", "Y", kNumber);
    plExpressionAST::Node* x = input("X", "X", kNumber);
    if (!valid({y, x}))
      return {};

    return {unary(NodeType::RadToDeg, Ops::ATan2(m_Ast, y, x)), kNumber};
  }

  if (pType->IsDerivedFrom<plParticleMathConstantNode>())
    return {Number(plParticleMathConstant::GetValue(accessor.GetValue("Constant").ConvertTo<plInt64>())), kNumber};

  if (pType->IsDerivedFrom<plParticleLogicNode>())
  {
    plExpressionAST::Node* a = input("A", "A", plParticleValueKind::Bool);
    plExpressionAST::Node* b = input("B", "B", plParticleValueKind::Bool);
    if (!valid({a, b}))
      return {};

    const NodeType::Enum type = iOperation == plParticleLogicOp::Or ? NodeType::LogicalOr : (iOperation == plParticleLogicOp::Xor ? NodeType::NotEqual : NodeType::LogicalAnd);
    return {binary(type, a, b), plParticleValueKind::Bool};
  }

  if (pType->IsDerivedFrom<plParticleNotNode>())
  {
    plExpressionAST::Node* v = input("Value", "Value", plParticleValueKind::Bool);
    return {v != nullptr ? unary(NodeType::LogicalNot, v) : nullptr, plParticleValueKind::Bool};
  }

  if (pType->IsDerivedFrom<plParticleRandomIntegerNode>())
  {
    plExpressionAST::Node* fMin = input("Min", "Min", kNumber);
    plExpressionAST::Node* fMax = input("Max", "Max", kNumber);
    if (!valid({fMin, fMax}))
      return {};

    return {Ops::RandomInteger(m_Ast, Input("RandomSeed", plProcessingStream::DataType::Int), accessor.GetValue("Seed").ConvertTo<plInt32>(), fMin, fMax), kNumber};
  }

  if (pType->IsDerivedFrom<plParticleRandomVectorNode>())
  {
    plExpressionAST::Node* pRadius = input("Radius", "Radius", kNumber);
    if (pRadius == nullptr)
      return {};

    const auto shape = static_cast<Ops::RandomVectorShape>(accessor.GetValue("Shape").ConvertTo<plInt64>());
    const float fCone = plAngle::MakeFromDegree(accessor.GetValue("ConeAngle").ConvertTo<float>()).GetRadian();
    return {Ops::RandomVector(m_Ast, Input("RandomSeed", plProcessingStream::DataType::Int), accessor.GetValue("Seed").ConvertTo<plInt32>(), shape, pRadius, fCone), kVector};
  }

  if (pType->IsDerivedFrom<plParticleVectorNoiseNode>())
  {
    plExpressionAST::Node* pPosition = input("Position", "Position", kVector);
    plExpressionAST::Node* pFrequency = input("Frequency", "Frequency", kNumber);
    if (!valid({pPosition, pFrequency}))
      return {};

    return {Ops::VectorNoise(m_Ast, pPosition, pFrequency, accessor.GetValue("Octaves").ConvertTo<plInt32>()), kVector};
  }

  if (pType->IsDerivedFrom<plParticleTransformNode>())
  {
    plExpressionAST::Node* v = input("Vector", "Vector", kVector);
    if (v == nullptr)
      return {};

    const bool bToWorld = accessor.GetValue("Space").ConvertTo<plInt64>() == plParticleTransformSpace::LocalToWorld;
    const Ops::Frame frame = BuildEffectFrame();

    if (accessor.GetValue("Kind").ConvertTo<plInt64>() == plParticleTransformKind::Direction)
      return {Ops::TransformDirection(m_Ast, frame, v, bToWorld), kVector};

    return {Ops::TransformPosition(m_Ast, frame, v, bToWorld), kVector};
  }

  if (pType->IsDerivedFrom<plParticleCurveOverLifeNode>() || pType->IsDerivedFrom<plParticleGradientOverLifeNode>())
    return BuildOverLife(pNode, ref_visited);

  out_bHandled = false;
  return {};
}

plParticleProgramOps::Frame plParticleGraphCompiler::BuildEffectFrame()
{
  plParticleProgramOps::Frame frame;
  frame.m_pPosition = Input(plParticleFrameInput::EffectPosition, plProcessingStream::DataType::Float3);
  frame.m_pForward = Input(plParticleFrameInput::EffectForward, plProcessingStream::DataType::Float3);
  frame.m_pRight = Input(plParticleFrameInput::EffectRight, plProcessingStream::DataType::Float3);
  frame.m_pUp = Input(plParticleFrameInput::EffectUp, plProcessingStream::DataType::Float3);
  frame.m_pScale = Input(plParticleFrameInput::EffectScale, plProcessingStream::DataType::Float3);
  return frame;
}

plExpressionAST::Node* plParticleGraphCompiler::BuildLifeInput(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited)
{
  if (GetInputSource(pNode, "T") != nullptr)
    return BuildInput(pNode, "T", nullptr, plParticleValueKind::Number, ref_visited).m_pNode;

  const auto& accessor = pNode->GetTypeAccessor();

  switch (accessor.GetValue("Input").ConvertTo<plInt64>())
  {
    case plParticleCurveInput::Age:
    case plParticleCurveInput::Speed:
    {
      // Input Max is the T of 1; the table read clamps beyond it
      const bool bAge = accessor.GetValue("Input").ConvertTo<plInt64>() == plParticleCurveInput::Age;
      const double fInvMax = 1.0 / plMath::Max(accessor.GetValue("InputMax").ConvertTo<double>(), 0.001);
      return m_Ast.CreateBinaryOperator(NodeType::Multiply, BuildParticleValue(bAge ? plParticleInputValue::Age : plParticleInputValue::Speed), Number(fInvMax));
    }

    default:
      return BuildParticleValue(plParticleInputValue::NormalizedAge);
  }
}

plParticleGraphCompiler::AstValue plParticleGraphCompiler::BuildOverLife(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited)
{
  constexpr plUInt32 n = plParticleProgramTables::s_uiTableSize;

  plExpressionAST::Node* t = BuildLifeInput(pNode, ref_visited);
  if (t == nullptr)
    return {};

  if (pNode->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleGradientOverLifeNode>())
  {
    // the asset when one is picked, else the stops, baked here
    const plString sAsset = FoldInput(pNode, "Gradient Asset", "GradientAsset", ref_visited).ConvertTo<plString>();

    plExpressionAST::Node* pSlot = nullptr;
    if (!sAsset.IsEmpty())
    {
      pSlot = m_Ast.CreateConstant(plVariant(GetAssetSlot(m_Assets.m_Gradients, sAsset)), DataType::Int);
    }
    else
    {
      plVec4 table[n];
      BakeInlineGradient(pNode, plArrayPtr<plVec4>(table, n));
      pSlot = AddInlineGradient(plArrayPtr<const plVec4>(table, n));
    }

    return {plParticleProgramOps::SampleGradient(m_Ast, t, pSlot), plParticleValueKind::Color};
  }

  plExpressionAST::Node* pMin = BuildInput(pNode, "Output Min", "OutputMin", plParticleValueKind::Number, ref_visited).m_pNode;
  plExpressionAST::Node* pMax = BuildInput(pNode, "Output Max", "OutputMax", plParticleValueKind::Number, ref_visited).m_pNode;
  if (pMin == nullptr || pMax == nullptr)
    return {};

  // an asset curve reads 0..1 the way Size Curve reads it; the inline curve is drawn 0..1 already
  const plString sAsset = FoldInput(pNode, "Curve Asset", "CurveAsset", ref_visited).ConvertTo<plString>();

  plExpressionAST::Node* pSlot = nullptr;
  if (!sAsset.IsEmpty())
  {
    pSlot = m_Ast.CreateConstant(plVariant(GetCurveSlot(sAsset, true)), DataType::Int);
  }
  else
  {
    float table[n];
    BakeInlineCurve(pNode, plArrayPtr<float>(table, n));
    pSlot = AddInlineCurve(plArrayPtr<const float>(table, n));
  }

  plExpressionAST::Node* pCurve = plParticleProgramOps::SampleCurve(m_Ast, t, pSlot);
  return {m_Ast.CreateTernaryOperator(NodeType::Lerp, pMin, pMax, pCurve), plParticleValueKind::Number};
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
  return pType->IsDerivedFrom<plParticleSampleGradientNode>() || pType->IsDerivedFrom<plParticleSampleCurveNode>() || pType->IsDerivedFrom<plParticleNoiseNode>() ||
         pType->IsDerivedFrom<plParticleVectorNoiseNode>();
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

plInt32 plParticleGraphCompiler::GetCurveSlot(const plString& sAsset, bool bNormalized)
{
  // the flags run parallel to the asset list
  m_Assets.m_CurveNormalized.SetCount(m_Assets.m_Curves.GetCount(), 0);

  const plUInt8 uiNormalized = bNormalized ? 1 : 0;
  for (plUInt32 i = 0; i < m_Assets.m_Curves.GetCount(); ++i)
  {
    if (m_Assets.m_Curves[i] == sAsset && m_Assets.m_CurveNormalized[i] == uiNormalized)
      return (plInt32)i;
  }

  m_Assets.m_Curves.PushBack(sAsset);
  m_Assets.m_CurveNormalized.PushBack(uiNormalized);
  return (plInt32)m_Assets.m_Curves.GetCount() - 1;
}

namespace
{
  /// Inline slots are numbered from here until the program compiles and the asset slots are known.
  constexpr plInt32 s_iInlineSlotPlaceholder = 0x40000000;
} // namespace

plExpressionAST::Node* plParticleGraphCompiler::AddInlineCurve(plArrayPtr<const float> table)
{
  InlineSlot& slot = m_InlineSlots.ExpandAndGetRef();
  slot.m_uiIndex = m_Assets.m_InlineCurves.GetCount() / plParticleProgramTables::s_uiTableSize;
  slot.m_bGradient = false;
  slot.m_pConstant = m_Ast.CreateConstant(plVariant(s_iInlineSlotPlaceholder + (plInt32)slot.m_uiIndex), DataType::Int);

  m_Assets.m_InlineCurves.PushBackRange(table);
  return slot.m_pConstant;
}

plExpressionAST::Node* plParticleGraphCompiler::AddInlineGradient(plArrayPtr<const plVec4> table)
{
  InlineSlot& slot = m_InlineSlots.ExpandAndGetRef();
  slot.m_uiIndex = m_Assets.m_InlineGradients.GetCount() / plParticleProgramTables::s_uiTableSize;
  slot.m_bGradient = true;
  slot.m_pConstant = m_Ast.CreateConstant(plVariant(s_iInlineSlotPlaceholder + (plInt32)slot.m_uiIndex), DataType::Int);

  m_Assets.m_InlineGradients.PushBackRange(table);
  return slot.m_pConstant;
}

const plDocumentObject* plParticleGraphCompiler::GetInlineCurveObject(const plDocumentObject* pNode)
{
  if (pNode == nullptr || !pNode->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleCurveOverLifeNode>())
    return nullptr;

  const plVariant curve = pNode->GetTypeAccessor().GetValue("Curve");
  return curve.IsA<plUuid>() ? pNode->GetDocumentObjectManager()->GetObject(curve.Get<plUuid>()) : nullptr;
}

void plParticleGraphCompiler::BakeInlineCurve(const plDocumentObject* pNode, plArrayPtr<float> out_table)
{
  const plUInt32 n = out_table.GetCount();

  plCurve1D curve;
  if (const plDocumentObject* pCurve = GetInlineCurveObject(pNode))
  {
    const auto& accessor = pCurve->GetTypeAccessor();
    const plInt32 iPoints = accessor.GetCount("ControlPoints");

    for (plInt32 i = 0; i < iPoints; ++i)
    {
      const plVariant pointGuid = accessor.GetValue("ControlPoints", i);
      const plDocumentObject* pPoint = pointGuid.IsA<plUuid>() ? pNode->GetDocumentObjectManager()->GetObject(pointGuid.Get<plUuid>()) : nullptr;
      if (pPoint == nullptr)
        continue;

      // the keys are edited in ticks, 4800 to the unit, like every curve of the editor
      const auto& point = pPoint->GetTypeAccessor();
      auto& cp = curve.AddControlPoint(point.GetValue("Tick").ConvertTo<double>() / 4800.0);
      cp.m_Position.y = point.GetValue("Value").ConvertTo<double>();
      cp.m_LeftTangent = point.GetValue("LeftTangent").ConvertTo<plVec2>();
      cp.m_RightTangent = point.GetValue("RightTangent").ConvertTo<plVec2>();
      cp.m_TangentModeLeft = (plCurveTangentMode::Enum)point.GetValue("LeftTangentMode").ConvertTo<plInt64>();
      cp.m_TangentModeRight = (plCurveTangentMode::Enum)point.GetValue("RightTangentMode").ConvertTo<plInt64>();
    }
  }

  // no keys: the straight line from 0 to 1
  if (curve.IsEmpty())
  {
    for (plUInt32 i = 0; i < n; ++i)
      out_table[i] = n > 1 ? (float)i / (n - 1) : 0.0f;
    return;
  }

  curve.SortControlPoints();
  curve.CreateLinearApproximation();

  for (plUInt32 i = 0; i < n; ++i)
    out_table[i] = (float)curve.Evaluate(n > 1 ? (double)i / (n - 1) : 0.0);
}

void plParticleGraphCompiler::BakeInlineGradient(const plDocumentObject* pNode, plArrayPtr<plVec4> out_table)
{
  struct Stop
  {
    float m_fPosition = 0.0f;
    plVec4 m_vColor = plVec4(1.0f);
  };

  plHybridArray<Stop, 8> stops;

  const auto& accessor = pNode->GetTypeAccessor();
  const plInt32 iStops = accessor.GetCount("Gradient");
  for (plInt32 i = 0; i < iStops; ++i)
  {
    const plVariant stopGuid = accessor.GetValue("Gradient", i);
    const plDocumentObject* pStop = stopGuid.IsA<plUuid>() ? pNode->GetDocumentObjectManager()->GetObject(stopGuid.Get<plUuid>()) : nullptr;
    if (pStop == nullptr)
      continue;

    const plColor color = pStop->GetTypeAccessor().GetValue("Color").ConvertTo<plColor>();

    Stop& stop = stops.ExpandAndGetRef();
    stop.m_fPosition = plMath::Saturate(pStop->GetTypeAccessor().GetValue("Position").ConvertTo<float>());
    stop.m_vColor.Set(color.r, color.g, color.b, color.a);
  }

  stops.Sort([](const Stop& a, const Stop& b)
    { return a.m_fPosition < b.m_fPosition; });

  const plUInt32 n = out_table.GetCount();
  for (plUInt32 i = 0; i < n; ++i)
  {
    const float t = n > 1 ? (float)i / (n - 1) : 0.0f;

    // white without stops; the first and last stop hold their colour out to the ends
    plVec4 vColor(1.0f);
    if (!stops.IsEmpty())
    {
      vColor = stops[0].m_vColor;
      for (plUInt32 s = 0; s < stops.GetCount(); ++s)
      {
        if (stops[s].m_fPosition > t)
        {
          if (s > 0)
          {
            const float fRange = stops[s].m_fPosition - stops[s - 1].m_fPosition;
            const float f = fRange > 0.0f ? (t - stops[s - 1].m_fPosition) / fRange : 0.0f;
            vColor = plMath::Lerp(stops[s - 1].m_vColor, stops[s].m_vColor, f);
          }
          break;
        }

        vColor = stops[s].m_vColor;
      }
    }

    out_table[i] = vColor;
  }
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
    plExpressionAST::Node* pFrequency = BuildInput(pNode, "Frequency", "Frequency", plParticleValueKind::Number, ref_visited).m_pNode;
    plExpressionAST::Node* pOctaves = BuildInput(pNode, "Octaves", "Octaves", plParticleValueKind::Number, ref_visited).m_pNode;
    if (pPosition == nullptr || pFrequency == nullptr || pOctaves == nullptr)
      return {};

    return {plParticleProgramOps::Noise(m_Ast, pPosition, pFrequency, pOctaves), plParticleValueKind::Number};
  }

  plExpressionAST::Node* pT = BuildInput(pNode, "T", "T", plParticleValueKind::Number, ref_visited).m_pNode;
  if (pT == nullptr)
    return {};

  // the asset picked on the node, or the one a Curve or Gradient node wired into it gives
  if (pType->IsDerivedFrom<plParticleSampleCurveNode>())
  {
    const plString sCurve = FoldInput(pNode, "Curve", "Curve", ref_visited).ConvertTo<plString>();
    if (sCurve.IsEmpty())
      return {};

    return {plParticleProgramOps::SampleCurve(m_Ast, pT, intConstant(GetCurveSlot(sCurve, false))), plParticleValueKind::Number};
  }

  // plParticleSampleGradientNode: one table read per component
  const plString sGradient = FoldInput(pNode, "Gradient", "Gradient", ref_visited).ConvertTo<plString>();
  if (sGradient.IsEmpty())
    return {};

  return {plParticleProgramOps::SampleGradient(m_Ast, pT, intConstant(GetAssetSlot(m_Assets.m_Gradients, sGradient))), plParticleValueKind::Color};
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

  // pi and tau are variables the code can read; the particle functions are declared for the parser and replaced
  // once it is done
  plStringBuilder sFullCode("var pi = 3.14159265; var tau = 6.2831853; out = ", sCode);

  FirstErrorLog log;
  plLogSystemScope logScope(&log);

  plExpressionParser parser;
  for (ExpressionCall call : {ExpressionCall::Random, ExpressionCall::Noise, ExpressionCall::Curve, ExpressionCall::Step, ExpressionCall::Sign, ExpressionCall::ATan2})
    parser.RegisterFunction(GetExpressionCallDesc(call));

  plExpressionParser::Options options;
  if (parser.Parse(sFullCode, inputs, outputs, options, out_ast).Failed())
  {
    if (out_pError != nullptr)
      *out_pError = log.m_sError;
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

plParticleTier plParticleGraphCompiler::GetExpressionCallTier(plStringView sCode)
{
  plExpressionAST ast;
  if (ParseExpression(sCode, ast).Failed() || ast.m_OutputNodes.GetCount() != 1 || ast.m_OutputNodes[0]->m_pExpression == nullptr)
    return plParticleTier::Constant;

  plParticleTier tier = plParticleTier::Constant;

  plHashTable<plExpressionAST::Node*, plExpressionAST::Node*> done;
  auto inspect = [&](plExpressionAST::Node* pCodeNode) -> plExpressionAST::Node*
  {
    const ExpressionCall call = GetExpressionCall(pCodeNode);
    if (call == ExpressionCall::Random)
      tier = plParticleTier::Particle;
    else if (call == ExpressionCall::Noise || call == ExpressionCall::Curve)
      tier = plMath::Max(tier, plParticleTier::Frame);

    return nullptr;
  };

  RewriteExpression(ast.m_OutputNodes[0]->m_pExpression, done, inspect);
  return tier;
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

  bool bFailed = false;

  // the inputs become what feeds the pins, and the particle functions what they stand for in the program
  auto replace = [&](plExpressionAST::Node* pCodeNode) -> plExpressionAST::Node*
  {
    const plInt32 iInput = GetExpressionInputIndex(pCodeNode);
    if (iInput >= 0)
      return arguments[iInput];

    const ExpressionCall call = GetExpressionCall(pCodeNode);
    if (call == ExpressionCall::None)
      return nullptr;

    auto callArguments = static_cast<plExpressionAST::FunctionCall*>(pCodeNode)->m_Arguments.GetArrayPtr();

    switch (call)
    {
      case ExpressionCall::Random:
      {
        plExpressionAST::Node* randomArguments[] = {Input("RandomSeed", plProcessingStream::DataType::Int), m_Ast.CreateUnaryOperator(NodeType::TypeConversion, callArguments[0], DataType::Int)};
        return m_Ast.CreateFunctionCall(plDefaultExpressionFunctions::s_RandomFunc.m_Desc, randomArguments);
      }

      case ExpressionCall::Noise:
      {
        plExpressionAST::Node* pPosition = m_Ast.CreateConstructorCall(DataType::Float3, callArguments.GetSubArray(0, 3));
        return plParticleProgramOps::Noise(m_Ast, pPosition, Number(1.0), callArguments.GetCount() > 3 ? callArguments[3] : Number(1.0));
      }

      case ExpressionCall::Curve:
      {
        const plString sCurve = FoldInput(pNode, "Curve", "Curve", ref_visited).ConvertTo<plString>();
        if (sCurve.IsEmpty())
        {
          bFailed = true;
          return Number(0.0);
        }

        return plParticleProgramOps::SampleCurve(m_Ast, callArguments[0], m_Ast.CreateConstant(plVariant(GetCurveSlot(sCurve, false)), DataType::Int));
      }

      case ExpressionCall::Step:
        return plParticleProgramOps::Step(m_Ast, callArguments[0], callArguments[1]);

      case ExpressionCall::Sign:
        return plParticleProgramOps::Sign(m_Ast, callArguments[0]);

      default:
        return plParticleProgramOps::ATan2(m_Ast, callArguments[0], callArguments[1]);
    }
  };

  // the code can share sub-expressions through variables; each of its nodes is rewritten once
  plHashTable<plExpressionAST::Node*, plExpressionAST::Node*> done;
  plExpressionAST::Node* pRoot = RewriteExpression(pAst->m_OutputNodes[0]->m_pExpression, done, replace);
  if (bFailed)
    return {};

  m_ExpressionAsts.PushBack(std::move(pAst));
  return {pRoot, plParticleValueKind::Number};
}

plVariant plParticleGraphCompiler::FoldExpression(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited)
{
  plExpressionAST ast;
  if (ParseExpression(pNode->GetTypeAccessor().GetValue("Expression").ConvertTo<plString>(), ast).Failed() || ast.m_OutputNodes.GetCount() != 1 ||
      ast.m_OutputNodes[0]->m_pExpression == nullptr)
    return plVariant();

  // the particle functions stand in with their neutral values, as the nodes do: random and noise at 0.5, a curve at 0
  plHashTable<plExpressionAST::Node*, plExpressionAST::Node*> done;
  auto replace = [&](plExpressionAST::Node* pCodeNode) -> plExpressionAST::Node*
  {
    auto callArguments = pCodeNode->m_Type == NodeType::FunctionCall ? static_cast<plExpressionAST::FunctionCall*>(pCodeNode)->m_Arguments.GetArrayPtr() : plArrayPtr<plExpressionAST::Node*>();

    switch (GetExpressionCall(pCodeNode))
    {
      case ExpressionCall::Random:
      case ExpressionCall::Noise:
        return plParticleProgramOps::Number(ast, 0.5f);
      case ExpressionCall::Curve:
        return plParticleProgramOps::Number(ast, 0.0f);
      case ExpressionCall::Step:
        return plParticleProgramOps::Step(ast, callArguments[0], callArguments[1]);
      case ExpressionCall::Sign:
        return plParticleProgramOps::Sign(ast, callArguments[0]);
      case ExpressionCall::ATan2:
        return plParticleProgramOps::ATan2(ast, callArguments[0], callArguments[1]);
      default:
        return nullptr;
    }
  };

  ast.m_OutputNodes[0]->m_pExpression = RewriteExpression(ast.m_OutputNodes[0]->m_pExpression, done, replace);

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
  return {BuildParticleValue(accessor.GetValue("Input").ConvertTo<plInt64>()), plParticleValueKind::Number};
}

plExpressionAST::Node* plParticleGraphCompiler::BuildParticleValue(plInt64 iInput)
{
  using DT = plProcessingStream::DataType;

  // LifeTime holds (remaining seconds, 1 / total seconds)
  auto lifeTime = [&](plExpressionAST::VectorComponent::Enum component) -> plExpressionAST::Node*
  { return m_Ast.CreateSwizzle(component, Input("LifeTime", DT::Float2)); };

  switch (iInput)
  {
    case plParticleInputValue::Age:
      if (m_bInitializeContext)
        return Number(0.0);
      return m_Ast.CreateBinaryOperator(NodeType::Subtract, m_Ast.CreateBinaryOperator(NodeType::Divide, Number(1.0), lifeTime(plExpressionAST::VectorComponent::Y)), lifeTime(plExpressionAST::VectorComponent::X));

    case plParticleInputValue::NormalizedAge:
      if (m_bInitializeContext)
        return Number(0.0);
      return m_Ast.CreateBinaryOperator(NodeType::Subtract, Number(1.0), m_Ast.CreateBinaryOperator(NodeType::Multiply, lifeTime(plExpressionAST::VectorComponent::X), lifeTime(plExpressionAST::VectorComponent::Y)));

    case plParticleInputValue::Lifetime:
      if (m_bInitializeContext)
        return Number(m_fSystemLifetime);
      return m_Ast.CreateBinaryOperator(NodeType::Divide, Number(1.0), lifeTime(plExpressionAST::VectorComponent::Y));

    case plParticleInputValue::RemainingLife:
      if (m_bInitializeContext)
        return Number(m_fSystemLifetime);
      return lifeTime(plExpressionAST::VectorComponent::X);

    case plParticleInputValue::Size:
      return Input("Size", DT::Half);

    case plParticleInputValue::RotationSpeed:
      return Input("RotationSpeed", DT::Half);

    default:
      return m_Ast.CreateUnaryOperator(NodeType::Length, Input("Velocity", DT::Float3));
  }
}

plUInt32 plParticleGraphCompiler::AddOutput(const plDocumentObject* pSource, plParticleValueKind::Enum targetKind, plUInt32 uiSourceOutput)
{
  plSet<const plDocumentObject*> visited;
  AstValue value = Build(pSource, visited, uiSourceOutput);

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

plUInt32 plParticleGraphCompiler::AddSpyOutput(const plDocumentObject* pSource, plUInt32 uiSourceOutput)
{
  plSet<const plDocumentObject*> visited;
  AstValue value = Build(pSource, visited, uiSourceOutput);

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
  // the asset lists are complete now: the inline tables take the slots after them
  for (const InlineSlot& slot : m_InlineSlots)
  {
    const plUInt32 uiAssetSlots = slot.m_bGradient ? m_Assets.m_Gradients.GetCount() : m_Assets.m_Curves.GetCount();
    slot.m_pConstant->m_Value = plVariant((plInt32)(uiAssetSlots + slot.m_uiIndex));
  }

  m_Assets.m_CurveNormalized.SetCount(m_Assets.m_Curves.GetCount(), 0);

  plExpressionCompiler compiler;
  return compiler.Compile(m_Ast, out_byteCode);
}
