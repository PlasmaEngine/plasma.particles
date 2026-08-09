#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeItem.h>
#include <GuiFoundation/VisualGraph/Pin.h>

#include <QApplication>
#include <QPainter>

namespace
{
  constexpr qreal kPad = 8.0;
  constexpr qreal kMinHeight = 46.0;

  /// Matches the event colour the graph uses for reaction pins.
  const QColor kAccent(217, 96, 92);
} // namespace

plQtParticleEffectNodeItem::plQtParticleEffectNodeItem() = default;

void plQtParticleEffectNodeItem::InitNode(const plVisualGraphObjectManager* pManager, const plDocumentObject* pObject)
{
  SUPER::InitNode(pManager, pObject);
  // flat look: no shadow, and the outline only appears on selection
  EnableDropShadow(false);
}

void plQtParticleEffectNodeItem::UpdateState()
{
  m_pTitleLabel->setPlainText(QStringLiteral("Effect"));

  const plInt32 iReactions = GetObject()->GetTypeAccessor().GetCount("EventReactions");

  plStringBuilder sSubtitle;
  if (iReactions > 0)
    sSubtitle.SetFormat("{0} event reaction{1}", iReactions, iReactions == 1 ? "" : "s");
  else
    sSubtitle = "no event reactions";

  m_pSubtitleLabel->setPlainText(sSubtitle.GetData());
  m_sCategoryRoot = QStringLiteral("Effect");
}

void plQtParticleEffectNodeItem::UpdateGeometry()
{
  prepareGeometryChange();

  const qreal w = m_fWidth;

  QRectF titleRect = m_pTitleLabel->boundingRect();
  QRectF subRect = m_pSubtitleLabel->boundingRect();

  m_pIcon->setPos(0, 0);
  m_pTitleLabel->setPos((w - titleRect.width()) * 0.5, kPad);
  m_pSubtitleLabel->setPos((w - subRect.width()) * 0.5, kPad + titleRect.height() - 4.0);

  m_fHeight = plMath::Max(kMinHeight, kPad + titleRect.height() + subRect.height());
  m_HeaderRect = QRectF(0, 0, w, m_fHeight);

  // Systems comes in on the top edge; the retired EventReactions pin stays hidden.
  qreal fPinX = w * 0.5;
  for (plUInt32 i = 0; i < GetInputPins().GetCount(); ++i)
  {
    plQtVisualGraphPin* pPin = GetInputPins()[i];
    const plVisualGraphPin* pPinData = pPin->GetPin();

    const bool bIsSystems = pPinData != nullptr && plStringUtils::IsEqual(pPinData->GetName(), "Systems");
    pPin->setVisible(bIsSystems);

    if (!bIsSystems)
      continue;

    const QRectF pinRect = pPin->GetPinRect();
    pPin->setPos(QPointF(fPinX - pinRect.width() * 0.5 - pinRect.x(), -pinRect.y() - pinRect.height() * 0.5));
  }

  for (plQtVisualGraphPin* pPin : GetOutputPins())
  {
    pPin->setVisible(false);
  }

  QPainterPath path;
  path.addRoundedRect(0, 0, w, m_fHeight, 6, 6);
  setPath(path);
}

void plQtParticleEffectNodeItem::paint(QPainter* pPainter, const QStyleOptionGraphicsItem* pOption, QWidget* pWidget)
{
  PL_IGNORE_UNUSED(pOption);
  PL_IGNORE_UNUSED(pWidget);

  const QPalette palette = QApplication::palette();

  pPainter->setRenderHint(QPainter::Antialiasing, true);

  pPainter->setPen(Qt::NoPen);
  pPainter->setBrush(palette.window().color().lighter(118));
  pPainter->drawPath(path());

  // accent strip along the top edge, where the systems arrive
  pPainter->setClipPath(path());
  pPainter->setBrush(kAccent);
  pPainter->drawRect(QRectF(0, 0, m_fWidth, 3.0));
  pPainter->setClipping(false);

  pPainter->setPen(QPen(isSelected() ? palette.highlight().color() : palette.mid().color(), isSelected() ? 2.0 : 1.0));
  pPainter->setBrush(Qt::NoBrush);
  pPainter->drawPath(path());
}
