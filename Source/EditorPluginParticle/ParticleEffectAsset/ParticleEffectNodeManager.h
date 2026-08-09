#pragma once

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <ToolsFoundation/VisualGraph/VisualGraphObjectManager.h>

/// Object manager for particle effect visual graphs.
///
/// The graph itself only contains the effect node and its system nodes. Blocks (emitters,
/// initializers, behaviors, render types) are owned by a system in one of its four ordered
/// context arrays, and event reactions are owned by the effect node.
class plParticleEffectNodeManager : public plVisualGraphObjectManager
{
public:
  /// Concrete, non-hidden factory types that can be added as a block of the given context,
  /// sorted by type name.
  static void GetBlockTypes(plParticleContext::Enum context, plDynamicArray<const plRTTI*>& out_types);

  /// Rebuilds the property pins of every system node.
  ///
  /// A node's pins are created when the node itself is added, which during loading happens before
  /// any of its blocks exist — so a freshly loaded document would otherwise have no property pins
  /// until something edited a context. Connections survive the rebuild.
  void RecreateAllSystemPins();

  /// Names the creation menu entries without the redundant type-name decoration: inside the
  /// particle graph, "plParticleMathNode" only usefully reads as "Math".
  virtual void GetNodeCreationTemplates(plDynamicArray<plVisualGraphNodeDesc>& out_templates) const override;

  /// Strips the plParticle prefix and the Node/Factory decoration from a type name.
  static plString FriendlyTypeName(const plRTTI* pType);

  virtual bool InternalIsNode(const plDocumentObject* pObject) const override;
  virtual void InternalCreatePins(const plDocumentObject* pObject, NodeInternal& ref_node) override;
  virtual void GetCreateableTypes(plHybridArray<const plRTTI*, 32>& ref_types) const override;
  virtual plStatus InternalCanConnect(const plVisualGraphPin& source, const plVisualGraphPin& target, CanConnectResult& out_result) const override;
  virtual bool InternalIsDynamicPinProperty(const plDocumentObject* pObject, const plAbstractProperty* pProp) const override;

private:
  /// Backs the plStringViews handed out in the creation templates, which do not own their text.
  mutable plDynamicArray<plString> m_CreationNames;
};