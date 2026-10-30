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

/// Reads the operator graph that feeds a system's property pins: tiers, folded constants, short
/// descriptions for the UI, problems, and the per-frame program.
///
/// One instance per document query; it caches per node, so build a new one after any edit.
class PL_EDITORPLUGINPARTICLE_DLL plParticleGraphCompiler
{
public:
  plParticleGraphCompiler(const plVisualGraphObjectManager* pManager, const plDocumentObject* pEffectNode);
  ~plParticleGraphCompiler();

  /// The node feeding one input pin, or null when the pin is unwired.
  const plDocumentObject* GetInputSource(const plDocumentObject* pNode, plStringView sPinName) const;

  plParticleTier GetTier(const plDocumentObject* pNode);

  /// The node's value with every per-frame input at its start value (time 0, parameters at their
  /// defaults). Constant wires fold to exactly this; per-frame wires start from it. Invalid when the
  /// node cannot produce a value.
  plVariant Fold(const plDocumentObject* pNode);

  /// A short text for the row a wire drives, such as "Intensity × 60".
  plString Describe(const plDocumentObject* pNode, plUInt32 uiDepth = 2) const;

  /// Why a node cannot produce a value, or empty when it can.
  plString GetIssue(const plDocumentObject* pNode) const;

  /// Per-particle programs: whether the program runs at spawn (Initialize) or every step (Update).
  /// At spawn a particle has no age yet and its lifetime is not chosen, so those read as 0 and as
  /// the system's lifetime.
  void SetParticleContext(bool bInitialize, double fSystemLifetime);

  /// True for the nodes that read per-particle data.
  static bool IsParticleInputNode(const plRTTI* pType);

  /// Adds the value of pSource as the next program output, converted to what the property takes.
  /// Returns the output's index, or plInvalidIndex when the subgraph cannot be expressed.
  plUInt32 AddOutput(const plDocumentObject* pSource, plParticleValueKind::Enum targetKind);

  bool HasOutputs() const { return !m_Ast.m_OutputNodes.IsEmpty(); }

  /// Adds the value of pSource as one number, for the value spy: a bool as 0 or 1, a vector as its
  /// length, a colour as its luminance. Returns plInvalidIndex when the node has no value.
  plUInt32 AddSpyOutput(const plDocumentObject* pSource);

  plResult Compile(plExpressionByteCode& out_byteCode);

  /// The gradients and curves the outputs sample, which the program has to carry.
  const plParticleProgramAssets& GetAssets() const { return m_Assets; }

  /// True for the nodes that sample assets or noise: the runtime evaluates them, so they are per
  /// frame even when everything feeding them is constant.
  static bool IsRuntimeSampledNode(const plRTTI* pType);

  /// Parses the code of an Expression node the way the runtime's 1.3 Expression block did:
  /// "out = <code>" with the inputs a, b, c and d. On failure out_pError gets the parser's message.
  static plResult ParseExpression(plStringView sCode, plExpressionAST& out_ast, plString* out_pError = nullptr);

private:
  struct AstValue
  {
    plExpressionAST::Node* m_pNode = nullptr;
    plParticleValueKind::Enum m_Kind = plParticleValueKind::None;
  };

  plVariant FoldInternal(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);
  plVariant FoldInput(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plSet<const plDocumentObject*>& ref_visited);

  AstValue Build(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);
  AstValue BuildInput(const plDocumentObject* pNode, const char* szPin, const char* szLiteral, plParticleValueKind::Enum kind, plSet<const plDocumentObject*>& ref_visited);
  AstValue Convert(const AstValue& value, plParticleValueKind::Enum kind);
  plExpressionAST::Node* Number(double fValue);
  plExpressionAST::Node* Vector(const plVec3& v);
  plExpressionAST::Node* Color(const plColor& c);
  plExpressionAST::Node* Input(plStringView sName, plProcessingStream::DataType type);

  /// A literal or folded value as a constant of the given kind.
  AstValue Constant(const plVariant& value, plParticleValueKind::Enum kind);

  AstValue BuildParticleInput(const plDocumentObject* pNode);
  AstValue BuildBlackboardEntry(const plDocumentObject* pNode);
  AstValue BuildSampled(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);
  AstValue BuildExpression(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);

  /// An Expression node's value with constant inputs, computed on the VM like the runtime would.
  plVariant FoldExpression(const plDocumentObject* pNode, plSet<const plDocumentObject*>& ref_visited);

  /// The slot of an asset in the program's list, added when it is new.
  static plInt32 GetAssetSlot(plHybridArray<plString, 2>& ref_assets, const plString& sAsset);

  const plVisualGraphObjectManager* m_pManager = nullptr;
  const plDocumentObject* m_pEffectNode = nullptr;
  bool m_bInitializeContext = false;
  double m_fSystemLifetime = 1.0;

  plHashTable<const plDocumentObject*, plParticleTier> m_Tiers;
  plHashTable<const plDocumentObject*, AstValue> m_Built;
  plHashTable<plString, plExpressionAST::Node*> m_Inputs;
  plExpressionAST m_Ast;
  plParticleProgramAssets m_Assets;

  /// The parsed code of Expression nodes; m_Ast links to their nodes, so they live as long as it does.
  plDynamicArray<plUniquePtr<plExpressionAST>> m_ExpressionAsts;
};
