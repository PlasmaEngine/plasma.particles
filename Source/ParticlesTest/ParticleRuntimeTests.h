#pragma once

#include <ParticlesTest/ParticleTests.h>

class plParticleEffectDescriptor;

/// Runtime checks that need no reference images: each sub-test builds its effect in code, places it
/// in the particle test scene and inspects the live effect instance after a number of frames.
class plGameEngineTestParticleRuntime : public plGameEngineTest
{
  using SUPER = plGameEngineTest;

public:
  virtual const char* GetTestName() const override;
  virtual plGameEngineTestApplication* CreateApplication() override;

private:
  enum SubTests
  {
    SharedEffectDefaults,
    BehaviorOrder,
    FixedTick,
    GPUEffectFinishes,
    FrameProgramParameter,
    ParticleProgramKill,
    ParticleProgramCustomAttribute,
    ParticleProgramCost,
    ParticleProgramSampleAssets,
    ConvertedLogicBlocks,
    PerParticleProperty,
    PreviewResimulation,
    GPUProgramReference,
    GPUProgramLowering,
    GPUProgramOnGPU,
    GPUSortAliveCount,
    GPUSpyReadback,
    EventSpawnPerEvent,
    EventInherit,
    EventQueueDrops,
    EventOncePerFrame,
    GPUEvents,
    BlackboardEntry,
    VelocityConeAxis,
    LodUpdateInterval,

    // artist requests (1.9.0, ParticleArtistRequestTests.cpp)
    SizePerAxis,
    NewSinkTargets,
    LookSettings,
    GPULoweringNewFeatures,
    DistortionPipeline,

    // P7 measurements: they always pass and append their numbers to the file PL_PARTICLES_BENCH names
    BenchMassDeaths,
    BenchRaycast,
    BenchFarLod,
    BenchMutex,
    BenchTestingChambers,
  };

  virtual void SetupSubTests() override;
  virtual plResult InitializeSubTest(plInt32 iIdentifier) override;
  virtual plTestAppRun RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount) override;

  plTestAppRun RunFrameProgramParameter();
  plTestAppRun RunPreviewResimulation();
  void MeasureProgramCost();

  void CheckGPUProgramReference();
  void CheckGPUProgramLowering();
  void SetupGPUProgramOnGPU(plParticleEffectDescriptor& ref_desc);
  plTestAppRun RunGPUProgramOnGPU();
  plTestAppRun RunGPUSortAliveCount();
  plTestAppRun RunGPUSpyReadback();

  // events and chaining (P6)
  void SetupEventTest(plInt32 iIdentifier, plParticleEffectDescriptor& ref_desc);
  plTestAppRun RunEventTest(plInt32 iIdentifier);
  plTestAppRun RunBlackboardEntry();
  void CheckOnEventVersion2();
  plUInt32 m_uiMaxEventsRaised = 0;
  plUInt32 m_uiMaxEventsDropped = 0;
  plUInt32 m_uiEventLatency = 0;
  bool m_bSparksLowered = false;

  // artist requests (ParticleArtistRequestTests.cpp)
  void SetupArtistRequestTest(plInt32 iIdentifier, plParticleEffectDescriptor& ref_desc);
  plTestAppRun RunArtistRequestTest(plInt32 iIdentifier);
  void CheckLookSettings();
  void CheckGPULoweringNewFeatures();
  void CheckDistortionPipeline();

  // performance (P7, ParticleRuntimeBenchmarks.cpp)
  void SetupBenchmark(plInt32 iIdentifier, plParticleEffectDescriptor& ref_desc);
  plTestAppRun RunBenchmark(plInt32 iIdentifier);
  void StartTestingChambersEffects();
  void MoveFarLodCopy();
  plTestAppRun RunLodUpdateInterval();
  void WriteBenchmarkResult(plStringView sName, double fAvgMs, double fMaxMs, plUInt32 uiSamples) const;

  struct BenchSeries
  {
    plString m_sName;
    double m_fSum = 0.0;
    double m_fMax = 0.0;
    plUInt32 m_uiSamples = 0;
    plUInt64 m_uiLastCount = 0;
  };
  plDynamicArray<BenchSeries> m_BenchSeries;
  plDynamicArray<plComponentHandle> m_BenchComponents;

  /// The lowered type of the effect's first system, or null (with a test failure).
  const class plParticleTypeGPU* GetLoweredType() const;

  void SetupConvertedLogicBlocks(plParticleEffectDescriptor& ref_desc);
  void CheckConvertedLogicBlocks();
  void CheckConditionalKillVersion1();

  void StartEffect(const plParticleEffectDescriptor& desc, plStringView sSharedInstanceName);
  class plParticleEffectInstance* GetEffect() const;

  plInt32 m_iFrame = 0;
  plUInt32 m_uiEffectCounter = 0;
  plComponentHandle m_hEffectComponent;
  plGameEngineTestApplication_Particles* m_pOwnApplication = nullptr;
};
