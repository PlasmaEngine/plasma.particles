#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/Assets/AssetStatusIndicator.moc.h>
#include <EditorFramework/DocumentWindow/OrbitCamViewWidget.moc.h>
#include <EditorFramework/InputContexts/EditorInputContext.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAssetWindow.moc.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectGraphScene.moc.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <GuiFoundation/ActionViews/MenuBarActionMapView.moc.h>
#include <GuiFoundation/ActionViews/ToolBarActionMapView.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <GuiFoundation/PropertyGrid/PropertyGridWidget.moc.h>
#include <GuiFoundation/VisualGraph/View.moc.h>
#include <QBoxLayout>
#include <QLineEdit>
#include <QSettings>
#include <SharedPluginAssets/Common/Messages.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <EditorFramework/Preferences/EditorPreferences.h>

plQtParticleEffectAssetDocumentWindow::plQtParticleEffectAssetDocumentWindow(plAssetDocument* pDocument)
  : plQtEngineDocumentWindow(pDocument)
{
  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plQtParticleEffectAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plQtParticleEffectAssetDocumentWindow::StructureEventHandler, this));

  plEditorPreferencesUser* pPreferences = plPreferences::QueryPreferences<plEditorPreferencesUser>();
  pPreferences->ApplyDefaultValues(m_LightSettings);

  // Menu Bar
  {
    plQtMenuBarActionMapView* pMenuBar = static_cast<plQtMenuBarActionMapView*>(menuBar());
    plActionContext context;
    context.m_sMapping = "ParticleEffectAssetMenuBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pMenuBar->SetActionContext(context);
  }

  // Tool Bar
  {
    plQtToolBarActionMapView* pToolBar = new plQtToolBarActionMapView("Toolbar", this);
    plActionContext context;
    context.m_sMapping = "ParticleEffectAssetToolBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pToolBar->SetActionContext(context);
    pToolBar->setObjectName("ParticleEffectAssetWindowToolBar");
    addToolBar(pToolBar);
  }

  // 3D View (central widget)
  {
    SetTargetFramerate(pPreferences->GetMaxFramerate());

    m_ViewConfig.m_Camera.LookAt(plVec3(-1.6f, 0, 0), plVec3(0, 0, 0), plVec3(0, 0, 1));
    m_ViewConfig.ApplyPerspectiveSetting(90);

    m_pViewWidget = new plQtOrbitCamViewWidget(this, &m_ViewConfig);
    m_pViewWidget->ConfigureRelative(plVec3(0), plVec3(5.0f), plVec3(-2, 0, 0.5f), 1.0f);
    AddViewWidget(m_pViewWidget);
    plQtViewWidgetContainer* pContainer = new plQtViewWidgetContainer(this, m_pViewWidget, "ParticleEffectAssetViewToolBar");
    m_pDockManager->setCentralWidget(pContainer);
  }

  // Visual Graph Panel
  {
    m_pScene = new plQtParticleEffectGraphScene(this);
    m_pScene->InitScene(static_cast<const plVisualGraphObjectManager*>(pDocument->GetObjectManager()));

    m_pView = new plQtVisualGraphView(this);
    m_pView->SetScene(m_pScene);

    plQtDocumentPanel* pGraphPanel = new plQtDocumentPanel(this, pDocument);
    pGraphPanel->setObjectName("ParticleEffectGraphView");
    pGraphPanel->setWindowTitle("Particle Graph");
    pGraphPanel->setWidget(m_pView);

    m_pDockManager->addDockWidget(ads::BottomDockWidgetArea, pGraphPanel);
  }

  // Property Grid Panel
  {
    plQtDocumentPanel* pPropertyPanel = new plQtDocumentPanel(this, pDocument);
    pPropertyPanel->setObjectName("ParticleEffectAssetDockWidget_Properties");
    pPropertyPanel->setWindowTitle("Properties");
    pPropertyPanel->show();

    plQtPropertyGridWidget* pPropertyGrid = new plQtPropertyGridWidget(pPropertyPanel, pDocument);

    QLineEdit* pPropertyFilter = new QLineEdit(pPropertyPanel);
    pPropertyFilter->setPlaceholderText("Filter properties...");
    pPropertyFilter->setClearButtonEnabled(true);
    connect(pPropertyFilter, &QLineEdit::textChanged, this, [pPropertyGrid](const QString& sText)
      { pPropertyGrid->SetFilterText(sText.toUtf8().data()); });

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    pWidget->setLayout(new QVBoxLayout());
    pWidget->setContentsMargins(0, 0, 0, 0);

    pWidget->layout()->setContentsMargins(0, 0, 0, 0);
    pWidget->layout()->addWidget(new plQtAssetStatusIndicator((plAssetDocument*)GetDocument()));
    pWidget->layout()->addWidget(pPropertyFilter);
    pWidget->layout()->addWidget(pPropertyGrid);

    pPropertyPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);

    m_pDockManager->addDockWidgetTab(ads::RightDockWidgetArea, pPropertyPanel);
  }

  m_pAssetDoc = static_cast<plParticleEffectAssetDocument*>(pDocument);

  // Clear stale window layout from the old particle editor (before visual graph).
  // The base class queues SlotRestoreDocumentLayout which would hide our new panels
  // if the saved state doesn't include them.
  {
    QSettings settings;
    settings.beginGroup("DocumentWindowLayouts");
    QByteArray state = settings.value("plQtParticleEffectAssetDocumentWindow").toByteArray();
    if (!state.isEmpty() && !state.contains("ParticleEffectGraphView"))
    {
      settings.remove("plQtParticleEffectAssetDocumentWindow");
    }
    settings.endGroup();
  }

  FinishWindowCreation();

  GetParticleDocument()->m_Events.AddEventHandler(plMakeDelegate(&plQtParticleEffectAssetDocumentWindow::ParticleEventHandler, this));
}

plQtParticleEffectAssetDocumentWindow::~plQtParticleEffectAssetDocumentWindow()
{
  GetParticleDocument()->m_Events.RemoveEventHandler(plMakeDelegate(&plQtParticleEffectAssetDocumentWindow::ParticleEventHandler, this));

  RestoreResource();

  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plQtParticleEffectAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plQtParticleEffectAssetDocumentWindow::PropertyEventHandler, this));
}

plParticleEffectAssetDocument* plQtParticleEffectAssetDocumentWindow::GetParticleDocument()
{
  return static_cast<plParticleEffectAssetDocument*>(GetDocument());
}

void plQtParticleEffectAssetDocumentWindow::SendLiveResourcePreview()
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

void plQtParticleEffectAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  m_bDoLiveResourceUpdate = true;
}

void plQtParticleEffectAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
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

void plQtParticleEffectAssetDocumentWindow::ParticleEventHandler(const plParticleEffectAssetEvent& e)
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

void plQtParticleEffectAssetDocumentWindow::InternalRedraw()
{
  plEditorInputContext::UpdateActiveInputContext();
  SendRedrawMsg();
  plQtEngineDocumentWindow::InternalRedraw();
}

void plQtParticleEffectAssetDocumentWindow::SendRedrawMsg()
{
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  if (m_bDoLiveResourceUpdate)
  {
    SendLiveResourcePreview();
    m_bDoLiveResourceUpdate = false;
  }

  {
    plSimulationSettingsMsgToEngine msg;
    msg.m_bSimulateWorld = !GetParticleDocument()->GetSimulationPaused();
    msg.m_fSimulationSpeed = GetParticleDocument()->GetSimulationSpeed();
    GetEditorEngineConnection()->SendMessage(&msg);
  }

  for (auto pView : m_ViewWidgets)
  {
    pView->SetEnablePicking(false);
    pView->UpdateCameraInterpolation();
    pView->SyncToEngine();
  }
}

void plQtParticleEffectAssetDocumentWindow::RestoreResource()
{
  plRestoreResourceMsgToEngine msg;
  msg.m_sResourceType = "Particle Effect";

  plStringBuilder tmp;
  msg.m_sResourceID = plConversionUtils::ToString(GetDocument()->GetGuid(), tmp);

  plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
}