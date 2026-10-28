#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/Actions/AssetActions.h>
#include <EditorFramework/Actions/ProjectActions.h>
#include <EditorFramework/Actions/ViewActions.h>
#include <EditorFramework/Actions/ViewLightActions.h>
#include <EditorPluginParticle/Actions/ParticleActions.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectShellPanels.h>
#include <GuiFoundation/Action/CommandHistoryActions.h>
#include <GuiFoundation/Action/DocumentActions.h>
#include <GuiFoundation/Action/StandardMenus.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Configuration/Plugin.h>

/// The package's icon folder: the icons under their ids (":/AssetIcons/Particle_Effect.svg" is
/// "EditorIcons/AssetIcons/Particle_Effect.svg"). The plugin binary lives in <package>/Bin/<configuration>.
static plString s_sIconFolder;

static void RegisterIconFolder()
{
  plStringBuilder sPlugin, sCopy;
  plPlugin::GetPluginPaths("plEditorPluginParticle", sPlugin, sCopy, 0);

  plStringBuilder sFolder = sPlugin.GetFileDirectory();
  sFolder.AppendPath("..", "..", "EditorIcons");
  sFolder.MakeCleanPath();

  s_sIconFolder = sFolder;
  plEditorUi::AddIconFolder(s_sIconFolder);
}

static void UnregisterIconFolder()
{
  plEditorUi::RemoveIconFolder(s_sIconFolder);
  s_sIconFolder.Clear();
}

void OnLoadPlugin()
{
  // Stamped so the log says which binary is actually live: the same package version can exist in
  // both the user store and the project's Plugins folder, and only one of them is loaded.
  plLog::Info("EditorPluginParticle loaded - built {0} {1}", __DATE__, __TIME__);

  RegisterIconFolder();

  plParticleActions::RegisterActions();
  plParticleEffectShellPanels::Register();

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
  }
}

void OnUnloadPlugin()
{
  plParticleEffectShellPanels::Unregister();
  plParticleActions::UnregisterActions();
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plParticleEffectAssetDocument::PropertyMetaStateEventHandler);

  UnregisterIconFolder();
}

PL_PLUGIN_ON_LOADED()
{
  OnLoadPlugin();
}

PL_PLUGIN_ON_UNLOADED()
{
  OnUnloadPlugin();
}
