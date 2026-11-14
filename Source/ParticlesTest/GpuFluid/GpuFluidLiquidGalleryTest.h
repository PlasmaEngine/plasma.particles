#pragma once

#include <Foundation/Containers/Map.h>
#include <GameEngineTest/TestClass/TestClass.h>
#include <ParticlesTest/GpuFluid/GpuFluidGalleryTest.h>

class plGpuFluidLiquidVolumeComponent;

/// \brief Liquid scenes of the GpuFluid gallery, with the numeric checks of the liquid solver.
///
/// - "Dam Break": Martin & Moyce's collapse of a water column (width a = 0.6 m, height 2a) in a closed tank 3.2 m long.
///   N5: particles == seeded - drained exactly on every readback, volume measure within +-3% over 600 ticks. N7: the
///   surge front against Martin & Moyce (1952), Table 2, at T = t sqrt(2g/a) = 1, 2 and 3.
/// - "Hydrostatic Tank": a tank half full at rest. N6: max |v| after 300 ticks.
/// - "Dam Break Video": 8 s of the dam break as PNG frames, when PL_GPUFLUID_VIDEO=1.
///
/// Uses the gallery's application (FluidGallery project); writes shots and CSVs under GpuFluidGallery/Liquid/.
class plGameEngineTestGpuFluidLiquidGallery : public plGameEngineTest
{
  using SUPER = plGameEngineTest;

public:
  virtual const char* GetTestName() const override;
  virtual plGameEngineTestApplication* CreateApplication() override;

protected:
  enum SubTests
  {
    DamBreak,
    HydrostaticTank,
    DamBreakVideo,
  };

  virtual void SetupSubTests() override;
  virtual plResult InitializeSubTest(plInt32 iIdentifier) override;
  virtual plResult DeInitializeSubTest(plInt32 iIdentifier) override;
  virtual plTestAppRun RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount) override;

private:
  struct TickSample
  {
    plUInt32 m_uiAlive = 0;
    plUInt32 m_uiSeeded = 0;
    plUInt32 m_uiDrained = 0;
    plUInt32 m_uiLost = 0;
    plUInt32 m_uiSubsteps = 0;
    plUInt32 m_uiActiveBricks = 0;
    float m_fVolumeRatio = 0.0f;
    float m_fFront = -1.0f;       ///< metres from the tank's left wall, -1 when unknown
    float m_fMaxSpeed = 0.0f;
    float m_fMeanSpeed = 0.0f;
    float m_fResidualRatio = 0.0f; ///< max |residual| / max |rhs| of the tick's last substep
    float m_fCentroidZ = 0.0f;
    plUInt32 m_uiNaN = 0;
    plUInt32 m_uiOverflow = 0;
  };

  plResult SetupDamBreak();
  plResult SetupHydrostatic();
  plGpuFluidLiquidVolumeComponent* GetVolume();

  plTestAppRun RunDamBreak(bool bVideo);
  plTestAppRun RunHydrostatic();

  /// Reads the newest stats and files them under their tick. False when nothing new arrived.
  bool CollectStats();

  /// Front of the surge from the histogram: the far edge of the last bin with enough particles.
  float ComputeFront(const plGpuFluidLiquidStats& stats) const;

  void AccumulateTimings();
  void ReportTimings(plStringBuilder& ref_sCsv);

  void EvaluateDamBreak();
  void EvaluateHydrostatic();

  plResult SaveShot(plStringView sName);

  plGpuFluidGalleryApplication* m_pOwnApplication = nullptr;
  plGameObjectHandle m_hVolumeObject;
  plInt32 m_iFrame = -1;
  bool m_bDepthOfFieldWasEnabled = true;
  bool m_bRestoreDepthOfField = false;
  bool m_bGpuStatsWasEnabled = false;

  plUInt64 m_uiLastStatsFrame = 0;
  plMap<plUInt32, TickSample> m_Ticks;
  plUInt32 m_uiLastTick = 0;
  plUInt32 m_uiN5Mismatches = 0;
  plUInt32 m_uiReadbacks = 0;
  float m_fColumnWidth = 0.6f;

  // GPU time per phase, summed over the measured frames.
  plMap<plString, double> m_PhaseMs;
  plUInt32 m_uiTimedFrames = 0;
  plUInt32 m_uiTimedParticles = 0;
  plUInt32 m_uiTimedSubsteps = 0;
};
