#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <Foundation/Basics.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class plOrbitCamViewWidget;
class plParticleEffectAssetDocument;

/// The particle effect's window: the preview, its timeline ("Timeline", plEditorUiParticleTimeline) and the effect graph
/// ("Graph", plEditorUiGraphView with plParticleEffectGraphController). The engine shell adds the properties and
/// arranges the panels (plParticleEffectShellPanels). Edits are sent to the engine process as a live preview of the
/// effect.
class plParticleEffectAssetDocumentWindow : public plEngineDocumentWindow
{
public:
  plParticleEffectAssetDocumentWindow(plAssetDocument* pDocument);
  ~plParticleEffectAssetDocumentWindow();

  plParticleEffectAssetDocument* GetParticleDocument();

protected:
  virtual void InternalRedraw() override;

private:
  void CreatePreview();
  void CreateGraph();
  void SendRedrawMsg();
  void RestoreResource();
  void SendLiveResourcePreview();
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void ParticleEventHandler(const plParticleEffectAssetEvent& e);

  plParticleEffectAssetDocument* m_pAssetDoc;

  plEngineViewConfig m_ViewConfig;
  plOrbitCamViewWidget* m_pViewWidget;

  bool m_bDoLiveResourceUpdate = true;

  plEngineViewLightSettings m_LightSettings;
};
