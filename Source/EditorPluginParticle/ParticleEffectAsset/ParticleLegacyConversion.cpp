#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphCompiler.h>
#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Serialization/GraphPatch.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ConditionalKill.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Expression.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_If.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_KillIf.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Remap.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SetAttribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Switch.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Command/VisualGraphCommands.h>

// The logic blocks of plasma.particles 1.3 (If, Switch, Remap, Conditional Kill, Expression and the
// old Set Attribute) become the graph nodes that compute their value plus a Set Attribute or Kill If
// at the block's place in Update. The context keeps its order, so the effect behaves as before.

namespace
{
  /// Conditional Kill had its own attribute and comparison lists until 1.4; documents name them.
  class plParticleConditionalKillPatch_1_2 : public plGraphPatch
  {
  public:
    plParticleConditionalKillPatch_1_2()
      : plGraphPatch("plParticleBehaviorFactory_ConditionalKill", 2)
    {
    }

    virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
    {
      if (plAbstractObjectNode::Property* pAttribute = pNode->FindProperty("Attribute"))
      {
        plStringBuilder s = pAttribute->m_Value.ConvertTo<plString>();
        s.ReplaceAll("plParticleKillAttribute::ColorAlpha", "plParticleAttribute::ColorA");
        s.ReplaceAll("plParticleKillAttribute::", "plParticleAttribute::");
        pAttribute->m_Value = plString(s);
      }

      if (plAbstractObjectNode::Property* pComparison = pNode->FindProperty("Comparison"))
      {
        plStringBuilder s = pComparison->m_Value.ConvertTo<plString>();
        s.ReplaceAll("plParticleComparisonOp::", "plParticleConditionOp::");
        pComparison->m_Value = plString(s);
      }
    }
  };

  plParticleConditionalKillPatch_1_2 g_plParticleConditionalKillPatch_1_2;

  /// What the old blocks read and write. The Expression bindings are the widest list; the If, Switch,
  /// Remap, Conditional Kill and Set Attribute attribute list maps into it.
  using Binding = plParticleExpressionBinding;

  Binding::Enum FromAttribute(plInt64 iAttribute)
  {
    switch (iAttribute)
    {
      case plParticleAttribute::PositionX:
        return Binding::PositionX;
      case plParticleAttribute::PositionY:
        return Binding::PositionY;
      case plParticleAttribute::PositionZ:
        return Binding::PositionZ;
      case plParticleAttribute::Speed:
        return Binding::Speed;
      case plParticleAttribute::LifeFraction:
        return Binding::LifeFraction;
      case plParticleAttribute::ColorR:
        return Binding::ColorR;
      case plParticleAttribute::ColorG:
        return Binding::ColorG;
      case plParticleAttribute::ColorB:
        return Binding::ColorB;
      case plParticleAttribute::ColorA:
        return Binding::ColorA;
      default:
        return Binding::Size;
    }
  }

  /// True when the code names the variable, as a whole word.
  bool UsesVariable(plStringView sCode, char cVariable)
  {
    const char* szStart = sCode.GetStartPointer();
    const char* szEnd = sCode.GetEndPointer();

    auto isWordChar = [](char c)
    { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };

    for (const char* p = szStart; p < szEnd; ++p)
    {
      if (*p != cVariable)
        continue;

      const bool bWordStart = p == szStart || !isWordChar(p[-1]);
      const bool bWordEnd = p + 1 == szEnd || !isWordChar(p[1]);
      if (bWordStart && bWordEnd)
        return true;
    }

    return false;
  }

  /// A number in the converted graph: a node's output, or a literal.
  struct Value
  {
    plUuid m_Node;
    double m_fLiteral = 0.0;

    bool IsNode() const { return m_Node.IsValid(); }
  };

  /// Builds the replacement of one old block inside an open transaction.
  class LogicBlockConverter
  {
  public:
    LogicBlockConverter(plCommandHistory* pHistory, plParticleEffectNodeManager* pManager, plDynamicArray<plUuid>& ref_created)
      : m_pHistory(pHistory)
      , m_pManager(pManager)
      , m_Created(ref_created)
    {
    }

    /// Places the nodes of the next blocks to the left of this system, below the ones placed before.
    void BeginSystem(const plDocumentObject* pSystem)
    {
      m_pSystem = pSystem;
      m_vOrigin = m_pManager->GetNodePos(pSystem);
      m_fRowY = m_vOrigin.y;
    }

    /// Replaces the block; false when it is not one of the old logic blocks.
    bool Convert(const plDocumentObject* pBlock)
    {
      const plRTTI* pType = pBlock->GetTypeAccessor().GetType();

      m_pBlock = pBlock;
      m_bEnabled = pBlock->GetTypeAccessor().GetValue("Enabled").ConvertTo<bool>();
      m_ColumnCounts.Clear();
      CollectOldWires();

      if (pType == plGetStaticRTTI<plParticleBehaviorFactory_If>())
        ConvertIf();
      else if (pType == plGetStaticRTTI<plParticleBehaviorFactory_Switch>())
        ConvertSwitch();
      else if (pType == plGetStaticRTTI<plParticleBehaviorFactory_Remap>())
        ConvertRemap();
      else if (pType == plGetStaticRTTI<plParticleBehaviorFactory_ConditionalKill>())
        ConvertConditionalKill();
      else if (pType == plGetStaticRTTI<plParticleBehaviorFactory_Expression>())
        ConvertExpression();
      else if (pType == plGetStaticRTTI<plParticleBehaviorFactory_SetAttribute>())
        ConvertSetAttribute();
      else
        return false;

      for (auto it = m_OldWires.GetIterator(); it.IsValid(); ++it)
        plLog::Warning("Particle conversion: the wire into '{}' of a 1.3 {} has no place in the graph and was dropped.", it.Key(), plParticleEffectNodeManager::FriendlyTypeName(pType));

      // the block's own wires go first: its new nodes took them over, and the system's pins for the block go with it
      RemoveOldWires();

      plRemoveObjectCommand cmd;
      cmd.m_Object = pBlock->GetGuid();
      m_pHistory->AddCommand(cmd).LogFailure();

      // the next block's nodes go below this one's
      float fBottom = m_fRowY;
      for (auto it = m_ColumnCounts.GetIterator(); it.IsValid(); ++it)
        fBottom = plMath::Max(fBottom, m_fRowY + it.Value() * s_fRowHeight);
      m_fRowY = fBottom + 40.0f;

      return true;
    }

  private:
    static constexpr float s_fColumnWidth = 230.0f;
    static constexpr float s_fRowHeight = 120.0f;

    const plDocumentObject* GetObject(const plUuid& guid) const { return m_pManager->GetObject(guid); }

    plVariant GetOld(const char* szProperty) const { return m_pBlock->GetTypeAccessor().GetValue(szProperty); }

    /// The wires into the old block's properties, by property. Each one is handed to the node input
    /// that takes over the property; any left over is reported.
    void CollectOldWires()
    {
      m_OldWires.Clear();

      plHybridArray<const plAbstractProperty*, 16> properties;
      m_pBlock->GetTypeAccessor().GetType()->GetAllProperties(properties);

      for (const plAbstractProperty* pProp : properties)
      {
        const plVisualGraphPin* pPin = m_pManager->GetInputPinByName(m_pSystem, plParticlePropertyPin::Make(m_pBlock, pProp->GetPropertyName()));
        if (pPin == nullptr)
          continue;

        for (const plVisualGraphConnection* pConnection : m_pManager->GetConnections(*pPin))
        {
          m_OldWires[pProp->GetPropertyName()] = pConnection->GetSourcePin().GetParent()->GetGuid();
          break;
        }
      }
    }

    /// Removes the wires into the old block's properties on the system.
    void RemoveOldWires()
    {
      plHybridArray<plUuid, 8> connections;

      plHybridArray<const plAbstractProperty*, 16> properties;
      m_pBlock->GetTypeAccessor().GetType()->GetAllProperties(properties);

      for (const plAbstractProperty* pProp : properties)
      {
        const plVisualGraphPin* pPin = m_pManager->GetInputPinByName(m_pSystem, plParticlePropertyPin::Make(m_pBlock, pProp->GetPropertyName()));
        if (pPin == nullptr)
          continue;

        for (const plVisualGraphConnection* pConnection : m_pManager->GetConnections(*pPin))
          connections.PushBack(pConnection->GetParent()->GetGuid());
      }

      for (const plUuid& connection : connections)
        plNodeCommands::DisconnectAndRemoveCommand(m_pHistory, connection).LogFailure();
    }

    /// An old block's number: what wired it, else its value.
    Value Old(const char* szProperty)
    {
      Value value;

      auto it = m_OldWires.Find(szProperty);
      if (it.IsValid())
      {
        value.m_Node = it.Value();
        m_OldWires.Remove(it);
        return value;
      }

      value.m_fLiteral = GetOld(szProperty).ConvertTo<double>();
      return value;
    }

    static Value Literal(double fValue)
    {
      Value value;
      value.m_fLiteral = fValue;
      return value;
    }

    plUuid AddNode(const plRTTI* pType, plUInt32 uiColumn)
    {
      const plUuid guid = plUuid::MakeUuid();

      {
        plAddObjectCommand cmd;
        cmd.m_pType = pType;
        cmd.m_NewObjectGuid = guid;
        cmd.m_Index = -1;
        m_pHistory->AddCommand(cmd).LogFailure();
      }

      // columns run leftwards from the system: what a node reads sits further left
      plUInt32& uiRow = m_ColumnCounts[uiColumn];
      {
        plMoveNodeCommand cmd;
        cmd.m_Object = guid;
        cmd.m_NewPos = plVec2(m_vOrigin.x - 30.0f - s_fColumnWidth * uiColumn, m_fRowY + s_fRowHeight * uiRow);
        m_pHistory->AddCommand(cmd).LogFailure();
      }
      ++uiRow;

      m_Created.PushBack(guid);
      return guid;
    }

    void Set(const plUuid& object, const char* szProperty, const plVariant& value)
    {
      plSetObjectPropertyCommand cmd;
      cmd.m_Object = object;
      cmd.m_sProperty = szProperty;
      cmd.m_NewValue = value;
      m_pHistory->AddCommand(cmd).LogFailure();
    }

    void Connect(const plUuid& source, const plDocumentObject* pTarget, plStringView sPin)
    {
      const plDocumentObject* pSource = GetObject(source);
      const plVisualGraphPin* pSourcePin = pSource != nullptr ? m_pManager->GetOutputPinByName(pSource, "Value") : nullptr;
      const plVisualGraphPin* pTargetPin = pTarget != nullptr ? m_pManager->GetInputPinByName(pTarget, sPin) : nullptr;

      if (pSourcePin == nullptr || pTargetPin == nullptr)
      {
        plLog::Error("Particle conversion: could not wire '{}'.", sPin);
        return;
      }

      plNodeCommands::AddAndConnectCommand(m_pHistory, m_pManager->GetConnectionType(), *pSourcePin, *pTargetPin).LogFailure();
    }

    /// Feeds a node input: a wire from a node value, or the literal into the property behind the pin.
    void Feed(const Value& value, const plUuid& node, const char* szPin)
    {
      if (value.IsNode())
        Connect(value.m_Node, GetObject(node), szPin);
      else
        Set(node, szPin, value.m_fLiteral);
    }

    /// A number the old block read from each particle.
    Value Read(Binding::Enum source, plUInt32 uiColumn)
    {
      Value value;

      switch (source)
      {
        case Binding::PositionX:
        case Binding::PositionY:
        case Binding::PositionZ:
        case Binding::VelocityX:
        case Binding::VelocityY:
        case Binding::VelocityZ:
        {
          const bool bPosition = source <= Binding::PositionZ;
          const plInt64 iComponent = bPosition ? (source - Binding::PositionX) : (source - Binding::VelocityX);

          value.m_Node = AddNode(plGetStaticRTTI<plParticleVectorReduceNode>(), uiColumn);
          Set(value.m_Node, "Operation", (plInt64)(plParticleVectorReduceOp::X + iComponent));

          const plUuid vector = AddNode(plGetStaticRTTI<plParticleInputVectorNode>(), uiColumn + 1);
          Set(vector, "Input", (plInt64)(bPosition ? plParticleInputVector::Position : plParticleInputVector::Velocity));
          Connect(vector, GetObject(value.m_Node), "Value");
          break;
        }

        case Binding::ColorR:
        case Binding::ColorG:
        case Binding::ColorB:
        case Binding::ColorA:
        {
          value.m_Node = AddNode(plGetStaticRTTI<plParticleColorChannelNode>(), uiColumn);
          Set(value.m_Node, "Operation", (plInt64)(plParticleColorChannelOp::R + (source - Binding::ColorR)));

          const plUuid color = AddNode(plGetStaticRTTI<plParticleInputColorNode>(), uiColumn + 1);
          Connect(color, GetObject(value.m_Node), "Value");
          break;
        }

        default:
        {
          plInt64 iInput = plParticleInputValue::Size;
          if (source == Binding::Speed)
            iInput = plParticleInputValue::Speed;
          else if (source == Binding::LifeFraction)
            iInput = plParticleInputValue::NormalizedAge;

          value.m_Node = AddNode(plGetStaticRTTI<plParticleInputValueNode>(), uiColumn);
          Set(value.m_Node, "Input", iInput);
          break;
        }
      }

      return value;
    }

    Value Math(plParticleMathOp::Enum op, const Value& a, const Value& b, plUInt32 uiColumn)
    {
      Value value;
      value.m_Node = AddNode(plGetStaticRTTI<plParticleMathNode>(), uiColumn);
      Set(value.m_Node, "Operation", (plInt64)op);
      Feed(a, value.m_Node, "A");
      Feed(b, value.m_Node, "B");
      return value;
    }

    Value Compare(plParticleCompareOp::Enum op, const Value& a, const Value& b, plUInt32 uiColumn)
    {
      Value value;
      value.m_Node = AddNode(plGetStaticRTTI<plParticleCompareNode>(), uiColumn);
      Set(value.m_Node, "Operation", (plInt64)op);
      Feed(a, value.m_Node, "A");
      Feed(b, value.m_Node, "B");
      return value;
    }

    /// The old blocks' comparison. Equal and Not Equal allowed 0.001 either way, so they compare the
    /// distance instead.
    Value Condition(plInt64 iOperation, const Value& a, const Value& threshold, plUInt32 uiColumn)
    {
      switch (iOperation)
      {
        case plParticleConditionOp::LessEqual:
          return Compare(plParticleCompareOp::LessOrEqual, a, threshold, uiColumn);
        case plParticleConditionOp::Greater:
          return Compare(plParticleCompareOp::Greater, a, threshold, uiColumn);
        case plParticleConditionOp::GreaterEqual:
          return Compare(plParticleCompareOp::GreaterOrEqual, a, threshold, uiColumn);
        case plParticleConditionOp::Equal:
        case plParticleConditionOp::NotEqual:
        {
          Value distance;
          distance.m_Node = AddNode(plGetStaticRTTI<plParticleFunctionNode>(), uiColumn + 1);
          Set(distance.m_Node, "Operation", (plInt64)plParticleFunctionOp::Abs);
          Feed(Math(plParticleMathOp::Subtract, a, threshold, uiColumn + 2), distance.m_Node, "Value");

          const bool bEqual = iOperation == plParticleConditionOp::Equal;
          return Compare(bEqual ? plParticleCompareOp::LessOrEqual : plParticleCompareOp::Greater, distance, Literal(0.001), uiColumn);
        }
        default:
          return Compare(plParticleCompareOp::Less, a, threshold, uiColumn);
      }
    }

    Value Branch(const Value& condition, const Value& whenTrue, const Value& whenFalse, plUInt32 uiColumn)
    {
      Value value;
      value.m_Node = AddNode(plGetStaticRTTI<plParticleBranchNode>(), uiColumn);
      Connect(condition.m_Node, GetObject(value.m_Node), "Condition");
      Feed(whenTrue, value.m_Node, "True");
      Feed(whenFalse, value.m_Node, "False");
      return value;
    }

    /// Adds a block of the given type where the old block is, so it runs at the same place.
    plUuid AddBlock(const plRTTI* pType)
    {
      const char* szProperty = plParticleContextInfo::Get(plParticleContext::Update).m_szProperty;

      plInt32 iIndex = 0;
      const plInt32 iCount = m_pSystem->GetTypeAccessor().GetCount(szProperty);
      for (plInt32 i = 0; i < iCount; ++i)
      {
        if (m_pSystem->GetTypeAccessor().GetValue(szProperty, i) == plVariant(m_pBlock->GetGuid()))
          iIndex = i;
      }

      const plUuid guid = plUuid::MakeUuid();

      plAddObjectCommand cmd;
      cmd.m_pType = pType;
      cmd.m_NewObjectGuid = guid;
      cmd.m_Parent = m_pSystem->GetGuid();
      cmd.m_sParentProperty = szProperty;
      cmd.m_Index = iIndex;
      m_pHistory->AddCommand(cmd).LogFailure();

      if (!m_bEnabled)
        Set(guid, "Enabled", false);

      m_Created.PushBack(guid);
      return guid;
    }

    /// Feeds a property of a new block: a wire into its pin on the system, or the literal.
    void FeedBlock(const Value& value, const plUuid& block, const char* szProperty)
    {
      if (value.IsNode())
        Connect(value.m_Node, m_pSystem, plParticlePropertyPin::Make(GetObject(block), szProperty));
      else
        Set(block, szProperty, (float)value.m_fLiteral);
    }

    /// A Set Attribute writing the value where the old block wrote it. False for Life Fraction,
    /// which the old blocks could not write either.
    bool Write(Binding::Enum target, const Value& value, plParticleSinkMode::Enum mode = plParticleSinkMode::Replace, bool bEnabled = true)
    {
      if (target == Binding::LifeFraction)
        return false;

      if (target == Binding::Speed)
      {
        WriteSpeed(value, bEnabled);
        return true;
      }

      plParticleSinkTarget::Enum sinkTarget = plParticleSinkTarget::Size;
      plParticleSinkComponent::Enum component = plParticleSinkComponent::All;

      if (target >= Binding::PositionX && target <= Binding::PositionZ)
      {
        sinkTarget = plParticleSinkTarget::Position;
        component = (plParticleSinkComponent::Enum)(plParticleSinkComponent::X + (target - Binding::PositionX));
      }
      else if (target >= Binding::VelocityX && target <= Binding::VelocityZ)
      {
        sinkTarget = plParticleSinkTarget::Velocity;
        component = (plParticleSinkComponent::Enum)(plParticleSinkComponent::X + (target - Binding::VelocityX));
      }
      else if (target >= Binding::ColorR && target <= Binding::ColorA)
      {
        sinkTarget = plParticleSinkTarget::Color;
        component = (plParticleSinkComponent::Enum)(plParticleSinkComponent::X + (target - Binding::ColorR));
      }

      const plUuid block = AddBlock(plGetStaticRTTI<plParticleBehaviorFactory_Attribute>());
      Set(block, "Target", (plInt64)sinkTarget);
      Set(block, "Component", (plInt64)component);
      Set(block, "Mode", (plInt64)mode);
      FeedBlock(value, block, "Number");

      if (!bEnabled)
        Set(block, "Enabled", false);

      return true;
    }

    /// The old blocks set the speed by rescaling the velocity, and left a velocity under 0.0001 alone:
    /// Velocity = Velocity x (Speed > 0.0001 ? value / Speed : 1).
    void WriteSpeed(const Value& value, bool bEnabled)
    {
      const Value speed = Read(Binding::Speed, 4);

      Value velocity;
      velocity.m_Node = AddNode(plGetStaticRTTI<plParticleInputVectorNode>(), 2);
      Set(velocity.m_Node, "Input", (plInt64)plParticleInputVector::Velocity);

      const Value moving = Compare(plParticleCompareOp::Greater, speed, Literal(0.0001), 3);
      const Value ratio = Math(plParticleMathOp::Divide, value, speed, 3);
      const Value scale = Branch(moving, ratio, Literal(1.0), 2);

      Value scaled;
      scaled.m_Node = AddNode(plGetStaticRTTI<plParticleScaleVectorNode>(), 1);
      Connect(velocity.m_Node, GetObject(scaled.m_Node), "Vector");
      Connect(scale.m_Node, GetObject(scaled.m_Node), "Scale");

      const plUuid block = AddBlock(plGetStaticRTTI<plParticleBehaviorFactory_Attribute>());
      Set(block, "Target", (plInt64)plParticleSinkTarget::Velocity);
      Set(block, "Mode", (plInt64)plParticleSinkMode::Replace);
      Connect(scaled.m_Node, m_pSystem, plParticlePropertyPin::Make(GetObject(block), "Vector"));

      if (!bEnabled)
        Set(block, "Enabled", false);
    }

    void ConvertIf()
    {
      const Binding::Enum output = FromAttribute(GetOld("OutputAttribute").ConvertTo<plInt64>());
      if (output == Binding::LifeFraction)
        return;

      const Value input = Read(FromAttribute(GetOld("ConditionInput").ConvertTo<plInt64>()), 3);
      const Value condition = Condition(GetOld("Comparison").ConvertTo<plInt64>(), input, Old("Threshold"), 2);
      Write(output, Branch(condition, Old("TrueValue"), Old("FalseValue"), 1));
    }

    void ConvertSwitch()
    {
      const Binding::Enum output = FromAttribute(GetOld("OutputAttribute").ConvertTo<plInt64>());
      if (output == Binding::LifeFraction)
        return;

      // the buckets are tested in order, so each Branch picks its bucket's value or everything after it
      const Value input = Read(FromAttribute(GetOld("InputAttribute").ConvertTo<plInt64>()), 3);
      const Value below1 = Compare(plParticleCompareOp::Less, input, Old("Threshold1"), 2);
      const Value below2 = Compare(plParticleCompareOp::Less, input, Old("Threshold2"), 2);
      const Value below3 = Compare(plParticleCompareOp::Less, input, Old("Threshold3"), 2);

      const Value value0 = Old("Value0");
      const Value value1 = Old("Value1");
      const Value value2 = Old("Value2");
      const Value value3 = Old("Value3");

      const Value bucket3 = Branch(below3, value2, value3, 1);
      const Value bucket2 = Branch(below2, value1, bucket3, 1);
      Write(output, Branch(below1, value0, bucket2, 1));
    }

    void ConvertRemap()
    {
      const Binding::Enum output = FromAttribute(GetOld("OutputAttribute").ConvertTo<plInt64>());
      const double fInMin = GetOld("InputMin").ConvertTo<double>();
      const double fInMax = GetOld("InputMax").ConvertTo<double>();

      // an empty input range left the particles untouched
      if (output == Binding::LifeFraction || plMath::IsZero(fInMax - fInMin, 0.0001))
        return;

      Value remap;
      remap.m_Node = AddNode(plGetStaticRTTI<plParticleRemapNode>(), 1);
      Feed(Read(FromAttribute(GetOld("InputAttribute").ConvertTo<plInt64>()), 2), remap.m_Node, "Value");
      Set(remap.m_Node, "InMin", fInMin);
      Set(remap.m_Node, "InMax", fInMax);
      Set(remap.m_Node, "OutMin", GetOld("OutputMin").ConvertTo<double>());
      Set(remap.m_Node, "OutMax", GetOld("OutputMax").ConvertTo<double>());
      Set(remap.m_Node, "Clamp", GetOld("ClampOutput").ConvertTo<bool>());

      Write(output, remap);
    }

    void ConvertConditionalKill()
    {
      const Value input = Read(FromAttribute(GetOld("Attribute").ConvertTo<plInt64>()), 2);
      const Value condition = Condition(GetOld("Comparison").ConvertTo<plInt64>(), input, Old("Threshold"), 1);

      const plUuid block = AddBlock(plGetStaticRTTI<plParticleBehaviorFactory_KillIf>());
      Connect(condition.m_Node, m_pSystem, plParticlePropertyPin::Make(GetObject(block), "Condition"));
    }

    void ConvertExpression()
    {
      const Binding::Enum output = (Binding::Enum)GetOld("Output").ConvertTo<plInt64>();
      if (output == Binding::LifeFraction)
        return;

      const plString sCode = GetOld("Expression").ConvertTo<plString>();

      Value expression;
      expression.m_Node = AddNode(plGetStaticRTTI<plParticleExpressionNode>(), 1);
      Set(expression.m_Node, "Expression", sCode);

      // only the variables the code uses get an input node
      const char* szInputs[] = {"InputA", "InputB", "InputC", "InputD"};
      const char* szPins[] = {"A", "B", "C", "D"};
      for (plUInt32 i = 0; i < 4; ++i)
      {
        if (UsesVariable(sCode, 'a' + (char)i))
          Feed(Read((Binding::Enum)GetOld(szInputs[i]).ConvertTo<plInt64>(), 2), expression.m_Node, szPins[i]);
      }

      // code that never compiled never ran; its Set Attribute starts switched off
      plExpressionAST ast;
      const bool bCompiles = plParticleGraphCompiler::ParseExpression(sCode, ast).Succeeded();
      if (!bCompiles)
        plLog::Warning("Particle conversion: the expression '{}' does not compile, so its Set Attribute is switched off, as the 1.3 block did nothing.", sCode);

      Write(output, expression, plParticleSinkMode::Replace, bCompiles);
    }

    void ConvertSetAttribute()
    {
      const Binding::Enum attribute = FromAttribute(GetOld("Attribute").ConvertTo<plInt64>());
      const plInt64 iMode = GetOld("Mode").ConvertTo<plInt64>();
      const Value value = Old("Value");

      if (attribute == Binding::LifeFraction)
        return;

      // Add added the value once per step. Value + amount with Replace keeps exactly that; the new
      // Add would be per second.
      if (iMode == plParticleSetAttributeMode::Add)
      {
        Write(attribute, Math(plParticleMathOp::Add, Read(attribute, 2), value, 1));
        return;
      }

      if (iMode == plParticleSetAttributeMode::Multiply)
      {
        // Speed is rescaled, not multiplied per component
        if (attribute == Binding::Speed)
          Write(attribute, Math(plParticleMathOp::Multiply, Read(attribute, 2), value, 1));
        else
          Write(attribute, value, plParticleSinkMode::Multiply);
        return;
      }

      Write(attribute, value);
    }

    plCommandHistory* m_pHistory = nullptr;
    plParticleEffectNodeManager* m_pManager = nullptr;
    plDynamicArray<plUuid>& m_Created;

    const plDocumentObject* m_pSystem = nullptr;
    const plDocumentObject* m_pBlock = nullptr;
    bool m_bEnabled = true;
    plMap<plString, plUuid> m_OldWires;

    plVec2 m_vOrigin = plVec2::MakeZero();
    float m_fRowY = 0.0f;
    plMap<plUInt32, plUInt32> m_ColumnCounts;
  };

  bool IsLogicBlock(const plRTTI* pType)
  {
    return pType == plGetStaticRTTI<plParticleBehaviorFactory_If>() || pType == plGetStaticRTTI<plParticleBehaviorFactory_Switch>() ||
           pType == plGetStaticRTTI<plParticleBehaviorFactory_Remap>() || pType == plGetStaticRTTI<plParticleBehaviorFactory_ConditionalKill>() ||
           pType == plGetStaticRTTI<plParticleBehaviorFactory_Expression>() || pType == plGetStaticRTTI<plParticleBehaviorFactory_SetAttribute>();
  }
} // namespace

void plParticleEffectAssetDocument::ConvertLogicBlocks()
{
  auto* pManager = static_cast<plParticleEffectNodeManager*>(GetObjectManager());
  const char* szUpdate = plParticleContextInfo::Get(plParticleContext::Update).m_szProperty;

  // find them first: converting adds and removes blocks
  struct OldBlock
  {
    const plDocumentObject* m_pSystem = nullptr;
    plUuid m_Block;
  };

  plHybridArray<OldBlock, 8> oldBlocks;

  for (const plDocumentObject* pSystem : pManager->GetRootObject()->GetChildren())
  {
    if (!pSystem->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
      continue;

    const plInt32 iCount = pSystem->GetTypeAccessor().GetCount(szUpdate);
    for (plInt32 i = 0; i < iCount; ++i)
    {
      const plVariant value = pSystem->GetTypeAccessor().GetValue(szUpdate, i);
      const plDocumentObject* pBlock = value.IsA<plUuid>() ? pManager->GetObject(value.Get<plUuid>()) : nullptr;

      if (pBlock != nullptr && IsLogicBlock(pBlock->GetTypeAccessor().GetType()))
        oldBlocks.PushBack({pSystem, pBlock->GetGuid()});
    }
  }

  if (oldBlocks.IsEmpty())
    return;

  plCommandHistory* pHistory = GetCommandHistory();
  pHistory->StartTransaction("Convert 1.3 Logic Blocks");

  LogicBlockConverter converter(pHistory, pManager, m_ConversionReport.m_Objects);
  const plDocumentObject* pCurrentSystem = nullptr;

  for (const OldBlock& old : oldBlocks)
  {
    if (old.m_pSystem != pCurrentSystem)
    {
      pCurrentSystem = old.m_pSystem;
      converter.BeginSystem(pCurrentSystem);
    }

    if (const plDocumentObject* pBlock = pManager->GetObject(old.m_Block); pBlock != nullptr && converter.Convert(pBlock))
      ++m_ConversionReport.m_uiBlocks;
  }

  pHistory->FinishTransaction();

  plLog::Info("Particle effect '{}': converted {} blocks from plasma.particles 1.3 into graph nodes.", GetDocumentPath(), m_ConversionReport.m_uiBlocks);
}
