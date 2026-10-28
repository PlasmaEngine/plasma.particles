#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/Document/DocumentNotices.h>
#include <EditorFramework/EngineShell/AssetDocumentContent.h>
#include <EditorFramework/EngineShell/DocumentFrame.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectEngineUi.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectShellPanels.h>
#include <EditorUi/DockManager.h>
#include <EditorUi/Widgets.h>
#include <ToolsFoundation/Selection/SelectionManager.h>

namespace
{
  void AddParticleEffectPanels(plEditorUiDocumentFrame& ref_frame)
  {
    plEditorUiDockManager& panels = ref_frame.GetPanels();
    if (panels.GetPanelSurface("Graph") == nullptr)
      return;

    panels.DockPanel("Graph", "View", plEditorUiDockSide::Left, 0.72f);
    panels.DockPanel("Properties", "View", plEditorUiDockSide::Bottom, 0.6f);

    // the timeline under the preview it scrubs
    if (panels.GetPanelSurface("Timeline") != nullptr)
    {
      // its row and tab when the view's height is known yet; the content centres itself in any height
      const float fView = panels.GetPanelRect("View").height;
      const float fRatio = fView > 0.0f ? plMath::Clamp((plEditorUiParticleTimeline::Height + 28.0f) / fView, 0.08f, 0.4f) : 0.22f;

      panels.DockPanel("Timeline", "View", plEditorUiDockSide::Bottom, fRatio);
    }
  }

  plEngineAssetPanelsProvider s_ParticleEffectPanelsProvider(plGetStaticRTTI<plParticleEffectAssetDocument>(), &AddParticleEffectPanels);

  /// Selects what the conversion made that still exists: the new blocks and their nodes.
  void SelectConverted(plParticleEffectAssetDocument* pDocument)
  {
    plDeque<const plDocumentObject*> selection;
    for (const plUuid& guid : pDocument->GetConversionReport().m_Objects)
    {
      if (const plDocumentObject* pObject = pDocument->GetObjectManager()->GetObject(guid))
        selection.PushBack(pObject);
    }

    pDocument->GetSelectionManager()->SetSelection(selection);
  }

  void DismissConversion(plParticleEffectAssetDocument* pDocument)
  {
    pDocument->DismissConversionReport();

    plParticleEffectAssetEvent e;
    e.m_pDocument = pDocument;
    e.m_Type = plParticleEffectAssetEvent::ConversionReportDismissed;
    pDocument->m_Events.Broadcast(e);

    plDocumentNotices::NotifyChanged();
  }

  /// The banner saying what opening the document converted from plasma.particles 1.3: Show converted selects the new
  /// blocks and nodes, Close takes the banner away.
  void ConversionNoticeRequested(const plDocumentNotices::Request& request)
  {
    auto* pDocument = plDynamicCast<plParticleEffectAssetDocument*>(request.m_pDocument);
    if (pDocument == nullptr)
      return;

    const plParticleEffectAssetDocument::ConversionReport& report = pDocument->GetConversionReport();
    if (report.m_uiBlocks == 0 || report.m_bDismissed)
      return;

    plDocumentNotice& notice = request.m_pNotices->ExpandAndGetRef();
    notice.m_Tone = plDocumentNotice::Tone::Info;
    notice.m_sIcon = plEditorUi::IconPath(plEditorUi::Icon::Info);

    plStringBuilder sText;
    sText.SetFormat("Converted {} {} from plasma.particles 1.3 into graph nodes.", report.m_uiBlocks, report.m_uiBlocks == 1 ? "block" : "blocks");
    notice.m_sText = sText;
    notice.m_sDetail = "The effect looks the same; saving keeps the new form.";

    plDocumentNotice::Action& show = notice.m_Actions.ExpandAndGetRef();
    show.m_sLabel = "Show converted";
    show.m_Callback = [pDocument]()
    { SelectConverted(pDocument); };

    plDocumentNotice::Action& close = notice.m_Actions.ExpandAndGetRef();
    close.m_sLabel = "Close";
    close.m_Callback = [pDocument]()
    { DismissConversion(pDocument); };
  }
} // namespace

void plParticleEffectShellPanels::Register()
{
  plEditorUiDocumentContentProvider::Register(&s_ParticleEffectPanelsProvider);
  plDocumentNotices::s_Requests.AddEventHandler(&ConversionNoticeRequested);
}

void plParticleEffectShellPanels::Unregister()
{
  plDocumentNotices::s_Requests.RemoveEventHandler(&ConversionNoticeRequested);
  plEditorUiDocumentContentProvider::Unregister(&s_ParticleEffectPanelsProvider);
}
