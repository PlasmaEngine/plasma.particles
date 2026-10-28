#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessViewContext.h>

class plParticleContext;
struct plRenderWorldExtractionEvent;

class plParticleViewContext : public plEngineProcessViewContext
{
public:
  plParticleViewContext(plParticleContext* pParticleContext);
  ~plParticleViewContext();

  void PositionThumbnailCamera(const plBoundingBoxSphere& bounds);

protected:
  virtual plViewHandle CreateView() override;
  virtual void Redraw(bool bRenderEditorGizmos) override;

  /// Debug drawings last one frame, and the engine process also renders frames without a redraw message (it shows the
  /// last views again), so the overlays are drawn as each frame's extraction begins; only for the editor's view.
  void ExtractionEventHandler(const plRenderWorldExtractionEvent& e);

  /// The stats overlay: one panel per system, top left.
  void DrawStats();

  /// What the moving preview reads as the effect's velocity, top right.
  void DrawPreviewMotion();

  /// The dashed path of the moving preview.
  void DrawPreviewPath();
  void DrawEventMarkers();

  plParticleContext* m_pParticleContext;
  bool m_bEditorView = false; ///< redrawn with gizmos; the thumbnail view never is
  plEventSubscriptionID m_ExtractionSubscription = 0;
};
