#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/Assets/AssetBrowserDlg.moc.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleSystemNodeItem.h>
#include <Foundation/Strings/TranslationLookup.h>
#include <Foundation/Types/VarianceTypes.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <GuiFoundation/UIServices/ColorDialog.moc.h>
#include <GuiFoundation/UIServices/UIServices.moc.h>
#include <GuiFoundation/VisualGraph/Pin.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Selection/SelectionManager.h>

#include <QApplication>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>

namespace
{
  // Layout metrics. The node is a fixed width so that a column of systems reads as a column.
  constexpr qreal kPad = 8.0;
  constexpr qreal kHeaderH = 26.0;
  constexpr qreal kCtxHeaderH = 20.0;
  constexpr qreal kRowH = 21.0;
  constexpr qreal kPropH = 17.0;
  constexpr qreal kAddH = 18.0;
  constexpr qreal kCtxGap = 10.0;
  constexpr qreal kAccentW = 3.0;
  constexpr int kDragThreshold = 4;

  /// Category colours, matching the pin colours the graph already used for each block kind.
  QColor ContextColor(plParticleContext::Enum context)
  {
    switch (context)
    {
      case plParticleContext::Spawn:
        return QColor(224, 145, 58);
      case plParticleContext::Initialize:
        return QColor(103, 184, 89);
      case plParticleContext::Update:
        return QColor(63, 169, 196);
      default:
        return QColor(168, 116, 201);
    }
  }

  plString BlockDisplayName(const plRTTI* pType) { return plParticleEffectNodeManager::FriendlyTypeName(pType); }

  /// One-line summary shown right-aligned on a collapsed block row, so the stack stays readable
  /// without opening the properties dock. Uses the type's plTitleAttribute when it has one.
  plString BlockSummary(const plDocumentObject* pObject)
  {
    const plRTTI* pType = pObject->GetTypeAccessor().GetType();

    const plTitleAttribute* pTitle = pType->GetAttributeByType<plTitleAttribute>();
    if (pTitle == nullptr)
      return plString();

    plStringBuilder sTitle = pTitle->GetTitle();

    plHybridArray<const plAbstractProperty*, 32> properties;
    pType->GetAllProperties(properties);

    plStringBuilder sValue, sToken;
    for (const plAbstractProperty* pProp : properties)
    {
      if (pProp->GetCategory() != plPropertyCategory::Member)
        continue;

      const plVariant value = pObject->GetTypeAccessor().GetValue(pProp->GetPropertyName());

      if (pProp->GetSpecificType()->IsDerivedFrom<plEnumBase>() || pProp->GetSpecificType()->IsDerivedFrom<plBitflagsBase>())
      {
        plReflectionUtils::EnumerationToString(pProp->GetSpecificType(), value.ConvertTo<plInt64>(), sValue);
        sValue = plTranslate(sValue);
      }
      else if (value.CanConvertTo<plString>())
      {
        sValue = value.ConvertTo<plString>();
      }
      else
      {
        sValue.Clear();
      }

      sToken.Set("{", pProp->GetPropertyName(), "}");
      sTitle.ReplaceAll(sToken, sValue);

      sToken.Set("{?", pProp->GetPropertyName(), "}");
      sTitle.ReplaceAll(sToken, value == plVariant(0) ? plStringView() : plStringView(sValue));
    }

    sTitle.Trim("\' :");

    if (sTitle.GetCharacterCount() > 22)
    {
      sTitle.Shrink(0, sTitle.GetCharacterCount() - 20);
      sTitle.Append("...");
    }

    return sTitle;
  }
} // namespace

plQtParticleSystemNodeItem::plQtParticleSystemNodeItem()
{
  setAcceptHoverEvents(true);
}

plQtParticleSystemNodeItem::~plQtParticleSystemNodeItem()
{
  if (m_bSubscribed && m_pOwnManager != nullptr)
  {
    m_pOwnManager->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plQtParticleSystemNodeItem::StructureEventHandler, this));
    m_pOwnManager->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plQtParticleSystemNodeItem::PropertyEventHandler, this));
    m_pOwnManager->GetDocument()->GetSelectionManager()->m_Events.RemoveEventHandler(
      plMakeDelegate(&plQtParticleSystemNodeItem::SelectionEventHandler, this));
  }
}

void plQtParticleSystemNodeItem::InitNode(const plVisualGraphObjectManager* pManager, const plDocumentObject* pObject)
{
  // The scene hands out a const manager, but a container node has to follow changes to objects
  // it does not own — its blocks — and those events are only published on the mutable manager.
  m_pOwnManager = const_cast<plVisualGraphObjectManager*>(pManager);

  SUPER::InitNode(pManager, pObject);

  if (!m_bSubscribed)
  {
    m_pOwnManager->m_StructureEvents.AddEventHandler(plMakeDelegate(&plQtParticleSystemNodeItem::StructureEventHandler, this));
    m_pOwnManager->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plQtParticleSystemNodeItem::PropertyEventHandler, this));
    m_pOwnManager->GetDocument()->GetSelectionManager()->m_Events.AddEventHandler(
      plMakeDelegate(&plQtParticleSystemNodeItem::SelectionEventHandler, this));
    m_bSubscribed = true;
  }

  // flat look: no shadow, and the outline only appears on selection
  EnableDropShadow(false);
}

void plQtParticleSystemNodeItem::UpdateState()
{
  const auto& accessor = GetObject()->GetTypeAccessor();

  plStringBuilder sName = accessor.GetValue("Name").ConvertTo<plString>();
  if (sName.IsEmpty())
    sName = "System";

  m_pTitleLabel->setPlainText(sName.GetData());

  plStringBuilder sTarget;
  plReflectionUtils::EnumerationToString(
    plGetStaticRTTI<plParticleSimulationTarget>(), accessor.GetValue("SimulationTarget").ConvertTo<plInt64>(), sTarget);
  sTarget = plTranslate(sTarget);

  m_pSubtitleLabel->setPlainText(sTarget.GetData());
  m_sCategoryRoot = QStringLiteral("System");
}

void plQtParticleSystemNodeItem::RebuildRows()
{
  const plDocumentObject* pSystem = GetObject();

  // A CPU system is never asked to lower, so there is nothing to report for it.
  const plInt64 iTarget = pSystem->GetTypeAccessor().GetValue("SimulationTarget").ConvertTo<plInt64>();
  m_bTargetsGpu = iTarget != (plInt64)plParticleSimulationTarget::CPU;
  m_sLoweringReason.Clear();
  m_pLoweringBlocker = nullptr;

  if (m_bTargetsGpu)
  {
    if (const auto* pDoc = plDynamicCast<const plParticleEffectAssetDocument*>(m_pOwnManager->GetDocument()))
    {
      const auto lowering = pDoc->QueryLowering(pSystem);
      m_sLoweringReason = lowering.m_sReason;
      m_pLoweringBlocker = lowering.m_pBlock;
    }
  }

  for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
  {
    ContextLayout& layout = m_Contexts[uiContext];
    layout.m_Blocks.Clear();

    const char* szProperty = plParticleContextInfo::Get((plParticleContext::Enum)uiContext).m_szProperty;
    const auto& accessor = pSystem->GetTypeAccessor();
    const plInt32 iCount = accessor.GetCount(szProperty);

    for (plInt32 i = 0; i < iCount; ++i)
    {
      const plVariant value = accessor.GetValue(szProperty, i);
      if (!value.IsA<plUuid>())
        continue;

      const plDocumentObject* pBlock = m_pOwnManager->GetObject(value.Get<plUuid>());
      if (pBlock == nullptr)
        continue;

      BlockRow& row = layout.m_Blocks.ExpandAndGetRef();
      row.m_pObject = pBlock;
      row.m_sTitle = BlockDisplayName(pBlock->GetTypeAccessor().GetType());
      row.m_sSummary = BlockSummary(pBlock);
      row.m_bExpanded = m_ExpandedBlocks.Contains(pBlock->GetGuid());

      const plVariant enabled = pBlock->GetTypeAccessor().GetValue("Enabled");
      row.m_bEnabled = !enabled.IsA<bool>() || enabled.Get<bool>();

      if (row.m_bExpanded)
        CollectProps(row);
    }
  }
}

void plQtParticleSystemNodeItem::CollectProps(BlockRow& ref_row) const
{
  ref_row.m_Props.Clear();

  const plDocumentObject* pBlock = ref_row.m_pObject;
  const plRTTI* pType = pBlock->GetTypeAccessor().GetType();

  // Honour the same dynamic visibility rules the properties dock uses, so a block does not show
  // fields that are irrelevant for its current mode.
  plMap<plString, plPropertyUiState> uiStates;
  plPropertyMetaState::GetSingleton()->GetTypePropertiesState(pBlock, uiStates);

  plHybridArray<const plAbstractProperty*, 32> properties;
  pType->GetAllProperties(properties);

  for (const plAbstractProperty* pProp : properties)
  {
    if (pProp->GetCategory() != plPropertyCategory::Member)
      continue;
    if (pProp->GetFlags().IsSet(plPropertyFlags::ReadOnly))
      continue;
    if (pProp->GetAttributeByType<plHiddenAttribute>() != nullptr)
      continue;

    auto itState = uiStates.Find(pProp->GetPropertyName());
    if (itState.IsValid() && itState.Value().m_Visibility == plPropertyUiState::Invisible)
      continue;

    const plVariant value = pBlock->GetTypeAccessor().GetValue(pProp->GetPropertyName());

    PropRow& prop = ref_row.m_Props.ExpandAndGetRef();
    prop.m_pProp = pProp;
    prop.m_sLabel = plTranslate(pProp->GetPropertyName());
    prop.m_Original = value;

    if (const plClampValueAttribute* pClamp = pProp->GetAttributeByType<plClampValueAttribute>())
    {
      if (pClamp->GetMinValue().CanConvertTo<double>() && pClamp->GetMaxValue().CanConvertTo<double>())
      {
        prop.m_bHasClamp = true;
        prop.m_fMin = pClamp->GetMinValue().ConvertTo<double>();
        prop.m_fMax = pClamp->GetMaxValue().ConvertTo<double>();
      }
    }

    const plRTTI* pPropType = pProp->GetSpecificType();

    if (pPropType->IsDerivedFrom<plEnumBase>() || pPropType->IsDerivedFrom<plBitflagsBase>())
    {
      plStringBuilder sValue;
      plReflectionUtils::EnumerationToString(pPropType, value.ConvertTo<plInt64>(), sValue);
      prop.m_Kind = pPropType->IsDerivedFrom<plEnumBase>() ? PropRow::Kind::Enum : PropRow::Kind::Text;
      prop.m_sValue = plTranslate(sValue);
    }
    else if (value.IsA<bool>())
    {
      prop.m_Kind = PropRow::Kind::Bool;
      prop.m_bBoolValue = value.Get<bool>();
    }
    else if (value.IsA<plColor>() || value.IsA<plColorGammaUB>())
    {
      prop.m_Kind = PropRow::Kind::Color;
      prop.m_Color = value.IsA<plColor>() ? plColorGammaUB(value.Get<plColor>()) : value.Get<plColorGammaUB>();
    }
    else if (value.IsA<plVarianceTypeFloat>() || value.IsA<plVarianceTypeTime>() || value.IsA<plVarianceTypeAngle>())
    {
      prop.m_Kind = PropRow::Kind::Number;
      prop.m_bHasVariance = true;

      if (value.IsA<plVarianceTypeFloat>())
      {
        prop.m_fNumber = value.Get<plVarianceTypeFloat>().m_Value;
        prop.m_fVariance = value.Get<plVarianceTypeFloat>().m_fVariance;
      }
      else if (value.IsA<plVarianceTypeTime>())
      {
        prop.m_fNumber = value.Get<plVarianceTypeTime>().m_Value.GetSeconds();
        prop.m_fVariance = value.Get<plVarianceTypeTime>().m_fVariance;
      }
      else
      {
        prop.m_fNumber = value.Get<plVarianceTypeAngle>().m_Value.GetDegree();
        prop.m_fVariance = value.Get<plVarianceTypeAngle>().m_fVariance;
      }
    }
    else if (value.IsA<plTime>())
    {
      prop.m_Kind = PropRow::Kind::Number;
      prop.m_fNumber = value.Get<plTime>().GetSeconds();
    }
    else if (value.IsA<plAngle>())
    {
      prop.m_Kind = PropRow::Kind::Number;
      prop.m_fNumber = value.Get<plAngle>().GetDegree();
    }
    else if (value.IsNumber() && !value.IsA<bool>())
    {
      prop.m_Kind = PropRow::Kind::Number;
      prop.m_fNumber = value.ConvertTo<double>();
      prop.m_bIsInteger = !value.IsFloatingPoint();
    }
    else if (pProp->GetAttributeByType<plAssetBrowserAttribute>() != nullptr)
    {
      prop.m_Kind = PropRow::Kind::Asset;

      const plString sRaw = value.ConvertTo<plString>();
      prop.m_sValue = sRaw.IsEmpty() ? plString("none") : plString("assigned");
    }
    else if (value.CanConvertTo<plString>())
    {
      plStringBuilder s = value.ConvertTo<plString>();
      s.ReplaceAll("\n", " ");
      if (s.GetCharacterCount() > 16)
      {
        s.Shrink(0, s.GetCharacterCount() - 14);
        s.Append("...");
      }
      prop.m_sValue = s.IsEmpty() ? plString("-") : plString(s);
    }
    else
    {
      prop.m_sValue = "...";
    }

    if (prop.m_Kind == PropRow::Kind::Number)
    {
      plStringBuilder s;

      if (prop.m_bIsInteger)
        s.SetFormat("{0}", (plInt64)prop.m_fNumber);
      else
        s.SetFormat("{0}", plArgF(prop.m_fNumber, 2));

      prop.m_sValue = s;

      if (prop.m_bHasVariance)
      {
        s.SetFormat("+-{0}", plArgF(prop.m_fVariance, 2));
        prop.m_sValue2 = s;
      }
    }
  }
}

void plQtParticleSystemNodeItem::UpdateGeometry()
{
  prepareGeometryChange();

  RebuildRows();

  const qreal w = m_fWidth;

  // Header: title and simulation-target badge, both centred by the base class convention.
  {
    QRectF titleRect = m_pTitleLabel->boundingRect();
    m_pTitleLabel->setPos((w - titleRect.width()) * 0.5, 3.0);

    QRectF subRect = m_pSubtitleLabel->boundingRect();
    m_pSubtitleLabel->setPos((w - subRect.width()) * 0.5, 3.0 + titleRect.height() - 4.0);

    m_pIcon->setPos(0, 0);
  }

  const qreal fHeaderBottom = kHeaderH + m_pSubtitleLabel->boundingRect().height();
  m_HeaderRect = QRectF(0, 0, w, fHeaderBottom);

  qreal y = fHeaderBottom + 5.0;

  if (m_bTargetsGpu)
  {
    m_VerdictRect = QRectF(kPad, y, w - 2 * kPad, 16.0);
    y += 20.0;
  }
  else
  {
    m_VerdictRect = QRectF();
  }

  y += kCtxGap - 5.0;

  for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
  {
    ContextLayout& layout = m_Contexts[uiContext];

    const qreal fTop = y;
    layout.m_HeaderRect = QRectF(kPad, y, w - 2 * kPad, kCtxHeaderH);
    y += kCtxHeaderH;

    if (m_bCollapsed[uiContext])
    {
      layout.m_AddRect = QRectF();
      for (BlockRow& row : layout.m_Blocks)
        row.m_Rect = QRectF();
    }
    else
    {
      y += 3.0;

      for (BlockRow& row : layout.m_Blocks)
      {
        const qreal fTopOfBlock = y;

        row.m_Rect = QRectF(kPad + kAccentW, y, w - 2 * kPad - kAccentW, kRowH - 2.0);
        row.m_EnableRect = QRectF(row.m_Rect.right() - 16.0, row.m_Rect.center().y() - 5.0, 10.0, 10.0);
        y += kRowH;

        for (PropRow& prop : row.m_Props)
        {
          prop.m_Rect = QRectF(row.m_Rect.left() + 10.0, y, row.m_Rect.width() - 12.0, kPropH - 1.0);
          // values get the wider share: enum names such as "Flipbook Animation" are long
          prop.m_ValueRect = QRectF(prop.m_Rect.left() + prop.m_Rect.width() * 0.42, prop.m_Rect.top(),
            prop.m_Rect.width() * 0.58, prop.m_Rect.height());

          // a variance pair splits that area into value and deviation, each scrubbable on its own
          if (prop.m_bHasVariance)
          {
            const qreal fHalf = prop.m_ValueRect.width() * 0.5;
            prop.m_ValueRect2 = QRectF(prop.m_ValueRect.left() + fHalf, prop.m_ValueRect.top(), fHalf, prop.m_ValueRect.height());
            prop.m_ValueRect.setWidth(fHalf - 4.0);
          }

          y += kPropH;
        }

        if (row.m_bExpanded)
          y += 3.0;

        row.m_FullRect = QRectF(row.m_Rect.left(), fTopOfBlock, row.m_Rect.width(), y - fTopOfBlock);
      }

      layout.m_AddRect = QRectF(kPad + kAccentW, y, w - 2 * kPad - kAccentW, kAddH);
      y += kAddH + 4.0;
    }

    layout.m_Rect = QRectF(kPad, fTop, w - 2 * kPad, y - fTop);
    y += kCtxGap;
  }

  m_fHeight = y - kCtxGap + kPad;

  // Inputs: the legacy category pins stay hidden, while property pins sit on the row they drive.
  // An unconnected pin only appears while its block is open, so a closed system stays clean.
  for (plQtVisualGraphPin* pPin : GetInputPins())
  {
    const plVisualGraphPin* pPinData = pPin->GetPin();

    plUuid block;
    plStringBuilder sProperty;

    if (pPinData == nullptr || !plParticlePropertyPin::Parse(pPinData->GetName(), block, sProperty))
    {
      pPin->setVisible(false);
      continue;
    }

    QPointF anchor;
    bool bRowVisible = false;

    if (!FindPinAnchor(block, sProperty, anchor, bRowVisible))
    {
      pPin->setVisible(false);
      continue;
    }

    // Ask the manager, not the Qt pin: connection items are created after every node is built, so
    // during the first layout the Qt pin always looks unconnected and its wire would be left
    // dangling at the node's origin until something forced a relayout.
    const bool bConnected = m_pOwnManager->HasConnections(*pPinData);

    if (!bRowVisible && !bConnected)
    {
      pPin->setVisible(false);
      continue;
    }

    const QRectF pinRect = pPin->GetPinRect();
    pPin->setVisible(true);
    pPin->setPos(anchor - QPointF(pinRect.x() + pinRect.width() * 0.5, pinRect.y() + pinRect.height() * 0.5));
  }

  for (plUInt32 i = 0; i < GetOutputPins().GetCount(); ++i)
  {
    plQtVisualGraphPin* pPin = GetOutputPins()[i];
    const QRectF pinRect = pPin->GetPinRect();
    pPin->setVisible(true);
    pPin->setPos(QPointF((w - pinRect.width()) * 0.5 - pinRect.x(), m_fHeight - pinRect.y() - pinRect.height() * 0.5));
  }

  QPainterPath path;
  path.addRoundedRect(0, 0, w, m_fHeight, 6, 6);
  setPath(path);
}

void plQtParticleSystemNodeItem::paint(QPainter* pPainter, const QStyleOptionGraphicsItem* pOption, QWidget* pWidget)
{
  PL_IGNORE_UNUSED(pOption);
  PL_IGNORE_UNUSED(pWidget);

  const QPalette palette = QApplication::palette();
  const QColor body = palette.window().color();
  const QColor line = palette.mid().color();
  const QColor text = palette.text().color();
  const QColor dim = palette.placeholderText().color();

  pPainter->setRenderHint(QPainter::Antialiasing, true);

  // body
  pPainter->setPen(Qt::NoPen);
  pPainter->setBrush(body.darker(115));
  pPainter->drawPath(path());

  // header
  pPainter->setClipPath(path());
  pPainter->setBrush(body.lighter(125));
  pPainter->drawRect(m_HeaderRect);
  pPainter->setClipping(false);

  // selection outline
  QPen borderPen(isSelected() ? palette.highlight().color() : line, isSelected() ? 2.0 : 1.0);
  pPainter->setPen(borderPen);
  pPainter->setBrush(Qt::NoBrush);
  pPainter->drawPath(path());

  QFont blockFont = QApplication::font();
  blockFont.setPointSizeF(blockFont.pointSizeF() * 0.92);

  // GPU verdict: what FindLoweringBlocker would say, before the effect is ever played
  if (m_bTargetsGpu && !m_VerdictRect.isEmpty())
  {
    const bool bLowers = m_sLoweringReason.IsEmpty();
    const QColor tint = bLowers ? QColor(103, 184, 89) : QColor(217, 96, 92);

    QColor fill = tint;
    fill.setAlpha(38);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(fill);
    pPainter->drawRoundedRect(m_VerdictRect, 3, 3);

    plStringBuilder sText;
    if (bLowers)
      sText = "Lowered to GPU";
    else
      sText.SetFormat("Falls back to CPU: {0}", m_sLoweringReason);

    QFont verdictFont = blockFont;
    verdictFont.setPointSizeF(verdictFont.pointSizeF() * 0.94);

    const QFontMetricsF verdictMetrics(verdictFont);
    const QRectF textRect = m_VerdictRect.adjusted(6, 0, -6, 0);

    pPainter->setFont(verdictFont);
    pPainter->setPen(tint);
    pPainter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
      verdictMetrics.elidedText(QString::fromUtf8(sText.GetData()), Qt::ElideRight, textRect.width()));
  }

  QFont ctxFont = blockFont;
  ctxFont.setBold(true);
  ctxFont.setCapitalization(QFont::AllUppercase);

  const auto& selection = m_pOwnManager->GetDocument()->GetSelectionManager()->GetSelection();

  for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
  {
    const ContextLayout& layout = m_Contexts[uiContext];
    const auto context = (plParticleContext::Enum)uiContext;
    const QColor accent = ContextColor(context);

    // context box + left accent bar
    QColor boxFill = body.darker(135);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(boxFill);
    pPainter->drawRoundedRect(layout.m_Rect, 4, 4);

    pPainter->setBrush(accent);
    pPainter->drawRect(QRectF(layout.m_Rect.left(), layout.m_Rect.top() + 2, kAccentW, layout.m_Rect.height() - 4));

    // Collapse arrow, painted rather than typed: a triangle glyph in a string literal depends on
    // the compiler reading this file as UTF-8 and on the UI font actually having that character.
    {
      const QPointF c(layout.m_HeaderRect.left() + kAccentW + 9, layout.m_HeaderRect.center().y());
      QPolygonF triangle;

      if (m_bCollapsed[uiContext])
        triangle << QPointF(c.x() - 2.5, c.y() - 4.0) << QPointF(c.x() + 3.5, c.y()) << QPointF(c.x() - 2.5, c.y() + 4.0);
      else
        triangle << QPointF(c.x() - 4.0, c.y() - 2.5) << QPointF(c.x() + 4.0, c.y() - 2.5) << QPointF(c.x(), c.y() + 3.5);

      pPainter->setPen(Qt::NoPen);
      pPainter->setBrush(accent);
      pPainter->drawPolygon(triangle);
    }

    // context header
    pPainter->setFont(ctxFont);
    pPainter->setPen(accent);

    QRectF labelRect = layout.m_HeaderRect.adjusted(kAccentW + 20, 0, -6, 0);
    pPainter->drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft, plParticleContextInfo::Get(context).m_szLabel);

    if (m_bCollapsed[uiContext])
    {
      pPainter->setPen(dim);
      pPainter->setFont(blockFont);
      pPainter->drawText(labelRect, Qt::AlignVCenter | Qt::AlignRight, QString::number(layout.m_Blocks.GetCount()));
      continue;
    }

    // blocks
    pPainter->setFont(blockFont);

    for (plUInt32 uiRow = 0; uiRow < layout.m_Blocks.GetCount(); ++uiRow)
    {
      const BlockRow& row = layout.m_Blocks[uiRow];
      const bool bSelected = selection.Contains(row.m_pObject);
      const bool bDragged = (m_bDragging && row.m_pObject == m_pDragBlock);

      const bool bBlocksLowering = (row.m_pObject == m_pLoweringBlocker);

      QColor rowFill = bSelected ? palette.highlight().color().darker(160) : body.lighter(112);
      if (bBlocksLowering)
        rowFill = QColor(70, 38, 42);
      if (!bSelected && row.m_pObject == m_pHoverBlock)
        rowFill = rowFill.lighter(125);
      if (bDragged)
        rowFill.setAlpha(90);

      pPainter->setPen(Qt::NoPen);
      pPainter->setBrush(rowFill);
      pPainter->drawRoundedRect(row.m_Rect, 3, 3);

      if (bSelected || bBlocksLowering)
      {
        pPainter->setPen(QPen(bSelected ? palette.highlight().color() : QColor(217, 96, 92), 1.0));
        pPainter->setBrush(Qt::NoBrush);
        pPainter->drawRoundedRect(row.m_Rect, 3, 3);
      }

      // expander: a dot when collapsed, a triangle when open, both in the context colour
      pPainter->setPen(Qt::NoPen);
      pPainter->setBrush(accent);

      const QPointF marker(row.m_Rect.left() + 8, row.m_Rect.center().y());
      if (row.m_bExpanded)
      {
        QPolygonF triangle;
        triangle << QPointF(marker.x() - 3.5, marker.y() - 2.0) << QPointF(marker.x() + 3.5, marker.y() - 2.0)
                 << QPointF(marker.x(), marker.y() + 3.0);
        pPainter->drawPolygon(triangle);
      }
      else
      {
        pPainter->drawEllipse(marker, 3.0, 3.0);
      }

      QRectF nameRect = row.m_Rect.adjusted(17, 0, -22, 0);
      pPainter->setPen(!row.m_bEnabled ? dim : (bBlocksLowering ? QColor(255, 154, 154) : text));
      pPainter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, row.m_sTitle.GetData());

      if (bBlocksLowering)
      {
        pPainter->setPen(QColor(217, 96, 92));
        pPainter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("CPU only"));
      }
      else if (!row.m_bExpanded && !row.m_sSummary.IsEmpty())
      {
        pPainter->setPen(dim);
        pPainter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignRight, row.m_sSummary.GetData());
      }

      // enable toggle; a disabled block stays authored but is dropped when the descriptor is built
      pPainter->setPen(QPen(dim, 1.0));
      pPainter->setBrush(row.m_bEnabled ? QBrush(accent) : QBrush(Qt::NoBrush));
      pPainter->drawRoundedRect(row.m_EnableRect, 2, 2);

      // expanded values
      const QFontMetricsF metrics(blockFont);

      for (const PropRow& prop : row.m_Props)
      {
        const QRectF labelRect(prop.m_Rect.left(), prop.m_Rect.top(), prop.m_ValueRect.left() - prop.m_Rect.left() - 6.0, prop.m_Rect.height());

        pPainter->setPen(dim);
        pPainter->drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft,
          metrics.elidedText(QString::fromUtf8(prop.m_sLabel.GetData()), Qt::ElideRight, labelRect.width()));

        switch (prop.m_Kind)
        {
          case PropRow::Kind::Bool:
          {
            const QRectF box(prop.m_ValueRect.left(), prop.m_ValueRect.center().y() - 5.0, 10.0, 10.0);
            pPainter->setPen(QPen(dim, 1.0));
            pPainter->setBrush(prop.m_bBoolValue ? accent : QBrush(Qt::NoBrush));
            pPainter->drawRoundedRect(box, 2, 2);
            break;
          }

          case PropRow::Kind::Color:
          {
            const QRectF swatch(prop.m_ValueRect.left(), prop.m_ValueRect.center().y() - 5.0, 22.0, 10.0);
            pPainter->setPen(QPen(dim, 1.0));
            pPainter->setBrush(QColor(prop.m_Color.r, prop.m_Color.g, prop.m_Color.b));
            pPainter->drawRoundedRect(swatch, 2, 2);
            break;
          }

          case PropRow::Kind::Enum:
          {
            const QRectF valueRect = prop.m_ValueRect.adjusted(0, 0, -10, 0);

            pPainter->setPen(text);
            pPainter->drawText(valueRect, Qt::AlignVCenter | Qt::AlignRight,
              metrics.elidedText(QString::fromUtf8(prop.m_sValue.GetData()), Qt::ElideRight, valueRect.width()));

            // a small caret marks the values that open a menu in place
            QPolygonF caret;
            const QPointF c(prop.m_ValueRect.right() - 4.0, prop.m_ValueRect.center().y());
            caret << QPointF(c.x() - 3.0, c.y() - 1.5) << QPointF(c.x() + 3.0, c.y() - 1.5) << QPointF(c.x(), c.y() + 2.5);
            pPainter->setPen(Qt::NoPen);
            pPainter->setBrush(dim);
            pPainter->drawPolygon(caret);
            break;
          }

          case PropRow::Kind::Number:
          {
            // a faint underline marks the fields that scrub
            pPainter->setPen(QPen(dim, 1.0, Qt::DotLine));
            pPainter->drawLine(prop.m_ValueRect.bottomLeft() + QPointF(2, -1), prop.m_ValueRect.bottomRight() + QPointF(-1, -1));

            pPainter->setPen(text);
            pPainter->drawText(prop.m_ValueRect, Qt::AlignVCenter | Qt::AlignRight, prop.m_sValue.GetData());

            if (prop.m_bHasVariance)
            {
              pPainter->setPen(QPen(dim, 1.0, Qt::DotLine));
              pPainter->drawLine(prop.m_ValueRect2.bottomLeft() + QPointF(2, -1), prop.m_ValueRect2.bottomRight() + QPointF(-1, -1));

              pPainter->setPen(dim);
              pPainter->drawText(prop.m_ValueRect2, Qt::AlignVCenter | Qt::AlignRight, prop.m_sValue2.GetData());
            }
            break;
          }

          case PropRow::Kind::Asset:
          {
            const bool bAssigned = prop.m_sValue != "none";
            pPainter->setPen(bAssigned ? text : dim);
            pPainter->drawText(prop.m_ValueRect.adjusted(0, 0, -12, 0), Qt::AlignVCenter | Qt::AlignRight,
              metrics.elidedText(QString::fromUtf8(prop.m_sValue.GetData()), Qt::ElideRight, prop.m_ValueRect.width() - 12));

            // a small square stands in for the asset browser button
            const QRectF button(prop.m_ValueRect.right() - 9, prop.m_ValueRect.center().y() - 4.5, 9.0, 9.0);
            pPainter->setPen(QPen(dim, 1.0));
            pPainter->setBrush(Qt::NoBrush);
            pPainter->drawRect(button);
            break;
          }

          default:
            pPainter->setPen(text);
            pPainter->drawText(prop.m_ValueRect, Qt::AlignVCenter | Qt::AlignRight,
              metrics.elidedText(QString::fromUtf8(prop.m_sValue.GetData()), Qt::ElideRight, prop.m_ValueRect.width()));
            break;
        }
      }
    }

    // drop indicator while reordering
    if (m_bDragging && m_iDragContext == (plInt32)uiContext && m_iDropIndex >= 0)
    {
      const qreal fY = layout.m_Blocks.IsEmpty()
                         ? layout.m_AddRect.top() - 2
                         : (m_iDropIndex >= (plInt32)layout.m_Blocks.GetCount()
                               ? layout.m_Blocks.PeekBack().m_Rect.bottom() + 1
                               : layout.m_Blocks[m_iDropIndex].m_Rect.top() - 1);

      pPainter->setPen(QPen(palette.highlight().color(), 2.0));
      pPainter->drawLine(QPointF(layout.m_Rect.left() + kAccentW, fY), QPointF(layout.m_Rect.right(), fY));
    }

    // add row
    QPen dashed(dim, 1.0, Qt::DashLine);
    pPainter->setPen(dashed);
    pPainter->setBrush(Qt::NoBrush);
    pPainter->drawRoundedRect(layout.m_AddRect, 3, 3);
    pPainter->setPen(dim);
    pPainter->drawText(layout.m_AddRect, Qt::AlignCenter, QStringLiteral("+ Add block"));
  }
}

//////////////////////////////////////////////////////////////////////////
// hit testing

plInt32 plQtParticleSystemNodeItem::HitTestContextHeader(const QPointF& localPos) const
{
  for (plUInt32 i = 0; i < plParticleContext::Count; ++i)
  {
    if (m_Contexts[i].m_HeaderRect.contains(localPos))
      return (plInt32)i;
  }
  return -1;
}

plInt32 plQtParticleSystemNodeItem::HitTestAddRow(const QPointF& localPos) const
{
  for (plUInt32 i = 0; i < plParticleContext::Count; ++i)
  {
    if (!m_bCollapsed[i] && m_Contexts[i].m_AddRect.contains(localPos))
      return (plInt32)i;
  }
  return -1;
}

const plQtParticleSystemNodeItem::BlockRow* plQtParticleSystemNodeItem::HitTestBlock(
  const QPointF& localPos, plInt32& out_iContext, plInt32& out_iIndex) const
{
  for (plUInt32 i = 0; i < plParticleContext::Count; ++i)
  {
    if (m_bCollapsed[i])
      continue;

    for (plUInt32 uiRow = 0; uiRow < m_Contexts[i].m_Blocks.GetCount(); ++uiRow)
    {
      if (m_Contexts[i].m_Blocks[uiRow].m_Rect.contains(localPos))
      {
        out_iContext = (plInt32)i;
        out_iIndex = (plInt32)uiRow;
        return &m_Contexts[i].m_Blocks[uiRow];
      }
    }
  }

  out_iContext = -1;
  out_iIndex = -1;
  return nullptr;
}

const plQtParticleSystemNodeItem::PropRow* plQtParticleSystemNodeItem::HitTestProp(const QPointF& localPos, const BlockRow*& out_pBlock) const
{
  for (plUInt32 i = 0; i < plParticleContext::Count; ++i)
  {
    if (m_bCollapsed[i])
      continue;

    for (const BlockRow& row : m_Contexts[i].m_Blocks)
    {
      if (!row.m_bExpanded)
        continue;

      for (const PropRow& prop : row.m_Props)
      {
        if (prop.m_Rect.contains(localPos))
        {
          out_pBlock = &row;
          return &prop;
        }
      }
    }
  }

  out_pBlock = nullptr;
  return nullptr;
}

void plQtParticlePinItem::SetPin(const plVisualGraphPin& pin)
{
  plQtVisualGraphPin::SetPin(pin);

  plUuid block;
  plStringBuilder sProperty;
  bool bCompact = plParticlePropertyPin::Parse(pin.GetName(), block, sProperty);

  // Operator pins sit on rows that already name them, so they get the same treatment. Only the
  // flow pins between systems and the effect keep a label.
  if (!bCompact && pin.GetParent() != nullptr)
  {
    plStringView sValueProperty;
    bCompact = plParticleValueKind::FromNode(pin.GetParent()->GetTypeAccessor().GetType(), sValueProperty) != plParticleValueKind::None;
  }

  if (!bCompact)
    return;

  // A pin sits on a short row next to text that already names it, so it gets a small dot and no
  // label. The default size comes from the label's line height, which is far too big here.
  m_pLabel->setPlainText(QString());
  m_bCompact = true;

  constexpr qreal kSize = 7.0;
  const QRectF bounds(0, 0, kSize, kSize);
  m_PinCenter = bounds.center();

  QPainterPath path;
  path.addEllipse(bounds);
  setPath(path);
}

QRectF plQtParticlePinItem::GetPinRect() const
{
  if (m_bCompact)
    return path().boundingRect();

  return plQtVisualGraphPin::GetPinRect();
}

bool plQtParticleSystemNodeItem::FindPinAnchor(const plUuid& block, plStringView sProperty, QPointF& out_pos, bool& out_bRowVisible) const
{
  out_bRowVisible = false;

  for (plUInt32 i = 0; i < plParticleContext::Count; ++i)
  {
    const ContextLayout& layout = m_Contexts[i];

    for (const BlockRow& row : layout.m_Blocks)
    {
      if (row.m_pObject == nullptr || row.m_pObject->GetGuid() != block)
        continue;

      // a closed context has no rows at all, so the whole system folds onto its header
      if (m_bCollapsed[i])
      {
        out_pos = QPointF(layout.m_HeaderRect.left() - 4.0, layout.m_HeaderRect.center().y());
        return true;
      }

      if (row.m_bExpanded)
      {
        for (const PropRow& prop : row.m_Props)
        {
          if (sProperty == prop.m_pProp->GetPropertyName())
          {
            out_bRowVisible = true;
            out_pos = QPointF(prop.m_Rect.left() - 8.0, prop.m_Rect.center().y());
            return true;
          }
        }
      }

      // block closed, or the property is filtered out right now: fold back onto the block row
      out_pos = QPointF(row.m_Rect.left() - 4.0, row.m_Rect.center().y());
      return true;
    }
  }

  return false;
}

plInt32 plQtParticleSystemNodeItem::DropIndexFor(plInt32 iContext, qreal fLocalY) const
{
  const ContextLayout& layout = m_Contexts[iContext];

  for (plUInt32 uiRow = 0; uiRow < layout.m_Blocks.GetCount(); ++uiRow)
  {
    if (fLocalY < layout.m_Blocks[uiRow].m_Rect.center().y())
      return (plInt32)uiRow;
  }

  return (plInt32)layout.m_Blocks.GetCount();
}

//////////////////////////////////////////////////////////////////////////
// interaction

void plQtParticleSystemNodeItem::mousePressEvent(QGraphicsSceneMouseEvent* pEvent)
{
  const QPointF local = pEvent->pos();

  if (pEvent->button() == Qt::LeftButton)
  {
    const plInt32 iHeader = HitTestContextHeader(local);
    if (iHeader >= 0)
    {
      m_bCollapsed[iHeader] = !m_bCollapsed[iHeader];
      UpdateGeometry();
      update();
      pEvent->accept();
      return;
    }

    const plInt32 iAdd = HitTestAddRow(local);
    if (iAdd >= 0)
    {
      ShowAddMenu((plParticleContext::Enum)iAdd, pEvent->screenPos());
      pEvent->accept();
      return;
    }

    // an expanded value row: bools and enums are edited here, everything else defers to the dock
    const BlockRow* pPropBlock = nullptr;
    if (const PropRow* pProp = HitTestProp(local, pPropBlock))
    {
      SelectBlock(pPropBlock->m_pObject);

      switch (pProp->m_Kind)
      {
        case PropRow::Kind::Bool:
          SetPropertyValue(pPropBlock->m_pObject, pProp->m_pProp, !pProp->m_bBoolValue);
          break;

        case PropRow::Kind::Enum:
          ShowEnumMenu(pPropBlock->m_pObject, pProp->m_pProp, pEvent->screenPos());
          break;

        case PropRow::Kind::Color:
          PickColor(pPropBlock->m_pObject, *pProp);
          break;

        case PropRow::Kind::Asset:
          PickAsset(pPropBlock->m_pObject, *pProp);
          break;

        case PropRow::Kind::Number:
        {
          // arm a scrub; a press that never moves opens the type-in dialog on release
          const bool bSecondary = pProp->m_bHasVariance && pProp->m_ValueRect2.contains(local);

          m_pScrubBlock = pPropBlock->m_pObject;
          m_ScrubProp = *pProp;
          m_bScrubSecondary = bSecondary;
          m_fScrubStartValue = bSecondary ? pProp->m_fVariance : pProp->m_fNumber;
          m_ScrubStartPos = local;
          m_bScrubbing = false;
          break;
        }

        default:
          break;
      }

      pEvent->accept();
      return;
    }

    plInt32 iContext = -1, iIndex = -1;
    if (const BlockRow* pRow = HitTestBlock(local, iContext, iIndex))
    {
      SelectBlock(pRow->m_pObject);

      if (pRow->m_EnableRect.adjusted(-3, -3, 3, 3).contains(local))
      {
        if (const plAbstractProperty* pEnabled = pRow->m_pObject->GetTypeAccessor().GetType()->FindPropertyByName("Enabled"))
          SetPropertyValue(pRow->m_pObject, pEnabled, !pRow->m_bEnabled);

        pEvent->accept();
        return;
      }

      // arm a drag; the node itself must not move while a block is being dragged. A press that
      // never turns into a drag toggles the block open, so the whole row is the disclosure header.
      m_pDragBlock = pRow->m_pObject;
      m_iDragContext = iContext;
      m_iDropIndex = -1;
      m_DragStart = local;
      m_bDragging = false;

      pEvent->accept();
      return;
    }
  }

  SUPER::mousePressEvent(pEvent);
}

void plQtParticleSystemNodeItem::mouseMoveEvent(QGraphicsSceneMouseEvent* pEvent)
{
  if (m_pScrubBlock != nullptr)
  {
    const qreal fDeltaX = pEvent->pos().x() - m_ScrubStartPos.x();

    if (!m_bScrubbing && plMath::Abs(fDeltaX) < kDragThreshold)
    {
      pEvent->accept();
      return;
    }

    if (!m_bScrubbing)
    {
      m_bScrubbing = true;

      // one transaction for the whole gesture, so a drag is a single undo step
      m_pOwnManager->GetDocument()->GetCommandHistory()->StartTransaction("Scrub Value");
      m_bScrubTransaction = true;
    }

    const PropRow& prop = m_ScrubProp;

    // step scales with magnitude so a rate of 200 and a size of 0.5 both feel right
    double fStep = prop.m_bIsInteger ? 0.25 : 0.01;
    if (!prop.m_bIsInteger)
    {
      const double fMagnitude = plMath::Abs(m_fScrubStartValue);
      if (fMagnitude > 100.0)
        fStep = 1.0;
      else if (fMagnitude > 10.0)
        fStep = 0.1;
    }

    double fNew = m_fScrubStartValue + fDeltaX * fStep;

    if (prop.m_bIsInteger)
      fNew = plMath::Round(fNew);

    if (prop.m_bHasClamp)
      fNew = plMath::Clamp(fNew, prop.m_fMin, prop.m_fMax);
    else if (m_bScrubSecondary)
      fNew = plMath::Max(fNew, 0.0); // a deviation is never negative

    const double fPrimary = m_bScrubSecondary ? prop.m_fNumber : fNew;
    const double fVariance = m_bScrubSecondary ? fNew : prop.m_fVariance;

    SetPropertyValue(m_pScrubBlock, prop.m_pProp, MakeNumberVariant(prop, fPrimary, fVariance));

    pEvent->accept();
    return;
  }

  if (m_pDragBlock != nullptr)
  {
    const QPointF local = pEvent->pos();

    if (!m_bDragging && (local - m_DragStart).manhattanLength() >= kDragThreshold)
      m_bDragging = true;

    if (m_bDragging)
    {
      m_iDropIndex = DropIndexFor(m_iDragContext, local.y());
      update();
    }

    pEvent->accept();
    return;
  }

  SUPER::mouseMoveEvent(pEvent);
}

void plQtParticleSystemNodeItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* pEvent)
{
  if (m_pScrubBlock != nullptr)
  {
    if (m_bScrubTransaction)
    {
      m_pOwnManager->GetDocument()->GetCommandHistory()->FinishTransaction();
      m_bScrubTransaction = false;
    }
    else
    {
      // never moved: treat it as a request to type the value
      TypeNumber(m_pScrubBlock, m_ScrubProp, m_bScrubSecondary);
    }

    m_pScrubBlock = nullptr;
    m_bScrubbing = false;

    pEvent->accept();
    return;
  }

  if (m_pDragBlock != nullptr)
  {
    if (m_bDragging && m_iDropIndex >= 0)
    {
      ReorderBlock(m_pDragBlock, (plParticleContext::Enum)m_iDragContext, m_iDropIndex);
    }
    else if (!m_bDragging)
    {
      const plUuid guid = m_pDragBlock->GetGuid();

      if (m_ExpandedBlocks.Contains(guid))
        m_ExpandedBlocks.Remove(guid);
      else
        m_ExpandedBlocks.Insert(guid);

      RefreshFromDocument();
    }

    m_pDragBlock = nullptr;
    m_iDragContext = -1;
    m_iDropIndex = -1;
    m_bDragging = false;
    update();

    pEvent->accept();
    return;
  }

  SUPER::mouseReleaseEvent(pEvent);
}

void plQtParticleSystemNodeItem::hoverMoveEvent(QGraphicsSceneHoverEvent* pEvent)
{
  plInt32 iContext = -1, iIndex = -1;
  const BlockRow* pRow = HitTestBlock(pEvent->pos(), iContext, iIndex);
  const plDocumentObject* pHover = pRow != nullptr ? pRow->m_pObject : nullptr;

  if (pHover != m_pHoverBlock)
  {
    m_pHoverBlock = pHover;
    update();
  }

  SUPER::hoverMoveEvent(pEvent);
}

void plQtParticleSystemNodeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* pEvent)
{
  if (m_pHoverBlock != nullptr)
  {
    m_pHoverBlock = nullptr;
    update();
  }

  SUPER::hoverLeaveEvent(pEvent);
}

bool plQtParticleSystemNodeItem::ShowBlockContextMenu(const QPointF& localPos, const QPoint& screenPos)
{
  plInt32 iContext = -1, iIndex = -1;
  const BlockRow* pRow = HitTestBlock(localPos, iContext, iIndex);

  if (pRow == nullptr)
    return false;

  const plDocumentObject* pBlock = pRow->m_pObject;
  const auto context = (plParticleContext::Enum)iContext;
  const plInt32 iCount = (plInt32)m_Contexts[iContext].m_Blocks.GetCount();

  SelectBlock(pBlock);

  plStringBuilder sRemove;
  sRemove.SetFormat("Remove '{0}'", pRow->m_sTitle);

  QMenu menu;
  QAction* pToggle = menu.addAction(pRow->m_bEnabled ? QStringLiteral("Disable") : QStringLiteral("Enable"));
  menu.addSeparator();
  QAction* pUp = menu.addAction(QStringLiteral("Move Up"));
  pUp->setEnabled(iIndex > 0);
  QAction* pDown = menu.addAction(QStringLiteral("Move Down"));
  pDown->setEnabled(iIndex + 1 < iCount);
  menu.addSeparator();
  QAction* pRemove = menu.addAction(QString::fromUtf8(sRemove.GetData()));

  const bool bWasEnabled = pRow->m_bEnabled;
  QAction* pChosen = menu.exec(screenPos);

  // Careful past this point: anything that edits the document rebuilds the rows, so pRow dangles.
  if (pChosen == pUp)
    ReorderBlock(pBlock, context, iIndex - 1);
  else if (pChosen == pDown)
    ReorderBlock(pBlock, context, iIndex + 2); // array move is relative to the pre-removal list
  else if (pChosen == pRemove)
    RemoveBlock(pBlock);
  else if (pChosen == pToggle)
  {
    if (const plAbstractProperty* pEnabled = pBlock->GetTypeAccessor().GetType()->FindPropertyByName("Enabled"))
      SetPropertyValue(pBlock, pEnabled, !bWasEnabled);
  }

  return true;
}

void plQtParticleSystemNodeItem::ExtendContextMenu(QMenu& ref_menu)
{
  const plDocumentObject* pSystem = GetObject();

  QAction* pSave = ref_menu.addAction(QStringLiteral("Save as Template..."));

  QObject::connect(pSave, &QAction::triggered, [this, pSystem]()
    {
      auto* pDocument = static_cast<plParticleEffectAssetDocument*>(m_pOwnManager->GetDocument());

      const plString sSystemName = pSystem->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();

      bool bOk = false;
      const QString sName = QInputDialog::getText(nullptr, QStringLiteral("Save System as Template"),
        QStringLiteral("Template name:"), QLineEdit::Normal, QString::fromUtf8(sSystemName.GetData()), &bOk);

      if (!bOk || sName.isEmpty())
        return;

      const plString sTemplateName = sName.toUtf8().data();
      const plStatus status = pDocument->SaveSystemAsTemplate(pSystem, sTemplateName);

      if (status.Failed())
      {
        plQtUiServices::MessageBoxStatus(status, "Saving the template failed.");
      }
      else
      {
        plQtUiServices::ShowAllDocumentsTemporaryStatusBarMessage(
          plFmt("Saved system '{}' as template '{}'.", sSystemName, sTemplateName), plTime::MakeFromSeconds(5));
      }
    });
}

void plQtParticleSystemNodeItem::SetPropertyValue(const plDocumentObject* pBlock, const plAbstractProperty* pProp, const plVariant& value)
{
  plCommandHistory* pHistory = m_pOwnManager->GetDocument()->GetCommandHistory();
  pHistory->StartTransaction("Change Value");

  plSetObjectPropertyCommand cmd;
  cmd.m_Object = pBlock->GetGuid();
  cmd.m_sProperty = pProp->GetPropertyName();
  cmd.m_NewValue = value;

  if (pHistory->AddCommand(cmd).Failed())
  {
    pHistory->CancelTransaction();
    return;
  }

  pHistory->FinishTransaction();
}

void plQtParticleSystemNodeItem::ShowEnumMenu(const plDocumentObject* pBlock, const plAbstractProperty* pProp, const QPoint& screenPos)
{
  plDynamicArray<plReflectionUtils::EnumKeyValuePair> entries;
  plReflectionUtils::GetEnumKeysAndValues(pProp->GetSpecificType(), entries, plReflectionUtils::EnumConversionMode::ValueNameOnly);

  if (entries.IsEmpty())
    return;

  const plInt64 iCurrent = pBlock->GetTypeAccessor().GetValue(pProp->GetPropertyName()).ConvertTo<plInt64>();

  QMenu menu;
  for (const auto& entry : entries)
  {
    // via a builder: a translated view is not guaranteed to be null terminated
    const plStringBuilder sLabel = plTranslate(entry.m_sKey);

    QAction* pAction = menu.addAction(QString::fromUtf8(sLabel.GetData()));
    pAction->setCheckable(true);
    pAction->setChecked(entry.m_iValue == iCurrent);
    pAction->setData(QVariant::fromValue<qlonglong>(entry.m_iValue));
  }

  QAction* pChosen = menu.exec(screenPos);
  if (pChosen == nullptr)
    return;

  SetPropertyValue(pBlock, pProp, plVariant((plInt64)pChosen->data().value<qlonglong>()));
}

QWidget* plQtParticleSystemNodeItem::GetDialogParent() const
{
  if (scene() != nullptr && !scene()->views().isEmpty())
    return scene()->views().first();

  return nullptr;
}

plVariant plQtParticleSystemNodeItem::MakeNumberVariant(const PropRow& prop, double fPrimary, double fVariance) const
{
  if (prop.m_Original.IsA<plVarianceTypeFloat>())
  {
    plVarianceTypeFloat v;
    v.m_Value = (float)fPrimary;
    v.m_fVariance = (float)fVariance;
    return plVariant(v);
  }

  if (prop.m_Original.IsA<plVarianceTypeTime>())
  {
    plVarianceTypeTime v;
    v.m_Value = plTime::MakeFromSeconds(fPrimary);
    v.m_fVariance = (float)fVariance;
    return plVariant(v);
  }

  if (prop.m_Original.IsA<plVarianceTypeAngle>())
  {
    plVarianceTypeAngle v;
    v.m_Value = plAngle::MakeFromDegree((float)fPrimary);
    v.m_fVariance = (float)fVariance;
    return plVariant(v);
  }

  if (prop.m_Original.IsA<plTime>())
    return plVariant(plTime::MakeFromSeconds(fPrimary));

  if (prop.m_Original.IsA<plAngle>())
    return plVariant(plAngle::MakeFromDegree((float)fPrimary));

  // plain numbers keep their exact storage type, otherwise the command is rejected
  return plVariant(fPrimary).ConvertTo(prop.m_Original.GetType());
}

void plQtParticleSystemNodeItem::TypeNumber(const plDocumentObject* pBlock, const PropRow& prop, bool bSecondary)
{
  const double fCurrent = bSecondary ? prop.m_fVariance : prop.m_fNumber;

  const double fMin = prop.m_bHasClamp ? prop.m_fMin : -1000000.0;
  const double fMax = prop.m_bHasClamp ? prop.m_fMax : 1000000.0;

  plStringBuilder sLabel = prop.m_sLabel;
  if (bSecondary)
    sLabel.Append(" (deviation)");

  bool bOk = false;
  const double fNew = QInputDialog::getDouble(GetDialogParent(), QStringLiteral("Set Value"), QString::fromUtf8(sLabel.GetData()),
    fCurrent, bSecondary ? 0.0 : fMin, fMax, prop.m_bIsInteger ? 0 : 3, &bOk);

  if (!bOk)
    return;

  const double fPrimary = bSecondary ? prop.m_fNumber : fNew;
  const double fVariance = bSecondary ? fNew : prop.m_fVariance;

  SetPropertyValue(pBlock, prop.m_pProp, MakeNumberVariant(prop, fPrimary, fVariance));
}

void plQtParticleSystemNodeItem::PickColor(const plDocumentObject* pBlock, const PropRow& prop)
{
  const bool bAlpha = prop.m_pProp->GetAttributeByType<plExposeColorAlphaAttribute>() != nullptr;
  const plColor initial = prop.m_Original.IsA<plColor>() ? prop.m_Original.Get<plColor>() : plColor(prop.m_Color);

  plQtColorDialog dlg(initial, GetDialogParent());
  dlg.ShowAlpha(bAlpha);
  dlg.ShowHDR(prop.m_Original.IsA<plColor>());

  plColor picked = initial;
  QObject::connect(&dlg, &plQtColorDialog::CurrentColorChanged, [&picked](const plColor& c)
    { picked = c; });

  if (dlg.exec() != QDialog::Accepted)
    return;

  SetPropertyValue(pBlock, prop.m_pProp, prop.m_Original.IsA<plColor>() ? plVariant(picked) : plVariant(plColorGammaUB(picked)));
}

void plQtParticleSystemNodeItem::PickAsset(const plDocumentObject* pBlock, const PropRow& prop)
{
  const plAssetBrowserAttribute* pAttr = prop.m_pProp->GetAttributeByType<plAssetBrowserAttribute>();
  if (pAttr == nullptr)
    return;

  const plString sCurrent = prop.m_Original.ConvertTo<plString>();
  const plUuid current = plConversionUtils::ConvertStringToUuid(sCurrent);

  plQtAssetBrowserDlg dlg(GetDialogParent(), current, pAttr->GetTypeFilter());
  if (dlg.exec() == 0)
    return;

  plStringBuilder sResult;
  const plUuid picked = dlg.GetSelectedAssetGuid();

  if (picked.IsValid())
    plConversionUtils::ToString(picked, sResult);
  else
    sResult = dlg.GetSelectedAssetPathRelative();

  SetPropertyValue(pBlock, prop.m_pProp, plVariant(plString(sResult)));
}

void plQtParticleSystemNodeItem::ShowAddMenu(plParticleContext::Enum context, const QPoint& screenPos)
{
  plDynamicArray<const plRTTI*> types;
  plParticleEffectNodeManager::GetBlockTypes(context, types);

  QMenu menu;
  menu.setTitle(QString::fromUtf8(plParticleContextInfo::Get(context).m_szLabel));

  plMap<QString, const plRTTI*> byName;
  for (const plRTTI* pType : types)
    byName[QString::fromUtf8(BlockDisplayName(pType).GetData())] = pType;

  for (auto it : byName)
  {
    QAction* pAction = menu.addAction(it.Key());
    pAction->setData(QVariant::fromValue<void*>((void*)it.Value()));
  }

  if (menu.isEmpty())
    return;

  QAction* pChosen = menu.exec(screenPos);
  if (pChosen == nullptr)
    return;

  const plRTTI* pType = static_cast<const plRTTI*>(pChosen->data().value<void*>());

  plCommandHistory* pHistory = m_pOwnManager->GetDocument()->GetCommandHistory();
  pHistory->StartTransaction("Add Block");

  plAddObjectCommand cmd;
  cmd.m_pType = pType;
  cmd.m_Parent = GetObject()->GetGuid();
  cmd.m_sParentProperty = plParticleContextInfo::Get(context).m_szProperty;
  cmd.m_Index = (plInt32)m_Contexts[context].m_Blocks.GetCount();

  if (pHistory->AddCommand(cmd).Failed())
  {
    pHistory->CancelTransaction();
    return;
  }

  pHistory->FinishTransaction();
}

void plQtParticleSystemNodeItem::RemoveBlock(const plDocumentObject* pBlock)
{
  plCommandHistory* pHistory = m_pOwnManager->GetDocument()->GetCommandHistory();
  pHistory->StartTransaction("Remove Block");

  plRemoveObjectCommand cmd;
  cmd.m_Object = pBlock->GetGuid();

  if (pHistory->AddCommand(cmd).Failed())
  {
    pHistory->CancelTransaction();
    return;
  }

  pHistory->FinishTransaction();
}

void plQtParticleSystemNodeItem::ReorderBlock(const plDocumentObject* pBlock, plParticleContext::Enum context, plInt32 iNewIndex)
{
  const plInt32 iOldIndex = pBlock->GetPropertyIndex().ConvertTo<plInt32>();

  // plMoveObjectCommand indexes into the list as it is before the move, so moving down by one
  // means targeting the slot after the next element.
  if (iNewIndex == iOldIndex || iNewIndex == iOldIndex + 1)
    return;

  plCommandHistory* pHistory = m_pOwnManager->GetDocument()->GetCommandHistory();
  pHistory->StartTransaction("Reorder Block");

  plMoveObjectCommand cmd;
  cmd.m_Object = pBlock->GetGuid();
  cmd.m_NewParent = GetObject()->GetGuid();
  cmd.m_sParentProperty = plParticleContextInfo::Get(context).m_szProperty;
  cmd.m_Index = iNewIndex;

  if (pHistory->AddCommand(cmd).Failed())
  {
    pHistory->CancelTransaction();
    return;
  }

  pHistory->FinishTransaction();
}

void plQtParticleSystemNodeItem::SelectBlock(const plDocumentObject* pBlock)
{
  m_pOwnManager->GetDocument()->GetSelectionManager()->SetSelection(pBlock);
}

//////////////////////////////////////////////////////////////////////////
// document change tracking

void plQtParticleSystemNodeItem::RefreshFromDocument()
{
  // UpdateState re-reads the header text: without it the name and simulation-target badge keep
  // whatever they were built with, because UpdateGeometry only lays the existing labels out.
  UpdateState();
  UpdateGeometry();
  update();
}

void plQtParticleSystemNodeItem::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  switch (e.m_EventType)
  {
    case plDocumentObjectStructureEvent::Type::AfterObjectAdded:
    case plDocumentObjectStructureEvent::Type::AfterObjectRemoved:
    case plDocumentObjectStructureEvent::Type::AfterObjectMoved2:
      if (e.m_pPreviousParent == GetObject() || e.m_pNewParent == GetObject())
        RefreshFromDocument();
      break;

    default:
      break;
  }
}

void plQtParticleSystemNodeItem::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  if (e.m_pObject == nullptr)
    return;

  // a block's own values feed its summary line, and the system's name feeds the header
  if (e.m_pObject == GetObject() || e.m_pObject->GetParent() == GetObject())
    RefreshFromDocument();
}

void plQtParticleSystemNodeItem::SelectionEventHandler(const plSelectionManagerEvent& e)
{
  PL_IGNORE_UNUSED(e);
  update();
}
