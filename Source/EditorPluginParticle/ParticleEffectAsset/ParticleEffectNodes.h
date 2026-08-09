#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Types/VarianceTypes.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Declarations.h>
#include <ParticlePlugin/Emitter/ParticleEmitter.h>
#include <ParticlePlugin/Events/ParticleEventReaction.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>
#include <ParticlePlugin/Type/ParticleType.h>
#include <ToolsFoundation/VisualGraph/VisualGraphObjectManager.h>

/// The four ordered slots a block can live in inside a system node.
///
/// Each context maps to one array property on plParticleSystemNode and to one of the flat lists
/// on plParticleSystemDescriptor. The order is fixed and matches the order the runtime runs them in.
struct plParticleContext
{
  using StorageType = plUInt8;

  enum Enum
  {
    Spawn,      ///< emitters — how many particles, and when
    Initialize, ///< initializers — what defines a particle at birth
    Update,     ///< behaviors — how it changes over time, in list order
    Output,     ///< types — how it is rendered

    Count,
    Default = Spawn
  };
};

/// Static description of one context: its property name, label and the factory base type it accepts.
struct plParticleContextInfo
{
  plParticleContext::Enum m_Context;
  const char* m_szProperty; ///< array property name on plParticleSystemNode
  const char* m_szLabel;    ///< UI label
  const plRTTI* (*m_FactoryBaseType)();

  static const plParticleContextInfo& Get(plParticleContext::Enum context);
  static const plRTTI* GetFactoryBaseType(plParticleContext::Enum context);

  /// Returns the context a factory type belongs in, or Count if it is not a block type.
  static plParticleContext::Enum FromFactoryType(const plRTTI* pType);
};

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
    Value, ///< an operator feeding one property of one block

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
/// Holds effect-level settings and owns the effect's event reactions. Systems connect to this
/// node's Systems input pin.
class plParticleEffectNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEffectNode, plReflectedClass);

public:
  ~plParticleEffectNode();

  plHybridArray<plParticleEventReactionFactory*, 2> m_EventReactions;

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
/// The system owns its blocks in four ordered arrays, one per context. Array order is the order
/// the runtime runs them in, so reordering a block in the editor is a plain array move.
/// The node's only graph connection is its System output pin, which feeds the effect node.
class plParticleSystemNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSystemNode, plReflectedClass);

public:
  ~plParticleSystemNode();

  plHybridArray<plParticleEmitterFactory*, 2> m_EmitterFactories;
  plHybridArray<plParticleInitializerFactory*, 8> m_InitializerFactories;
  plHybridArray<plParticleBehaviorFactory*, 8> m_BehaviorFactories;
  plHybridArray<plParticleTypeFactory*, 2> m_TypeFactories;

  plString m_sName;
  bool m_bVisible = true;
  plEnum<plParticleSimulationTarget> m_SimulationTarget;
  plVarianceTypeTime m_LifeTime;
  plString m_sOnDeathEvent;
  plString m_sLifeScaleParameter;
};

/// A named constant that can drive one or more block properties.
///
/// The value is folded into the built descriptor when the asset transforms, so a wire costs
/// nothing at runtime and works for any property — unlike the runtime's parameter system, which
/// only honours a handful of hardcoded string fields.
class plParticleValueNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleValueNode, plReflectedClass);

public:
  plString m_sName;
  double m_fValue = 1.0;
};

/// A named colour that can drive colour properties.
class plParticleColorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleColorNode, plReflectedClass);

public:
  plString m_sName;
  plColor m_Value = plColor::White;
};

/// Base for the asset-reference operators. Each subclass only differs by which asset kind its
/// browser offers, which is what makes a texture node refuse to fill a gradient slot.
class plParticleAssetNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleAssetNode, plReflectedClass);

public:
  plString m_sName;
};

class plParticleTextureNode : public plParticleAssetNode
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTextureNode, plParticleAssetNode);

public:
  plString m_sTexture;
};

class plParticleGradientNode : public plParticleAssetNode
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleGradientNode, plParticleAssetNode);

public:
  plString m_sGradient;
};

class plParticleCurveNode : public plParticleAssetNode
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleCurveNode, plParticleAssetNode);

public:
  plString m_sCurve;
};

struct plParticleMathOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    Add,
    Subtract,
    Multiply,
    Divide,
    Min,
    Max,

    Default = Multiply
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleMathOp);

struct plParticleCompareOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual,
    Equal,
    NotEqual,

    Default = Greater
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleCompareOp);

/// Combines two numbers. Either input can be wired or left as the literal on the node.
class plParticleMathNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleMathNode, plReflectedClass);

public:
  plEnum<plParticleMathOp> m_Operation;
  double m_fA = 1.0;
  double m_fB = 1.0;
};

/// Compares two numbers and produces a bool for a Branch node to switch on.
class plParticleCompareNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleCompareNode, plReflectedClass);

public:
  plEnum<plParticleCompareOp> m_Operation;
  double m_fA = 0.0;
  double m_fB = 0.0;
};

/// Picks one of two numbers. This is the If behavior's job, done in the graph instead of per particle.
class plParticleBranchNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBranchNode, plReflectedClass);

public:
  double m_fTrue = 1.0;
  double m_fFalse = 0.0;
};

/// A bool constant, so a Branch condition can be authored without a Compare.
class plParticleBoolNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBoolNode, plReflectedClass);

public:
  plString m_sName;
  bool m_bValue = true;
};

/// Turns a number into a bool: anything other than zero is true.
class plParticleToBoolNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleToBoolNode, plReflectedClass);

public:
  double m_fValue = 0.0;
};

/// Reads one of the effect's named float parameters as a bool: non-zero is true.
///
/// The effect only stores float and colour parameters, so this is a convenience wrapper rather
/// than a parameter type of its own.
class plParticleBoolParameterNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBoolParameterNode, plReflectedClass);

public:
  plString m_sName;
};

/// Reads one of the effect's named colour parameters.
class plParticleColorParameterNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleColorParameterNode, plReflectedClass);

public:
  plString m_sName;
};

/// Reads one of the effect's named float parameters.
///
/// The value folded into the descriptor is the parameter's default. The parameter also exists at
/// runtime, so gameplay can still drive it through plParticleEffectController::SetParameter — but
/// only for the properties the runtime already looks parameters up for.
class plParticleParameterNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleParameterNode, plReflectedClass);

public:
  plString m_sName;
};

/// What an operator node produces, and what a block property expects. A wire is only allowed when
/// the two agree, so a texture cannot be dropped into a numeric port.
struct plParticleValueKind
{
  enum Enum : plUInt8
  {
    None,
    Number,   ///< int / float / time / angle / variance
    Bool,
    Color,
    Vector,
    Texture,
    Gradient,
    Curve,
    OtherAsset, ///< material, mesh, effect: wireable but with no operator yet
    Text,
  };

  /// The kind a property accepts, or None when it must stay constant (enums, flags, structs).
  static Enum FromProperty(const plAbstractProperty* pProp);

  /// The kind an operator node produces, plus the property holding its value. The property is
  /// empty for computed nodes, whose value comes from evaluating their inputs.
  static Enum FromNode(const plRTTI* pNodeType, plStringView& out_sValueProperty);

  /// The kind one of an operator node's input pins accepts, or None if there is no such pin.
  static Enum FromNodeInput(const plRTTI* pNodeType, plStringView sPinName);
};

/// The input pins an operator node offers, and the literal used when one is left unwired.
struct plParticleOperatorInputs
{
  struct Input
  {
    const char* m_szPin;
    const char* m_szLiteral; ///< property holding the value used when the pin is unconnected
  };

  static plArrayPtr<const Input> Get(const plRTTI* pNodeType);
};

/// Encodes and decodes the pin name identifying one property of one block.
///
/// Connections store pin names as plain strings, so a per-property pin needs no engine support:
/// the name carries the owning block's guid and the property it feeds.
struct plParticlePropertyPin
{
  static plString Make(const plUuid& block, plStringView sProperty);
  static bool Parse(plStringView sPinName, plUuid& out_block, plStringBuilder& out_sProperty);

  /// True for properties an operator is allowed to drive: anything with a value kind, which is
  /// everything except the settings that pick a code path.
  static bool IsWireable(const plAbstractProperty* pProp);
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