#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/DocumentWindow/OrbitCamViewWidget.h>
#include <EditorFramework/InputContexts/EditorInputContext.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAssetWindow.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectGraphView.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectEngineUi.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <GuiFoundation/EngineUi/EngineGraphView.h>
#include <SharedPluginAssets/Common/Messages.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <EditorFramework/Preferences/EditorPreferences.h>

plParticleEffectAssetDocumentWindow::plParticleEffectAssetDocumentWindow(plAssetDocument* pDocument)
  : plEngineDocumentWindow(pDocument)
{
  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plParticleEffectAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plParticleEffectAssetDocumentWindow::StructureEventHandler, this));

  plEditorPreferencesUser* pPreferences = plPreferences::QueryPreferences<plEditorPreferencesUser>();
  pPreferences->ApplyDefaultValues(m_LightSettings);

  SetMenuBarActions("ParticleEffectAssetMenuBar");
  SetToolBarActions("ParticleEffectAssetToolBar");

  SetTargetFramerate(pPreferences->GetMaxFramerate());

  m_pAssetDoc = static_cast<plParticleEffectAssetDocument*>(pDocument);

  CreatePreview();
  CreateGraph();

  GetParticleDocument()->m_Events.AddEventHandler(plMakeDelegate(&plParticleEffectAssetDocumentWindow::ParticleEventHandler, this));
}

plParticleEffectAssetDocumentWindow::~plParticleEffectAssetDocumentWindow()
{
  GetParticleDocument()->m_Events.RemoveEventHandler(plMakeDelegate(&plParticleEffectAssetDocumentWindow::ParticleEventHandler, this));

  RestoreResource();

  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plParticleEffectAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plParticleEffectAssetDocumentWindow::PropertyEventHandler, this));
}

plParticleEffectAssetDocument* plParticleEffectAssetDocumentWindow::GetParticleDocument()
{
  return static_cast<plParticleEffectAssetDocument*>(GetDocument());
}

void plParticleEffectAssetDocumentWindow::CreatePreview()
{
  m_ViewConfig.m_Camera.LookAt(plVec3(-1.6f, 0, 0), plVec3(0, 0, 0), plVec3(0, 0, 1));
  m_ViewConfig.ApplyPerspectiveSetting(90);

  m_pViewWidget = new plOrbitCamViewWidget(this, &m_ViewConfig);
  m_pViewWidget->ConfigureRelative(plVec3(0), plVec3(5.0f), plVec3(-2, 0, 0.5f), 1.0f);
  AddViewWidget(m_pViewWidget);
  m_pViewWidget->SetToolBarActions("ParticleEffectAssetViewToolBar");

  // the timeline sits under the preview it scrubs (plParticleEffectShellPanels)
  AddPanel<plEditorUiParticleTimeline>({"Timeline", "Timeline", plEditorUiDockRole::Bottom}, GetParticleDocument());
}

void plParticleEffectAssetDocumentWindow::CreateGraph()
{
  const plVisualGraphObjectManager* pGraphManager = static_cast<const plVisualGraphObjectManager*>(GetDocument()->GetObjectManager());

  plUniquePtr<plVisualGraphController> pController = PL_DEFAULT_NEW(plParticleEffectGraphController);
  pController->Init(pGraphManager);

  AddPanel<plEditorUiGraphView>({"Graph", "Particle Graph", plEditorUiDockRole::Left}, std::move(pController));
}

void plParticleEffectAssetDocumentWindow::SendLiveResourcePreview()
{
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  plResourceUpdateMsgToEngine msg;
  msg.m_sResourceType = "Particle Effect";

  plStringBuilder tmp;
  msg.m_sResourceID = plConversionUtils::ToString(GetDocument()->GetGuid(), tmp);

  plContiguousMemoryStreamStorage streamStorage;
  plMemoryStreamWriter memoryWriter(&streamStorage);

  // Write Path
  plStringBuilder sAbsFilePath = GetParticleDocument()->GetDocumentPath();
  sAbsFilePath.ChangeFileExtension("plParticleEffect");

  // Write Header
  memoryWriter << sAbsFilePath;
  const plUInt64 uiHash = plAssetCurator::GetSingleton()->GetAssetTransformHash(GetParticleDocument()->GetGuid());
  plAssetFileHeader AssetHeader;
  AssetHeader.SetFileHashAndVersion(uiHash, GetParticleDocument()->GetAssetTypeVersion());
  AssetHeader.Write(memoryWriter).IgnoreResult();

  // Write Asset Data
  GetParticleDocument()->WriteResource(memoryWriter);
  msg.m_Data = plArrayPtr<const plUInt8>(streamStorage.GetData(), streamStorage.GetStorageSize32());

  plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);

  plLog::Dev("Particles: sent live preview update for '{}' ({} bytes)", msg.m_sResourceID, streamStorage.GetStorageSize32());
}

void plParticleEffectAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  m_bDoLiveResourceUpdate = true;
}

void plParticleEffectAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  switch (e.m_EventType)
  {
    case plDocumentObjectStructureEvent::Type::AfterObjectAdded:
    case plDocumentObjectStructureEvent::Type::AfterObjectMoved2:
    case plDocumentObjectStructureEvent::Type::AfterObjectRemoved:
      m_bDoLiveResourceUpdate = true;
      break;

    default:
      break;
  }
}

void plParticleEffectAssetDocumentWindow::ParticleEventHandler(const plParticleEffectAssetEvent& e)
{
  switch (e.m_Type)
  {
    case plParticleEffectAssetEvent::RestartEffect:
    {
      plEditorEngineRestartSimulationMsg msg;
      GetEditorEngineConnection()->SendMessage(&msg);
    }
    break;

    case plParticleEffectAssetEvent::AutoRestartChanged:
    {
      plEditorEngineLoopAnimationMsg msg;
      msg.m_bLoop = GetParticleDocument()->GetAutoRestart();
      GetEditorEngineConnection()->SendMessage(&msg);
    }
    break;

    default:
      break;
  }
}

void plParticleEffectAssetDocumentWindow::InternalRedraw()
{
  plEditorInputContext::UpdateActiveInputContext();
  SendRedrawMsg();
  plEngineDocumentWindow::InternalRedraw();
}

void plParticleEffectAssetDocumentWindow::SendRedrawMsg()
{
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  if (m_bDoLiveResourceUpdate)
  {
    SendLiveResourcePreview();
    m_bDoLiveResourceUpdate = false;

    // an edit can change what the spied node computes
    GetParticleDocument()->SetSpyDirty(true);
  }

  {
    plSimulationSettingsMsgToEngine msg;
    msg.m_bSimulateWorld = !GetParticleDocument()->GetSimulationPaused();
    msg.m_fSimulationSpeed = GetParticleDocument()->GetSimulationSpeed();
    GetEditorEngineConnection()->SendMessage(&msg);
  }

  {
    plParticlePreviewSettingsMsgToEngine msg;
    GetParticleDocument()->FillPreviewSettings(msg);
    GetEditorEngineConnection()->SendMessage(&msg);
  }

  if (GetParticleDocument()->IsSpyDirty())
  {
    // an empty program stops the spy, also for a node with nothing to spy on
    plParticleSpyMsgToEngine msg;
    GetParticleDocument()->BuildSpyMessage(msg);
    GetEditorEngineConnection()->SendMessage(&msg);
    GetParticleDocument()->SetSpyDirty(false);
  }

  for (auto pView : m_ViewWidgets)
  {
    pView->SetEnablePicking(false);
    pView->UpdateCameraInterpolation();
    pView->SyncToEngine();
  }
}

void plParticleEffectAssetDocumentWindow::RestoreResource()
{
  plRestoreResourceMsgToEngine msg;
  msg.m_sResourceType = "Particle Effect";

  plStringBuilder tmp;
  msg.m_sResourceID = plConversionUtils::ToString(GetDocument()->GetGuid(), tmp);

  plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
}
