#pragma once

#include <Core/ResourceManager/ResourceHandle.h>
#include <Foundation/Containers/Deque.h>
#include <ParticlePlugin/Emitter/ParticleEmitter.h>
#include <ParticlePlugin/Events/ParticleEvent.h>

/// \brief How On Event turns the events of a frame into particles.
struct PL_PARTICLEPLUGIN_DLL plParticleOnEventSpawn
{
  using StorageType = plUInt8;

  enum Enum
  {
    PerEvent,     ///< every event spawns its own particles, around the event's position
    OncePerFrame, ///< one batch per frame however many events came in, around the effect (before plasma.particles 1.6)

    Default = PerEvent
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleOnEventSpawn);

class PL_PARTICLEPLUGIN_DLL plParticleEmitterFactory_OnEvent final : public plParticleEmitterFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEmitterFactory_OnEvent, plParticleEmitterFactory);

public:
  plParticleEmitterFactory_OnEvent();
  ~plParticleEmitterFactory_OnEvent();

  virtual const plRTTI* GetEmitterType() const override;
  virtual void CopyEmitterProperties(plParticleEmitter* pEmitter, bool bFirstTime) const override;
  virtual void QueryMaxParticleCount(plUInt32& out_uiMaxParticlesAbs, plUInt32& out_uiMaxParticlesPerSecond) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plString m_sEventName;
  plUInt32 m_uiSpawnCountMin = 1;
  plUInt32 m_uiSpawnCountRange = 0;
  plString m_sSpawnCountScaleParameter;

  plEnum<plParticleOnEventSpawn> m_SpawnMode;
  float m_fInheritVelocity = 0.0f; ///< share of the raising particle's velocity added to the new ones'
  bool m_bInheritColor = false;    ///< multiplies the new particles' colour by the raising particle's
  bool m_bInheritSize = false;     ///< multiplies the new particles' size by the raising particle's
};

class PL_PARTICLEPLUGIN_DLL plParticleEmitter_OnEvent final : public plParticleEmitter
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEmitter_OnEvent, plParticleEmitter);

public:
  plParticleEmitter_OnEvent();

  plTempHashedString m_sEventName;
  plUInt32 m_uiSpawnCountMin = 1;
  plUInt32 m_uiSpawnCountRange = 0;
  plTempHashedString m_sSpawnCountScaleParameter;

  plEnum<plParticleOnEventSpawn> m_SpawnMode;
  float m_fInheritVelocity = 0.0f;
  bool m_bInheritColor = false;
  bool m_bInheritSize = false;

  /// The particles of one event, spawned by the system around the event's position.
  struct Batch
  {
    plParticleEvent m_Event;
    plUInt32 m_uiCount = 0;
  };

  /// Per event: the batches of this step, filled by ComputeSpawnCount and spawned by the system after
  /// the step's processing.
  plArrayPtr<const Batch> GetBatches() const { return m_Batches; }
  void ClearBatches() { m_Batches.Clear(); }

  /// For the editor's stats: the events that spawned particles in the last step, and how many particles.
  plUInt32 GetLastEventCount() const { return m_uiLastEventCount; }
  plUInt32 GetLastSpawnCount() const { return m_uiLastSpawnCount; }

  /// The same since the system started, for rates.
  plUInt64 GetTotalEventCount() const { return m_uiTotalEventCount; }
  plUInt64 GetTotalSpawnCount() const { return m_uiTotalSpawnCount; }

  virtual void CreateRequiredStreams() override;

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  virtual plParticleEmitterState IsFinished() override;
  virtual plUInt32 ComputeSpawnCount(const plTime& tDiff) override;

  virtual void ProcessEventQueue(plParticleEventQueue queue) override;

  plUInt32 ComputeEventSpawnCount();

  bool m_bSpawn = false;
  plDynamicArray<plParticleEvent> m_PendingEvents; ///< per event: the matching events of this frame
  plDynamicArray<Batch> m_Batches;
  plUInt32 m_uiLastEventCount = 0;
  plUInt32 m_uiLastSpawnCount = 0;
  plUInt64 m_uiTotalEventCount = 0;
  plUInt64 m_uiTotalSpawnCount = 0;

  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
};

/// \brief Writes the Event Position, Event Velocity and Event Normal inputs of the particles a per-event
/// On Event spawns, before the Initialize blocks run. The system adds it when it has an On Event block;
/// particles spawned any other way read zero.
class PL_PARTICLEPLUGIN_DLL plParticleEventInputs final : public plParticleModule
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleEventInputs, plParticleModule);

public:
  plParticleEventInputs();

  virtual void CreateRequiredStreams() override {}
  virtual void QueryOptionalStreams() override;

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;
  virtual void Process(plUInt64 uiNumElements) override {}

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamNormal = nullptr;
};
