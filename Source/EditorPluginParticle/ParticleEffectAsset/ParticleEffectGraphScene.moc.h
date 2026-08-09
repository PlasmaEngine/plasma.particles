#pragma once

#include <Foundation/Basics.h>
#include <GuiFoundation/VisualGraph/Scene.moc.h>

class plQtParticleEffectGraphScene : public plQtVisualGraphScene
{
  Q_OBJECT

public:
  plQtParticleEffectGraphScene(QObject* pParent = nullptr);
  ~plQtParticleEffectGraphScene();

protected:
  // The base scene builds its menu from itemAt and never forwards to the item under the cursor,
  // so a right-click on a block would otherwise hit the node-level Remove and delete the system.
  virtual void contextMenuEvent(QGraphicsSceneContextMenuEvent* pEvent) override;
  virtual void keyPressEvent(QKeyEvent* pEvent) override;

private:
  class plQtParticleSystemNodeItem* FindSystemItemAt(const QPointF& scenePos) const;
};