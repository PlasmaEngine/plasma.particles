#pragma once

#include <ParticlePlugin/Behavior/ParticleBehavior_Raycast.h> // plParticleRaycastHitReaction
#include <ParticlePlugin/System/ParticleProgram.h>
#include <ParticlePlugin/Type/GPU/ParticleGPUProgram.h>
#include <ParticlePlugin/Type/ParticleType.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h> // plQuadParticleOrientation
#include <RendererFoundation/RendererFoundationDLL.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/GPUParticleData.h>

struct plGPUParticleSystemInfo;
class plGALReadbackBufferHelper;

using plTexture2DResourceHandle = plTypedResourceHandle<class plTexture2DResource>;
using plTexture3DResourceHandle = plTypedResourceHandle<class plTexture3DResource>;

class PL_PARTICLEPLUGIN_DLL plParticleTypeGPUFactory final : public plParticleTypeFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeGPUFactory, plParticleTypeFactory);

public:
  virtual const plRTTI* GetTypeType() const override;
  virtual void CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plUInt32 m_uiMaxParticles = 10000;
  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plString m_sTexture;

  float m_fGravity = 9.81f;
  float m_fDragCoefficient = 0.0f;
  float m_fWindStrength = 0.0f;

  bool m_bEnableDepthCollision = false;
  bool m_bEnableSDFCollision = false;
  plEnum<plParticleRaycastHitReaction> m_CollisionReaction;
  float m_fCollisionBounceFactor = 0.5f;
  float m_fCollisionSlideFactor = 0.5f;
  float m_fCollisionThickness = 0.5f;

  plColor m_ColorStart = plColor::White;
  plColor m_ColorEnd = plColor(1, 1, 1, 0);

  plEnum<plGPUParticleRenderType> m_GPURenderType;
  plUInt32 m_uiMaxTrailPoints = 16;
  float m_fVelocityStretch = 1.0f;
};

class PL_PARTICLEPLUGIN_DLL plParticleTypeGPU final : public plParticleType
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeGPU, plParticleType);

public:
  plParticleTypeGPU();
  ~plParticleTypeGPU();

  virtual void CreateRequiredStreams() override;
  virtual void QueryOptionalStreams() override;
  virtual bool HasActiveGPUElements() const override;
  virtual float GetMaxParticleRadius(float fParticleSize) const override;
  virtual void ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const override;

  plUInt32 m_uiMaxGPUParticles = 10000;
  plEnum<plParticleTypeRenderMode> m_RenderMode;
  plTexture2DResourceHandle m_hTexture;

  float m_fGravity = 9.81f;
  float m_fDragCoefficient = 0.0f;
  float m_fWindStrength = 0.0f;

  bool m_bEnableDepthCollision = false;
  bool m_bEnableSDFCollision = false;
  plEnum<plParticleRaycastHitReaction> m_CollisionReaction;
  float m_fCollisionBounceFactor = 0.5f;
  float m_fCollisionSlideFactor = 0.5f;
  float m_fCollisionThickness = 0.5f;

  plColor m_ColorStart = plColor::White;
  plColor m_ColorEnd = plColor(1, 1, 1, 0);

  plEnum<plGPUParticleRenderType> m_GPURenderType;
  plUInt32 m_uiMaxTrailPoints = 16;
  float m_fVelocityStretch = 1.0f;

  // ---- lowered-module state, configured by the CPU->GPU lowering (SimulationTarget GPU/Auto) ----
  // GPU_PARTICLE_FEATURE_* bits selecting which lowered modules run in the simulate shader
  plUInt32 m_uiFeatureFlags = 0;
  bool m_bLowered = false;          // when set, gravity comes from the physics module * factor
  float m_fGravityFactor = 1.0f;
  float m_fRiseSpeed = 0.0f;
  float m_fFriction = 0.0f;
  float m_fLinearDrag = 0.0f;
  float m_fFadeStartAlpha = 1.0f;
  float m_fFadeExponent = 1.0f;
  float m_fColorMaxSpeed = 1.0f;
  float m_fSizeBase = 0.0f;
  float m_fSizeScale = 1.0f;
  plVec3 m_vTurbScrollSpeed = plVec3::MakeZero();
  float m_fTurbStrength = 0.0f;
  float m_fTurbFrequency = 1.0f;
  plUInt8 m_uiTurbOctaves = 1;
  plVec3 m_vVectorFieldSize = plVec3(4.0f);
  plVec3 m_vVectorFieldOffset = plVec3::MakeZero();
  float m_fVectorFieldStrength = 1.0f;
  plVec3 m_vBoundsOffset = plVec3::MakeZero();
  plVec3 m_vBoundsExtents = plVec3(2.0f);
  // attractor: the target and axis are effect-relative when the attractor targets the effect origin
  bool m_bAttractorAtEffectOrigin = false;
  plVec3 m_vAttractorPos = plVec3::MakeZero();
  plVec3 m_vAttractorAxis = plVec3(0, 0, 1);
  float m_fAttractorForce = 0.0f;
  float m_fAttractorMaxDist = 10.0f;
  float m_fAttractorMinDist = 0.1f;
  plUInt8 m_uiAttractorShape = 0;
  float m_fPullAlongStrength = 0.0f;
  plVec3 m_vSphereOffset = plVec3::MakeZero();
  float m_fSphereRadius = 3.0f;
  bool m_bSceneForces = false;
  float m_fSceneForceInfluence = 1.0f;
  plTempHashedString m_sSceneForceParameter;
  bool m_bFlies = false;
  float m_fFliesSpeed = 0.2f;
  float m_fFliesPathLength = 0.2f;
  float m_fFliesMaxEmitterDistance = 0.5f;
  plAngle m_FliesMaxSteeringAngle;
  plTexture2DResourceHandle m_hGradientLUT;
  plTexture2DResourceHandle m_hSizeCurveLUT;
  plTexture3DResourceHandle m_hTurbulenceNoise;
  plTexture3DResourceHandle m_hVectorFieldTexture;

  /// The Update blocks' graph programs, run by the simulate kernel, and what their per-particle values are scaled by.
  plParticleGPUProgramSet m_Programs;
  float m_fProgramDragScale = 1.0f;
  float m_fProgramForceSign = 1.0f;

  /// The blocks the programs were built from; the spy rebuilds them with its own program added.
  plDynamicArray<const plParticleBehaviorFactory*> m_LoweredBehaviors;

  /// Which CPU renderer draws this lowered system from structs the simulate dispatch writes.
  /// Legacy means the authored GPU type's own render path instead.
  enum class CPURenderPath : plUInt8
  {
    Legacy,
    Quad,
    Point,
    Trail,
  };

  CPURenderPath m_CPURenderPath = CPURenderPath::Legacy;

  // ---- quad rendering: a lowered quad system is drawn by the ordinary quad renderer, from
  // per-particle structs the simulate dispatch writes, so it keeps the full quad feature set ----
  bool m_bRenderAsQuad = false;
  plEnum<plQuadParticleOrientation> m_QuadOrientation;
  plEnum<plParticleTextureAtlasType> m_TextureAtlasType;
  plUInt8 m_uiNumSpritesX = 1;
  plUInt8 m_uiNumSpritesY = 1;
  plTempHashedString m_sTintColorParameter;
  plTexture2DResourceHandle m_hDistortionTexture;
  plTexture2DResourceHandle m_hSixWayMapA;
  plTexture2DResourceHandle m_hSixWayMapB;
  float m_fSixWayAbsorption = 1.0f;
  float m_fDistortionStrength = 0;
  float m_fStretch = 1;
  plEnum<plParticleLightingMode> m_LightingMode;
  float m_fNormalCurvature = 0.5f;
  float m_fLightDirectionality = 0.5f;
  plMaterialResourceHandle m_hCustomMaterial;
  plParticleLook m_Look;

  virtual void RequestRequiredWorldModulesForCache(plParticleWorldModule* pParticleModule) override;

  /// \name Value spy
  /// The particle editor's value spy on a system that runs on the GPU: the kernel computes the program's
  /// value for every particle, and it is read back about four times a second, a few frames late.
  ///@{

  /// Starts spying with a program whose output is named "Spy", or stops with null.
  void SetSpyProgram(const plParticleProgramData* pProgram);
  bool IsSpying() const { return !m_SpyProgram.IsEmpty(); }
  const plParticleProgramData* GetSpyProgram() const { return IsSpying() ? &m_SpyProgram : nullptr; }

  /// The values of the particles alive in the newest readback; false when none arrived since the last call.
  bool FetchSpyValues(plDynamicArray<float>& out_values);

  /// The range the drawn particles are tinted over (the last readback's min and max).
  void SetSpyRange(float fMin, float fMax);
  ///@}

  /// \name Events
  /// The kernel raises the system's death and collision events; a ring of readbacks brings them back
  /// and they reach the effect's queue a few frames late, as if the CPU had raised them.
  ///@{

  plParticleEventNames m_DeathEvents;   ///< the system's death events
  plParticleEventNames m_CollideEvents; ///< Raycast's collision events

  bool RaisesEvents() const { return !m_DeathEvents.IsEmpty() || !m_CollideEvents.IsEmpty(); }

  /// How many frames late the last events arrived, for the editor's stats; 0 before any did.
  plUInt32 GetEventLatencyFrames() const { return m_uiEventLatencyFrames; }
  ///@}

  /// \name GPU buffers, for tools and tests that read them back
  ///@{
  plGALBufferHandle GetParticleBuffer() const { return m_hParticleBuffer; }   ///< the pool of plGPUParticle
  plGALBufferHandle GetSortKeyBuffer() const { return m_hSortKeyBuffer; }     ///< a float per sort slot
  plGALBufferHandle GetSortArgsBuffer() const { return m_hSortArgsBuffer; }   ///< {groups, 1, 1, sorted count}
  plGALBufferHandle GetDrawArgsBuffer() const { return m_hDrawArgsBuffer; }   ///< {vertex count, 1, 0, 0}
  ///@}

protected:
  friend class plParticleTypeGPUFactory;

  virtual void Process(plUInt64 uiNumElements) override {}
  virtual void StepParticleSystem(const plTime& tDiff, plUInt32 uiNumNewParticles) override;

  // pull-along state, tracked exactly like the CPU behavior: the emitter movement per step
  bool m_bPullAlongFirstStep = true;
  plVec3 m_vLastEmitterPosition = plVec3::MakeZero();

  // flies picks a new heading only when its path-length timer elapses, same as the CPU behavior
  plTime m_FliesTimeToChangeDir;
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  void EnsureGPUBuffers() const;
  void DestroyGPUBuffers();
  void AddQuadRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const;
  void AddPointRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const;
  void AddTrailRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const;
  void AddGPURenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform, plUInt32 uiActiveSlotCount, plArrayPtr<plGPUParticle> newParticles) const;
  void QueueSimulation(const plTransform& instanceTransform, plUInt32 uiActiveSlotCount, plArrayPtr<plGPUParticle> newParticles, plArrayPtr<plUInt32> newSeeds) const;

  /// The graph programs' part of the hand-off: their words and this frame's data, in frame memory.
  void FillProgramInfo(plGPUParticleSystemInfo& ref_info) const;

  /// The events' part of the hand-off: the buffer, and a free readback of the ring for this frame's.
  void FillEventInfo(plGPUParticleSystemInfo& ref_info) const;

  /// Hands the events of the readbacks that arrived to the effect. Runs at extraction, when no update task
  /// touches the effect's queue.
  void ReceiveEvents() const;
  plArrayPtr<plGPUParticleForceField> GatherForceFields(const plTransform& instanceTransform) const;

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamLifeTime = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
  plProcessingStream* m_pStreamRotationSpeed = nullptr;
  plProcessingStream* m_pStreamRotationOffset = nullptr;
  plProcessingStream* m_pStreamAxis = nullptr;      // optional, drives the fixed quad orientations
  plProcessingStream* m_pStreamVariation = nullptr; // optional, texture atlas variation
  plProcessingStream* m_pStreamRandomSeed = nullptr; // only when a graph program reads the seed

  mutable plGALBufferHandle m_hParticleBuffer;
  mutable plGALBufferHandle m_hCounterBuffer;
  mutable plGALBufferHandle m_hAliveListBuffer;
  mutable plGALBufferHandle m_hDrawArgsBuffer;
  mutable plGALBufferHandle m_hQuadBaseBuffer;
  mutable plGALBufferHandle m_hQuadBillboardBuffer;
  mutable plGALBufferHandle m_hQuadTangentBuffer;
  mutable plGALBufferHandle m_hTrailDataBuffer;
  mutable plGALBufferHandle m_hTrailPointsPackedBuffer;
  mutable plGALBufferHandle m_hSortKeyBuffer;
  mutable plGALBufferHandle m_hTrailPositionBuffer;
  mutable plGALBufferHandle m_hSeedBuffer; // random seed per pool slot, for graph programs that read it
  mutable plGALBufferHandle m_hSortArgsBuffer; // indirect dispatch size of sort and scatter, from the alive count

  // the graph programs' words and data, and the spawns' seeds, which the particle pass updates every frame
  mutable plGALBufferHandle m_hProgramWordBuffer;
  mutable plGALBufferHandle m_hProgramDataBuffer;
  mutable plGALBufferHandle m_hSeedStagingBuffer;

  // value spy: the program, a float per slot (NaN when dead), and its readback
  plParticleProgramData m_SpyProgram;
  mutable plGALBufferHandle m_hSpyBuffer;
  mutable plUniquePtr<plGALReadbackBufferHelper> m_pSpyReadback;
  mutable bool m_bSpyReadbackPending = false;
  mutable plTime m_LastSpyReadback;
  float m_fSpyMin = 0.0f;
  float m_fSpyMax = 1.0f;

  // events: the buffer the kernel appends to, and the readbacks of the last frames' copies of it. A
  // readback is only looked at two frames after its request, when the render thread has recorded it.
  struct EventReadback
  {
    plUniquePtr<plGALReadbackBufferHelper> m_pHelper;
    plUInt64 m_uiRequestFrame = 0;
    plUInt64 m_hReceivedFence = 0; ///< the fence of the copy already handed on
    bool m_bRequested = false;
  };

  static constexpr plUInt32 s_uiNumEventReadbacks = 4;
  mutable plGALBufferHandle m_hEventBuffer;
  mutable EventReadback m_EventReadbacks[s_uiNumEventReadbacks];
  mutable plUInt32 m_uiEventLatencyFrames = 0;

  mutable bool m_bGPUBuffersCreated = false;
  mutable plUInt32 m_uiTrailWriteIndex = 0;

  void PackSpawnedParticles(plUInt64 uiStartIndex, plUInt64 uiNumElements);

  // Everything below accumulates over the simulation steps between two simulation hand-offs, so
  // fixed tick, catch-up steps and frames without extraction lose nothing.
  // Spawns are packed when they happen: the streams compact on every death, so an index range
  // would not survive until extraction.
  mutable plDynamicArray<plGPUParticle> m_PendingSpawns;
  mutable plDynamicArray<plUInt32> m_PendingSeeds; // one per pending spawn while the programs read seeds
  mutable plTime m_PendingSimulationTime;
  mutable plVec3 m_vPendingPull = plVec3::MakeZero();
  mutable bool m_bPendingFliesChangeDir = false;

  // effect time at which the last particle handed to the GPU runs out of life
  plTime m_LastParticleDeathTime;

  // longest total life of any particle handed to the GPU, in seconds; bounds the distance it can travel
  float m_fLongestLifeSeconds = 0.0f;

  // velocity-aligned quads stretch by the distance of one step, which must not collapse while paused
  plTime m_LastStepDuration;

  // the simulation is handed off once per frame, however many views extract the effect
  mutable plUInt64 m_uiLastQueuedFrame = plUInt64(-1);
  mutable plUInt32 m_uiActiveSlotCount = 0;
};