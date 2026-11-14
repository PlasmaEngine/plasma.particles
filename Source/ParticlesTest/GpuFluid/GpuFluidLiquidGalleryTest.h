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
///   same checks, plus max |v| over the last 60 ticks. Mandatory (plan amendment A2) and known to fail with the
///   default classification (cells with particles or phi < 0): the surface cells' classification follows the
///   particles' noise, and the pressure of a cell that is liquid beside one that is air pushes sideways, so the
///   surface keeps moving and kicks of up to about 1 m/s recur. Liquid cells from phi < 0 alone with a particle radius
///   of 0.65 cells (PL_GPUFLUID_LIQ_LEVEL_SET_ONLY=1 PL_GPUFLUID_LIQ_PARTICLE_RADIUS=0.65) pass it, but the dam break
///   then keeps sloshing at 0.27 m/s mean after 20 s and its rest-depth check fails.
/// - "Dam Break Video": 8 s of the dam break as PNG frames, the rendered surface, when PL_GPUFLUID_VIDEO=1.
/// - "Pour Into Glass": a nozzle (an inflow of 0.92 l/s) pours into a glass of analytic box walls for 8 s, then the
///   water settles for 2 s. Particles == seeded - drained exactly, the seeded count against the flow rate, no NaN,
///   everything inside the glass at the end, the settled depth against the poured volume over the glass's inner area
///   (+-5%: a jet packing the pool), and a surface mesh on every tick without overflow.
/// - "Pour Into Glass Video": the same as PNG frames, when PL_GPUFLUID_VIDEO=1.
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
    PourIntoGlass,
    PourIntoGlassVideo,
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
    plUInt32 m_uiAbove1cm = 0;          ///< particles faster than 1 cm/s
    plUInt32 m_uiSurfaceVertices = 0;   ///< surface-nets mesh of the frame
    plUInt32 m_uiSurfaceQuads = 0;
    plUInt32 m_uiSurfaceOverflow = 0;
  };

  plResult SetupDamBreak();
  plResult SetupHydrostatic(bool bRandomSeed);
  plResult SetupPour();
  plGpuFluidLiquidVolumeComponent* GetVolume();

  /// The liquid scenes' stage: a dark floor, a checker of fTile squares over the given area (x/y centre and half
  /// size) for the refraction to show, the sun and the sky.
  void AddLiquidStage(const plVec2& vCheckerCenter, const plVec2& vCheckerHalfSize, float fTile, const plVec3& vToSun);

  plTestAppRun RunDamBreak(bool bVideo);
  plTestAppRun RunHydrostatic(bool bRandomSeed);
  plTestAppRun RunPour(bool bVideo);

  /// Saves frame m_iFrame - 1 of a video sub-test (the screenshot of this Run()) and its caption.
  void SaveVideoFrame(plStringView sFolder, plStringView sTitle);

  /// Surface mesh numbers over the ticks: largest vertex and quad counts, and how many ticks had none or overflowed.
  void ReportSurface(plStringView sName, plUInt32 uiFirstTick);

  /// Reads the newest stats and files them under their tick. False when nothing new arrived.
  bool CollectStats();

  /// Front of the surge from the histogram: the far edge of the last bin with enough particles.
  float ComputeFront(const plGpuFluidLiquidStats& stats) const;

  void AccumulateTimings();
  void ReportTimings(plStringBuilder& ref_sCsv);

  void EvaluateDamBreak();
  void EvaluateHydrostatic(bool bRandomSeed);
  void EvaluatePour();

  plResult SaveShot(plStringView sName);

  plGpuFluidGalleryApplication* m_pOwnApplication = nullptr;
  plGameObjectHandle m_hVolumeObject;
  plInt32 m_iFrame = -1;
  bool m_bDepthOfFieldWasEnabled = true;
  bool m_bRestoreDepthOfField = false;
  bool m_bGpuStatsWasEnabled = false;
  bool m_bVirtualShadowsWereEnabled = true;
  int m_iTransparencyModeWas = 4;

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
  plUInt32 m_uiTimedQuads = 0;

  // Pour Into Glass: the glass's inside in domain cells, for the containment check.
  plVec3 m_vGlassMin = plVec3::MakeZero();
  plVec3 m_vGlassMax = plVec3::MakeZero();
  bool m_bPourStopped = false; ///< the nozzle was switched off for the settling ticks
  plGpuFluidLiquidStats m_LastStats;
};
