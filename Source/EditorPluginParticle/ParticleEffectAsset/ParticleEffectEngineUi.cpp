#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectEngineUi.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>
#include <GameUiPlugin/Components/UiMessages.h>

namespace
{
  constexpr float s_fPadding = 10.0f;

  void Place(plUiElement* pElement, const plRectFloat& rect)
  {
    pElement->m_vAnchorMin = pElement->m_vAnchorMax = plVec2::MakeZero();
    pElement->m_vOffsets = plVec4(rect.x, rect.y, rect.width, rect.height);
  }

  float TextWidth(plStringView sText, plUInt32 uiSize = plEditorUi::FontSize)
  {
    return plMath::Ceil(plEditorUi::MeasureText(sText, uiSize)) + 1.0f;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plEditorUiParticleTimeline
//////////////////////////////////////////////////////////////////////////

namespace
{
  const plHashedString s_sTimelineDraw = plMakeHashedString("Timeline.Draw");

  constexpr double s_fFrame = 1.0 / 60.0;
  constexpr float s_fStepWidth = 30.0f;
  constexpr float s_fTimeWidth = 190.0f;
  constexpr float s_fBandHeight = 32.0f; ///< playhead, rail, ticks and second labels
} // namespace

plEditorUiParticleTimeline::plEditorUiParticleTimeline(plParticleEffectAssetDocument* pDocument)
  : plEditorUiContent("ParticleTimeline")
  , m_pDocument(pDocument)
{
  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiParticleTimeline::OnPointer, this));

  m_pDocument->m_Events.AddEventHandler(plMakeDelegate(&plEditorUiParticleTimeline::EffectEventHandler, this));
}

plEditorUiParticleTimeline::~plEditorUiParticleTimeline()
{
  m_pDocument->m_Events.RemoveEventHandler(plMakeDelegate(&plEditorUiParticleTimeline::EffectEventHandler, this));

  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plEditorUiParticleTimeline::EffectEventHandler(const plParticleEffectAssetEvent& e)
{
  if (e.m_Type != plParticleEffectAssetEvent::PreviewStatusChanged)
    return;

  // the track grows in steps of four seconds, so the playhead never runs off its end
  const double fTime = m_pDocument->GetPreviewStatus().m_fEffectTime;
  while (fTime > m_fSpan)
    m_fSpan += 4.0;

  m_bDrawDirty = true;
  RequestRedraw();
}

void plEditorUiParticleTimeline::OnUiUpdate()
{
  if (!IsReady())
    return;

  if (GetSurface()->GetLogicalSize() != m_vBuiltSize)
    m_bRebuild = true;

  if (m_bRebuild)
    BuildDocument();

  if (m_bDrawDirty)
    Draw();
}

void plEditorUiParticleTimeline::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();
  const float fHeight = m_vBuiltSize.y;
  const float fButtonY = (fHeight - ControlHeight) * 0.5f;
  plBlackboard& board = GetSurface()->GetBlackboard();

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();

  // the drawing under everything: the track and the time
  plUiDrawList* pDraw = DrawList(pRoot, "Timeline.Drawing", s_sTimelineDraw.GetView());
  Fill(pDraw);

  plUiButton* pBack = IconButton(pRoot, Icon::StepBack, "Timeline.Back");
  Place(pBack, plRectFloat(s_fPadding, fButtonY, s_fStepWidth, ControlHeight));
  SetToolTip(board, "Timeline.Back", "Step back one frame (1/60 s)");

  plUiButton* pForward = IconButton(pRoot, Icon::StepForward, "Timeline.Forward");
  Place(pForward, plRectFloat(s_fPadding + s_fStepWidth + 4.0f, fButtonY, s_fStepWidth, ControlHeight));
  SetToolTip(board, "Timeline.Forward", "Step one frame (1/60 s)");

  // the track is a band of fixed height in the middle; a taller panel only pads it
  const float fTrackX = s_fPadding + 2.0f * s_fStepWidth + 4.0f + 14.0f;
  m_TrackRect = plRectFloat(fTrackX, plMath::Max((fHeight - s_fBandHeight) * 0.5f, 0.0f), plMath::Max(m_vBuiltSize.x - fTrackX - s_fTimeWidth - s_fPadding, 1.0f), s_fBandHeight);

  SetDocument(Finish(pRoot));
  m_bDrawDirty = true;
}

void plEditorUiParticleTimeline::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();

  const double fTime = m_pDocument->GetPreviewStatus().m_fEffectTime;
  const plRectFloat& track = m_TrackRect;
  const float fRailY = track.y + 11.0f;

  m_Writer.Rect(plRectFloat(0, -1, m_vBuiltSize.x, 1), Line());

  // the rail, filled up to the playhead
  const float fHead = track.x + static_cast<float>(plMath::Clamp(fTime / m_fSpan, 0.0, 1.0)) * track.width;
  m_Writer.Rect(plRectFloat(track.x, fRailY, track.width, 4.0f), Field(), 2.0f);
  m_Writer.Rect(plRectFloat(track.x, fRailY, fHead - track.x, 4.0f), Muted(), 2.0f);

  // a tick per second while they stay apart; labels every 1, 2, 5, 10, 20, 30 or 60 seconds, at least 40 px apart
  const float fPerSecond = static_cast<float>(track.width / m_fSpan);
  plInt32 iLabelEvery = 60;
  for (plInt32 iStep : {1, 2, 5, 10, 20, 30, 60})
  {
    if (iStep * fPerSecond >= 40.0f)
    {
      iLabelEvery = iStep;
      break;
    }
  }

  plStringBuilder s;
  for (plInt32 i = 0; i <= static_cast<plInt32>(m_fSpan); ++i)
  {
    const bool bLabel = i % iLabelEvery == 0;
    if (!bLabel && fPerSecond < 5.0f)
      continue;

    const float x = track.x + i * fPerSecond;
    m_Writer.Line(plVec2(x, fRailY + 6.0f), plVec2(x, fRailY + (bLabel ? 10.0f : 8.0f)), Dim());

    if (bLabel)
    {
      s.SetFormat("{}s", i);
      const float fLabel = TextWidth(s, MinFontSize);
      const float fLabelX = plMath::Clamp(x - fLabel * 0.5f, track.x, track.Right() - fLabel);
      m_Writer.Text(plRectFloat(fLabelX, fRailY + 9.0f, fLabel, 12.0f), s, Dim(), MinFontSize);
    }
  }

  // the playhead in the accent colour
  m_Writer.Line(plVec2(fHead, fRailY - 6.0f), plVec2(fHead, fRailY + 10.0f), Accent(), 2.0f);
  const plVec2 arrow[] = {plVec2(fHead - 5.0f, fRailY - 9.0f), plVec2(fHead + 5.0f, fRailY - 9.0f), plVec2(fHead, fRailY - 3.0f)};
  m_Writer.Triangles(arrow, Accent());

  // the time, the frame number in the code face
  s.SetFormat("{} s \xC2\xB7 frame {}", plArgF(fTime, 2), static_cast<plInt64>(plMath::Round(fTime / s_fFrame))); // ·
  m_Writer.Text(plRectFloat(m_vBuiltSize.x - s_fPadding - s_fTimeWidth, 0, s_fTimeWidth, m_vBuiltSize.y), s, Muted(), FontSize - 1, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sTimelineDraw);
}

void plEditorUiParticleTimeline::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  // the track takes the whole row's height between the buttons and the time, so it is easy to hit
  const plRectFloat hit(m_TrackRect.x - 6.0f, 0.0f, m_TrackRect.width + 12.0f, m_vBuiltSize.y);

  switch (e.m_Type)
  {
    case Type::Press:
      if (e.m_Button == plEditorUiPointerEvent::Button::Left && hit.Contains(e.m_vPosition))
      {
        m_bScrubbing = true;
        ScrubTo(e.m_vPosition.x);
      }
      return;

    case Type::DragStart:
    case Type::DragMove:
    case Type::Move:
      if (m_bScrubbing)
        ScrubTo(e.m_vPosition.x);
      return;

    case Type::Release:
    case Type::Drop:
    case Type::DragCancel:
      m_bScrubbing = false;
      return;

    default:
      return;
  }
}

void plEditorUiParticleTimeline::ScrubTo(float fX)
{
  // whole frames, so a scrub and the step buttons land on the same times
  const double fTime = plMath::Clamp(static_cast<double>((fX - m_TrackRect.x) / m_TrackRect.width), 0.0, 1.0) * m_fSpan;
  m_pDocument->SetPreviewTime(plMath::Round(fTime / s_fFrame) * s_fFrame);
}

void plEditorUiParticleTimeline::Step(int iFrames)
{
  const double fFrame = plMath::Round(m_pDocument->GetPreviewStatus().m_fEffectTime / s_fFrame) + iFrames;
  m_pDocument->SetPreviewTime(plMath::Max(fFrame, 0.0) * s_fFrame);
}

void plEditorUiParticleTimeline::OnUiEvent(const plMsgUiEvent& event)
{
  if (event.m_sIdentifier.GetView() == "Timeline.Back")
    Step(-1);
  else if (event.m_sIdentifier.GetView() == "Timeline.Forward")
    Step(1);
}
