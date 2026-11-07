#include <EnginePluginParticle/EnginePluginParticlePCH.h>

#include <EnginePluginParticle/ParticleAsset/ParticleContext.h>
#include <EnginePluginParticle/ParticleAsset/ParticleView.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/GPUParticleProgram.h>

plParticleViewContext::plParticleViewContext(plParticleContext* pParticleContext)
  : plEngineProcessViewContext(pParticleContext)
{
  m_pParticleContext = pParticleContext;
  m_ExtractionSubscription = plRenderWorld::GetExtractionEvent().AddEventHandler(plMakeDelegate(&plParticleViewContext::ExtractionEventHandler, this));
}

plParticleViewContext::~plParticleViewContext()
{
  plRenderWorld::GetExtractionEvent().RemoveEventHandler(m_ExtractionSubscription);
}

void plParticleViewContext::PositionThumbnailCamera(const plBoundingBoxSphere& bounds)
{
  m_Camera.SetCameraMode(plCameraMode::PerspectiveFixedFovX, 45.0f, 0.1f, 1000.0f);

  FocusCameraOnObject(m_Camera, bounds, 45.0f, -plVec3(-1.8f, 1.8f, 1.0f));
}

plViewHandle plParticleViewContext::CreateView()
{
  plView* pView = nullptr;
  plRenderWorld::CreateView("Particle Editor - View", pView);
  pView->SetCameraUsageHint(plCameraUsageHint::EditorView);

  pView->SetRenderPipelineResource(CreateDefaultRenderPipeline());

  plEngineProcessDocumentContext* pDocumentContext = GetDocumentContext();
  pView->SetWorld(pDocumentContext->GetWorld());
  pView->SetCamera(&m_Camera);
  return pView->GetHandle();
}

void plParticleViewContext::Redraw(bool bRenderEditorGizmos)
{
  m_bEditorView |= bRenderEditorGizmos;

  plEngineProcessViewContext::Redraw(bRenderEditorGizmos);
}

void plParticleViewContext::ExtractionEventHandler(const plRenderWorldExtractionEvent& e)
{
  if (e.m_Type != plRenderWorldExtractionEvent::Type::BeginExtraction || !m_bEditorView || !plRenderWorld::GetMainViews().Contains(m_hView))
    return;

  if (m_pParticleContext->GetShowStats())
    DrawStats();

  if (m_pParticleContext->GetShowEvents())
    DrawEventMarkers();

  DrawPreviewMotion();
  DrawPreviewPath();
}

void plParticleViewContext::DrawEventMarkers()
{
  // a small cross where each event of the last half second happened, in the graph's event colour
  const auto events = m_pParticleContext->GetRecentEvents();
  if (events.IsEmpty())
    return;

  constexpr float fArm = 0.12f;
  plDynamicArray<plDebugRenderer::Line> lines;
  lines.Reserve(events.GetCount() * 3);

  for (const plParticleContext::RecentEvent& e : events)
  {
    lines.PushBack(plDebugRenderer::Line(e.m_vPosition - plVec3(fArm, 0, 0), e.m_vPosition + plVec3(fArm, 0, 0)));
    lines.PushBack(plDebugRenderer::Line(e.m_vPosition - plVec3(0, fArm, 0), e.m_vPosition + plVec3(0, fArm, 0)));
    lines.PushBack(plDebugRenderer::Line(e.m_vPosition - plVec3(0, 0, fArm), e.m_vPosition + plVec3(0, 0, fArm)));
  }

  plDebugRenderer::DrawLines(m_hView, lines, plColor(plColorGammaUB(224, 163, 92)));
}

void plParticleViewContext::DrawPreviewPath()
{
  // where the moving preview goes: a dashed line through one round of its motion, in the selection blue
  const plParticlePreviewMotion::Enum motion = m_pParticleContext->GetMotion();
  const double fPeriod = plParticleContext::GetPreviewPeriod(motion);
  if (fPeriod <= 0.0)
    return;

  constexpr plUInt32 uiSegments = 96;
  plHybridArray<plDebugRenderer::Line, uiSegments / 2> lines;
  for (plUInt32 i = 0; i < uiSegments; i += 2)
  {
    const plVec3 vStart = plParticleContext::GetPreviewPosition(motion, fPeriod * i / uiSegments);
    const plVec3 vEnd = plParticleContext::GetPreviewPosition(motion, fPeriod * (i + 1) / uiSegments);
    lines.PushBack(plDebugRenderer::Line(vStart, vEnd));
  }

  plDebugRenderer::DrawLines(m_hView, lines, plColor(plColorGammaUB(79, 143, 224, 115)));
}

namespace
{
  // neutral surfaces like the viewport's exposure panel; the CPU fallback in the warning colour
  const plColor s_PanelColor(0.02f, 0.02f, 0.02f, 0.72f);
  const plColor s_LabelColor(0.62f, 0.65f, 0.69f, 1.0f);
  const plColor s_ValueColor(0.88f, 0.89f, 0.91f, 1.0f);
  const plColor s_CpuColor(0.84f, 0.71f, 0.36f, 1.0f);

  constexpr float s_fPadding = 8.0f;
  constexpr float s_fLine = 16.0f;
  constexpr float s_fLabelWidth = 112.0f;
  constexpr float s_fPanelWidth = 340.0f;
} // namespace

void plParticleViewContext::DrawStats()
{
  const auto& systems = m_pParticleContext->GetSystemStats();
  if (systems.IsEmpty())
    return;

  plView* pView = nullptr;
  if (!plRenderWorld::TryGetView(m_hView, pView))
    return;

  // panels go down the left side, and into another column before they reach the frame rate readout at the bottom
  const float fBottom = pView->GetViewport().height - 40.0f;
  float fX = 10.0f;
  float fY = 10.0f;
  float fColumnWidth = s_fPanelWidth;

  for (const plParticleContext::SystemStats& stats : systems)
  {
    // a sorted GPU system has a row for the sort, and events have theirs
    const plUInt32 uiRows = 5 + (stats.m_uiSortSlots > 0 ? 1 : 0) + (stats.m_sUpdateInterval.IsEmpty() ? 0 : 1) + (stats.m_sRaises.IsEmpty() ? 0 : 1) + (stats.m_sSpawnedBy.IsEmpty() ? 0 : 1);
    const float fHeight = 2.0f * s_fPadding + s_fLine * (uiRows + 1);

    // wide enough for the longest value, such as why a system stays on the CPU (the debug font is monospaced,
    // about 7.2 px per character at 13)
    const plUInt32 uiLongest = plMath::Max(plMath::Max(stats.m_sRunsOn.GetCharacterCount(), stats.m_sUpdateInterval.GetCharacterCount()), stats.m_sRaises.GetCharacterCount(), stats.m_sSpawnedBy.GetCharacterCount());
    const float fPanelWidth = plMath::Max(s_fPanelWidth, 2.0f * s_fPadding + s_fLabelWidth + 7.2f * uiLongest);

    if (fY > 10.0f && fY + fHeight > fBottom)
    {
      fX += fColumnWidth + 6.0f;
      fY = 10.0f;
      fColumnWidth = s_fPanelWidth;
    }

    fColumnWidth = plMath::Max(fColumnWidth, fPanelWidth);
    plDebugRenderer::Draw2DRectangle(m_hView, plRectFloat(fX, fY, fPanelWidth, fHeight), 0.0f, s_PanelColor);

    const plInt32 iLeft = (plInt32)(fX + s_fPadding);
    const plInt32 iValue = (plInt32)(fX + s_fPadding + s_fLabelWidth);
    plInt32 iY = (plInt32)(fY + s_fPadding);

    plStringBuilder sName = stats.m_sName;
    sName.ToUpper();
    plDebugRenderer::Draw2DText(m_hView, sName, plVec2I32(iLeft, iY), s_LabelColor, 13);
    iY += (plInt32)s_fLine;

    auto row = [&](plStringView sLabel, plStringView sValue, const plColor& color)
    {
      plDebugRenderer::Draw2DText(m_hView, sLabel, plVec2I32(iLeft, iY), s_LabelColor, 13);
      plDebugRenderer::Draw2DText(m_hView, sValue, plVec2I32(iValue, iY), color, 13);
      iY += (plInt32)s_fLine;
    };

    plStringBuilder s;

    s.SetFormat("{} / {}", stats.m_uiParticles, stats.m_uiMaxParticles);
    row("Particles", s, s_ValueColor);

    row("Runs on", stats.m_sRunsOn, stats.m_sRunsOn.StartsWith("CPU \xC2\xB7") ? s_CpuColor : s_ValueColor);

    if (stats.m_uiGPURegisters > 0)
      s.SetFormat("{} ops \xC2\xB7 {} / {} registers", stats.m_uiProgramOps, stats.m_uiGPURegisters, GPU_PROGRAM_MAX_REGISTERS); // ·
    else
      s.SetFormat("{} ops", stats.m_uiProgramOps);
    row("Programs", s, s_ValueColor);

    // the CPU's part of a GPU system is only the spawning; its simulation time is on the GPU
    if (stats.m_bOnGPU)
      s = "on the GPU";
    else
      s.SetFormat("{} ms / step", plArgF(stats.m_UpdateDuration.GetMilliseconds(), 3));
    row("Simulation", s, s_ValueColor);

    if (!stats.m_sUpdateInterval.IsEmpty())
      row("Update", stats.m_sUpdateInterval, s_ValueColor);

    if (stats.m_uiSortSlots > 0)
    {
      s.SetFormat("at most {} of {} slots", stats.m_uiSortedAtMost, stats.m_uiSortSlots);
      row("Sorted", s, s_ValueColor);
    }

    if (!stats.m_sRaises.IsEmpty())
      row("Raises", stats.m_sRaises, s_ValueColor);

    if (!stats.m_sSpawnedBy.IsEmpty())
      row("Spawned by", stats.m_sSpawnedBy, s_ValueColor);

    if (stats.m_vBoundsSize.IsZero())
      s = "empty";
    else
      s.SetFormat("{} x {} x {} m", plArgF(stats.m_vBoundsSize.x, 1), plArgF(stats.m_vBoundsSize.y, 1), plArgF(stats.m_vBoundsSize.z, 1));
    row("Bounds", s, s_ValueColor);

    fY += fHeight + 6.0f;
  }
}

void plParticleViewContext::DrawPreviewMotion()
{
  plView* pView = nullptr;
  if (!plRenderWorld::TryGetView(m_hView, pView))
    return;

  plStringBuilder sText;
  if (m_pParticleContext->GetMotion() == plParticlePreviewMotion::Static)
    sText = "Preview still";
  else
    sText.SetFormat("Effect Velocity {} m/s", plArgF(m_pParticleContext->GetPreviewSpeed(), 1));

  const float fWidth = 190.0f;
  const plRectFloat viewport = pView->GetViewport();
  const plRectFloat rect(viewport.width - fWidth - 10.0f, 10.0f, fWidth, s_fLine + 2.0f * 4.0f);

  plDebugRenderer::Draw2DRectangle(m_hView, rect, 0.0f, s_PanelColor);
  plDebugRenderer::Draw2DText(m_hView, sText, plVec2I32((plInt32)(rect.Right() - 8.0f), (plInt32)(rect.y + 4.0f)), s_LabelColor, 13, plDebugTextHAlign::Right);
}
