#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Types/VarianceTypes.h>
#include <GuiFoundation/Widgets/CurveEditData.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Declarations.h>
#include <ParticlePlugin/Emitter/ParticleEmitter.h>
#include <ParticlePlugin/Events/ParticleEventReaction.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>
#include <ParticlePlugin/Type/ParticleType.h>
#include <ToolsFoundation/VisualGraph/VisualGraphObjectManager.h>

/// The ordered slots a block can live in inside a system node.
///
/// Each context maps to one array property on plParticleSystemNode. The first four map to the flat
/// lists of plParticleSystemDescriptor, in the order the runtime runs them; Events blocks become the
/// event names the descriptor and its Raycast blocks raise.
struct plParticleContext
{
  using StorageType = plUInt8;

  enum Enum
  {
    Spawn,      ///< emitters — how many particles, and when
    Initialize, ///< initializers — what defines a particle at birth
    Update,     ///< behaviors — how it changes over time, in list order
    Output,     ///< types — how it is rendered
    Events,     ///< event raisers — what the system tells other systems (plParticleEventRaiserBlock)

    Count,
    Default = Spawn
  };
};

/// A block of the Events context: it names an event the system raises when something happens to a
/// particle. On Event blocks in Spawn spawn on it, and the effect's event reactions react to it.
/// Authoring only: the asset build turns these into the descriptor's event names.
class plParticleEventRaiserBlock : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEventRaiserBlock, plReflectedClass);

public:
  /// abstract, so only the kinds of raiser are offered as blocks
  virtual ~plParticleEventRaiserBlock() = 0;

  plString m_sEventName;
  bool m_bEnabled = true; ///< as on every block: a disabled one is left out of the build
  plString m_sPinId;      ///< as on every block (plParticlePropertyPin)
};

/// Raises the event for every particle that dies, however it dies: age, Kill If, a collision, the bounds.
class plParticleRaiseOnDeathBlock : public plParticleEventRaiserBlock
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRaiseOnDeathBlock, plParticleEventRaiserBlock);
};

/// Raises the event for every collision the system's Raycast blocks find.
class plParticleRaiseOnCollisionBlock : public plParticleEventRaiserBlock
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRaiseOnCollisionBlock, plParticleEventRaiserBlock);
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
  plHybridArray<plParticleEventRaiserBlock*, 2> m_EventRaisers;

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
    Modulo, ///< the remainder of A / B, with the sign of A

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

/// Combines two values. Either input can be wired or left as the literal on the node.
///
/// Like every adaptive operator it computes in the widest kind wired into it: numbers, vectors or colours, with a
/// number applied to every component of the others.
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

/// Picks one of two values. This is the If behavior's job, done in the graph instead of per particle.
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

/// Reads one of the effect's named float parameters, live.
///
/// A wire from it is evaluated every step, so gameplay drives any property it reaches through
/// plParticleEffectController::SetParameter. The parameter's default is what the effect starts with.
class plParticleParameterNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleParameterNode, plReflectedClass);

public:
  plString m_sName;
};

/// The per-step numbers a System Value node reads.
struct plParticleSystemValue
{
  using StorageType = plUInt8;

  enum Enum
  {
    Time,           ///< seconds on the effect's own clock
    DeltaTime,      ///< length of the current step
    CameraDistance, ///< effect origin to the main camera
    ParticleCount,  ///< alive particles in this system
    SpawnScale,     ///< LOD x budget x Particles.SpawnCountScale

    Default = Time
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleSystemValue);

/// The per-step vectors a System Vector node reads.
struct plParticleSystemVector
{
  using StorageType = plUInt8;

  enum Enum
  {
    EffectPosition, ///< effect origin in simulation space
    EffectForward,  ///< the effect's +X
    EffectUp,       ///< the effect's +Z
    EffectVelocity, ///< the owner's velocity
    CameraPosition, ///< the main camera
    EffectRight,    ///< the effect's +Y
    EffectScale,    ///< the effect's scale per axis
    CameraForward,  ///< the main camera's view direction
    CameraRight,
    CameraUp,

    Default = EffectPosition
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleSystemVector);

/// A number that changes every step: time, camera distance, particle count.
class plParticleSystemValueNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSystemValueNode, plReflectedClass);

public:
  plEnum<plParticleSystemValue> m_Input;
};

/// A vector that changes every step: the effect's position, axes and velocity, the camera position.
class plParticleSystemVectorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSystemVectorNode, plReflectedClass);

public:
  plEnum<plParticleSystemVector> m_Input;
};

/// A vector constant.
class plParticleVectorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleVectorNode, plReflectedClass);

public:
  plString m_sName;
  plVec3 m_vValue = plVec3(0, 0, 1);
};

struct plParticleFunctionOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    Sine,   ///< of degrees
    Cosine, ///< of degrees
    Abs,
    Floor,
    Ceil,
    Frac,
    Sqrt,
    Saturate, ///< clamp to 0..1
    Negate,
    OneMinus,
    Tan,  ///< of degrees
    ASin, ///< in degrees
    ACos, ///< in degrees
    ATan, ///< in degrees
    Exp,
    Log, ///< natural logarithm
    Log2,
    Round,
    Trunc,
    Sign,
    Reciprocal, ///< 1 / x, and 0 for 0
    Square,
    DegreesToRadians,
    RadiansToDegrees,

    Default = Sine
  };

  /// The ops whose input (Sin, Cos, Tan) or result (ASin, ACos, ATan) is an angle.
  static bool IsTrigonometric(plInt64 iOperation);
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleFunctionOp);

/// One value in, one value out, per component for vectors and colours.
class plParticleFunctionNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleFunctionNode, plReflectedClass);

public:
  plEnum<plParticleFunctionOp> m_Operation;
  double m_fValue = 0.0;

  /// The angles of the trigonometric ops are radians instead of degrees. Nodes of type version 1 load with it set.
  bool m_bRadians = false;
};

/// Maps a value from one range to another, by default clamped to the target range. Every range end is a pin; an
/// unwired one uses the typed value.
class plParticleRemapNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRemapNode, plReflectedClass);

public:
  double m_fValue = 0.0;
  double m_fInMin = 0.0;
  double m_fInMax = 1.0;
  double m_fOutMin = 0.0;
  double m_fOutMax = 1.0;
  bool m_bClamp = true;
};

/// A + (B - A) * T.
class plParticleLerpNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleLerpNode, plReflectedClass);

public:
  double m_fA = 0.0;
  double m_fB = 1.0;
  double m_fT = 0.5;
};

/// Keeps a value between Min and Max.
class plParticleClampNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleClampNode, plReflectedClass);

public:
  double m_fValue = 0.0;
  double m_fMin = 0.0;
  double m_fMax = 1.0;
};

struct plParticleVectorMathOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    Add,
    Subtract,
    Multiply, ///< per component
    Cross,
    Min,
    Max,

    Default = Add
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleVectorMathOp);

/// Combines two vectors.
class plParticleVectorMathNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleVectorMathNode, plReflectedClass);

public:
  plEnum<plParticleVectorMathOp> m_Operation;
  plVec3 m_vA = plVec3::MakeZero();
  plVec3 m_vB = plVec3::MakeZero();
};

struct plParticleVectorFunctionOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    Normalize,
    Negate,
    Abs,

    Default = Normalize
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleVectorFunctionOp);

/// One vector in, one vector out.
class plParticleVectorFunctionNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleVectorFunctionNode, plReflectedClass);

public:
  plEnum<plParticleVectorFunctionOp> m_Operation;
  plVec3 m_vValue = plVec3(0, 0, 1);
};

struct plParticleVectorReduceOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    Length,
    X,
    Y,
    Z,

    Default = Length
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleVectorReduceOp);

/// A number from a vector: its length or one component.
class plParticleVectorReduceNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleVectorReduceNode, plReflectedClass);

public:
  plEnum<plParticleVectorReduceOp> m_Operation;
  plVec3 m_vValue = plVec3::MakeZero();
};

struct plParticleColorChannelOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    R,
    G,
    B,
    A,

    Default = A
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleColorChannelOp);

/// One channel of a colour as a number.
class plParticleColorChannelNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleColorChannelNode, plReflectedClass);

public:
  plEnum<plParticleColorChannelOp> m_Operation;
  plColor m_Value = plColor::White;
};

/// A formula as text, of up to four numbers A to D, such as "a * (1 + 0.5 * sin(b * 12.57))".
///
/// The code compiles into the same program as the rest of the graph; variables are a, b, c and d. Besides the
/// expression VM's functions it can call random(seed), noise(x, y, z[, octaves]), curve(t) (the node's Curve),
/// step(edge, x), sign(x) and atan2(y, x), and read pi and tau. Its angles are radians, as in most code.
class plParticleExpressionNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleExpressionNode, plReflectedClass);

public:
  plString m_sExpression = "a";
  double m_fA = 0.0;
  double m_fB = 0.0;
  double m_fC = 0.0;
  double m_fD = 0.0;
  plString m_sCurve; ///< what curve(t) samples
};

/// The dot product of two vectors.
class plParticleDotNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleDotNode, plReflectedClass);

public:
  plVec3 m_vA = plVec3::MakeZero();
  plVec3 m_vB = plVec3::MakeZero();
};

/// A vector times a number.
class plParticleScaleVectorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleScaleVectorNode, plReflectedClass);

public:
  plVec3 m_vVector = plVec3(0, 0, 1);
  double m_fScale = 1.0;
};

/// A vector from three numbers.
class plParticleMakeVectorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleMakeVectorNode, plReflectedClass);

public:
  double m_fX = 0.0;
  double m_fY = 0.0;
  double m_fZ = 0.0;
};

/// The colour of a gradient asset at T, from 0 at the gradient's start to 1 at its end. Evaluated
/// while the effect runs (per frame at least), because the gradient is only known to the runtime.
/// A Gradient node wired into its Gradient pin replaces the one picked on the node.
class plParticleSampleGradientNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSampleGradientNode, plReflectedClass);

public:
  plString m_sGradient;
  double m_fT = 0.0;
};

/// The value of a curve asset at T, from 0 at the curve's start to 1 at its end, as the curve stores it.
/// Evaluated while the effect runs, like Sample Gradient. A Curve node wired into its Curve pin replaces the one
/// picked on the node.
class plParticleSampleCurveNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSampleCurveNode, plReflectedClass);

public:
  plString m_sCurve;
  double m_fT = 0.0;
};

/// Smooth 3D noise in 0..1 at Position x Frequency, summed over Octaves of rising frequency. Octaves takes constant and
/// per-frame values only.
class plParticleNoiseNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleNoiseNode, plReflectedClass);

public:
  plVec3 m_vPosition = plVec3::MakeZero();
  double m_fFrequency = 1.0;
  plUInt32 m_uiOctaves = 1;
};

/// The per-particle numbers a Particle Value node reads.
struct plParticleInputValue
{
  using StorageType = plUInt8;

  enum Enum
  {
    Speed,         ///< length of the velocity
    Age,           ///< seconds since spawn; 0 in Initialize
    NormalizedAge, ///< age / lifetime; 0 in Initialize
    Lifetime,      ///< seconds; the system's lifetime in Initialize, where a particle's own is not set yet
    Size,
    RotationSpeed, ///< radians per second
    RemainingLife, ///< seconds until it dies: lifetime minus age; the system's lifetime in Initialize

    Default = Speed
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleInputValue);

/// The per-particle vectors a Particle Vector node reads.
struct plParticleInputVector
{
  using StorageType = plUInt8;

  enum Enum
  {
    Position,
    Velocity,
    Axis, ///< mesh and shape initializers write the surface normal here

    Default = Position
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleInputVector);

/// A number of each particle. Per-particle wires drive Set Attribute and Kill If.
class plParticleInputValueNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInputValueNode, plReflectedClass);

public:
  plEnum<plParticleInputValue> m_Input;
};

/// A vector of each particle.
class plParticleInputVectorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInputVectorNode, plReflectedClass);

public:
  plEnum<plParticleInputVector> m_Input;
};

/// The colour of each particle.
class plParticleInputColorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInputColorNode, plReflectedClass);
};

/// A random number in 0..1 that stays the same for a particle all its life. Nodes with different
/// seeds give unrelated numbers.
class plParticleRandomNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRandomNode, plReflectedClass);

public:
  plInt32 m_iSeed = 0;
};

/// Reads a custom attribute of the system: a named per-particle value a Set Attribute block writes.
class plParticleCustomAttributeNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleCustomAttributeNode, plReflectedClass);

public:
  plString m_sName;
};

class plParticleCustomFloatNode : public plParticleCustomAttributeNode
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleCustomFloatNode, plParticleCustomAttributeNode);
};

class plParticleCustomVectorNode : public plParticleCustomAttributeNode
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleCustomVectorNode, plParticleCustomAttributeNode);
};

class plParticleCustomColorNode : public plParticleCustomAttributeNode
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleCustomColorNode, plParticleCustomAttributeNode);
};

/// The vectors of the event that spawned a particle, which an Event node reads.
struct plParticleEventInput
{
  using StorageType = plUInt8;

  enum Enum
  {
    EventPosition, ///< where the event happened
    EventVelocity, ///< the velocity of the particle that raised it
    EventNormal,   ///< the surface normal of a collision; zero for a death

    Default = EventPosition
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleEventInput);

/// A vector of the event that spawned each particle: per particle, for systems whose On Event spawns
/// per event. Particles spawned any other way read zero.
class plParticleEventInputNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEventInputNode, plReflectedClass);

public:
  plEnum<plParticleEventInput> m_Input;
};

/// Reads an entry of a global blackboard once per frame, so game state drives the effect. A missing
/// entry reads as the node's Default.
class plParticleBlackboardNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBlackboardNode, plReflectedClass);

public:
  plString m_sEntry;
  plString m_sBlackboard;
};

/// A number entry: a float, an int or a bool (0 or 1).
class plParticleBlackboardEntryNode : public plParticleBlackboardNode
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBlackboardEntryNode, plParticleBlackboardNode);

public:
  double m_fDefault = 0.0;
};

/// A vector entry.
class plParticleBlackboardVectorNode : public plParticleBlackboardNode
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBlackboardVectorNode, plParticleBlackboardNode);

public:
  plVec3 m_vDefault = plVec3::MakeZero();
};

/// A colour entry.
class plParticleBlackboardColorNode : public plParticleBlackboardNode
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBlackboardColorNode, plParticleBlackboardNode);

public:
  plColor m_Default = plColor::White;
};

//////////////////////////////////////////////////////////////////////////
// Vector and colour

/// A vector's three components, each on its own output.
class plParticleSplitVectorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSplitVectorNode, plReflectedClass);

public:
  plVec3 m_vVector = plVec3::MakeZero();
};

/// A colour's four channels, each on its own output.
class plParticleSplitColorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSplitColorNode, plReflectedClass);

public:
  plColor m_Colour = plColor::White;
};

/// A colour from four numbers; an unwired channel uses the typed value.
class plParticleMakeColorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleMakeColorNode, plReflectedClass);

public:
  double m_fR = 1.0;
  double m_fG = 1.0;
  double m_fB = 1.0;
  double m_fA = 1.0;
};

/// A vector with its components reordered or repeated by a pattern of three of x, y and z, such as "zyx" or "xxz".
class plParticleSwizzleNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSwizzleNode, plReflectedClass);

public:
  plString m_sPattern = "xyz";
  plVec3 m_vVector = plVec3::MakeZero();

  /// The components the pattern picks (0 to 2); false when it is not three of x, y, z (or r, g, b).
  static bool ParsePattern(plStringView sPattern, plUInt32* out_pComponents);
};

/// The distance between two points.
class plParticleDistanceNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleDistanceNode, plReflectedClass);

public:
  plVec3 m_vA = plVec3::MakeZero();
  plVec3 m_vB = plVec3::MakeZero();
};

/// The cross product of two vectors.
class plParticleCrossNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleCrossNode, plReflectedClass);

public:
  plVec3 m_vA = plVec3(1, 0, 0);
  plVec3 m_vB = plVec3(0, 1, 0);
};

/// A direction mirrored about a normal.
class plParticleReflectNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleReflectNode, plReflectedClass);

public:
  plVec3 m_vDirection = plVec3(0, 0, -1);
  plVec3 m_vNormal = plVec3(0, 0, 1);
};

/// The part of a vector along another one.
class plParticleProjectNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleProjectNode, plReflectedClass);

public:
  plVec3 m_vVector = plVec3(1, 0, 0);
  plVec3 m_vOnto = plVec3(0, 0, 1);
};

/// A vector rotated about an axis by an angle in degrees, right-handed.
class plParticleRotateAboutAxisNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRotateAboutAxisNode, plReflectedClass);

public:
  plVec3 m_vVector = plVec3(1, 0, 0);
  plVec3 m_vAxis = plVec3(0, 0, 1);
  double m_fDegrees = 90.0;
};

//////////////////////////////////////////////////////////////////////////
// Maths

/// Base raised to Exponent, per component.
class plParticlePowerNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticlePowerNode, plReflectedClass);

public:
  double m_fBase = 1.0;
  double m_fExponent = 2.0;
};

/// 0 below Edge, 1 at or above it, per component.
class plParticleStepNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleStepNode, plReflectedClass);

public:
  double m_fEdge = 0.5;
  double m_fX = 0.0;
};

/// A smooth 0-to-1 ramp of X between Edge 0 and Edge 1, per component; Smoother flattens its ends further.
class plParticleSmoothStepNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSmoothStepNode, plReflectedClass);

public:
  double m_fEdge0 = 0.0;
  double m_fEdge1 = 1.0;
  double m_fX = 0.0;
  bool m_bSmoother = false;
};

/// Where Value sits between A and B: 0 at A, 1 at B.
class plParticleInverseLerpNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInverseLerpNode, plReflectedClass);

public:
  double m_fA = 0.0;
  double m_fB = 1.0;
  double m_fValue = 0.5;
  bool m_bClamp = true;
};

/// The angle of (X, Y) from the X axis, in degrees, -180..180.
class plParticleATan2Node : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleATan2Node, plReflectedClass);

public:
  double m_fY = 0.0;
  double m_fX = 1.0;
};

/// The constants a Math Constant node gives.
struct plParticleMathConstant
{
  using StorageType = plUInt8;

  enum Enum
  {
    Pi,
    Tau, ///< 2 pi
    E,   ///< Euler's number

    Default = Pi
  };

  static double GetValue(plInt64 iConstant);
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleMathConstant);

class plParticleMathConstantNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleMathConstantNode, plReflectedClass);

public:
  plEnum<plParticleMathConstant> m_Constant;
};

//////////////////////////////////////////////////////////////////////////
// Logic

struct plParticleLogicOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    And,
    Or,
    Xor, ///< one of the two, not both

    Default = And
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleLogicOp);

/// Combines two conditions.
class plParticleLogicNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleLogicNode, plReflectedClass);

public:
  plEnum<plParticleLogicOp> m_Operation;
  bool m_bA = true;
  bool m_bB = true;
};

/// The opposite of a condition.
class plParticleNotNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleNotNode, plReflectedClass);

public:
  bool m_bValue = false;
};

//////////////////////////////////////////////////////////////////////////
// Random and noise

/// A random value between Min and Max that stays the same for a particle all its life; vectors and colours get their
/// own random value per component.
class plParticleRandomRangeNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRandomRangeNode, plReflectedClass);

public:
  plInt32 m_iSeed = 0;
  double m_fMin = 0.0;
  double m_fMax = 1.0;
};

/// A random whole number from Min to Max, both included, the same for a particle all its life.
class plParticleRandomIntegerNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRandomIntegerNode, plReflectedClass);

public:
  plInt32 m_iSeed = 0;
  plInt32 m_iMin = 0;
  plInt32 m_iMax = 9;
};

struct plParticleRandomVectorShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    InSphere,
    OnSphere,
    InBox,  ///< -Radius to +Radius on every axis
    InCone, ///< a direction at most Cone Angle off +Z, Radius long

    Default = InSphere
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleRandomVectorShape);

/// A random vector in or on a shape, the same for a particle all its life.
class plParticleRandomVectorNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleRandomVectorNode, plReflectedClass);

public:
  plEnum<plParticleRandomVectorShape> m_Shape;
  plInt32 m_iSeed = 0;
  double m_fRadius = 1.0;
  double m_fConeAngle = 30.0; ///< degrees
};

/// Three independent channels of smooth noise at Position x Frequency, each -1..1, for jitter and wobble.
class plParticleVectorNoiseNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleVectorNoiseNode, plReflectedClass);

public:
  plVec3 m_vPosition = plVec3::MakeZero();
  double m_fFrequency = 1.0;
  plUInt32 m_uiOctaves = 1;
};

//////////////////////////////////////////////////////////////////////////
// Effect and camera

struct plParticleTransformSpace
{
  using StorageType = plUInt8;

  enum Enum
  {
    LocalToWorld, ///< from the effect's own frame into simulation space
    WorldToLocal,

    Default = LocalToWorld
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleTransformSpace);

struct plParticleTransformKind
{
  using StorageType = plUInt8;

  enum Enum
  {
    Position,  ///< moved, rotated and scaled
    Direction, ///< rotated only

    Default = Position
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleTransformKind);

/// A position or direction moved between the effect's local frame and simulation space, with the effect transform of
/// the current step.
class plParticleTransformNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTransformNode, plReflectedClass);

public:
  plEnum<plParticleTransformSpace> m_Space;
  plEnum<plParticleTransformKind> m_Kind;
  plVec3 m_vVector = plVec3::MakeZero();
};

//////////////////////////////////////////////////////////////////////////
// Curves over life

/// What the T of a Curve Over Life or Gradient Over Life node is, when its T pin is unwired.
struct plParticleCurveInput
{
  using StorageType = plUInt8;

  enum Enum
  {
    NormalizedAge, ///< 0 at birth, 1 at death
    Age,           ///< seconds since birth / Input Max
    Speed,         ///< speed / Input Max

    Default = NormalizedAge
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleCurveInput);

/// The palette presets of Curve Over Life. A preset node gets its curve and a Set Attribute block once it is added, and
/// the property goes back to None.
struct plParticleCurvePreset
{
  using StorageType = plUInt8;

  enum Enum
  {
    None,
    SizeOverLife,
    OpacityOverLife,
    EmissiveOverLife,

    Default = None
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_EDITORPLUGINPARTICLE_DLL, plParticleCurvePreset);

/// A curve over a particle's life, mapped into Output Min..Max.
///
/// The curve is drawn from 0 to 1 in both directions and stored in the effect; with no keys it is a straight line from
/// 0 to 1. A Curve asset replaces it, its values mapped to 0..1 the way Size Curve maps them. The T pin, when wired,
/// replaces the Input choice.
class plParticleCurveOverLifeNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleCurveOverLifeNode, plReflectedClass);

public:
  plEnum<plParticleCurveInput> m_Input;
  double m_fInputMax = 1.0; ///< seconds for Age, metres per second for Speed: the T of 1
  plSingleCurveData m_Curve;
  plString m_sCurve;
  double m_fOutputMin = 0.0;
  double m_fOutputMax = 1.0;
  plEnum<plParticleCurvePreset> m_Preset;
};

/// One stop of an inline gradient: a colour (HDR, with alpha) at a position from 0 to 1.
class plParticleGradientStop : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleGradientStop, plReflectedClass);

public:
  float m_fPosition = 0.0f;
  plColor m_Color = plColor::White;
};

/// A colour gradient over a particle's life.
///
/// The stops are stored in the effect and blend linearly; with none the gradient is white. A Gradient asset replaces
/// them. The T pin, when wired, replaces the Input choice.
class plParticleGradientOverLifeNode : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleGradientOverLifeNode, plReflectedClass);

public:
  plEnum<plParticleCurveInput> m_Input;
  double m_fInputMax = 1.0;
  plDynamicArray<plParticleGradientStop> m_Stops;
  plString m_sGradient;
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
    Adaptive, ///< an operator input that takes a number, vector or colour; the node computes in the widest one wired
  };

  /// The kind a property accepts, or None when it must stay constant (enums, flags, structs).
  static Enum FromProperty(const plAbstractProperty* pProp);

  /// The kind an operator node produces with nothing wired, plus the property holding its value. The property is
  /// empty for computed nodes, whose value comes from evaluating their inputs. Adaptive nodes give Number here;
  /// plParticleGraphCompiler::GetOutputKind gives what they produce with their wires.
  static Enum FromNode(const plRTTI* pNodeType, plStringView& out_sValueProperty);

  /// The kind one of an operator node's input pins accepts, or None if there is no such pin.
  static Enum FromNodeInput(const plRTTI* pNodeType, plStringView sPinName);

  /// True for operator nodes whose Adaptive inputs decide what they produce.
  static bool IsAdaptiveNode(const plRTTI* pNodeType);

  /// Whether a value of one kind can feed an input of another: the same kind, numbers and bools both ways,
  /// vectors and colours both ways (a vector becomes a colour with A = 1, a colour a vector without its A), and
  /// numbers, bools, vectors and colours into an Adaptive input.
  static bool CanConvert(Enum source, Enum target);

  /// The wider of two kinds an adaptive node meets: Number (and Bool) < Vector < Color.
  static Enum Widen(Enum a, Enum b);

  /// "number", "vector", "colour" and so on, for messages.
  static const char* GetName(Enum kind);
};

/// The input pins an operator node offers, and the literal used when one is left unwired.
struct plParticleOperatorInputs
{
  struct Input
  {
    const char* m_szPin;
    const char* m_szLiteral; ///< property holding the value used when the pin is unconnected
    plParticleValueKind::Enum m_Kind;
    bool m_bUniform = false; ///< takes constant and per-frame values only, never per-particle ones
  };

  static plArrayPtr<const Input> Get(const plRTTI* pNodeType);

  /// The input of a node by pin name, or null.
  static const Input* Find(const plRTTI* pNodeType, plStringView sPinName);
};

/// The output pins an operator node offers. Most have one, "Value"; Split Vector and Split Colour have one per
/// component.
struct plParticleOperatorOutputs
{
  struct Output
  {
    const char* m_szPin;
    plParticleValueKind::Enum m_Kind; ///< None: the node's own kind (plParticleValueKind::FromNode)
  };

  static plArrayPtr<const Output> Get(const plRTTI* pNodeType);

  /// The index of an output pin by name; 0 for a name the node does not have.
  static plUInt32 FindIndex(const plRTTI* pNodeType, plStringView sPinName);
};

/// Encodes and decodes the pin name identifying one property of one block.
///
/// Connections store pin names as plain strings, so a per-property pin needs no engine support:
/// the name carries the owning block's key and the property it feeds. The key is the block's PinId
/// when it has one, else its guid. Paste gives objects new guids but keeps PinId, which is why a
/// copy stamps every block's key into PinId first: the pasted wires still name their blocks.
struct plParticlePropertyPin
{
  static plString Make(const plDocumentObject* pBlock, plStringView sProperty);
  static bool Parse(plStringView sPinName, plStringBuilder& out_sKey, plStringBuilder& out_sProperty);

  /// The key pin names use for a block.
  static plString GetKey(const plDocumentObject* pBlock);

  /// The block of pSystemNode a pin name refers to, or null.
  static const plDocumentObject* FindBlock(const plDocumentObject* pSystemNode, plStringView sPinName, plStringBuilder& out_sProperty);

  /// Writes each block's key into its PinId in a copied graph, so the copy's wires survive the paste.
  static void StampKeys(plAbstractObjectGraph& ref_graph);

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