#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectGraphScene.moc.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleSystemNodeItem.h>
#include <GuiFoundation/UIServices/ImageCache.moc.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Selection/SelectionManager.h>

#include <QGraphicsSceneContextMenuEvent>
#include <QKeyEvent>

plQtParticleEffectGraphScene::plQtParticleEffectGraphScene(QObject* pParent)
  : plQtVisualGraphScene(pParent)
{
  // Systems stack vertically and feed straight down into the effect, so a routed or curved wire
  // only adds noise here.
  SetConnectionStyle(plQtVisualGraphScene::ConnectionStyle::StraightLine);

  // Asset thumbnails load asynchronously: without this the preview nodes keep drawing the loading
  // placeholder until something else happens to repaint them.
  connect(plQtImageCache::GetSingleton(), &plQtImageCache::ImageLoaded, this,
    [this](QString, QModelIndex, QVariant, QVariant)
    { update(); });

  connect(plQtImageCache::GetSingleton(), &plQtImageCache::ImageInvalidated, this,
    [this](QString, plUInt32)
    { update(); });
}

plQtParticleEffectGraphScene::~plQtParticleEffectGraphScene() = default;

plQtParticleSystemNodeItem* plQtParticleEffectGraphScene::FindSystemItemAt(const QPointF& scenePos) const
{
  QTransform identity;
  QGraphicsItem* pItem = itemAt(scenePos, identity);

  // the hit may land on a child item (a pin), so walk up to the node
  while (pItem != nullptr && pItem->type() != plQtVisualGraphScene::Node)
    pItem = pItem->parentItem();

  return dynamic_cast<plQtParticleSystemNodeItem*>(pItem);
}

void plQtParticleEffectGraphScene::contextMenuEvent(QGraphicsSceneContextMenuEvent* pEvent)
{
  if (plQtParticleSystemNodeItem* pSystem = FindSystemItemAt(pEvent->scenePos()))
  {
    if (pSystem->ShowBlockContextMenu(pSystem->mapFromScene(pEvent->scenePos()), pEvent->screenPos()))
    {
      pEvent->accept();
      return;
    }
  }

  plQtVisualGraphScene::contextMenuEvent(pEvent);
}

void plQtParticleEffectGraphScene::keyPressEvent(QKeyEvent* pEvent)
{
  // Delete on a selected block removes that block; without this it would fall through to the
  // scene's node deletion and take the whole system with it.
  if (pEvent->key() == Qt::Key_Delete && GetDocument() != nullptr)
  {
    const auto& selection = GetDocument()->GetSelectionManager()->GetSelection();

    if (selection.GetCount() == 1 && selection[0]->GetParent() != nullptr &&
        selection[0]->GetParent()->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
    {
      plCommandHistory* pHistory = GetDocument()->GetCommandHistory();
      pHistory->StartTransaction("Remove Block");

      plRemoveObjectCommand cmd;
      cmd.m_Object = selection[0]->GetGuid();

      if (pHistory->AddCommand(cmd).Failed())
        pHistory->CancelTransaction();
      else
        pHistory->FinishTransaction();

      pEvent->accept();
      return;
    }
  }

  plQtVisualGraphScene::keyPressEvent(pEvent);
}
