#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphCompiler.h>
#include <Foundation/IO/MemoryStream.h>
#include <ToolsFoundation/VisualGraph/VisualGraphObjectManager.h>

// The particle editor's preview: stats overlay, moving preview, timeline and value spy (P4). The
// engine side is plParticleContext and plParticlePreviewModule.

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEffectAssetPreferences, 1, plRTTIDefaultAllocator<plParticleEffectAssetPreferences>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("ShowStats", m_bShowStats)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("ShowEvents", m_bShowEvents)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ENUM_MEMBER_PROPERTY("PreviewMotion", plParticlePreviewMotion, m_PreviewMotion),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleEffectAssetPreferences::plParticleEffectAssetPreferences()
  : plPreferences(Domain::Document, "ParticleEffectAsset")
{
}

bool plParticleEffectAssetDocument::GetShowStats() const
{
  return plPreferences::QueryPreferences<plParticleEffectAssetPreferences>(this)->m_bShowStats;
}

void plParticleEffectAssetDocument::SetShowStats(bool bShow)
{
  auto* pPreferences = plPreferences::QueryPreferences<plParticleEffectAssetPreferences>(this);
  if (pPreferences->m_bShowStats == bShow)
    return;

  pPreferences->m_bShowStats = bShow;
  pPreferences->TriggerPreferencesChangedEvent();

  plParticleEffectAssetEvent e;
  e.m_pDocument = this;
  e.m_Type = plParticleEffectAssetEvent::PreviewSettingsChanged;
  m_Events.Broadcast(e);
}

bool plParticleEffectAssetDocument::GetShowEvents() const
{
  return plPreferences::QueryPreferences<plParticleEffectAssetPreferences>(this)->m_bShowEvents;
}

void plParticleEffectAssetDocument::SetShowEvents(bool bShow)
{
  auto* pPreferences = plPreferences::QueryPreferences<plParticleEffectAssetPreferences>(this);
  if (pPreferences->m_bShowEvents == bShow)
    return;

  pPreferences->m_bShowEvents = bShow;
  pPreferences->TriggerPreferencesChangedEvent();

  plParticleEffectAssetEvent e;
  e.m_pDocument = this;
  e.m_Type = plParticleEffectAssetEvent::PreviewSettingsChanged;
  m_Events.Broadcast(e);
}

plParticlePreviewMotion::Enum plParticleEffectAssetDocument::GetPreviewMotion() const
{
  return plPreferences::QueryPreferences<plParticleEffectAssetPreferences>(this)->m_PreviewMotion;
}

void plParticleEffectAssetDocument::SetPreviewMotion(plParticlePreviewMotion::Enum motion)
{
  auto* pPreferences = plPreferences::QueryPreferences<plParticleEffectAssetPreferences>(this);
  if (pPreferences->m_PreviewMotion == motion)
    return;

  pPreferences->m_PreviewMotion = motion;
  pPreferences->TriggerPreferencesChangedEvent();

  plParticleEffectAssetEvent e;
  e.m_pDocument = this;
  e.m_Type = plParticleEffectAssetEvent::PreviewSettingsChanged;
  m_Events.Broadcast(e);
}

void plParticleEffectAssetDocument::FillPreviewSettings(plParticlePreviewSettingsMsgToEngine& out_msg) const
{
  out_msg.m_DocumentGuid = GetGuid();
  out_msg.m_bShowStats = GetShowStats();
  out_msg.m_bShowEvents = GetShowEvents();
  out_msg.m_Motion = GetPreviewMotion();
  out_msg.m_SystemNames.Clear();

  // the descriptor lists the systems in the order they are wired into the effect
  if (const plDocumentObject* pEffectNode = FindEffectNode())
  {
    plDynamicArray<const plDocumentObject*> systems;
    CollectConnectedNodes(pEffectNode, "Systems", systems);

    for (const plDocumentObject* pSystem : systems)
      out_msg.m_SystemNames.PushBack(pSystem->GetTypeAccessor().GetValue("Name").ConvertTo<plString>());
  }

  // the preview says which of the Blackboard nodes' entries it does not have
  out_msg.m_BlackboardEntries.Clear();
  for (const plDocumentObject* pNode : GetObjectManager()->GetRootObject()->GetChildren())
  {
    if (const plString sPath = GetBlackboardPath(pNode); !sPath.IsEmpty() && !out_msg.m_BlackboardEntries.Contains(sPath))
      out_msg.m_BlackboardEntries.PushBack(sPath);
  }
}

plString plParticleEffectAssetDocument::GetBlackboardPath(const plDocumentObject* pNode)
{
  if (!pNode->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleBlackboardNode>())
    return {};

  const plString sEntry = pNode->GetTypeAccessor().GetValue("Entry").ConvertTo<plString>();
  if (sEntry.IsEmpty())
    return {};

  const plString sBlackboard = pNode->GetTypeAccessor().GetValue("Blackboard").ConvertTo<plString>();

  plStringBuilder sPath(sBlackboard.IsEmpty() ? plStringView("Global") : sBlackboard.GetView(), "|", sEntry);
  return sPath;
}

void plParticleEffectAssetDocument::SetPreviewTime(double fTime)
{
  SetSimulationPaused(true);

  plParticleSetPreviewTimeMsgToEngine msg;
  msg.m_DocumentGuid = GetGuid();
  msg.m_fTime = plMath::Max(fTime, 0.0);
  SendMessageToEngine(&msg);

  // the playhead moves now; the preview's own report follows
  m_PreviewStatus.m_fEffectTime = msg.m_fTime;

  plParticleEffectAssetEvent e;
  e.m_pDocument = this;
  e.m_Type = plParticleEffectAssetEvent::PreviewStatusChanged;
  m_Events.Broadcast(e);
}

void plParticleEffectAssetDocument::SetSpyNode(const plDocumentObject* pNode)
{
  const plUuid node = pNode != nullptr ? pNode->GetGuid() : plUuid();
  if (node == m_SpyNode)
    return;

  m_SpyNode = node;
  m_bSpyDirty = true;

  // the last result belongs to the previous node
  m_PreviewStatus.m_SpyNode = plUuid();
  m_PreviewStatus.m_uiSpyCount = 0;
}

namespace
{
  /// The first system a node's value reaches, following its wires downstream through other operators.
  const plDocumentObject* FindFedSystem(const plVisualGraphObjectManager* pManager, const plDocumentObject* pNode)
  {
    plSet<const plDocumentObject*> visited;
    plHybridArray<const plDocumentObject*, 8> pending;
    pending.PushBack(pNode);

    while (!pending.IsEmpty())
    {
      const plDocumentObject* pCurrent = pending[0];
      pending.RemoveAtAndCopy(0);

      if (visited.Contains(pCurrent))
        continue;
      visited.Insert(pCurrent);

      for (const auto& pOutput : pManager->GetOutputPins(pCurrent))
      {
        for (const plVisualGraphConnection* pConnection : pManager->GetConnections(*pOutput))
        {
          const plDocumentObject* pTarget = pConnection->GetTargetPin().GetParent();
          if (pTarget->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
            return pTarget;

          pending.PushBack(pTarget);
        }
      }
    }

    return nullptr;
  }
} // namespace

bool plParticleEffectAssetDocument::BuildSpyMessage(plParticleSpyMsgToEngine& out_msg) const
{
  out_msg.m_DocumentGuid = GetGuid();
  out_msg.m_Node = m_SpyNode;
  out_msg.m_Program.Clear();

  const plDocumentObject* pNode = m_SpyNode.IsValid() ? GetObjectManager()->GetObject(m_SpyNode) : nullptr;
  const plDocumentObject* pEffectNode = FindEffectNode();
  if (pNode == nullptr || pEffectNode == nullptr)
    return false;

  const auto* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());

  const plDocumentObject* pSystem = FindFedSystem(pManager, pNode);
  if (pSystem == nullptr)
    return false;

  plDynamicArray<const plDocumentObject*> systems;
  CollectConnectedNodes(pEffectNode, "Systems", systems);

  const plUInt32 uiSystem = systems.IndexOf(pSystem);
  if (uiSystem == plInvalidIndex)
    return false;

  plParticleGraphCompiler compiler(pManager, pEffectNode);
  compiler.SetParticleContext(false, pSystem->GetTypeAccessor().GetValue("LifeTime").Get<plVarianceTypeTime>().m_Value.GetSeconds());

  // a constant shows its value on the node already
  const plParticleTier tier = compiler.GetTier(pNode);
  if (tier == plParticleTier::Constant)
    return false;

  plParticleProgramData program;
  {
    plMuteLog mute;
    plLogSystemScope muteScope(&mute);

    if (compiler.AddSpyOutput(pNode) == plInvalidIndex || compiler.Compile(program.m_ByteCode).Failed())
      return false;
  }

  program.m_Outputs.PushBack("Spy");
  program.m_Assets = compiler.GetAssets();

  plMemoryStreamContainerWrapperStorage<plDataBuffer> storage(&out_msg.m_Program);
  plMemoryStreamWriter writer(&storage);
  program.Save(writer);

  out_msg.m_uiSystem = uiSystem;
  out_msg.m_bPerFrame = tier == plParticleTier::Frame;
  return true;
}

void plParticleEffectAssetDocument::HandleEngineMessage(const plEditorEngineDocumentMsg* pMsg)
{
  if (const auto* pStatus = plDynamicCast<const plParticlePreviewStatusMsgToEditor*>(pMsg))
  {
    m_PreviewStatus = *pStatus;

    plParticleEffectAssetEvent e;
    e.m_pDocument = this;
    e.m_Type = plParticleEffectAssetEvent::PreviewStatusChanged;
    m_Events.Broadcast(e);
    return;
  }

  SUPER::HandleEngineMessage(pMsg);
}
