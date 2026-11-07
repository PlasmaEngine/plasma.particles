#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EnginePluginParticle/EnginePluginParticleDLL.h>
#include <EnginePluginParticle/ParticleAsset/ParticlePreviewMessages.h>
#include <ParticlePlugin/Resources/ParticleEffectResource.h>
#include <RendererCore/Meshes/MeshResource.h>

class plParticleComponent;
class plParticlePreviewModule;
class plParticleSystemDescriptor;
class plParticleSystemInstance;

using plParticleEffectResourceHandle = plTypedResourceHandle<class plParticleEffectResource>;

class PL_ENGINEPLUGINPARTICLE_DLL plParticleContext : public plEngineProcessDocumentContext
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleContext, plEngineProcessDocumentContext);

public:
  plParticleContext();
  ~plParticleContext();

  virtual void HandleMessage(const plEditorEngineDocumentMsg* pMsg) override;

  /// What the stats overlay shows for one system.
  struct SystemStats
  {
    plString m_sName;
    plUInt64 m_uiParticles = 0;
    plUInt64 m_uiMaxParticles = 0;
    plString m_sRunsOn;       ///< "CPU", "GPU" or "CPU · <why it does not lower>"
    bool m_bOnCpuByProgram = false;
    plUInt32 m_uiProgramOps = 0;
    plTime m_UpdateDuration;
    plString m_sUpdateInterval; ///< "Gradient, Fade every 4th frame (32 m)"; empty without either block
    plVec3 m_vBoundsSize = plVec3::MakeZero();

    // a system on the GPU
    bool m_bOnGPU = false;
    plUInt32 m_uiGPURegisters = 0; ///< the most any of its graph programs uses
    plUInt32 m_uiSortedAtMost = 0; ///< blended systems: the sort covers at most this many slots
    plUInt32 m_uiSortSlots = 0;    ///< ... of these

    // events
    plString m_sRaises;    ///< "14 / frame · 0 dropped"; empty when the system raises none
    plString m_sSpawnedBy; ///< "Hit · 14 events · 8 each"; empty without an On Event
  };

  bool GetShowStats() const { return m_bShowStats; }

  /// Where events happened in the last half second, in world space, for the preview's markers.
  struct RecentEvent
  {
    plVec3 m_vPosition;
    plTime m_Time;
  };

  bool GetShowEvents() const { return m_bShowEvents; }
  plArrayPtr<const RecentEvent> GetRecentEvents() const { return m_RecentEvents; }
  const plDynamicArray<SystemStats>& GetSystemStats() const { return m_SystemStats; }

  plParticlePreviewMotion::Enum GetMotion() const { return m_Motion; }

  /// The speed of the preview object, which the effect sees as its velocity.
  float GetPreviewSpeed() const { return m_fPreviewSpeed; }

  /// Where a motion puts the preview object at time t, and the speed it keeps; the overlay draws the path from it.
  static plVec3 GetPreviewPosition(plParticlePreviewMotion::Enum motion, double t, float* out_pSpeed = nullptr);

  /// The seconds one round of a motion takes (zero for Static).
  static double GetPreviewPeriod(plParticlePreviewMotion::Enum motion);

protected:
  virtual void OnInitialize() override;
  virtual void UpdateDocumentContext() override;

  virtual plEngineProcessViewContext* CreateViewContext() override;
  virtual void DestroyViewContext(plEngineProcessViewContext* pContext) override;
  virtual void OnThumbnailViewContextRequested() override;
  virtual bool UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext) override;

  void RestartEffect();
  void SetAutoRestartEffect(bool loop);

  /// Restarts the effect and simulates it to fTime in 1/60 s steps, with the preview seed.
  void SimulateTo(double fTime);

  /// Moves the preview object along its path at the world clock's time.
  void MovePreview();

  void UpdateSystemStats();
  void CollectRecentEvents();
  void GatherEventStats(plUInt32 uiSystem, const plParticleSystemDescriptor& desc, const plParticleSystemInstance& system, SystemStats& ref_stats);

  /// A system's event counts at the start of the current second, and the rates of the last full one.
  struct EventRate
  {
    plTime m_Start;
    plUInt64 m_uiRaised = 0;
    plUInt64 m_uiDropped = 0;
    plUInt64 m_uiEvents = 0;
    plUInt64 m_uiSpawned = 0;
    float m_fRaised = 0.0f;
    float m_fDropped = 0.0f;
    float m_fEvents = 0.0f;
    float m_fEach = 0.0f;
  };
  plDynamicArray<EventRate> m_EventRates;
  void SendStatus();

private:
  plBoundingBoxSphere m_ThumbnailBoundingVolume;
  plParticleEffectResourceHandle m_hParticle;
  plMeshResourceHandle m_hPreviewMeshResource;
  plParticleComponent* m_pComponent = nullptr;
  plParticlePreviewModule* m_pPreviewModule = nullptr;

  bool m_bShowStats = true;
  bool m_bShowEvents = true;
  plDynamicArray<RecentEvent> m_RecentEvents;
  plTime m_LastEventCollection; ///< the effect time the recent events were last taken at
  plParticlePreviewMotion::Enum m_Motion = plParticlePreviewMotion::Static;
  plDynamicArray<plString> m_SystemNames;
  plDynamicArray<plString> m_BlackboardEntries;
  plDynamicArray<SystemStats> m_SystemStats;
  float m_fPreviewSpeed = 0.0f;
  plTime m_LastStatus;
};
