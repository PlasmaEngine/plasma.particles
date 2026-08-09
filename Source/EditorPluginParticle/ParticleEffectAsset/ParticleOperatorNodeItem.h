#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>
#include <GuiFoundation/VisualGraph/Node.h>

/// Graph item for the operator nodes, with their values editable in place.
///
/// Operator nodes are small — one to three values each — so instead of a title and a row of pins
/// they draw their properties as rows and edit them directly, the same way a block does. An input
/// pin sits on the row of the literal it overrides, which is what makes "wired or typed" readable
/// at a glance.
class plQtParticleOperatorNodeItem : public plQtVisualGraphNode
{
public:
  using SUPER = plQtVisualGraphNode;

  plQtParticleOperatorNodeItem();

  virtual void InitNode(const plVisualGraphObjectManager* pManager, const plDocumentObject* pObject) override;
  virtual void UpdateGeometry() override;
  virtual void UpdateState() override;

protected:
  virtual void paint(QPainter* pPainter, const QStyleOptionGraphicsItem* pOption, QWidget* pWidget) override;
  virtual void mousePressEvent(QGraphicsSceneMouseEvent* pEvent) override;
  virtual void mouseMoveEvent(QGraphicsSceneMouseEvent* pEvent) override;
  virtual void mouseReleaseEvent(QGraphicsSceneMouseEvent* pEvent) override;

private:
  struct Row
  {
    enum class Kind : plUInt8
    {
      Number,
      Enum,
      Color,
      Asset,
      Text,
      PinOnly, ///< an input with no literal behind it, such as a Branch condition
    };

    const plAbstractProperty* m_pProp = nullptr;
    plString m_sLabel;
    plString m_sValue;
    plString m_sPin; ///< input pin sitting on this row, if any
    plVariant m_Original;
    QRectF m_Rect;
    QRectF m_ValueRect;
    Kind m_Kind = Kind::Number;
    double m_fNumber = 0.0;
    plColorGammaUB m_Color;
    bool m_bDriven = false; ///< the pin on this row is connected, so the literal is ignored
  };

  void RebuildRows();

  /// Whether the pin on this row is currently wired. Queried live rather than cached, so a
  /// connection change never has to touch the layout.
  bool IsRowDriven(const Row& row) const;

  const Row* HitTestRow(const QPointF& localPos) const;
  void Refresh();

  void SetValue(const plAbstractProperty* pProp, const plVariant& value);
  void ShowEnumMenu(const Row& row, const QPoint& screenPos);
  void PickColor(const Row& row);
  void PickAsset(const Row& row);
  void TypeValue(const Row& row);

  /// True when a value on this node differs from what the rows were built with.
  bool HasChanged() const;

  plVisualGraphObjectManager* m_pOwnManager = nullptr;

  /// Texture, colour, gradient and curve nodes drop the label/value rows and show the value itself
  /// filling the node body.
  bool BuildPreview();

  /// Re-asks the image cache for the preview thumbnail. Returns true when one arrived that we did
  /// not have before, which is the point at which the node has to resize to its aspect ratio.
  bool QueryPreviewPixmap();

  plHybridArray<Row, 6> m_Rows;
  const plAbstractProperty* m_pPreviewProp = nullptr;
  QRectF m_PreviewRect;
  QPixmap m_Preview;
  plUuid m_PreviewAsset;
  plVariant m_PreviewOriginal;
  plColorGammaUB m_PreviewColor;
  bool m_bPreviewIsColor = false;
  QColor m_BadgeColor;
  qreal m_fWidth = 176.0;
  qreal m_fHeight = 0.0;

  const plAbstractProperty* m_pScrubProp = nullptr;
  Row m_ScrubRow;
  bool m_bScrubbing = false;
  bool m_bScrubTransaction = false;
  double m_fScrubStartValue = 0.0;
  QPointF m_ScrubStartPos;
};
