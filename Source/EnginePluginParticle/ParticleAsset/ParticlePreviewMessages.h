#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EnginePluginParticle/EnginePluginParticleDLL.h>

/// How the particle editor moves the preview object.
struct PL_ENGINEPLUGINPARTICLE_DLL plParticlePreviewMotion
{
  using StorageType = plUInt8;

  enum Enum
  {
    Static,
    Orbit, ///< a circle around the origin
    Line,  ///< back and forth along Y

    Default = Static
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_ENGINEPLUGINPARTICLE_DLL, plParticlePreviewMotion);

/// The particle editor's preview settings, sent with every redraw.
class PL_ENGINEPLUGINPARTICLE_DLL plParticlePreviewSettingsMsgToEngine : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plParticlePreviewSettingsMsgToEngine, plEditorEngineDocumentMsg);

public:
  bool m_bShowStats = true;
  bool m_bShowEvents = true;
  plEnum<plParticlePreviewMotion> m_Motion;

  /// The names of the effect's systems, in descriptor order; the runtime does not keep them.
  plDynamicArray<plString> m_SystemNames;

  /// The blackboard entries the graph's Blackboard nodes read, as "<blackboard>|<entry>".
  plDynamicArray<plString> m_BlackboardEntries;
};

/// Shows the effect at this time: it restarts and is re-simulated in 1/60 s steps, with the preview's
/// fixed seed, so a scrub shows what played.
class PL_ENGINEPLUGINPARTICLE_DLL plParticleSetPreviewTimeMsgToEngine : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSetPreviewTimeMsgToEngine, plEditorEngineDocumentMsg);

public:
  double m_fTime = 0.0;
};

/// The node to spy on: a program computing its value as one number for one system. An empty program
/// stops the spy.
class PL_ENGINEPLUGINPARTICLE_DLL plParticleSpyMsgToEngine : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleSpyMsgToEngine, plEditorEngineDocumentMsg);

public:
  plUuid m_Node;
  plUInt32 m_uiSystem = 0;
  bool m_bPerFrame = false; ///< one value for the system, not one per particle
  plDataBuffer m_Program;   ///< plParticleProgramData::Save, output "Spy"
};

/// The preview's state, about ten times a second: the effect time and the spied value.
class PL_ENGINEPLUGINPARTICLE_DLL plParticlePreviewStatusMsgToEditor : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plParticlePreviewStatusMsgToEditor, plEditorEngineDocumentMsg);

public:
  double m_fEffectTime = 0.0;
  bool m_bEffectAlive = false;

  plUuid m_SpyNode;      ///< invalid when nothing is spied on
  plUInt32 m_uiSpyCount = 0; ///< particles the values come from; 1 for a per-frame node
  float m_fSpyMin = 0.0f;
  float m_fSpyAvg = 0.0f;
  float m_fSpyMax = 0.0f;
  plDynamicArray<plUInt32> m_SpyBins; ///< a histogram from min to max
  bool m_bSpyOnGPU = false;           ///< read back from a system on the GPU, a few frames late

  /// Of the settings' blackboard entries, those the preview's world does not have; their nodes read the default.
  plDynamicArray<plString> m_MissingBlackboardEntries;
};
