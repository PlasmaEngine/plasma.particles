#pragma once

#include <ToolsFoundation/VisualGraph/VisualGraphObjectManager.h>

/// Object manager for particle effect visual graphs.
///
/// Manages nodes representing particle systems (layers), emitters, initializers, behaviors,
/// renderers, and event reactions. Validates connections so that only compatible pin categories
/// can be linked (e.g., an emitter output can only connect to a system's emitter input).
class plParticleEffectNodeManager : public plVisualGraphObjectManager
{
public:
  virtual bool InternalIsNode(const plDocumentObject* pObject) const override;
  virtual void InternalCreatePins(const plDocumentObject* pObject, NodeInternal& ref_node) override;
  virtual void GetCreateableTypes(plHybridArray<const plRTTI*, 32>& ref_types) const override;
  virtual plStatus InternalCanConnect(const plVisualGraphPin& source, const plVisualGraphPin& target, CanConnectResult& out_result) const override;
  virtual bool InternalIsDynamicPinProperty(const plDocumentObject* pObject, const plAbstractProperty* pProp) const override;
};