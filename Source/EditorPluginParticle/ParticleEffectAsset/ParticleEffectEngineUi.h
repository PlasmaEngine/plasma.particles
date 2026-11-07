#pragma once

#include <EditorUi/Content.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

class plParticleEffectAssetDocument;
struct plParticleEffectAssetEvent;
struct plEditorUiPointerEvent;

/// \brief The preview's timeline: step one frame back or forward, a track to scrub with a tick per second and the
/// playhead, and the effect time. Scrubbing and stepping pause the preview and show the effect re-simulated there.
class plEditorUiParticleTimeline : public plEditorUiContent
{
public:
  explicit plEditorUiParticleTimeline(plParticleEffectAssetDocument* pDocument);
  ~plEditorUiParticleTimeline() override;

  static constexpr float Height = 40.0f;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  void BuildDocument();
  void Draw();
  void OnPointer(const plEditorUiPointerEvent& e);
  void EffectEventHandler(const plParticleEffectAssetEvent& e);
  void ScrubTo(float fX);
  void Step(int iFrames);

  plParticleEffectAssetDocument* m_pDocument = nullptr;
  plRectFloat m_TrackRect;
  double m_fSpan = 8.0; ///< seconds the track shows; grows with the effect
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  bool m_bScrubbing = false;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};
