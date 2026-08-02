#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.moc.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <Foundation/Basics.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class plQtOrbitCamViewWidget;
class plParticleEffectAssetDocument;
class plQtPropertyGridWidget;
class plQtParticleEffectGraphScene;
class plQtVisualGraphView;


class plQtParticleEffectAssetDocumentWindow : public plQtEngineDocumentWindow
{
  Q_OBJECT

public:
  plQtParticleEffectAssetDocumentWindow(plAssetDocument* pDocument);
  ~plQtParticleEffectAssetDocumentWindow();

  plParticleEffectAssetDocument* GetParticleDocument();

protected:
  virtual void InternalRedraw() override;
private:
  void SendRedrawMsg();
  void RestoreResource();
  void SendLiveResourcePreview();
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void ParticleEventHandler(const plParticleEffectAssetEvent& e);

  plParticleEffectAssetDocument* m_pAssetDoc;

  plEngineViewConfig m_ViewConfig;
  plQtOrbitCamViewWidget* m_pViewWidget;

  plQtParticleEffectGraphScene* m_pScene = nullptr;
  plQtVisualGraphView* m_pView = nullptr;

  bool m_bDoLiveResourceUpdate = true;

  plEngineViewLightSettings m_LightSettings;
};

