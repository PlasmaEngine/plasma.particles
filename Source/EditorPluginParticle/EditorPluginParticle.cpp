#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/Actions/AssetActions.h>
#include <EditorFramework/Actions/ProjectActions.h>
#include <EditorFramework/Actions/ViewActions.h>
#include <EditorFramework/Actions/ViewLightActions.h>
#include <EditorPluginParticle/Actions/ParticleActions.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <GuiFoundation/Action/CommandHistoryActions.h>
#include <GuiFoundation/Action/DocumentActions.h>
#include <GuiFoundation/Action/StandardMenus.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <GuiFoundation/VisualGraph/Pin.h>
#include <GuiFoundation/VisualGraph/Scene.moc.h>

void OnLoadPlugin()
{
  plParticleActions::RegisterActions();

  // Particle Effect
  {
    // Menu Bar
    {
      plActionMapManager::RegisterActionMap("ParticleEffectAssetMenuBar").IgnoreResult();
      plStandardMenus::MapActions("ParticleEffectAssetMenuBar", plStandardMenuTypes::Default | plStandardMenuTypes::Edit);
      plProjectActions::MapActions("ParticleEffectAssetMenuBar");
      plDocumentActions::MapMenuActions("ParticleEffectAssetMenuBar");
      plAssetActions::MapMenuActions("ParticleEffectAssetMenuBar");
      plCommandHistoryActions::MapActions("ParticleEffectAssetMenuBar");
    }

    // Tool Bar
    {
      plActionMapManager::RegisterActionMap("ParticleEffectAssetToolBar").IgnoreResult();
      plDocumentActions::MapToolbarActions("ParticleEffectAssetToolBar");
      plCommandHistoryActions::MapActions("ParticleEffectAssetToolBar", "");
      plAssetActions::MapToolBarActions("ParticleEffectAssetToolBar", true);
      plParticleActions::MapActions("ParticleEffectAssetToolBar");
    }

    // View Tool Bar
    {
      plActionMapManager::RegisterActionMap("ParticleEffectAssetViewToolBar").IgnoreResult();
      plViewActions::MapToolbarActions("ParticleEffectAssetViewToolBar", plViewActions::RenderMode | plViewActions::ActivateRemoteProcess);
      plViewLightActions::MapToolbarActions("ParticleEffectAssetViewToolBar");
    }

    plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plParticleEffectAssetDocument::PropertyMetaStateEventHandler);

    // Register visual graph pin factory for particle graph pins
    plQtVisualGraphScene::GetPinFactory().RegisterCreator(
      plGetStaticRTTI<plParticleGraphPin>(),
      [](const plRTTI* pRtti) -> plQtVisualGraphPin*
      { return new plQtVisualGraphPin(); });
  }
}

void OnUnloadPlugin()
{
  plParticleActions::UnregisterActions();
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plParticleEffectAssetDocument::PropertyMetaStateEventHandler);

  plQtVisualGraphScene::GetPinFactory().UnregisterCreator(plGetStaticRTTI<plParticleGraphPin>());
}

PL_PLUGIN_ON_LOADED()
{
  OnLoadPlugin();
}

PL_PLUGIN_ON_UNLOADED()
{
  OnUnloadPlugin();
}
