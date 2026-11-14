#pragma once

#include <Foundation/Containers/Map.h>
#include <GameEngineTest/TestClass/TestClass.h>
#include <ParticlesTest/GpuFluid/GpuFluidGalleryTest.h>

class plGpuFluidLiquidVolumeComponent;

/// \brief Liquid scenes of the GpuFluid gallery, with the numeric checks of the liquid solver.
///
/// - "Dam Break": Martin & Moyce's collapse of a water column (width a = 0.6 m, height 2a) in a closed tank 3.2 m long.
///   N5: particles == seeded - drained exactly on every readback, volume measure within +-3% on every tick of 600,
///   the pool depth from the particle centroid at the end, and the centroid's tick-to-tick change over the last 5 s.
///   N2: relative residual of every tick's last solve. N7: the surge front against Martin & Moyce (1952), Table 2, at
///   T = t sqrt(2g/a) = 1, 2 and 3 (t the simulated time).
/// - "Hydrostatic Tank": a tank about half full, stratified seed (exactly n0 per cell), the surface half way up a
///   cell, the substep count switched between 2 and 5. N6: max |v| after 300 ticks; volume and centroid on every
///   tick; N2. With exactly n0 particles per cell this is a discrete equilibrium the solve keeps; it checks that dt
///   changes and a partial surface layer do not disturb it.
/// - "Hydrostatic Tank Random Seed": the same tank with the particles anywhere in the box (cell counts vary), the
///   same checks. Runs only with PL_GPUFLUID_LIQ_STRICT_N6=1: it fails in P6 (empty cells inside the pool are
///   classified air, and the hydrostatic pressure drives a jet into each; the level-set classification fixes that).
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
    HydrostaticTankRandom,
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
    float m_fSimTime = 0.0f;       ///< seconds simulated up to this tick (less than tick * dt in slow motion)
    plUInt32 m_uiNaN = 0;
    plUInt32 m_uiOverflow = 0;
  };

  plResult SetupDamBreak();
  plResult SetupHydrostatic(bool bRandomSeed);
  plGpuFluidLiquidVolumeComponent* GetVolume();

  plTestAppRun RunDamBreak(bool bVideo);
  plTestAppRun RunHydrostatic(bool bRandomSeed);

  /// Reads the newest stats and files them under their tick. False when nothing new arrived.
  bool CollectStats();

  /// Front of the surge from the histogram: the far edge of the last bin with enough particles.
  float ComputeFront(const plGpuFluidLiquidStats& stats) const;

  void AccumulateTimings();
  void ReportTimings(plStringBuilder& ref_sCsv);

  void EvaluateDamBreak();
  void EvaluateHydrostatic(bool bRandomSeed);

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
