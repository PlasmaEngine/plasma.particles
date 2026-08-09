#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <GuiFoundation/VisualGraph/Node.h>
#include <GuiFoundation/VisualGraph/Pin.h>

/// Pin item that keeps property pins unlabelled.
///
/// A property pin's name encodes the owning block's guid, which is exactly what the default label
/// would draw. The block row already names the property, so the pin only needs its dot.
class plQtParticlePinItem : public plQtVisualGraphPin
{
public:
  virtual void SetPin(const plVisualGraphPin& pin) override;
  virtual QRectF GetPinRect() const override;

private:
  bool m_bCompact = false;
};

struct plDocumentObjectStructureEvent;
struct plDocumentObjectPropertyEvent;
struct plSelectionManagerEvent;

/// Graph item that draws a particle system as a vertical stack of contexts.
///
/// Particles flow downwards: Spawn feeds Initialize, Initialize feeds Update, Update feeds Output.
/// Each context lists the blocks it owns, in the order the runtime runs them. Blocks are child
/// objects of the system in one of its four array properties, so selecting a block routes the
/// properties dock at it, and reordering one is a plain array move.
class plQtParticleSystemNodeItem : public plQtVisualGraphNode
{
public:
  using SUPER = plQtVisualGraphNode;

  plQtParticleSystemNodeItem();
  ~plQtParticleSystemNodeItem();

  virtual void InitNode(const plVisualGraphObjectManager* pManager, const plDocumentObject* pObject) override;
  virtual void UpdateGeometry() override;
  virtual void UpdateState() override;

  /// Shows the block menu when localPos is over a block row. Returns false if it is not, so the
  /// caller can fall back to the scene's node menu.
  ///
  /// The scene handles context menus itself via itemAt and never dispatches them to items, so this
  /// has to be driven from plQtParticleEffectGraphScene rather than from an item event handler.
  bool ShowBlockContextMenu(const QPointF& localPos, const QPoint& screenPos);

  /// Adds "Save as Template..." to the scene's node menu.
  virtual void ExtendContextMenu(QMenu& ref_menu) override;

protected:
  virtual void paint(QPainter* pPainter, const QStyleOptionGraphicsItem* pOption, QWidget* pWidget) override;
  virtual void mousePressEvent(QGraphicsSceneMouseEvent* pEvent) override;
  virtual void mouseMoveEvent(QGraphicsSceneMouseEvent* pEvent) override;
  virtual void mouseReleaseEvent(QGraphicsSceneMouseEvent* pEvent) override;
  virtual void hoverMoveEvent(QGraphicsSceneHoverEvent* pEvent) override;
  virtual void hoverLeaveEvent(QGraphicsSceneHoverEvent* pEvent) override;

private:
  /// One editable value inside an expanded block.
  ///
  /// Bools and enums are edited in place because a single click is enough for them. Everything
  /// else shows its value and hands off to the properties dock, which already has the right editor.
  struct PropRow
  {
    enum class Kind : plUInt8
    {
      Text,
      Bool,
      Enum,
      Color,
      Number, ///< float / int / time, optionally with a variance field beside it
      Asset
    };

    const plAbstractProperty* m_pProp = nullptr;
    plString m_sLabel;
    plString m_sValue;  ///< primary field, already formatted
    plString m_sValue2; ///< variance field
    plVariant m_Original;
    QRectF m_Rect;
    QRectF m_ValueRect;
    QRectF m_ValueRect2;
    Kind m_Kind = Kind::Text;
    bool m_bBoolValue = false;
    plColorGammaUB m_Color;

    double m_fNumber = 0.0;
    double m_fVariance = 0.0;
    bool m_bHasVariance = false;
    bool m_bIsInteger = false;
    bool m_bHasClamp = false;
    double m_fMin = 0.0;
    double m_fMax = 0.0;
  };

  struct BlockRow
  {
    const plDocumentObject* m_pObject = nullptr;
    QRectF m_Rect;     ///< the title row
    QRectF m_FullRect; ///< title row plus any expanded property rows
    QRectF m_EnableRect;
    plString m_sTitle;
    plString m_sSummary;
    bool m_bExpanded = false;
    bool m_bEnabled = true;
    plHybridArray<PropRow, 12> m_Props;
  };

  struct ContextLayout
  {
    QRectF m_HeaderRect;
    QRectF m_AddRect;
    QRectF m_Rect; ///< header plus body
    plHybridArray<BlockRow, 8> m_Blocks;
  };

  void RebuildRows();
  void CollectProps(BlockRow& ref_row) const;
  void RefreshFromDocument();

  plInt32 HitTestContextHeader(const QPointF& localPos) const;
  plInt32 HitTestAddRow(const QPointF& localPos) const;
  const BlockRow* HitTestBlock(const QPointF& localPos, plInt32& out_iContext, plInt32& out_iIndex) const;
  const PropRow* HitTestProp(const QPointF& localPos, const BlockRow*& out_pBlock) const;
  plInt32 DropIndexFor(plInt32 iContext, qreal fLocalY) const;

  /// Where a property pin should sit: on its own row when the block is open, otherwise folded back
  /// to the block row or the context header. Returns false when the property no longer exists.
  bool FindPinAnchor(const plUuid& block, plStringView sProperty, QPointF& out_pos, bool& out_bRowVisible) const;

  void SetPropertyValue(const plDocumentObject* pBlock, const plAbstractProperty* pProp, const plVariant& value);
  void ShowEnumMenu(const plDocumentObject* pBlock, const plAbstractProperty* pProp, const QPoint& screenPos);
  void PickColor(const plDocumentObject* pBlock, const PropRow& prop);
  void PickAsset(const plDocumentObject* pBlock, const PropRow& prop);
  void TypeNumber(const plDocumentObject* pBlock, const PropRow& prop, bool bSecondary);

  /// Rebuilds the property's value from a scrubbed or typed number, keeping the original type.
  plVariant MakeNumberVariant(const PropRow& prop, double fPrimary, double fVariance) const;
  QWidget* GetDialogParent() const;
  void ShowAddMenu(plParticleContext::Enum context, const QPoint& screenPos);
  void RemoveBlock(const plDocumentObject* pBlock);
  void ReorderBlock(const plDocumentObject* pBlock, plParticleContext::Enum context, plInt32 iNewIndex);
  void SelectBlock(const plDocumentObject* pBlock);

  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void SelectionEventHandler(const plSelectionManagerEvent& e);

  plVisualGraphObjectManager* m_pOwnManager = nullptr;
  bool m_bSubscribed = false;

  ContextLayout m_Contexts[plParticleContext::Count];
  bool m_bCollapsed[plParticleContext::Count] = {};
  plSet<plUuid> m_ExpandedBlocks; ///< survives the row rebuild that every document change triggers

  // GPU lowering verdict, recomputed with the rows
  plString m_sLoweringReason;
  const plDocumentObject* m_pLoweringBlocker = nullptr;
  bool m_bTargetsGpu = false;
  QRectF m_VerdictRect;
  qreal m_fWidth = 268.0;
  qreal m_fHeight = 0.0;

  const plDocumentObject* m_pHoverBlock = nullptr;

  // In-progress number scrub. The row is held by value: writing the property rebuilds every row,
  // so a pointer into m_Contexts would dangle after the first step of the drag.
  const plDocumentObject* m_pScrubBlock = nullptr;
  PropRow m_ScrubProp;
  bool m_bScrubSecondary = false;
  bool m_bScrubbing = false;
  bool m_bScrubTransaction = false;
  double m_fScrubStartValue = 0.0;
  QPointF m_ScrubStartPos;

  // in-progress block drag
  const plDocumentObject* m_pDragBlock = nullptr;
  plInt32 m_iDragContext = -1;
  plInt32 m_iDropIndex = -1;
  QPointF m_DragStart;
  bool m_bDragging = false;
};
