#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>
#include <GuiFoundation/VisualGraph/Node.h>

/// Graph item for the effect node: a wide, shallow bar that systems flow down into.
///
/// The default node item puts its inputs down the left edge, which reads as a left-to-right graph.
/// Particles flow downwards through a system and then into the effect, so the Systems pin sits on
/// the top edge instead. The retired EventReactions pin is hidden — reactions are an owned array
/// on the effect node and are edited through the properties dock.
class plQtParticleEffectNodeItem : public plQtVisualGraphNode
{
public:
  using SUPER = plQtVisualGraphNode;

  plQtParticleEffectNodeItem();

  virtual void InitNode(const plVisualGraphObjectManager* pManager, const plDocumentObject* pObject) override;
  virtual void UpdateGeometry() override;
  virtual void UpdateState() override;

protected:
  virtual void paint(QPainter* pPainter, const QStyleOptionGraphicsItem* pOption, QWidget* pWidget) override;

private:
  qreal m_fWidth = 268.0;
  qreal m_fHeight = 0.0;
};
