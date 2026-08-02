#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/Assets/AssetDocument.h>
#include <Foundation/Communication/Event.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>

class plParticleEffectAssetDocument;
struct plPropertyMetaStateEvent;

struct plParticleEffectAssetEvent
{
  enum Type
  {
    RestartEffect,
    AutoRestartChanged,
    SimulationSpeedChanged,
    RenderVisualizersChanged,
  };

  plParticleEffectAssetDocument* m_pDocument;
  Type m_Type;
};

class plParticleEffectAssetDocument : public plAssetDocument
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEffectAssetDocument, plAssetDocument);

public:
  plParticleEffectAssetDocument(plStringView sDocumentPath);
  ~plParticleEffectAssetDocument();

  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

  void WriteResource(plStreamWriter& inout_stream) const;

  void TriggerRestartEffect();

  plEvent<const plParticleEffectAssetEvent&> m_Events;

  void SetAutoRestart(bool bEnable);
  bool GetAutoRestart() const { return m_bAutoRestart; }

  void SetSimulationPaused(bool bPaused);
  bool GetSimulationPaused() const { return m_bSimulationPaused; }

  void SetSimulationSpeed(float fSpeed);
  float GetSimulationSpeed() const { return m_fSimulationSpeed; }

  bool GetRenderVisualizers() const { return m_bRenderVisualizers; }
  void SetRenderVisualizers(bool b);

  virtual plResult ComputeObjectTransformation(const plDocumentObject* pObject, plTransform& out_result) const override;

protected:
  virtual void InitializeAfterLoading(bool bFirstTimeCreation) override;
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
  virtual plTransformStatus InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo) override;

  virtual void InternalGetMetaDataHash(const plDocumentObject* pObject, plUInt64& inout_uiHash) const override;
  virtual void AttachMetaDataBeforeSaving(plAbstractObjectGraph& graph) const override;
  virtual void RestoreMetaDataAfterLoading(const plAbstractObjectGraph& graph, bool bUndoable) override;

  virtual void GetSupportedMimeTypesForPasting(plHybridArray<plString, 4>& out_MimeTypes) const override;
  virtual bool CopySelectedObjects(plAbstractObjectGraph& out_objectGraph, plStringBuilder& out_MimeType) const override;
  virtual bool Paste(const plArrayPtr<PasteInfo>& info, const plAbstractObjectGraph& objectGraph, bool bAllowPickedPosition, plStringView sMimeType) override;

private:
  void BuildDescriptorFromGraph(plParticleEffectDescriptor& out_desc) const;
  const plDocumentObject* FindEffectNode() const;
  void CollectConnectedNodes(const plDocumentObject* pNode, plStringView sPinName, plDynamicArray<const plDocumentObject*>& out_nodes) const;

  plEngineViewLightSettings m_LightSettings;

  bool m_bSimulationPaused = false;
  bool m_bAutoRestart = true;
  bool m_bRenderVisualizers = false;
  float m_fSimulationSpeed = 1.0f;
};