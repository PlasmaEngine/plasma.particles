#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/Actions/AssetActions.h>
#include <EditorFramework/Actions/ProjectActions.h>
#include <EditorFramework/Actions/ViewActions.h>
#include <EditorFramework/Actions/ViewLightActions.h>
#include <EditorPluginParticle/Actions/ParticleActions.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeItem.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleOperatorNodeItem.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleSystemNodeItem.h>
#include <GuiFoundation/Action/CommandHistoryActions.h>
#include <GuiFoundation/Action/DocumentActions.h>
#include <GuiFoundation/Action/StandardMenus.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <GuiFoundation/VisualGraph/Pin.h>
#include <GuiFoundation/VisualGraph/Scene.moc.h>

void OnLoadPlugin()
{
  // Stamped so the log says which binary is actually live: the same package version can exist in
  // both the user store and the project's Plugins folder, and only one of them is loaded.
  plLog::Info("EditorPluginParticle loaded - built {0} {1}", __DATE__, __TIME__);

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
      { return new plQtParticlePinItem(); });

    // A system draws itself as a stack of contexts holding its blocks, rather than as a node
    // with one pin per block category.
    plQtVisualGraphScene::GetNodeFactory().RegisterCreator(
      plGetStaticRTTI<plParticleSystemNode>(),
      [](const plRTTI* pRtti) -> plQtVisualGraphNode*
      { return new plQtParticleSystemNodeItem(); });

    // The effect is a bar the systems flow down into, so its pin belongs on the top edge.
    plQtVisualGraphScene::GetNodeFactory().RegisterCreator(
      plGetStaticRTTI<plParticleEffectNode>(),
      [](const plRTTI* pRtti) -> plQtVisualGraphNode*
      { return new plQtParticleEffectNodeItem(); });

    // Operators are small enough to edit in place rather than through the properties dock.
    auto operatorCreator = [](const plRTTI* pRtti) -> plQtVisualGraphNode*
    { return new plQtParticleOperatorNodeItem(); };

    for (const plRTTI* pOperatorType : {plGetStaticRTTI<plParticleValueNode>(), plGetStaticRTTI<plParticleColorNode>(),
           plGetStaticRTTI<plParticleTextureNode>(), plGetStaticRTTI<plParticleGradientNode>(), plGetStaticRTTI<plParticleCurveNode>(),
           plGetStaticRTTI<plParticleMathNode>(), plGetStaticRTTI<plParticleCompareNode>(), plGetStaticRTTI<plParticleBranchNode>(),
           plGetStaticRTTI<plParticleParameterNode>(), plGetStaticRTTI<plParticleBoolNode>(),
           plGetStaticRTTI<plParticleColorParameterNode>(), plGetStaticRTTI<plParticleToBoolNode>(),
           plGetStaticRTTI<plParticleBoolParameterNode>()})
    {
      plQtVisualGraphScene::GetNodeFactory().RegisterCreator(pOperatorType, operatorCreator);
    }
  }
}

void OnUnloadPlugin()
{
  plParticleActions::UnregisterActions();
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plParticleEffectAssetDocument::PropertyMetaStateEventHandler);

  plQtVisualGraphScene::GetPinFactory().UnregisterCreator(plGetStaticRTTI<plParticleGraphPin>());
  plQtVisualGraphScene::GetNodeFactory().UnregisterCreator(plGetStaticRTTI<plParticleSystemNode>());
  plQtVisualGraphScene::GetNodeFactory().UnregisterCreator(plGetStaticRTTI<plParticleEffectNode>());

  // Every creator registered above has to go, or a reloaded plugin leaves the factory pointing at
  // code that has been unloaded.
  for (const plRTTI* pOperatorType : {plGetStaticRTTI<plParticleValueNode>(), plGetStaticRTTI<plParticleColorNode>(),
         plGetStaticRTTI<plParticleTextureNode>(), plGetStaticRTTI<plParticleGradientNode>(), plGetStaticRTTI<plParticleCurveNode>(),
         plGetStaticRTTI<plParticleMathNode>(), plGetStaticRTTI<plParticleCompareNode>(), plGetStaticRTTI<plParticleBranchNode>(),
         plGetStaticRTTI<plParticleParameterNode>(), plGetStaticRTTI<plParticleBoolNode>(),
         plGetStaticRTTI<plParticleColorParameterNode>(), plGetStaticRTTI<plParticleToBoolNode>(),
           plGetStaticRTTI<plParticleBoolParameterNode>()})
  {
    plQtVisualGraphScene::GetNodeFactory().UnregisterCreator(pOperatorType);
  }
}

PL_PLUGIN_ON_LOADED()
{
  OnLoadPlugin();
}

PL_PLUGIN_ON_UNLOADED()
{
  OnUnloadPlugin();
}
