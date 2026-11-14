#pragma once

#include <Foundation/Containers/Map.h>
#include <GameEngineTest/TestClass/TestClass.h>
#include <ParticlesTest/GpuFluid/GpuFluidGalleryTest.h>

class plGpuFluidLiquidVolumeComponent;

/// \brief Liquid scenes of the GpuFluid gallery, with the numeric checks of the liquid solver.
///
/// - "Dam Break": Martin & Moyce's collapse of a water column (width a = 0.6 m, height 2a) in a closed tank 3.2 m long.
///   N5: particles == seeded - drained exactly on every readback, volume measure within +-3% on every tick of 600,
///   the pool depth from the particle centroid at the end (+-3% once the potential energy of the slosh still running at
///   20 s is taken out by equipartition, +-5% as it stands), and the centroid's tick-to-tick change over the last 5 s.
///   N2: relative residual of every tick's last solve. N7: the surge front against Martin & Moyce (1952), Table 2, at
///   T = t sqrt(2g/a) = 1, 2 and 3 (t the simulated time).
/// - "Hydrostatic Tank": a tank about half full, stratified seed (exactly n0 per cell), the surface half way up a
///   cell, the substep count switched between 2 and 5. N6: max |v| after 300 ticks; volume and centroid on every
///   tick; N2. With exactly n0 particles per cell this is a discrete equilibrium the solve keeps; it checks that dt
///   changes and a partial surface layer do not disturb it.
/// - "Hydrostatic Tank Random Seed": the same tank with the particles anywhere in the box (cell counts vary), the
///   same checks, plus max |v| over the last 60 ticks. Mandatory (plan amendment A2). P7's classification (cells with
///   particles or phi < 0) failed it: the surface cells followed the particles' noise and their pressure pushed
///   sideways, so kicks of up to 1 m/s recurred. Since P8 liquid cells are phi < tau of the level set alone, and the
///   density correction is a smooth two-sided one, and it passes.
/// - "Dam Break Video": 8 s of the dam break as PNG frames, the rendered surface, when PL_GPUFLUID_VIDEO=1.
/// - "Pour Into Glass": a nozzle (an inflow of 0.92 l/s) pours into a glass of analytic box walls for 8 s, then the
///   water settles for 2 s. Particles == seeded - drained exactly, the seeded count against the flow rate, no NaN,
///   everything inside the glass at the end, the settled depth against the poured volume over the glass's inner area
///   (+-5%: a jet packing the pool), and a surface mesh on every tick without overflow.
/// - "Pour Into Glass Video": the same as PNG frames, when PL_GPUFLUID_VIDEO=1.
/// - "Medium Room": the plan's Medium reference load, an 8 x 6 m room filled to 0.5 m at 6.25 cm (128 x 96 x 64 cells,
///   393,216 particles) on the Medium tier's defaults (at most 3 substeps, slow motion beyond; the tier's MGPCG
///   iterations), starting from 0.75 m on one half and 0.25 m on the other so it sloshes. GPU time per Liquid phase
///   over frames 60..239 (MediumRoom_timings.csv: ns per particle-substep, solve per active brick-substep), N2
///   percentiles, N5, the substep cap.
/// - "No Liquid": the stage without a liquid volume; the liquid pass's own GPU passes (copies, back faces, surface) must
///   not run once the content flag of the earlier sub-tests has expired.
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
    MediumRoom,
    NoLiquid,
    HydrostaticTankRandomQuarter,
    HydrostaticTankRandomThreeQuarter,
    MediumRoomSubstepCost,
    FloodingRoom,
    FloodingRoomVideo,
    SleepAndWake,
    OpenSideOutflow,
    StaticSolids,
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
    float m_fMeanSquaredSpeed = 0.0f;
    float m_fResidualRatio = 0.0f; ///< max |residual| / max |rhs| of the tick's last substep
    float m_fCentroidZ = 0.0f;
    float m_fSimTime = 0.0f;       ///< seconds simulated up to this tick (less than tick * dt in slow motion)
    plUInt32 m_uiNaN = 0;
    plUInt32 m_uiOverflow = 0;
    plUInt32 m_uiAbove1cm = 0;          ///< particles faster than 1 cm/s
    plUInt32 m_uiSurfaceVertices = 0;   ///< surface-nets mesh of the frame
    plUInt32 m_uiSurfaceQuads = 0;
    plUInt32 m_uiSurfaceOverflow = 0;
    plUInt32 m_uiAwakeBricks = 0;
    plUInt32 m_uiSleepingBricks = 0;
    plUInt32 m_uiInSolid = 0;
    plUInt32 m_uiMaxCellParticles = 0;
    plVec3U32 m_vMaxCell = plVec3U32::MakeZero();
    plUInt32 m_uiMaxCellType = 0;
    float m_fMaxCellPhi = 0.0f;
    float m_fMaxCellSdf = 0.0f;
    plUInt32 m_uiSolidCells = 0;
    float m_fProbes[4] = {};   ///< water depth from the particles of each probe's 3 x 3 column (m)
    float m_fProbePhi[4] = {}; ///< the same from the level set's zero crossing
  };

  /// A static solid of the scenes with baked solids: a box (world centre, full size, rotation), drawn and handed to the
  /// liquid's bake through plGpuFluidTestSolidComponent (the plMsgExtractGeometry contract of a Jolt static actor).
  struct BakedBox
  {
    plVec3 m_vCenter = plVec3::MakeZero();
    plVec3 m_vSize = plVec3(1.0f);
    plQuat m_qRotation = plQuat::MakeIdentity();
  };

  /// Signed distance (m) from a world point to a baked box, negative inside.
  static float BoxDistance(const BakedBox& box, const plVec3& vPoint);

  /// Cells of the liquid domain (world origin of its min corner, cells, cell size) whose centre the bake must make
  /// solid: inside a box or closer to its surface than the minimum thickness's half (the union of the boxes).
  static plUInt32 CountExpectedSolidCells(const plDynamicArray<BakedBox>& boxes, const plVec3& vDomainMin, const plVec3U32& vCells, float fCellSize);

  /// Free water volume below height fHeight over the domain's floor (world z fFloor), the boxes taken out (grown by the
  /// minimum thickness's half), by sampling the floor on a fine grid.
  static float FreeVolumeBelow(const plDynamicArray<BakedBox>& boxes, const plVec3& vDomainMin, const plVec2& vFloorSize, float fGrow, float fHeight);

  /// Adds a box to the bake (and draws it when color.a > 0).
  void AddBakedBox(const BakedBox& box, const plColor& color);

  plResult SetupStaticSolids();
  plResult SetupFloodingRoom(bool bVideo);
  plResult SetupSleepAndWake();
  plResult SetupOpenSideOutflow();
  plTestAppRun RunStaticSolids();
  plTestAppRun RunFloodingRoom(bool bVideo);
  plTestAppRun RunSleepAndWake();
  plTestAppRun RunOpenSideOutflow();
  void EvaluateFloodingRoom();

  /// Flood level the inflow's volume gives at the given simulated pouring time, from FreeVolumeBelow.
  float ExpectedFloodLevel(float fVolume) const;

  // Scenes with baked solids: the boxes, the component that hands them to the bake, the expected solid cells.
  plDynamicArray<BakedBox> m_BakedBoxes;
  plGameObjectHandle m_hSolidsObject;
  plUInt32 m_uiExpectedSolidCells = 0;
  plVec3 m_vDomainMin = plVec3::MakeZero(); ///< world position of the liquid domain's min corner
  plVec3U32 m_vDomainCells = plVec3U32::MakeZero();
  float m_fDomainCellSize = 0.0f;

  // Static Solids: the single-sided wall's cell (x) and the frame of the rebake.
  plUInt32 m_uiWallCell = 0;
  plUInt32 m_uiMaxParticleCellX = 0;
  plUInt32 m_uiMaxInSolid = 0;
  plInt32 m_iRebakeFrame = -1;

  // Flooding Room: the inflow's flow (m^3/s), when it ran (ticks), when the drain opened, the peak particle count.
  float m_fFloodFlow = 0.0f;
  plUInt32 m_uiFloodStartTick = 0;
  plUInt32 m_uiFloodStopTick = 0;
  plUInt32 m_uiDrainTick = 0;
  plUInt32 m_uiFloodEndTick = 0;
  bool m_bFloodStarted = false;
  bool m_bFloodStopped = false;
  bool m_bDrainOpened = false;
  float m_fFloodStartSim = -1.0f;
  float m_fFloodStopSim = -1.0f;
  plUInt32 m_uiPeakAlive = 0;
  plUInt32 m_uiFloodProbeCount = 0;

  // Sleep And Wake: the frame the volume was first seen asleep, and the inflow's start.
  plInt32 m_iAsleepFrame = -1;
  plInt32 m_iWakeFrame = -1;
  plUInt64 m_uiTicksAsleepAtWake = 0;
  plUInt32 m_uiSleepingBricksSeen = 0;
  plUInt32 m_uiAliveAtSleep = 0;
  bool m_bWokeUp = false;

  plResult SetupDamBreak();
  /// fDepthCells: the fill height in cells; its fraction is how full the top layer is.
  plResult SetupHydrostatic(bool bRandomSeed, float fDepthCells = 16.5f);
  plResult SetupPour();
  plResult SetupMediumRoom();
  plResult SetupNoLiquid();
  plGpuFluidLiquidVolumeComponent* GetVolume();

  /// The liquid scenes' stage: a dark floor, a checker of fTile squares over the given area (x/y centre and half
  /// size) for the refraction to show, the sun and the sky.
  void AddLiquidStage(const plVec2& vCheckerCenter, const plVec2& vCheckerHalfSize, float fTile, const plVec3& vToSun);

  plTestAppRun RunDamBreak(bool bVideo);
  plTestAppRun RunHydrostatic(bool bRandomSeed);
  plTestAppRun RunPour(bool bVideo);
  plTestAppRun RunMediumRoom();
  plTestAppRun RunMediumRoomCost();
  plTestAppRun RunNoLiquid();

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
  void EvaluateMediumRoom();

  /// Median, 95th percentile and maximum of the relative residual (N2) over the ticks from uiFirstTick on.
  void ReportResiduals(plStringView sName, plUInt32 uiFirstTick, float& out_fMedian, float& out_fP95, float& out_fMax) const;

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
  plUInt32 m_uiTimedActiveBricks = 0;

  // No Liquid: when the sub-test started, and the frame the liquid pass's stats were removed (-1: not yet).
  plTime m_NoLiquidStart;
  plInt32 m_iNoLiquidClearedFrame = -1;

  // Pour Into Glass: the glass's inside in domain cells, for the containment check.
  plVec3 m_vGlassMin = plVec3::MakeZero();
  plVec3 m_vGlassMax = plVec3::MakeZero();
  bool m_bPourStopped = false; ///< the nozzle was switched off for the settling ticks
  plGpuFluidLiquidStats m_LastStats;
  plString m_sHydroName; ///< shot and CSV name of the running hydrostatic sub-test

  // Medium Room Substep Cost: per segment of a forced substep count, the wall time of each frame (V-sync off), the
  // GPU time of the whole frame ("RENDER FRAME") and the sum of the substep phases' pass timings.
  struct CostSegment
  {
    plUInt32 m_uiSubsteps = 0;
    plDynamicArray<double> m_WallMs;
    plDynamicArray<double> m_FrameGpuMs;
    plDynamicArray<double> m_PhaseMs;
  };
  plHybridArray<CostSegment, 4> m_CostSegments;
  plTime m_LastFrameTime;
  bool m_bVSyncWas = false;
  bool m_bRestoreVSync = false;
};
