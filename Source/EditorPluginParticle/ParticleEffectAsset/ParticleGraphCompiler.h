#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <Foundation/CodeUtils/Expression/ExpressionAST.h>
#include <Foundation/Containers/HashTable.h>
#include <ParticlePlugin/System/ParticleProgram.h>

class plExpressionByteCode;

/// When the value on a wire is computed.
///
/// A wire's tier is the slowest tier among the nodes feeding it: a constant folds when the asset
/// is built, a per-frame value is computed once per simulation step, a per-particle value (P2) once
/// per particle.
enum class plParticleTier : plUInt8
{
  Constant,
  Frame,
  Particle,
};

/// Reads the operator graph that feeds a system's property pins: tiers, kinds, folded constants, short
/// descriptions for the UI, problems, and the per-frame program.
///
/// A wire starts at one output of a node; nodes with several outputs (Split Vector, Split Colour) are
/// addressed by node and output index. One instance per document query; it caches per node, so build a
/// new one after any edit.
class PL_EDITORPLUGINPARTICLE_DLL plParticleGraphCompiler
{
public:
  plParticleGraphCompiler(const plVisualGraphObjectManager* pManager, const plDocumentObject* pEffectNode);
  ~plParticleGraphCompiler();

  /// The node feeding one input pin, or null when the pin is unwired. out_pOutput gets which of its outputs.
  const plDocumentObject* GetInputSource(const plDocumentObject* pNode, plStringView sPinName, plUInt32* out_pOutput = nullptr) const;

  /// The index of the output pin a wire leaves from.
  static plUInt32 GetOutputIndex(const plVisualGraphPin& sourcePin);

  /// When a node's values are computed; every output of a node shares it.
  plParticleTier GetTier(const plDocumentObject* pNode);

  /// What one output of a node produces with its wires: an adaptive node gives the widest kind wired into it.
  plParticleValueKind::Enum GetOutputKind(const plDocumentObject* pNode, plUInt32 uiOutput = 0);

  /// One output's value with every per-frame input at its start value (time 0, parameters at their
  /// defaults). Constant wires fold to exactly this; per-frame wires start from it. Invalid when the
  /// node cannot produce a value.
  plVariant Fold(const plDocumentObject* pNode, plUInt32 uiOutput = 0);

  /// A short text for the row a wire drives, such as "Intensity × 60".
  plString Describe(const plDocumentObject* pNode, plUInt32 uiDepth = 2, plUInt32 uiOutput = 0) const;

  /// Why a node cannot produce a value or where its value goes wrong, or empty when nothing does.
  plString GetIssue(const plDocumentObject* pNode);

  /// What a node should know that is not a fault, such as a Curve Over Life that keeps its system on the CPU, or
  /// empty.
  plString GetNote(const plDocumentObject* pNode);

  /// Per-particle programs: whether the program runs at spawn (Initialize) or every step (Update).
  /// At spawn a particle has no age yet and its lifetime is not chosen, so those read as 0 and as
  /// the system's lifetime.
  void SetParticleContext(bool bInitialize, double fSystemLifetime);

  /// True for the nodes that read per-particle data.
  static bool IsParticleInputNode(const plRTTI* pType);

  /// Adds one output of pSource as the next program output, converted to what the property takes.
  /// Returns the output's index, or plInvalidIndex when the subgraph cannot be expressed.
  plUInt32 AddOutput(const plDocumentObject* pSource, plParticleValueKind::Enum targetKind, plUInt32 uiSourceOutput = 0);

  bool HasOutputs() const { return !m_Ast.m_OutputNodes.IsEmpty(); }

  /// Adds one output of pSource as one number, for the value spy: a bool as 0 or 1, a vector as its
  /// length, a colour as its luminance. Returns plInvalidIndex when the node has no value.
  plUInt32 AddSpyOutput(const plDocumentObject* pSource, plUInt32 uiSourceOutput = 0);

  /// Compiles the outputs. The inline tables get their slots after the asset slots here, once the asset lists are
  /// complete.
  plResult Compile(plExpressionByteCode& out_byteCode);

  /// The gradients and curves the outputs sample, which the program has to carry.
  const plParticleProgramAssets& GetAssets() const { return m_Assets; }

  /// True for the nodes that sample assets or noise: the runtime evaluates them, so they are per
  /// frame even when everything feeding them is constant.
  static bool IsRuntimeSampledNode(const plRTTI* pType);

  /// Parses the code of an Expression node the way the runtime's 1.3 Expression block did:
  /// "out = <code>" with the inputs a, b, c and d, plus pi, tau and the particle functions (random, noise,
  /// curve, step, sign, atan2). On failure out_pError gets the parser's message.
  static plResult ParseExpression(plStringView sCode, plExpressionAST& out_ast, plString* out_pError = nullptr);

  /// The inline curve of a Curve Over Life node baked into a table of its 0..1 values: its keys, or the straight
  /// line from 0 to 1 when it has none.
  static void BakeInlineCurve(const plDocumentObject* pNode, plArrayPtr<float> out_table);

  /// The inline stops of a Gradient Over Life node baked into a table of colours; white when it has none.
  static void BakeInlineGradient(const plDocumentObject* pNode, plArrayPtr<plVec4> out_table);

  /// The plSingleCurveData object a Curve Over Life node edits, or null.
  static const plDocumentObject* GetInlineCurveObject(const plDocumentObject* pNode);

private:
  struct AstValue
  {
    plExpressionAST::Node* m_pNode = nullptr;
    plParticleValueKind::Enum m_Kind = plParticleValueKind::None;
  };

  /// Every output of a built node.
  struct BuiltNode
  {
    AstValue m_Outputs[4];
  };

  /// A slot constant of an inline table, given its number when the program compiles.
  struct InlineSlot
  {
    plExpressionAST::Constant* m_pConstant = nullptr;
    plUInt32 m_uiIndex = 0;
    bool m_bGradient = false;
  };

  plVariant FoldInternal(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, plUInt32 uiOutput = 0);
  plVariant FoldInput(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plSet<const plDocumentObject*>& ref_visited);
  plVariant FoldNewNode(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, plUInt32 uiOutput, bool& out_bHandled);

  /// A pin's folded value as four lanes in the given kind: a number in all four, a vector in xyz.
  plVec4 FoldLanes(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plParticleValueKind::Enum kind, plSet<const plDocumentObject*>& ref_visited);

  AstValue Build(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, plUInt32 uiOutput = 0);
  void BuildNode(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, BuiltNode& out_built);
  AstValue BuildNewNode(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited, BuiltNode& out_built, bool& out_bHandled);
  AstValue BuildInput(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plParticleValueKind::Enum kind, plSet<const plDocumentObject*>& ref_visited);
  AstValue Convert(const AstValue& value, plParticleValueKind::Enum kind);
  plExpressionAST::Node* Number(double fValue);
  plExpressionAST::Node* Vector(const plVec3& v);
  plExpressionAST::Node* Color(const plColor& c);
  plExpressionAST::Node* Input(plStringView sName, plProcessingStream::DataType type);

  /// A literal or folded value as a constant of the given kind; a number fills every component.
  AstValue Constant(const plVariant& value, plParticleValueKind::Enum kind);

  AstValue BuildParticleInput(const plDocumentObject* pNode);
  plExpressionAST::Node* BuildParticleValue(plInt64 iInput);
  AstValue BuildBlackboardEntry(const plDocumentObject* pNode);
  AstValue BuildSampled(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);
  AstValue BuildExpression(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);
  AstValue BuildOverLife(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);

  /// The T of a Curve Over Life or Gradient Over Life node: its wire, else what its Input choice reads.
  plExpressionAST::Node* BuildLifeInput(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);

  /// The effect's frame inputs, for the Transform nodes.
  plParticleProgramOps::Frame BuildEffectFrame();

  /// An Expression node's value with constant inputs, computed on the VM like the runtime would.
  plVariant FoldExpression(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);

  /// The tier the functions an Expression node's code calls need: per particle for random, per frame for noise and curves.
  static plParticleTier GetExpressionCallTier(plStringView sCode);

  /// The slot of a gradient asset in the program's list, added when it is new.
  static plInt32 GetAssetSlot(plHybridArray<plString, 2>& ref_assets, const plString& sAsset);

  /// The slot of a curve asset, read raw or mapped to 0..1, added when it is new.
  plInt32 GetCurveSlot(const plString& sAsset, bool bNormalized);

  /// The slot constant of a new inline table.
  plExpressionAST::Node* AddInlineCurve(plArrayPtr<const float> table);
  plExpressionAST::Node* AddInlineGradient(plArrayPtr<const plVec4> table);

  /// The Set Attribute targets one node's values reach, and whether they are in a system that runs on the GPU.
  struct SinkTarget
  {
    plInt64 m_iTarget = 0;
    bool m_bInitialize = false;
    bool m_bGpuSystem = false;
  };
  void CollectSinkTargets(const plDocumentObject* pNode, plDynamicArray<SinkTarget>& out_targets) const;

  const plVisualGraphObjectManager* m_pManager = nullptr;
  const plDocumentObject* m_pEffectNode = nullptr;
  bool m_bInitializeContext = false;
  double m_fSystemLifetime = 1.0;

  plHashTable<const plDocumentObject*, plParticleTier> m_Tiers;
  plHashTable<const plDocumentObject*, plParticleValueKind::Enum> m_Kinds;
  plHashTable<const plDocumentObject*, BuiltNode> m_Built;
  plHashTable<plString, plExpressionAST::Node*> m_Inputs;
  plExpressionAST m_Ast;
  plParticleProgramAssets m_Assets;
  plHybridArray<InlineSlot, 4> m_InlineSlots;

  /// The parsed code of Expression nodes; m_Ast links to their nodes, so they live as long as it does.
  plDynamicArray<plUniquePtr<plExpressionAST>> m_ExpressionAsts;
};
