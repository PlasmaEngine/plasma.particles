#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/Assets/AssetBrowserDlg.moc.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <GuiFoundation/UIServices/ImageCache.moc.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleOperatorNodeItem.h>
#include <Foundation/Strings/TranslationLookup.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/UIServices/ColorDialog.moc.h>
#include <GuiFoundation/VisualGraph/Pin.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Document/Document.h>

#include <QApplication>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>

namespace
{
  constexpr qreal kPad = 7.0;
  constexpr qreal kHeaderH = 21.0;
  constexpr qreal kRowH = 18.0;
  constexpr int kDragThreshold = 4;

  const QColor kAccent(170, 180, 196);
} // namespace

plQtParticleOperatorNodeItem::plQtParticleOperatorNodeItem() = default;

void plQtParticleOperatorNodeItem::InitNode(const plVisualGraphObjectManager* pManager, const plDocumentObject* pObject)
{
  m_pOwnManager = const_cast<plVisualGraphObjectManager*>(pManager);

  SUPER::InitNode(pManager, pObject);

  // Deliberately no event subscriptions: InitNode runs inside the broadcast that created this
  // node, and plEvent forbids adding or removing handlers while broadcasting. Changes are picked
  // up by comparing values at paint time instead, which also needs no teardown.
  // flat look: no shadow, and the outline only appears on selection
  EnableDropShadow(false);
}

void plQtParticleOperatorNodeItem::UpdateState()
{
  const plRTTI* pType = GetObject()->GetTypeAccessor().GetType();

  // A named value identifies itself; an unnamed one falls back to what kind of node it is. The
  // name is edited from the properties dock — it is not one of the values worth a row here.
  plStringBuilder sTitle = GetObject()->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();

  if (sTitle.IsEmpty())
    sTitle = plParticleEffectNodeManager::FriendlyTypeName(pType);

  m_pTitleLabel->setPlainText(sTitle.GetData());

  // The badge is coloured by what the node produces, so it matches its output pin and the wires
  // leaving it — the same read-the-type-at-a-glance idea the render graph uses.
  plStringView sValueProperty;
  switch (plParticleValueKind::FromNode(pType, sValueProperty))
  {
    case plParticleValueKind::Number:
      m_BadgeColor = QColor(1, 163, 243);
      break;
    case plParticleValueKind::Bool:
      m_BadgeColor = QColor(217, 96, 92);
      break;
    case plParticleValueKind::Color:
      m_BadgeColor = QColor(224, 179, 58);
      break;
    case plParticleValueKind::Texture:
      m_BadgeColor = QColor(168, 116, 201);
      break;
    case plParticleValueKind::Gradient:
      m_BadgeColor = QColor(224, 145, 58);
      break;
    case plParticleValueKind::Curve:
      m_BadgeColor = QColor(103, 184, 89);
      break;
    default:
      m_BadgeColor = kAccent;
      break;
  }

  if (const plCategoryAttribute* pCategory = pType->GetAttributeByType<plCategoryAttribute>())
  {
    plStringBuilder sCategory = pCategory->GetCategory();
    sCategory.ToUpper();

    m_pSubtitleLabel->setPlainText(sCategory.GetData());
    m_pSubtitleLabel->setDefaultTextColor(m_BadgeColor);
    m_sCategoryRoot = QString::fromUtf8(pCategory->GetCategory());
  }
  else
  {
    m_pSubtitleLabel->setPlainText(QString());
  }
}

bool plQtParticleOperatorNodeItem::BuildPreview()
{
  m_pPreviewProp = nullptr;
  m_Preview = QPixmap();
  m_bPreviewIsColor = false;

  const plDocumentObject* pNode = GetObject();
  const plRTTI* pType = pNode->GetTypeAccessor().GetType();

  plStringView sValueProperty;
  const plParticleValueKind::Enum kind = plParticleValueKind::FromNode(pType, sValueProperty);

  if (sValueProperty.IsEmpty())
    return false;

  const plAbstractProperty* pProp = pType->FindPropertyByName(sValueProperty);
  if (pProp == nullptr)
    return false;

  const plVariant value = pNode->GetTypeAccessor().GetValue(sValueProperty);
  m_PreviewOriginal = value;

  if (kind == plParticleValueKind::Color)
  {
    m_pPreviewProp = pProp;
    m_bPreviewIsColor = true;
    m_PreviewColor = value.IsA<plColor>() ? plColorGammaUB(value.Get<plColor>()) : value.ConvertTo<plColorGammaUB>();
    return true;
  }

  if (kind != plParticleValueKind::Texture && kind != plParticleValueKind::Gradient && kind != plParticleValueKind::Curve)
    return false;

  m_pPreviewProp = pProp;
  m_PreviewAsset = plConversionUtils::ConvertStringToUuid(value.ConvertTo<plString>());

  QueryPreviewPixmap();
  return true;
}

bool plQtParticleOperatorNodeItem::QueryPreviewPixmap()
{
  if (!m_PreviewAsset.IsValid())
    return false;

  // The asset's own thumbnail is the preview, which works for every asset kind without this code
  // knowing how to read any of them. The cache loads asynchronously, so the first ask usually
  // returns the loading placeholder and the real image arrives later.
  plAssetCurator::plLockedSubAsset pSubAsset = plAssetCurator::GetSingleton()->GetSubAsset(m_PreviewAsset);
  if (!pSubAsset.isValid())
    return false;

  const plString sThumbnail = pSubAsset->m_pAssetInfo->GetManager()->GenerateResourceThumbnailPath(
    pSubAsset->m_pAssetInfo->m_Path, pSubAsset->m_Data.m_sName);

  plUInt32 uiImageID = 0;
  const QPixmap* pPixmap = plQtImageCache::GetSingleton()->QueryPixmapForType(
    pSubAsset->m_Data.m_sSubAssetsDocumentTypeName, sThumbnail, QModelIndex(), QVariant(), QVariant(), &uiImageID);

  if (pPixmap == nullptr)
    return false;

  // A still-loading entry hands back the placeholder, which is fixed size: taking its aspect ratio
  // would size the node to the wrong shape and never correct itself.
  const bool bIsNew = m_Preview.isNull() || m_Preview.cacheKey() != pPixmap->cacheKey();
  m_Preview = *pPixmap;

  return bIsNew;
}

void plQtParticleOperatorNodeItem::RebuildRows()
{
  m_Rows.Clear();

  if (BuildPreview())
    return;

  const plDocumentObject* pNode = GetObject();
  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  // which literal each input pin overrides, so the pin can sit on that row
  const auto inputs = plParticleOperatorInputs::Get(pType);

  plHybridArray<const plAbstractProperty*, 16> properties;
  pType->GetAllProperties(properties);

  for (const plAbstractProperty* pProp : properties)
  {
    if (pProp->GetCategory() != plPropertyCategory::Member || pProp->GetFlags().IsSet(plPropertyFlags::ReadOnly))
      continue;

    // the name is the node's header, not a row
    if (plStringUtils::IsEqual(pProp->GetPropertyName(), "Name"))
      continue;

    const plVariant value = accessor.GetValue(pProp->GetPropertyName());

    Row& row = m_Rows.ExpandAndGetRef();
    row.m_pProp = pProp;
    row.m_sLabel = plTranslate(pProp->GetPropertyName());
    row.m_Original = value;

    for (const auto& input : inputs)
    {
      if (input.m_szLiteral != nullptr && plStringUtils::IsEqual(input.m_szLiteral, pProp->GetPropertyName()))
      {
        row.m_sPin = input.m_szPin;
        break;
      }
    }

    if (pProp->GetSpecificType()->IsDerivedFrom<plEnumBase>())
    {
      plStringBuilder s;
      plReflectionUtils::EnumerationToString(pProp->GetSpecificType(), value.ConvertTo<plInt64>(), s);
      row.m_Kind = Row::Kind::Enum;
      row.m_sValue = plTranslate(s);
    }
    else if (value.IsA<plColor>() || value.IsA<plColorGammaUB>())
    {
      row.m_Kind = Row::Kind::Color;
      row.m_Color = value.IsA<plColor>() ? plColorGammaUB(value.Get<plColor>()) : value.Get<plColorGammaUB>();
    }
    else if (pProp->GetAttributeByType<plAssetBrowserAttribute>() != nullptr)
    {
      row.m_Kind = Row::Kind::Asset;
      row.m_sValue = value.ConvertTo<plString>().IsEmpty() ? plString("none") : plString("assigned");
    }
    else if (value.IsNumber() && !value.IsA<bool>())
    {
      plStringBuilder s;
      row.m_Kind = Row::Kind::Number;
      row.m_fNumber = value.ConvertTo<double>();
      s.SetFormat("{0}", plArgF(row.m_fNumber, 2));
      row.m_sValue = s;
    }
    else
    {
      plStringBuilder s = value.ConvertTo<plString>();
      row.m_Kind = Row::Kind::Text;
      row.m_sValue = s.IsEmpty() ? plString("-") : plString(s);
    }
  }

  // inputs with nothing to type, e.g. a Branch condition, still need a row to hang the pin on
  for (const auto& input : inputs)
  {
    if (input.m_szLiteral != nullptr)
      continue;

    Row& row = m_Rows.ExpandAndGetRef();
    row.m_Kind = Row::Kind::PinOnly;
    row.m_sLabel = plTranslate(input.m_szPin);
    row.m_sPin = input.m_szPin;
  }

}

bool plQtParticleOperatorNodeItem::IsRowDriven(const Row& row) const
{
  if (row.m_sPin.IsEmpty() || m_pOwnManager == nullptr || GetObject() == nullptr)
    return false;

  const plVisualGraphPin* pPin = m_pOwnManager->GetInputPinByName(GetObject(), row.m_sPin);
  return pPin != nullptr && m_pOwnManager->HasConnections(*pPin);
}

void plQtParticleOperatorNodeItem::UpdateGeometry()
{
  prepareGeometryChange();

  RebuildRows();

  const QRectF titleRect = m_pTitleLabel->boundingRect();
  const QRectF subRect = m_pSubtitleLabel->boundingRect();
  const bool bHasSubtitle = !m_pSubtitleLabel->toPlainText().isEmpty();

  // wide enough for the header, never narrower than a readable row
  const qreal w = plMath::Max(m_fWidth, plMath::Max(titleRect.width(), subRect.width()) + 34.0);

  // title centred, category badge centred underneath it
  m_pTitleLabel->setPos((w - titleRect.width()) * 0.5, 2.0);
  m_pIcon->setPos(0, 0);

  if (bHasSubtitle)
    m_pSubtitleLabel->setPos((w - subRect.width()) * 0.5, titleRect.height() + 1.0);

  const qreal fHeaderBottom =
    plMath::Max(kHeaderH, titleRect.height() + (bHasSubtitle ? subRect.height() + 4.0 : 0.0) + 4.0);
  m_HeaderRect = QRectF(0, 0, w, fHeaderBottom);

  qreal y = fHeaderBottom + 4.0;

  if (m_pPreviewProp != nullptr)
  {
    const qreal fPreviewWidth = w - 2 * kPad;

    // The node grows to the image's shape rather than squashing it into a fixed band. Clamped so a
    // very tall or very wide asset cannot turn the node into a sliver or a wall.
    qreal fPreviewHeight = 46.0;

    if (!m_Preview.isNull() && m_Preview.width() > 0)
    {
      const qreal fAspect = (qreal)m_Preview.height() / (qreal)m_Preview.width();
      fPreviewHeight = plMath::Clamp(fPreviewWidth * fAspect, 20.0, 240.0);
    }

    m_PreviewRect = QRectF(kPad, y, fPreviewWidth, fPreviewHeight);
    y += m_PreviewRect.height() + 2.0;
  }
  else
  {
    m_PreviewRect = QRectF();
  }

  for (Row& row : m_Rows)
  {
    row.m_Rect = QRectF(kPad, y, w - 2 * kPad, kRowH - 1.0);
    row.m_ValueRect = QRectF(row.m_Rect.left() + row.m_Rect.width() * 0.44, row.m_Rect.top(), row.m_Rect.width() * 0.56, row.m_Rect.height());
    y += kRowH;
  }

  m_fHeight = y + 5.0;

  // inputs sit on their row; the single output sits against the header on the right
  for (plQtVisualGraphPin* pPin : GetInputPins())
  {
    const plVisualGraphPin* pPinData = pPin->GetPin();
    const QRectF pinRect = pPin->GetPinRect();
    bool bPlaced = false;

    for (const Row& row : m_Rows)
    {
      if (pPinData != nullptr && !row.m_sPin.IsEmpty() && row.m_sPin == pPinData->GetName())
      {
        pPin->setVisible(true);
        pPin->setPos(QPointF(-pinRect.x() - pinRect.width() * 0.5, row.m_Rect.center().y() - pinRect.y() - pinRect.height() * 0.5));
        bPlaced = true;
        break;
      }
    }

    if (!bPlaced)
      pPin->setVisible(false);
  }

  // the output sits on the right edge, centred on the value area rather than the header, so it
  // lines up with what the node actually produces
  const qreal fBodyCenterY = m_Rows.IsEmpty() && m_PreviewRect.isEmpty()
                               ? m_HeaderRect.center().y()
                               : (m_PreviewRect.isEmpty() ? (m_Rows[0].m_Rect.top() + m_Rows.PeekBack().m_Rect.bottom()) * 0.5
                                                          : m_PreviewRect.center().y());

  for (plQtVisualGraphPin* pPin : GetOutputPins())
  {
    const QRectF pinRect = pPin->GetPinRect();
    pPin->setVisible(true);
    pPin->setPos(QPointF(w - pinRect.x() - pinRect.width() * 0.5, fBodyCenterY - pinRect.y() - pinRect.height() * 0.5));
  }

  QPainterPath path;
  path.addRoundedRect(0, 0, w, m_fHeight, 5, 5);
  setPath(path);
}

void plQtParticleOperatorNodeItem::paint(QPainter* pPainter, const QStyleOptionGraphicsItem* pOption, QWidget* pWidget)
{
  PL_IGNORE_UNUSED(pOption);
  PL_IGNORE_UNUSED(pWidget);

  // Values are polled here rather than through an event handler: paint runs outside the document's
  // broadcasts, where subscribing is not allowed. UpdateGeometry only runs when something actually
  // differs, so this cannot loop.
  if (HasChanged())
  {
    Refresh();
  }
  else if (m_pPreviewProp != nullptr && !m_bPreviewIsColor && QueryPreviewPixmap())
  {
    // the thumbnail finished loading since the last paint, so re-fit the node to it
    Refresh();
  }

  const QPalette palette = QApplication::palette();
  const QColor body = palette.window().color();
  const QColor text = palette.text().color();
  const QColor dim = palette.placeholderText().color();

  pPainter->setRenderHint(QPainter::Antialiasing, true);

  pPainter->setPen(Qt::NoPen);
  pPainter->setBrush(body.darker(112));
  pPainter->drawPath(path());

  pPainter->setClipPath(path());
  pPainter->setBrush(body.lighter(122));
  pPainter->drawRect(m_HeaderRect);
  pPainter->setClipping(false);

  // category badge behind the subtitle, the same idiom the graph's other nodes use
  if (!m_pSubtitleLabel->toPlainText().isEmpty())
  {
    QRectF badge = m_pSubtitleLabel->boundingRect();
    badge.translate(m_pSubtitleLabel->pos());
    badge.adjust(-5, 0, 5, 0);

    QColor fill = m_BadgeColor;
    fill.setAlpha(45);

    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(fill);
    pPainter->drawRoundedRect(badge, 3, 3);
  }

  pPainter->setPen(QPen(isSelected() ? palette.highlight().color() : palette.mid().color(), isSelected() ? 2.0 : 1.0));
  pPainter->setBrush(Qt::NoBrush);
  pPainter->drawPath(path());

  QFont rowFont = QApplication::font();
  rowFont.setPointSizeF(rowFont.pointSizeF() * 0.9);
  pPainter->setFont(rowFont);

  const QFontMetricsF metrics(rowFont);

  // the value itself, filling the node body
  if (!m_PreviewRect.isEmpty())
  {
    if (m_bPreviewIsColor)
    {
      pPainter->setPen(Qt::NoPen);
      pPainter->setBrush(QColor(m_PreviewColor.r, m_PreviewColor.g, m_PreviewColor.b));
      pPainter->drawRoundedRect(m_PreviewRect, 3, 3);
    }
    else if (!m_Preview.isNull())
    {
      QPainterPath clip;
      clip.addRoundedRect(m_PreviewRect, 3, 3);
      pPainter->setClipPath(clip);
      pPainter->setRenderHint(QPainter::SmoothPixmapTransform, true);
      pPainter->drawPixmap(m_PreviewRect.toRect(), m_Preview);
      pPainter->setClipping(false);
    }
    else
    {
      pPainter->setPen(QPen(dim, 1.0, Qt::DashLine));
      pPainter->setBrush(Qt::NoBrush);
      pPainter->drawRoundedRect(m_PreviewRect, 3, 3);
      pPainter->setPen(dim);
      pPainter->drawText(m_PreviewRect, Qt::AlignCenter, QStringLiteral("none"));
    }

    pPainter->setPen(QPen(palette.mid().color(), 1.0));
    pPainter->setBrush(Qt::NoBrush);
    pPainter->drawRoundedRect(m_PreviewRect, 3, 3);
  }

  for (const Row& row : m_Rows)
  {
    const QRectF labelRect(row.m_Rect.left(), row.m_Rect.top(), row.m_ValueRect.left() - row.m_Rect.left() - 4.0, row.m_Rect.height());

    pPainter->setPen(dim);
    pPainter->drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft,
      metrics.elidedText(QString::fromUtf8(row.m_sLabel.GetData()), Qt::ElideRight, labelRect.width()));

    if (row.m_Kind == Row::Kind::PinOnly)
      continue;

    if (IsRowDriven(row))
    {
      // the wire supplies this one; the typed value is inert
      pPainter->setPen(dim);
      pPainter->drawText(row.m_ValueRect, Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("wired"));
      continue;
    }

    switch (row.m_Kind)
    {
      case Row::Kind::Color:
      {
        const QRectF swatch(row.m_ValueRect.right() - 24, row.m_ValueRect.center().y() - 5.0, 22.0, 10.0);
        pPainter->setPen(QPen(dim, 1.0));
        pPainter->setBrush(QColor(row.m_Color.r, row.m_Color.g, row.m_Color.b));
        pPainter->drawRoundedRect(swatch, 2, 2);
        break;
      }

      case Row::Kind::Number:
        pPainter->setPen(QPen(dim, 1.0, Qt::DotLine));
        pPainter->drawLine(row.m_ValueRect.bottomLeft() + QPointF(2, -1), row.m_ValueRect.bottomRight() + QPointF(-1, -1));
        pPainter->setPen(text);
        pPainter->drawText(row.m_ValueRect, Qt::AlignVCenter | Qt::AlignRight, row.m_sValue.GetData());
        break;

      default:
        pPainter->setPen(text);
        pPainter->drawText(row.m_ValueRect, Qt::AlignVCenter | Qt::AlignRight,
          metrics.elidedText(QString::fromUtf8(row.m_sValue.GetData()), Qt::ElideRight, row.m_ValueRect.width()));
        break;
    }
  }
}

const plQtParticleOperatorNodeItem::Row* plQtParticleOperatorNodeItem::HitTestRow(const QPointF& localPos) const
{
  for (const Row& row : m_Rows)
  {
    if (row.m_Kind != Row::Kind::PinOnly && !IsRowDriven(row) && row.m_Rect.contains(localPos))
      return &row;
  }

  return nullptr;
}

void plQtParticleOperatorNodeItem::mousePressEvent(QGraphicsSceneMouseEvent* pEvent)
{
  if (pEvent->button() == Qt::LeftButton)
  {
    if (m_pPreviewProp != nullptr && m_PreviewRect.contains(pEvent->pos()))
    {
      Row row;
      row.m_pProp = m_pPreviewProp;
      row.m_Original = GetObject()->GetTypeAccessor().GetValue(m_pPreviewProp->GetPropertyName());
      row.m_Color = m_PreviewColor;

      if (m_bPreviewIsColor)
        PickColor(row);
      else
        PickAsset(row);

      pEvent->accept();
      return;
    }

    if (const Row* pRow = HitTestRow(pEvent->pos()))
    {
      switch (pRow->m_Kind)
      {
        case Row::Kind::Enum:
          ShowEnumMenu(*pRow, pEvent->screenPos());
          break;

        case Row::Kind::Color:
          PickColor(*pRow);
          break;

        case Row::Kind::Asset:
          PickAsset(*pRow);
          break;

        case Row::Kind::Number:
          m_pScrubProp = pRow->m_pProp;
          m_ScrubRow = *pRow;
          m_fScrubStartValue = pRow->m_fNumber;
          m_ScrubStartPos = pEvent->pos();
          m_bScrubbing = false;
          break;

        default:
          // a bool is one click, not a dialog
          if (pRow->m_Original.IsA<bool>())
            SetValue(pRow->m_pProp, plVariant(!pRow->m_Original.Get<bool>()));
          else
            TypeValue(*pRow);
          break;
      }

      pEvent->accept();
      return;
    }
  }

  SUPER::mousePressEvent(pEvent);
}

void plQtParticleOperatorNodeItem::mouseMoveEvent(QGraphicsSceneMouseEvent* pEvent)
{
  if (m_pScrubProp != nullptr)
  {
    const qreal fDelta = pEvent->pos().x() - m_ScrubStartPos.x();

    if (!m_bScrubbing && plMath::Abs(fDelta) < kDragThreshold)
    {
      pEvent->accept();
      return;
    }

    if (!m_bScrubbing)
    {
      m_bScrubbing = true;
      m_pOwnManager->GetDocument()->GetCommandHistory()->StartTransaction("Scrub Value");
      m_bScrubTransaction = true;
    }

    const double fMagnitude = plMath::Abs(m_fScrubStartValue);
    const double fStep = fMagnitude > 100.0 ? 1.0 : (fMagnitude > 10.0 ? 0.1 : 0.01);

    SetValue(m_pScrubProp, plVariant(m_fScrubStartValue + fDelta * fStep).ConvertTo(m_ScrubRow.m_Original.GetType()));

    pEvent->accept();
    return;
  }

  SUPER::mouseMoveEvent(pEvent);
}

void plQtParticleOperatorNodeItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* pEvent)
{
  if (m_pScrubProp != nullptr)
  {
    if (m_bScrubTransaction)
    {
      m_pOwnManager->GetDocument()->GetCommandHistory()->FinishTransaction();
      m_bScrubTransaction = false;
    }
    else
    {
      TypeValue(m_ScrubRow);
    }

    m_pScrubProp = nullptr;
    m_bScrubbing = false;

    pEvent->accept();
    return;
  }

  SUPER::mouseReleaseEvent(pEvent);
}

void plQtParticleOperatorNodeItem::SetValue(const plAbstractProperty* pProp, const plVariant& value)
{
  if (!value.IsValid())
    return;

  plCommandHistory* pHistory = m_pOwnManager->GetDocument()->GetCommandHistory();
  pHistory->StartTransaction("Change Value");

  plSetObjectPropertyCommand cmd;
  cmd.m_Object = GetObject()->GetGuid();
  cmd.m_sProperty = pProp->GetPropertyName();
  cmd.m_NewValue = value;

  if (pHistory->AddCommand(cmd).Failed())
    pHistory->CancelTransaction();
  else
    pHistory->FinishTransaction();
}

void plQtParticleOperatorNodeItem::ShowEnumMenu(const Row& row, const QPoint& screenPos)
{
  plDynamicArray<plReflectionUtils::EnumKeyValuePair> entries;
  plReflectionUtils::GetEnumKeysAndValues(row.m_pProp->GetSpecificType(), entries, plReflectionUtils::EnumConversionMode::ValueNameOnly);

  if (entries.IsEmpty())
    return;

  const plInt64 iCurrent = row.m_Original.ConvertTo<plInt64>();

  QMenu menu;
  for (const auto& entry : entries)
  {
    const plStringBuilder sLabel = plTranslate(entry.m_sKey);
    QAction* pAction = menu.addAction(QString::fromUtf8(sLabel.GetData()));
    pAction->setCheckable(true);
    pAction->setChecked(entry.m_iValue == iCurrent);
    pAction->setData(QVariant::fromValue<qlonglong>(entry.m_iValue));
  }

  const plAbstractProperty* pProp = row.m_pProp;
  QAction* pChosen = menu.exec(screenPos);

  if (pChosen != nullptr)
    SetValue(pProp, plVariant((plInt64)pChosen->data().value<qlonglong>()));
}

void plQtParticleOperatorNodeItem::PickColor(const Row& row)
{
  QWidget* pParent = (scene() != nullptr && !scene()->views().isEmpty()) ? scene()->views().first() : nullptr;

  const bool bIsLinear = row.m_Original.IsA<plColor>();
  const plColor initial = bIsLinear ? row.m_Original.Get<plColor>() : plColor(row.m_Color);

  plQtColorDialog dlg(initial, pParent);
  dlg.ShowAlpha(row.m_pProp->GetAttributeByType<plExposeColorAlphaAttribute>() != nullptr);
  dlg.ShowHDR(bIsLinear);

  plColor picked = initial;
  QObject::connect(&dlg, &plQtColorDialog::CurrentColorChanged, [&picked](const plColor& c)
    { picked = c; });

  const plAbstractProperty* pProp = row.m_pProp;

  if (dlg.exec() == QDialog::Accepted)
    SetValue(pProp, bIsLinear ? plVariant(picked) : plVariant(plColorGammaUB(picked)));
}

void plQtParticleOperatorNodeItem::PickAsset(const Row& row)
{
  const plAssetBrowserAttribute* pAttr = row.m_pProp->GetAttributeByType<plAssetBrowserAttribute>();
  if (pAttr == nullptr)
    return;

  QWidget* pParent = (scene() != nullptr && !scene()->views().isEmpty()) ? scene()->views().first() : nullptr;
  const plUuid current = plConversionUtils::ConvertStringToUuid(row.m_Original.ConvertTo<plString>());

  plQtAssetBrowserDlg dlg(pParent, current, pAttr->GetTypeFilter());
  const plAbstractProperty* pProp = row.m_pProp;

  if (dlg.exec() == 0)
    return;

  plStringBuilder sResult;
  if (dlg.GetSelectedAssetGuid().IsValid())
    plConversionUtils::ToString(dlg.GetSelectedAssetGuid(), sResult);
  else
    sResult = dlg.GetSelectedAssetPathRelative();

  SetValue(pProp, plVariant(plString(sResult)));
}

void plQtParticleOperatorNodeItem::TypeValue(const Row& row)
{
  QWidget* pParent = (scene() != nullptr && !scene()->views().isEmpty()) ? scene()->views().first() : nullptr;
  const plAbstractProperty* pProp = row.m_pProp;

  if (pProp == nullptr)
    return;

  if (row.m_Kind == Row::Kind::Number)
  {
    bool bOk = false;
    const double fNew = QInputDialog::getDouble(
      pParent, QStringLiteral("Set Value"), QString::fromUtf8(row.m_sLabel.GetData()), row.m_fNumber, -1000000.0, 1000000.0, 3, &bOk);

    if (bOk)
      SetValue(pProp, plVariant(fNew).ConvertTo(row.m_Original.GetType()));

    return;
  }

  bool bOk = false;
  const QString sNew = QInputDialog::getText(pParent, QStringLiteral("Set Value"), QString::fromUtf8(row.m_sLabel.GetData()),
    QLineEdit::Normal, QString::fromUtf8(row.m_Original.ConvertTo<plString>().GetData()), &bOk);

  if (bOk)
    SetValue(pProp, plVariant(plString(sNew.toUtf8().constData())));
}

void plQtParticleOperatorNodeItem::Refresh()
{
  UpdateState();
  UpdateGeometry();
  update();
}

bool plQtParticleOperatorNodeItem::HasChanged() const
{
  if (m_pOwnManager == nullptr || GetObject() == nullptr)
    return false;

  const auto& accessor = GetObject()->GetTypeAccessor();

  // the preview node's single value
  if (m_pPreviewProp != nullptr)
    return accessor.GetValue(m_pPreviewProp->GetPropertyName()) != m_PreviewOriginal;

  for (const Row& row : m_Rows)
  {
    if (row.m_pProp != nullptr && accessor.GetValue(row.m_pProp->GetPropertyName()) != row.m_Original)
      return true;
  }

  return false;
}
