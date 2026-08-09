#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/Assets/AssetDocument.h>
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
  };

  plParticleEffectAssetDocument* m_pDocument;
  Type m_Type;
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

  /// Serializes the given system node with all its blocks into
  /// '<project>/Particles/Templates/<sName>.plParticleTemplate'. Overwrites without asking.
  plStatus SaveSystemAsTemplate(const plDocumentObject* pSystemNode, plStringView sName) const;

  /// Instantiates a template into this effect: pastes the system (new guids), places it right
  /// of the existing systems and connects it to the effect node. Undoable as one transaction.
  plStatus AddSystemFromTemplate(plStringView sAbsFilePath);

protected:
  virtual void InitializeAfterLoading(bool bFirstTimeCreation) override;
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

  /// Feeds the effect's parameter names into the dynamic enum the Parameter node picks from, so
  /// the dropdown offers exactly the parameters this effect declares.
  void UpdateParameterNameEnum() const;
  void ParameterPropertyEventHandler(const plDocumentObjectPropertyEvent& e);

  plEngineViewLightSettings m_LightSettings;

  bool m_bSimulationPaused = false;
  bool m_bAutoRestart = true;
  bool m_bRenderVisualizers = false;
  float m_fSimulationSpeed = 1.0f;
};