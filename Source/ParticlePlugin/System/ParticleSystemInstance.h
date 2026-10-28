#pragma once

#include <Foundation/DataProcessing/Stream/ProcessingStream.h>
#include <Foundation/DataProcessing/Stream/ProcessingStreamGroup.h>
#include <Foundation/Math/BoundingBoxSphere.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Types/UniquePtr.h>
#include <ParticlePlugin/Declarations.h>
#include <ParticlePlugin/Events/ParticleEvent.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

struct plMsgExtractRenderData;
class plParticleFrameProgramRunner;
class plParticleEventInputs;

/// \brief A particle system stores all data for one 'layer' of a running particle effect
class PL_PARTICLEPLUGIN_DLL plParticleSystemInstance
{
public:
  plParticleSystemInstance();
  ~plParticleSystemInstance();

  void Construct(plUInt32 uiMaxParticles, plWorld* pWorld, plParticleEffectInstance* pOwnerEffect, float fSpawnCountMultiplier);
  void Destruct();

  bool IsVisible() const { return m_bVisible; }

  void SetEmitterEnabled(bool bEnable) { m_bEmitterEnabled = bEnable; }
  bool GetEmitterEnabled() const { return m_bEmitterEnabled; }

  bool HasActiveParticles() const;

  void ConfigureFromTemplate(const plParticleSystemDescriptor* pTemplate);
  void Finalize();

  void ReinitializeStreamProcessors(const plParticleSystemDescriptor* pTemplate);

  void CreateStreamProcessors(const plParticleSystemDescriptor* pTemplate);

  void SetupOptionalStreams();

  void SetTransform(const plTransform& transform, const plVec3& vParticleStartVelocity);
  const plTransform& GetTransform() const { return m_Transform; }
  const plVec3& GetParticleStartVelocity() const { return m_vParticleStartVelocity; }

  plParticleSystemState::Enum Update(const plTime& diff);

  plWorld* GetWorld() const { return m_pWorld; }

  plUInt64 GetMaxParticles() const { return m_StreamGroup.GetNumElements(); }
  plUInt64 GetNumActiveParticles() const { return m_StreamGroup.GetNumActiveElements(); }

  /// True when the behaviors were replaced by the GPU simulation.
  bool IsLoweredToGPU() const { return m_bLoweredToGPU; }

  /// The render types; a lowered system has one plParticleTypeGPU.
  plArrayPtr<plParticleType* const> GetTypes() const { return m_Types; }

  plArrayPtr<plParticleEmitter* const> GetEmitters() const { return m_Emitters; }

  /// How long the last Update took on the CPU, for the editor's stats.
  plTime GetLastUpdateDuration() const { return m_LastUpdateDuration; }



  /// \brief Returns the desired stream, if it already exists, nullptr otherwise.
  plProcessingStream* QueryStream(const char* szName, plProcessingStream::DataType type) const;

  /// \brief Returns the desired stream, if it already exists, creates it otherwise.
  void CreateStream(const char* szName, plProcessingStream::DataType type, plProcessingStream** pStream, plParticleStreamBinding& ref_binding, bool bExpectInitializedValue);

  void ProcessEventQueue(plParticleEventQueue queue);

  /// \brief Queues an event raised by one of this system's particles, counted for the editor's stats.
  void RaiseEvent(const plParticleEvent& e);

  /// \brief Counts events raised and dropped before they could be queued (a GPU system's beyond its buffer).
  void CountDroppedEvents(plUInt32 uiCount);

  /// The events this system raised in its last step, and how many of them the full queue dropped.
  plUInt32 GetEventsRaised() const { return m_uiLastEventsRaised; }
  plUInt32 GetEventsDropped() const { return m_uiLastEventsDropped; }

  /// All events this system raised since it started, and how many were dropped; for rates.
  plUInt64 GetTotalEventsRaised() const { return m_uiTotalEventsRaised; }
  plUInt64 GetTotalEventsDropped() const { return m_uiTotalEventsDropped; }

  /// While a per-event On Event spawns the particles of one event: that event and the emitter; null otherwise.
  const plParticleEvent* GetSpawnEvent() const { return m_pSpawnEvent; }
  const plParticleEmitter* GetSpawnEmitter() const { return m_pSpawnEmitter; }

  plParticleEffectInstance* GetOwnerEffect() const { return m_pOwnerEffect; }
  plParticleWorldModule* GetOwnerWorldModule() const;

  void ExtractSystemRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const;

  using ParticleDeathHandler = plEvent<const plStreamGroupElementRemovedEvent&>::Handler;

  void AddParticleDeathEventHandler(ParticleDeathHandler handler);
  void RemoveParticleDeathEventHandler(ParticleDeathHandler handler);

  void SetBoundingVolume(const plBoundingBoxSphere& volume, float fMaxParticleSize);
  const plBoundingBoxSphere& GetBoundingVolume() const { return m_BoundingVolume; }

  bool IsContinuous() const;

  float GetSpawnCountMultiplier() const { return m_fSpawnCountMultiplier; }

private:
  friend class plParticleFrameProgramRunner; // writes driven values into the modules

  bool IsEmitterConfigEqual(const plParticleSystemDescriptor* pTemplate) const;
  bool IsInitializerConfigEqual(const plParticleSystemDescriptor* pTemplate) const;
  bool IsBehaviorConfigEqual(const plParticleSystemDescriptor* pTemplate) const;
  bool IsTypeConfigEqual(const plParticleSystemDescriptor* pTemplate) const;
  bool IsFinalizerConfigEqual(const plParticleSystemDescriptor* pTemplate) const;

  void CreateStreamZeroInitializers();

  /// Spawns the batches of the per-event On Event emitters, each around its event's position.
  void SpawnEventBatches();

  plHybridArray<plParticleEmitter*, 2> m_Emitters;
  plHybridArray<plParticleInitializer*, 8> m_Initializers;
  plHybridArray<plParticleBehavior*, 8> m_Behaviors;
  plHybridArray<plParticleFinalizer*, 2> m_Finalizers;
  plHybridArray<plParticleType*, 4> m_Types;

  bool m_bVisible; // typically used in editor to hide a system
  bool m_bEmitterEnabled;
  bool m_bLoweredToGPU = false; // behaviors replaced by a configured plParticleTypeGPU (SimulationTarget GPU/Auto)
  plParticleEffectInstance* m_pOwnerEffect;
  plWorld* m_pWorld;
  plTransform m_Transform;
  plVec3 m_vParticleStartVelocity;
  float m_fSpawnCountMultiplier = 1.0f;
  plTime m_LastUpdateDuration;

  plUInt32 m_uiEventsRaised = 0;
  plUInt32 m_uiEventsDropped = 0;
  plUInt32 m_uiLastEventsRaised = 0;
  plUInt32 m_uiLastEventsDropped = 0;
  plUInt64 m_uiTotalEventsRaised = 0;
  plUInt64 m_uiTotalEventsDropped = 0;
  const plParticleEvent* m_pSpawnEvent = nullptr;
  const plParticleEmitter* m_pSpawnEmitter = nullptr;

  plProcessingStreamGroup m_StreamGroup;
  plParticleEventInputs* m_pEventInputs = nullptr; ///< owned by m_StreamGroup; only with an On Event emitter

  struct StreamInfo
  {
    plString m_sName;
    bool m_bGetsInitialized = false;
    bool m_bInUse = false;
    plProcessingStreamProcessor* m_pDefaultInitializer = nullptr;
  };

  plHybridArray<StreamInfo, 16> m_StreamInfo;

  // only allocated for systems whose graph has per-frame inputs
  plUniquePtr<plParticleFrameProgramRunner> m_pFrameProgram;

  // culling data
  plBoundingBoxSphere m_BoundingVolume;
};
