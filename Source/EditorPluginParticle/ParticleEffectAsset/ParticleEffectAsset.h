#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/Assets/AssetDocument.h>
#include <EditorFramework/Preferences/Preferences.h>
#include <EnginePluginParticle/ParticleAsset/ParticlePreviewMessages.h>
#include <Foundation/Communication/Event.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>

class plParticleEffectAssetDocument;
struct plPropertyMetaStateEvent;

struct plParticleEffectAssetEvent
{
  enum Type
  {
    RestartEffect,
    AutoRestartChanged,
    SimulationSpeedChanged,
    RenderVisualizersChanged,
    PreviewSettingsChanged, ///< stats overlay or preview motion
    PreviewStatusChanged,   ///< a new effect time or spy result from the preview
    ConversionReportDismissed,
  };

  plParticleEffectAssetDocument* m_pDocument;
  Type m_Type;
};

/// The particle editor's preview settings, per document and kept with the editor's settings for it,
/// not in the asset.
class plParticleEffectAssetPreferences : public plPreferences
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEffectAssetPreferences, plPreferences);

public:
  plParticleEffectAssetPreferences();

  bool m_bShowStats = true;
  bool m_bShowEvents = true;
  plEnum<plParticlePreviewMotion> m_PreviewMotion;
};

class plParticleEffectAssetDocument : public plAssetDocument
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEffectAssetDocument, plAssetDocument);

public:
  plParticleEffectAssetDocument(plStringView sDocumentPath);
  ~plParticleEffectAssetDocument();

  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

  void WriteResource(plStreamWriter& inout_stream) const;

  void TriggerRestartEffect();

  plEvent<const plParticleEffectAssetEvent&> m_Events;

  void SetAutoRestart(bool bEnable);
  bool GetAutoRestart() const { return m_bAutoRestart; }

  void SetSimulationPaused(bool bPaused);
  bool GetSimulationPaused() const { return m_bSimulationPaused; }

  void SetSimulationSpeed(float fSpeed);
  float GetSimulationSpeed() const { return m_fSimulationSpeed; }

  bool GetRenderVisualizers() const { return m_bRenderVisualizers; }
  void SetRenderVisualizers(bool b);

  virtual plResult ComputeObjectTransformation(const plDocumentObject* pObject, plTransform& out_result) const override;

  /// Why a system cannot run on the GPU, answered from the same code the runtime uses.
  struct LoweringResult
  {
    plString m_sReason;                      ///< empty when the system lowers
    const plDocumentObject* m_pBlock = nullptr; ///< the block to blame, when one can be identified
  };

  /// Builds a throwaway descriptor for one system node and asks plParticleGPULowering about it.
  LoweringResult QueryLowering(const plDocumentObject* pSystemNode) const;

  /// What the per-particle wires of one system compile to, and the custom attributes its blocks write.
  struct ParticleProgramInfo
  {
    struct CustomAttribute
    {
      plString m_sName;
      plString m_sType; ///< "float", "vector" or "color"
    };

    bool m_bInitializeProgram = false;
    bool m_bUpdateProgram = false;
    plUInt32 m_uiInitializeInstructions = 0; ///< summed over the Initialize blocks' programs
    plUInt32 m_uiUpdateInstructions = 0;
    plHybridArray<CustomAttribute, 8> m_CustomAttributes;
  };

  /// Compiles the per-particle programs of one system node the way the transform does.
  ParticleProgramInfo QueryParticlePrograms(const plDocumentObject* pSystemNode) const;

  /// A system holds at most this many custom attributes.
  static constexpr plUInt32 s_uiMaxCustomAttributes = 8;

  /// One entry of the system template library: a system node and its blocks, stored as a DDL
  /// graph file (same serialization as graph copy/paste) under Particles/Templates in any data
  /// directory. Packages ship read-only starter templates that way; saving from the editor
  /// writes into the project.
  struct SystemTemplate
  {
    plString m_sName;             ///< the file name without extension
    plString m_sFilePath;         ///< absolute path
    bool m_bProjectLocal = false; ///< found in the project rather than shipped by a package
  };

  /// Lists every template found in the 'Particles/Templates' folder of any mounted data
  /// directory. Shipped templates first, then project-local ones, each group sorted by name.
  static void GetAvailableSystemTemplates(plDynamicArray<SystemTemplate>& out_templates);

  /// What a template holds, as its blocks in context order: "Continuous, Sphere Position, Fade Out, Quad".
  static plString DescribeSystemTemplate(plStringView sAbsFilePath);

  /// Serializes the given system node with all its blocks into
  /// '<project>/Particles/Templates/<sName>.plParticleTemplate'. Overwrites without asking.
  plStatus SaveSystemAsTemplate(const plDocumentObject* pSystemNode, plStringView sName) const;

  /// Instantiates a template into this effect: pastes the system (new guids), places it right
  /// of the existing systems and connects it to the effect node. Undoable as one transaction.
  plStatus AddSystemFromTemplate(plStringView sAbsFilePath);

  /// Adds a system that spawns where pSource's particles die: pSource raises an event on death (named
  /// after it when it has none), and the new system's On Event spawns per event on it.
  plStatus AddSystemOnDeaths(const plDocumentObject* pSource);

  /// What opening the document converted from the 1.3 logic blocks (If, Switch, Remap, Conditional
  /// Kill, Expression, the old Set Attribute). Only known in the session that converted them.
  struct ConversionReport
  {
    plUInt32 m_uiBlocks = 0;
    plDynamicArray<plUuid> m_Objects; ///< the blocks and nodes the conversion made
    bool m_bDismissed = false;        ///< the bar that reports it was closed
  };

  const ConversionReport& GetConversionReport() const { return m_ConversionReport; }
  void DismissConversionReport() { m_ConversionReport.m_bDismissed = true; }

  /// True for a block or node the conversion made in this session.
  bool IsConverted(const plUuid& object) const { return m_ConversionReport.m_Objects.Contains(object); }

  // Preview (P4)

  bool GetShowStats() const;
  void SetShowStats(bool bShow);

  /// Markers in the preview where events happened in the last half second.
  bool GetShowEvents() const;
  void SetShowEvents(bool bShow);

  /// A Blackboard node's "<blackboard>|<entry>", or empty for other nodes and nodes without an entry.
  static plString GetBlackboardPath(const plDocumentObject* pNode);

  plParticlePreviewMotion::Enum GetPreviewMotion() const;
  void SetPreviewMotion(plParticlePreviewMotion::Enum motion);

  /// The settings message the window sends with every redraw.
  void FillPreviewSettings(plParticlePreviewSettingsMsgToEngine& out_msg) const;

  /// Shows the effect at this time in the preview, which pauses.
  void SetPreviewTime(double fTime);

  /// The node the value spy follows; null stops it. The window sends the program with its next redraw.
  void SetSpyNode(const plDocumentObject* pNode);
  const plUuid& GetSpyNode() const { return m_SpyNode; }

  /// Compiles the spied node into a program for the system it feeds. False when there is nothing to
  /// spy on: no node, a constant, or a node that reaches no system.
  bool BuildSpyMessage(plParticleSpyMsgToEngine& out_msg) const;

  bool IsSpyDirty() const { return m_bSpyDirty; }
  void SetSpyDirty(bool bDirty) { m_bSpyDirty = bDirty; }

  /// The last state the preview reported.
  const plParticlePreviewStatusMsgToEditor& GetPreviewStatus() const { return m_PreviewStatus; }

protected:
  virtual void InitializeAfterLoading(bool bFirstTimeCreation) override;
  virtual void HandleEngineMessage(const plEditorEngineDocumentMsg* pMsg) override;
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
  virtual plTransformStatus InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo) override;

  virtual void InternalGetMetaDataHash(const plDocumentObject* pObject, plUInt64& inout_uiHash) const override;
  virtual void AttachMetaDataBeforeSaving(plAbstractObjectGraph& graph) const override;
  virtual void RestoreMetaDataAfterLoading(const plAbstractObjectGraph& graph, bool bUndoable) override;

  virtual void GetSupportedMimeTypesForPasting(plHybridArray<plString, 4>& out_MimeTypes) const override;
  virtual bool CopySelectedObjects(plAbstractObjectGraph& out_objectGraph, plStringBuilder& out_MimeType) const override;
  virtual bool Paste(const plArrayPtr<PasteInfo>& info, const plAbstractObjectGraph& objectGraph, bool bAllowPickedPosition, plStringView sMimeType) override;

private:
  void BuildDescriptorFromGraph(plParticleEffectDescriptor& out_desc) const;
  const plDocumentObject* FindEffectNode() const;
  void CollectConnectedNodes(const plDocumentObject* pNode, plStringView sPinName, plDynamicArray<const plDocumentObject*>& out_nodes) const;

  /// Converts a pre-rework graph, where blocks were root-level nodes wired into a system by pins,
  /// into the context arrays the system now owns. Returns true if anything was migrated.
  bool MigrateBlocksIntoContexts();

  /// Rewrites the 1.3 logic blocks into graph nodes plus a Set Attribute or Kill If at the same place,
  /// so the effect behaves exactly as before. Fills m_ConversionReport (ParticleLegacyConversion.cpp).
  void ConvertLogicBlocks();

  /// Feeds the effect's parameter names into the dynamic enum the Parameter node picks from, so
  /// the dropdown offers exactly the parameters this effect declares.
  void UpdateParameterNameEnum() const;

public:
  /// The events the effect raises: the systems' On Death Event and Raycast's On Collide Event.
  void GetRaisedEvents(plDynamicArray<plString>& out_events) const;

private:
  /// Fills On Event's event-name dropdown with the events this effect raises and listens to.
  void UpdateEventNameEnum() const;

  /// The system's Events blocks into the names the descriptor and its Raycast blocks raise.
  void BuildEventRaisers(const plDocumentObject* pSystemNode, plParticleSystemDescriptor& ref_desc) const;

  /// Before 1.6 a system's death event and a Raycast's collision event were properties; they become Events blocks.
  bool MigrateEventsIntoBlocks();
  void ParameterPropertyEventHandler(const plDocumentObjectPropertyEvent& e);

  plEngineViewLightSettings m_LightSettings;

  bool m_bSimulationPaused = false;
  bool m_bAutoRestart = true;
  bool m_bRenderVisualizers = false;
  float m_fSimulationSpeed = 1.0f;

  ConversionReport m_ConversionReport;

  plUuid m_SpyNode;
  bool m_bSpyDirty = false;
  plParticlePreviewStatusMsgToEditor m_PreviewStatus;
};