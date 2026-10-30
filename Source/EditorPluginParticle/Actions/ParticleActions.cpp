#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/Actions/ParticleActions.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/UIServices/UIServices.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTemplatesMenuAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

plActionDescriptorHandle plParticleActions::s_hCategory;
plActionDescriptorHandle plParticleActions::s_hPauseEffect;
plActionDescriptorHandle plParticleActions::s_hRestartEffect;
plActionDescriptorHandle plParticleActions::s_hAutoRestart;
plActionDescriptorHandle plParticleActions::s_hSimulationSpeedMenu;
plActionDescriptorHandle plParticleActions::s_hSimulationSpeed[10];
plActionDescriptorHandle plParticleActions::s_hRenderVisualizers;
plActionDescriptorHandle plParticleActions::s_hTemplatesMenu;
plActionDescriptorHandle plParticleActions::s_hShowStats;
plActionDescriptorHandle plParticleActions::s_hShowEvents;
plActionDescriptorHandle plParticleActions::s_hPreviewStatic;
plActionDescriptorHandle plParticleActions::s_hPreviewOrbit;
plActionDescriptorHandle plParticleActions::s_hPreviewLine;


void plParticleActions::RegisterActions()
{
  s_hCategory = PL_REGISTER_CATEGORY("ParticleCategory");
  s_hPauseEffect =
    PL_REGISTER_ACTION_1("PFX.Pause", plActionScope::Document, "Particles", "Pause", plParticleAction, plParticleAction::ActionType::PauseEffect);
  s_hRestartEffect =
    PL_REGISTER_ACTION_1("PFX.Restart", plActionScope::Document, "Particles", "F5", plParticleAction, plParticleAction::ActionType::RestartEffect);
  s_hAutoRestart =
    PL_REGISTER_ACTION_1("PFX.AutoRestart", plActionScope::Document, "Particles", "", plParticleAction, plParticleAction::ActionType::AutoRestart);

  s_hSimulationSpeedMenu = PL_REGISTER_MENU_WITH_ICON("PFX.Speed.Menu", ":/EditorPluginParticle/Icons/Speed.svg");
  s_hSimulationSpeed[0] = PL_REGISTER_ACTION_2(
    "PFX.Speed.01", plActionScope::Document, "Particles", "Ctrl+1", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 0.1f);
  s_hSimulationSpeed[1] = PL_REGISTER_ACTION_2(
    "PFX.Speed.025", plActionScope::Document, "Particles", "Ctrl+2", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 0.25f);
  s_hSimulationSpeed[2] = PL_REGISTER_ACTION_2(
    "PFX.Speed.05", plActionScope::Document, "Particles", "Ctrl+3", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 0.5f);
  s_hSimulationSpeed[3] = PL_REGISTER_ACTION_2(
    "PFX.Speed.1", plActionScope::Document, "Particles", "Ctrl+4", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 1.0f);
  s_hSimulationSpeed[4] = PL_REGISTER_ACTION_2(
    "PFX.Speed.15", plActionScope::Document, "Particles", "Ctrl+5", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 1.5f);
  s_hSimulationSpeed[5] = PL_REGISTER_ACTION_2(
    "PFX.Speed.2", plActionScope::Document, "Particles", "Ctrl+6", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 2.0f);
  s_hSimulationSpeed[6] = PL_REGISTER_ACTION_2(
    "PFX.Speed.3", plActionScope::Document, "Particles", "Ctrl+7", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 3.0f);
  s_hSimulationSpeed[7] = PL_REGISTER_ACTION_2(
    "PFX.Speed.4", plActionScope::Document, "Particles", "Ctrl+8", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 4.0f);
  s_hSimulationSpeed[8] = PL_REGISTER_ACTION_2(
    "PFX.Speed.5", plActionScope::Document, "Particles", "Ctrl+9", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 5.0f);
  s_hSimulationSpeed[9] = PL_REGISTER_ACTION_2(
    "PFX.Speed.10", plActionScope::Document, "Particles", "Ctrl+0", plParticleAction, plParticleAction::ActionType::SimulationSpeed, 10.0f);
  s_hRenderVisualizers = PL_REGISTER_ACTION_1(
    "PFX.Render.Visualizers", plActionScope::Document, "Particles", "V", plParticleAction, plParticleAction::ActionType::RenderVisualizers);
  s_hTemplatesMenu = PL_REGISTER_DYNAMIC_MENU("PFX.Templates", plParticleTemplatesMenuAction, ":/GuiFoundation/Icons/DocumentAdd.svg");
  s_hShowStats = PL_REGISTER_ACTION_1("PFX.Stats", plActionScope::Document, "Particles", "", plParticleAction, plParticleAction::ActionType::ShowStats);
  s_hShowEvents = PL_REGISTER_ACTION_1("PFX.Events", plActionScope::Document, "Particles", "", plParticleAction, plParticleAction::ActionType::ShowEvents);
  s_hPreviewStatic = PL_REGISTER_ACTION_1("PFX.Preview.Static", plActionScope::Document, "Particles", "", plParticleAction, plParticleAction::ActionType::PreviewStatic);
  s_hPreviewOrbit = PL_REGISTER_ACTION_1("PFX.Preview.Orbit", plActionScope::Document, "Particles", "", plParticleAction, plParticleAction::ActionType::PreviewOrbit);
  s_hPreviewLine = PL_REGISTER_ACTION_1("PFX.Preview.Line", plActionScope::Document, "Particles", "", plParticleAction, plParticleAction::ActionType::PreviewLine);
}

void plParticleActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hCategory);
  plActionManager::UnregisterAction(s_hPauseEffect);
  plActionManager::UnregisterAction(s_hRestartEffect);
  plActionManager::UnregisterAction(s_hAutoRestart);
  plActionManager::UnregisterAction(s_hSimulationSpeedMenu);
  plActionManager::UnregisterAction(s_hRenderVisualizers);
  plActionManager::UnregisterAction(s_hTemplatesMenu);
  plActionManager::UnregisterAction(s_hShowStats);
  plActionManager::UnregisterAction(s_hShowEvents);
  plActionManager::UnregisterAction(s_hPreviewStatic);
  plActionManager::UnregisterAction(s_hPreviewOrbit);
  plActionManager::UnregisterAction(s_hPreviewLine);

  for (int i = 0; i < PL_ARRAY_SIZE(s_hSimulationSpeed); ++i)
    plActionManager::UnregisterAction(s_hSimulationSpeed[i]);
}

void plParticleActions::MapActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pMap != nullptr, "The given mapping ('{0}') does not exist, mapping the actions failed!", sMapping);

  pMap->MapAction(s_hCategory, "", 11.0f);

  const char* szSubPath = "ParticleCategory";

  pMap->MapAction(s_hPauseEffect, szSubPath, 0.5f);
  pMap->MapAction(s_hRestartEffect, szSubPath, 1.0f);
  pMap->MapAction(s_hAutoRestart, szSubPath, 2.0f);

  pMap->MapAction(s_hSimulationSpeedMenu, szSubPath, 3.0f);

  plStringBuilder sSubPath(szSubPath, "/PFX.Speed.Menu");

  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_hSimulationSpeed); ++i)
    pMap->MapAction(s_hSimulationSpeed[i], sSubPath, i + 1.0f);

  pMap->MapAction(s_hRenderVisualizers, szSubPath, 4.0f);
  pMap->MapAction(s_hTemplatesMenu, szSubPath, 5.0f);

  // the preview: how it moves, and its stats overlay
  pMap->MapAction(s_hPreviewStatic, szSubPath, 6.0f);
  pMap->MapAction(s_hPreviewOrbit, szSubPath, 6.1f);
  pMap->MapAction(s_hPreviewLine, szSubPath, 6.2f);
  pMap->MapAction(s_hShowStats, szSubPath, 7.0f);
  pMap->MapAction(s_hShowEvents, szSubPath, 7.1f);
}

plParticleAction::plParticleAction(const plActionContext& context, const char* szName, plParticleAction::ActionType type, float fSimSpeed)
  : plButtonAction(context, szName, false, "")
{
  m_Type = type;
  m_fSimSpeed = fSimSpeed;

  m_pEffectDocument = const_cast<plParticleEffectAssetDocument*>(static_cast<const plParticleEffectAssetDocument*>(context.m_pDocument));
  m_pEffectDocument->m_Events.AddEventHandler(plMakeDelegate(&plParticleAction::EffectEventHandler, this));

  switch (m_Type)
  {
    case ActionType::PauseEffect:
      SetIconPath(":/EditorPluginParticle/Icons/Pause.svg");
      break;

    case ActionType::RestartEffect:
      SetIconPath(":/EditorPluginParticle/Icons/Restart.svg");
      break;

    case ActionType::AutoRestart:
      SetIconPath(":/EditorPluginParticle/Icons/Loop.svg");
      break;

    case ActionType::RenderVisualizers:
      SetCheckable(true);
      SetIconPath(":/EditorFramework/Icons/Visualizers.svg");
      SetChecked(m_pEffectDocument->GetRenderVisualizers());
      break;

    default:
      break;
  }

  UpdateState();
}


plParticleAction::~plParticleAction()
{
  m_pEffectDocument->m_Events.RemoveEventHandler(plMakeDelegate(&plParticleAction::EffectEventHandler, this));
}

void plParticleAction::Execute(const plVariant& value)
{
  switch (m_Type)
  {
    case ActionType::PauseEffect:
      m_pEffectDocument->SetSimulationPaused(!m_pEffectDocument->GetSimulationPaused());
      return;

    case ActionType::RestartEffect:
      m_pEffectDocument->TriggerRestartEffect();
      return;

    case ActionType::AutoRestart:
      m_pEffectDocument->SetAutoRestart(!m_pEffectDocument->GetAutoRestart());
      return;

    case ActionType::SimulationSpeed:
      m_pEffectDocument->SetSimulationSpeed(m_fSimSpeed);
      return;

    case ActionType::RenderVisualizers:
      m_pEffectDocument->SetRenderVisualizers(!m_pEffectDocument->GetRenderVisualizers());
      return;

    case ActionType::ShowStats:
      m_pEffectDocument->SetShowStats(!m_pEffectDocument->GetShowStats());
      return;

    case ActionType::ShowEvents:
      m_pEffectDocument->SetShowEvents(!m_pEffectDocument->GetShowEvents());
      return;

    case ActionType::PreviewStatic:
      m_pEffectDocument->SetPreviewMotion(plParticlePreviewMotion::Static);
      return;

    case ActionType::PreviewOrbit:
      m_pEffectDocument->SetPreviewMotion(plParticlePreviewMotion::Orbit);
      return;

    case ActionType::PreviewLine:
      m_pEffectDocument->SetPreviewMotion(plParticlePreviewMotion::Line);
      return;
  }
}

void plParticleAction::EffectEventHandler(const plParticleEffectAssetEvent& e)
{
  switch (e.m_Type)
  {
    case plParticleEffectAssetEvent::AutoRestartChanged:
    case plParticleEffectAssetEvent::SimulationSpeedChanged:
    case plParticleEffectAssetEvent::RenderVisualizersChanged:
    case plParticleEffectAssetEvent::PreviewSettingsChanged:
      UpdateState();
      break;

    default:
      break;
  }
}

void plParticleTemplatesMenuAction::GetEntries(plDynamicArray<Item>& out_entries)
{
  out_entries.Clear();

  plDynamicArray<plParticleEffectAssetDocument::SystemTemplate> templates;
  plParticleEffectAssetDocument::GetAvailableSystemTemplates(templates);

  bool bPrevWasProjectLocal = false;
  for (const auto& tmpl : templates)
  {
    // shipped templates come first; a separator marks where the project's own begin
    if (tmpl.m_bProjectLocal && !bPrevWasProjectLocal && !out_entries.IsEmpty())
    {
      Item& sep = out_entries.ExpandAndGetRef();
      sep.m_ItemFlags = Item::ItemFlags::Separator;
    }
    bPrevWasProjectLocal = tmpl.m_bProjectLocal;

    Item& item = out_entries.ExpandAndGetRef();
    item.m_sDisplay = tmpl.m_sName;
    item.m_UserValue = tmpl.m_sFilePath;
  }

  if (out_entries.IsEmpty())
  {
    Item& item = out_entries.ExpandAndGetRef();
    item.m_sDisplay = "<no templates found>";
  }
}

void plParticleTemplatesMenuAction::Execute(const plVariant& value)
{
  // the "<no templates found>" placeholder has no file attached
  if (!value.IsA<plString>())
    return;

  auto* pDocument = const_cast<plParticleEffectAssetDocument*>(static_cast<const plParticleEffectAssetDocument*>(m_Context.m_pDocument));

  const plStatus status = pDocument->AddSystemFromTemplate(value.Get<plString>());
  plEditorUiServices::MessageBoxStatus(status, "Adding the system from the template failed.");
}

void plParticleAction::UpdateState()
{
  if (m_Type == ActionType::PauseEffect)
  {
    SetCheckable(true);
    SetChecked(m_pEffectDocument->GetSimulationPaused());
  }

  if (m_Type == ActionType::AutoRestart)
  {
    SetCheckable(true);
    SetChecked(m_pEffectDocument->GetAutoRestart());
  }

  if (m_Type == ActionType::SimulationSpeed)
  {
    SetCheckable(true);
    SetChecked(m_pEffectDocument->GetSimulationSpeed() == m_fSimSpeed);
  }

  if (m_Type == ActionType::RenderVisualizers)
  {
    SetCheckable(true);
    SetChecked(m_pEffectDocument->GetRenderVisualizers());
  }

  if (m_Type == ActionType::ShowStats)
  {
    SetCheckable(true);
    SetChecked(m_pEffectDocument->GetShowStats());
  }

  if (m_Type == ActionType::ShowEvents)
  {
    SetCheckable(true);
    SetChecked(m_pEffectDocument->GetShowEvents());
  }

  if (m_Type == ActionType::PreviewStatic || m_Type == ActionType::PreviewOrbit || m_Type == ActionType::PreviewLine)
  {
    const plParticlePreviewMotion::Enum motion = m_Type == ActionType::PreviewOrbit ? plParticlePreviewMotion::Orbit : (m_Type == ActionType::PreviewLine ? plParticlePreviewMotion::Line : plParticlePreviewMotion::Static);
    SetCheckable(true);
    SetChecked(m_pEffectDocument->GetPreviewMotion() == motion);
  }
}
