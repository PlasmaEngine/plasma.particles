#include <ParticlesTest/ParticlesTestPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/Utilities/Stats.h>
#include <GameEngine/GameState/GameState.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidLiquidVolumeComponent.h>
#include <ParticlesTest/GpuFluid/GpuFluidLiquidGalleryTest.h>

static plGameEngineTestGpuFluidLiquidGallery s_GameEngineTestGpuFluidLiquidGallery;

namespace
{
  const plVec3 s_vUp(0.0f, 0.0f, 1.0f);
  constexpr float s_fGravity = 9.81f;
  constexpr float s_fTickSeconds = 1.0f / 30.0f; // the test application's fixed step

  // Dam break: Martin & Moyce's two-dimensional collapse, column width a and height 2a (n^2 = 2), in a tank long enough
  // for the front to reach Z/a = 5.
  constexpr float s_fDamA = 0.6f;
  constexpr float s_fDamCellSize = 0.02f;
  const plVec3 s_vDamTank(3.2f, 0.64f, 1.28f);
  constexpr plUInt32 s_uiDamTicks = 600;
  constexpr plUInt32 s_uiVideoFrames = 240;

  // N5: the volume measure stays within this of the seeded volume.
  constexpr float s_fVolumeTolerance = 0.03f;

  // N7: the front within this fraction of Martin & Moyce.
  constexpr float s_fFrontTolerance = 0.10f;

  // Front: particles in the bottom two cells, binned along x in quarter cells; a bin counts with at least this many.
  constexpr float s_fFrontHeight = 2.0f * s_fDamCellSize;
  constexpr float s_fFrontBin = 0.25f * s_fDamCellSize;
  // A full bin holds about 64; the threshold keeps loose spray ahead of the water from counting as the front.
  constexpr plUInt32 s_uiFrontMinCount = 10;
  constexpr plUInt32 s_uiFrontGapBins = 4;

  // Hydrostatic tank: half full, at rest.
  const plVec3 s_vHydroTank(1.12f, 0.64f, 0.64f);
  constexpr float s_fHydroCellSize = 0.02f;
  constexpr plUInt32 s_uiHydroTicks = 300;
  constexpr float s_fHydroMaxSpeed = 0.01f;

  // Martin & Moyce 1952, Part IV, Table 2 (rectangular section, plane symmetry, n^2 = 2), a = 2 1/4 in, the "mean"
  // column: Z/a against T = n t sqrt(g/a) = t sqrt(2g/a). The paper shifts T of every run so that Z/a = 1.44 at the
  // tabulated T (the release time of the column was not known exactly).
  struct MMPoint
  {
    float m_fT;
    float m_fZ;
  };
  constexpr MMPoint s_MartinMoyce[] = {{0.41f, 1.11f}, {0.84f, 1.22f}, {1.19f, 1.44f}, {1.43f, 1.67f}, {1.63f, 1.89f}, {1.83f, 2.11f}, {1.98f, 2.33f}, {2.20f, 2.56f},
    {2.32f, 2.78f}, {2.51f, 3.00f}, {2.65f, 3.22f}, {2.83f, 3.44f}, {2.97f, 3.67f}, {3.11f, 3.89f}, {3.33f, 4.11f}};

  float MartinMoyceZ(float fT)
  {
    const plUInt32 n = PL_ARRAY_SIZE(s_MartinMoyce);
    if (fT <= s_MartinMoyce[0].m_fT)
      return 1.0f + (s_MartinMoyce[0].m_fZ - 1.0f) * plMath::Max(fT, 0.0f) / s_MartinMoyce[0].m_fT;
    for (plUInt32 i = 1; i < n; ++i)
    {
      if (fT <= s_MartinMoyce[i].m_fT)
      {
        const float f = (fT - s_MartinMoyce[i - 1].m_fT) / (s_MartinMoyce[i].m_fT - s_MartinMoyce[i - 1].m_fT);
        return plMath::Lerp(s_MartinMoyce[i - 1].m_fZ, s_MartinMoyce[i].m_fZ, f);
      }
    }
    return s_MartinMoyce[n - 1].m_fZ;
  }

  constexpr const char* s_szDepthOfFieldCVar = "Rendering.DepthOfField.Enable";
  constexpr const char* s_szGpuStatsCVar = "Rendering.Profiling.GPUStats";

  bool GetCVarBool(const char* szName, bool bDefault)
  {
    if (auto* pCVar = static_cast<plCVarBool*>(plCVar::FindCVarByName(szName)))
      return pCVar->GetValue();
    return bDefault;
  }

  void SetCVarBool(const char* szName, bool bValue)
  {
    if (auto* pCVar = static_cast<plCVarBool*>(plCVar::FindCVarByName(szName)))
      *pCVar = bValue;
  }

  const char* s_szPhases[] = {"GpuFluid Liquid/Begin", "GpuFluid Liquid/Seed", "GpuFluid Liquid/Sort", "GpuFluid Liquid/P2G", "GpuFluid Liquid/Solve", "GpuFluid Liquid/G2P",
    "GpuFluid Liquid/Render", "GpuFluid Liquid/Readback"};

  void WriteText(plStringView sPath, const plStringBuilder& sText)
  {
    plFileWriter file;
    if (file.Open(sPath).Succeeded())
      file.WriteBytes(sText.GetData(), sText.GetElementCount()).IgnoreResult();
  }
} // namespace

const char* plGameEngineTestGpuFluidLiquidGallery::GetTestName() const
{
  return "GpuFluid Liquid Gallery";
}

plGameEngineTestApplication* plGameEngineTestGpuFluidLiquidGallery::CreateApplication()
{
  m_pOwnApplication = PL_DEFAULT_NEW(plGpuFluidGalleryApplication);
  return m_pOwnApplication;
}

void plGameEngineTestGpuFluidLiquidGallery::SetupSubTests()
{
  AddSubTest("Dam Break", SubTests::DamBreak);
  AddSubTest("Hydrostatic Tank", SubTests::HydrostaticTank);
  AddSubTest("Dam Break Video", SubTests::DamBreakVideo);
}

plResult plGameEngineTestGpuFluidLiquidGallery::InitializeSubTest(plInt32 iIdentifier)
{
  PL_SUCCEED_OR_RETURN(SUPER::InitializeSubTest(iIdentifier));

  m_bDepthOfFieldWasEnabled = GetCVarBool(s_szDepthOfFieldCVar, true);
  m_bGpuStatsWasEnabled = GetCVarBool(s_szGpuStatsCVar, false);
  m_bRestoreDepthOfField = true;

  m_iFrame = -1;
  m_uiLastStatsFrame = 0;
  m_Ticks.Clear();
  m_uiLastTick = 0;
  m_uiN5Mismatches = 0;
  m_uiReadbacks = 0;
  m_PhaseMs.Clear();
  m_uiTimedFrames = 0;
  m_uiTimedParticles = 0;
  m_uiTimedSubsteps = 0;

  // Per-phase GPU times are exported as stats while this is on.
  SetCVarBool(s_szGpuStatsCVar, true);

  switch (iIdentifier)
  {
    case SubTests::DamBreak:
    case SubTests::DamBreakVideo:
      return SetupDamBreak();

    case SubTests::HydrostaticTank:
      return SetupHydrostatic();
  }

  return PL_FAILURE;
}

plResult plGameEngineTestGpuFluidLiquidGallery::DeInitializeSubTest(plInt32 iIdentifier)
{
  if (m_bRestoreDepthOfField)
  {
    SetCVarBool(s_szDepthOfFieldCVar, m_bDepthOfFieldWasEnabled);
    SetCVarBool(s_szGpuStatsCVar, m_bGpuStatsWasEnabled);
    m_bRestoreDepthOfField = false;
  }

  return SUPER::DeInitializeSubTest(iIdentifier);
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount)
{
  ++m_iFrame;

  switch (iIdentifier)
  {
    case SubTests::DamBreak:
      return RunDamBreak(false);
    case SubTests::HydrostaticTank:
      return RunHydrostatic();
    case SubTests::DamBreakVideo:
      return RunDamBreak(true);
  }

  return plTestAppRun::Quit;
}

plGpuFluidLiquidVolumeComponent* plGameEngineTestGpuFluidLiquidGallery::GetVolume()
{
  plGameObject* pObject = nullptr;
  plGpuFluidLiquidVolumeComponent* pVolume = nullptr;
  if (m_pOwnApplication->GetWorld()->TryGetObject(m_hVolumeObject, pObject))
    pObject->TryGetComponentOfBaseType(pVolume);
  return pVolume;
}

plResult plGameEngineTestGpuFluidLiquidGallery::SaveShot(plStringView sName)
{
  plStringBuilder sPath;
  sPath.SetFormat(":imgout/GpuFluidGallery/Liquid/{}.png", sName);
  return m_pOwnApplication->GetLastScreenshot().SaveTo(sPath);
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupDamBreak()
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());
  pWorld->Clear();
  pWorld->GetRandomNumberGenerator().Initialize(0x47465036u);
  pWorld->GetClock().Reset(false);

  SetCVarBool(s_szDepthOfFieldCVar, false);

  // The sun on the camera's side, so the inside of the tank is lit.
  m_pOwnApplication->AddSunAndSky(plVec3(-0.35f, 0.6f, 0.7f), 10.0f);
  m_pOwnApplication->AddBox("Ground", plVec3(0.0f, 0.0f, -0.1f), plVec3(2000.0f, 2000.0f, 0.2f), plColor(0.32f, 0.30f, 0.28f));

  // The tank: a back wall and two end walls drawn just outside the domain; the side facing the camera is left open.
  // The camera looks from +y towards -y, so +x (the direction the water runs) is to the right of the picture.
  const plColor wall(0.75f, 0.75f, 0.72f);
  m_pOwnApplication->AddBox("TankBack", plVec3(0.0f, -0.5f * s_vDamTank.y - 0.02f, 0.5f * s_vDamTank.z), plVec3(s_vDamTank.x + 0.08f, 0.04f, s_vDamTank.z), wall);
  m_pOwnApplication->AddBox("TankLeft", plVec3(-0.5f * s_vDamTank.x - 0.02f, 0.0f, 0.5f * s_vDamTank.z), plVec3(0.04f, s_vDamTank.y, s_vDamTank.z), wall);
  m_pOwnApplication->AddBox("TankRight", plVec3(0.5f * s_vDamTank.x + 0.02f, 0.0f, 0.5f * s_vDamTank.z), plVec3(0.04f, s_vDamTank.y, s_vDamTank.z), wall);

  plGameObjectDesc desc;
  desc.m_sName.Assign("DamBreak");
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * s_vDamTank.z);
  plGameObject* pObject = nullptr;
  m_hVolumeObject = pWorld->CreateObject(desc, pObject);

  plGpuFluidLiquidVolumeComponent* pLiquid = nullptr;
  pWorld->GetOrCreateComponentManager<plGpuFluidLiquidVolumeComponentManager>()->CreateComponent(pObject, pLiquid);
  pLiquid->m_vExtents = s_vDamTank;
  pLiquid->m_fCellSize = s_fDamCellSize;
  pLiquid->m_uiParticlesPerCell = 4;
  pLiquid->m_uiMaxParticles = 320 * 1024;
  pLiquid->m_uiMaxSubsteps = 8;
  pLiquid->m_uiPressureCycles = 3;
  pLiquid->m_fFlipBlend = 0.1f;
  pLiquid->m_fDebugTopSpeed = 4.0f;
  pLiquid->m_Tuning.m_fFrontHeight = s_fFrontHeight;
  pLiquid->m_Tuning.m_fFrontBinSize = s_fFrontBin;

  plGpuFluidLiquidFillBox& column = pLiquid->m_FillBoxes.ExpandAndGetRef();
  column.m_vCenter = plVec3(-0.5f * s_vDamTank.x + 0.5f * s_fDamA, 0.0f, -0.5f * s_vDamTank.z + s_fDamA);
  column.m_vHalfExtents = plVec3(0.5f * s_fDamA, 0.5f * s_vDamTank.y, s_fDamA);

  m_fColumnWidth = s_fDamA;

  if (plGameState* pGameState = plDynamicCast<plGameState*>(m_pOwnApplication->GetActiveGameState()))
    pGameState->GetMainCamera()->LookAt(plVec3(0.0f, 2.45f, 1.0f), plVec3(0.0f, 0.0f, 0.5f), s_vUp);

  return PL_SUCCESS;
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupHydrostatic()
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());
  pWorld->Clear();
  pWorld->GetRandomNumberGenerator().Initialize(0x47465037u);
  pWorld->GetClock().Reset(false);

  SetCVarBool(s_szDepthOfFieldCVar, false);

  m_pOwnApplication->AddSunAndSky(plVec3(-0.35f, 0.6f, 0.7f), 10.0f);
  m_pOwnApplication->AddBox("Ground", plVec3(0.0f, 0.0f, -0.1f), plVec3(2000.0f, 2000.0f, 0.2f), plColor(0.32f, 0.30f, 0.28f));
  const plColor wall(0.75f, 0.75f, 0.72f);
  m_pOwnApplication->AddBox("TankBack", plVec3(0.0f, -0.5f * s_vHydroTank.y - 0.02f, 0.5f * s_vHydroTank.z), plVec3(s_vHydroTank.x + 0.08f, 0.04f, s_vHydroTank.z), wall);

  plGameObjectDesc desc;
  desc.m_sName.Assign("Hydrostatic");
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * s_vHydroTank.z);
  plGameObject* pObject = nullptr;
  m_hVolumeObject = pWorld->CreateObject(desc, pObject);

  plGpuFluidLiquidVolumeComponent* pLiquid = nullptr;
  pWorld->GetOrCreateComponentManager<plGpuFluidLiquidVolumeComponentManager>()->CreateComponent(pObject, pLiquid);
  pLiquid->m_vExtents = s_vHydroTank;
  pLiquid->m_fCellSize = s_fHydroCellSize;
  pLiquid->m_uiParticlesPerCell = 4;
  pLiquid->m_uiMaxParticles = 160 * 1024;
  pLiquid->m_uiMaxSubsteps = 8;
  pLiquid->m_uiPressureCycles = 3;
  pLiquid->m_fDebugTopSpeed = 0.05f;

  plGpuFluidLiquidFillBox& fill = pLiquid->m_FillBoxes.ExpandAndGetRef();
  fill.m_vCenter = plVec3(0.0f, 0.0f, -0.25f * s_vHydroTank.z);
  fill.m_vHalfExtents = plVec3(0.5f * s_vHydroTank.x, 0.5f * s_vHydroTank.y, 0.25f * s_vHydroTank.z);

  if (plGameState* pGameState = plDynamicCast<plGameState*>(m_pOwnApplication->GetActiveGameState()))
    pGameState->GetMainCamera()->LookAt(plVec3(0.0f, 1.3f, 0.75f), plVec3(0.0f, 0.0f, 0.2f), s_vUp);

  return PL_SUCCESS;
}

float plGameEngineTestGpuFluidLiquidGallery::ComputeFront(const plGpuFluidLiquidStats& stats) const
{
  if (stats.m_FrontHistogram.IsEmpty() || stats.m_fFrontBinSize <= 0.0f)
    return -1.0f;

  // From the wall, the first run of empty bins ends the surge; a splash ahead of it does not count.
  plUInt32 uiLast = plInvalidIndex;
  plUInt32 uiGap = 0;
  for (plUInt32 i = 0; i < stats.m_FrontHistogram.GetCount(); ++i)
  {
    if (stats.m_FrontHistogram[i] >= s_uiFrontMinCount)
    {
      uiLast = i;
      uiGap = 0;
    }
    else if (uiLast != plInvalidIndex && ++uiGap >= s_uiFrontGapBins)
    {
      break;
    }
  }

  return uiLast == plInvalidIndex ? -1.0f : static_cast<float>(uiLast + 1) * stats.m_fFrontBinSize;
}

bool plGameEngineTestGpuFluidLiquidGallery::CollectStats()
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetReadMarker());

  plGpuFluidLiquidVolumeComponent* pVolume = GetVolume();
  plGpuFluidLiquidStats stats;
  if (pVolume == nullptr || !pVolume->GetStats(stats) || stats.m_uiFrame == m_uiLastStatsFrame)
    return false;

  m_uiLastStatsFrame = stats.m_uiFrame;
  ++m_uiReadbacks;

  // N5: no particle appears or disappears except through an open face.
  if (stats.m_uiAlive != stats.m_uiSeeded - stats.m_uiDrained || stats.m_uiLost != 0)
  {
    if (m_uiN5Mismatches < 5)
      plTestFramework::Output(plTestOutput::Warning, "N5 tick %u: alive %u, seeded %u, drained %u, lost %u", stats.m_uiTicks, stats.m_uiAlive, stats.m_uiSeeded, stats.m_uiDrained, stats.m_uiLost);
    ++m_uiN5Mismatches;
  }

  TickSample& sample = m_Ticks[stats.m_uiTicks];
  sample.m_uiAlive = stats.m_uiAlive;
  sample.m_uiSeeded = stats.m_uiSeeded;
  sample.m_uiDrained = stats.m_uiDrained;
  sample.m_uiLost = stats.m_uiLost;
  sample.m_uiSubsteps = stats.m_uiSubsteps;
  sample.m_uiActiveBricks = stats.m_uiActiveBricks;
  sample.m_fVolumeRatio = stats.m_fTargetVolumeCells > 0.0f ? stats.m_fVolumeCells / stats.m_fTargetVolumeCells : 0.0f;
  sample.m_fFront = ComputeFront(stats);
  sample.m_fMaxSpeed = stats.m_fMaxSpeed;
  sample.m_fMeanSpeed = stats.m_fMeanSpeed;
  sample.m_fResidualRatio = stats.m_fRhsMax > 0.0f ? stats.m_fResidualMax / stats.m_fRhsMax : 0.0f;
  sample.m_fCentroidZ = stats.m_vCentroid.z;
  sample.m_uiNaN = stats.m_uiNaN;
  sample.m_uiOverflow = stats.m_uiOverflow;

  m_uiLastTick = plMath::Max(m_uiLastTick, stats.m_uiTicks);
  m_uiTimedParticles = stats.m_uiAlive;
  return true;
}

void plGameEngineTestGpuFluidLiquidGallery::AccumulateTimings()
{
  bool bAny = false;
  for (const char* szPhase : s_szPhases)
  {
    plStringBuilder sName("Renderer/GPU/", szPhase, "[ms]");
    const plVariant value = plStats::GetStatValue(sName);
    if (value.IsValid() && value.CanConvertTo<double>())
    {
      m_PhaseMs[szPhase] += value.ConvertTo<double>();
      bAny = true;
    }
  }

  if (bAny)
  {
    ++m_uiTimedFrames;
    if (const TickSample* pSample = m_Ticks.GetValue(m_uiLastTick))
      m_uiTimedSubsteps += pSample->m_uiSubsteps;
  }
}

void plGameEngineTestGpuFluidLiquidGallery::ReportTimings(plStringBuilder& ref_sCsv)
{
  if (m_uiTimedFrames == 0)
  {
    plTestFramework::Output(plTestOutput::Warning, "No GPU pass timings arrived (Rendering.Profiling.GPUStats)");
    return;
  }

  ref_sCsv.Append("phase,mean_ms_per_frame\n");
  double fTotal = 0.0;
  for (const char* szPhase : s_szPhases)
  {
    const double fMean = m_PhaseMs.GetValueOrDefault(szPhase, 0.0) / m_uiTimedFrames;
    // Seed runs on the first tick only; its stat keeps that frame's value, so it is listed but not summed.
    if (!plStringUtils::IsEqual(szPhase, "GpuFluid Liquid/Seed"))
      fTotal += fMean;
    ref_sCsv.AppendFormat("{},{}\n", szPhase, fMean);
    plTestFramework::Output(plTestOutput::Message, "GPU %s: %.3f ms per frame", szPhase, fMean);
  }
  ref_sCsv.AppendFormat("total,{}\n", fTotal);
  plTestFramework::Output(plTestOutput::Message, "GPU liquid total %.3f ms per frame over %u frames, %u particles, %.2f substeps per frame on average", fTotal, m_uiTimedFrames, m_uiTimedParticles,
    static_cast<double>(m_uiTimedSubsteps) / m_uiTimedFrames);
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunDamBreak(bool bVideo)
{
  if (bVideo && plEnvironmentVariableUtils::GetValueInt("PL_GPUFLUID_VIDEO", 0) != 1)
  {
    plTestFramework::Output(plTestOutput::Details, "Skipped: set PL_GPUFLUID_VIDEO=1 to record the frames.");
    return plTestAppRun::Quit;
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  if (m_iFrame == 0)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    if (plGpuFluidLiquidVolumeComponent* pVolume = GetVolume())
      PL_TEST_BOOL_MSG(!pVolume->WasPublished(), "The liquid was drawn on its fresh-resource frame");
  }

  CollectStats();

  if (bVideo)
  {
    // The screenshot of this Run() is the frame that was just rendered.
    if (m_iFrame >= 1)
    {
      plStringBuilder sName;
      sName.SetFormat("DamBreakVideo/frame_{}", plArgU(static_cast<plUInt32>(m_iFrame - 1), 5, true));
      PL_TEST_BOOL(SaveShot(sName).Succeeded());
      if (m_iFrame == 1)
      {
        plStringBuilder sTitle("P6 Dam Break: APIC/FLIP liquid (230K particles, 2 cm cells), particles coloured by speed\n");
        WriteText(":imgout/GpuFluidGallery/Liquid/DamBreakVideo/title.txt", sTitle);
      }
    }
    return static_cast<plUInt32>(m_iFrame) >= s_uiVideoFrames ? plTestAppRun::Quit : plTestAppRun::Continue;
  }

  if (m_iFrame >= 90 && m_iFrame < 290)
    AccumulateTimings();

  static const plInt32 s_Shots[] = {6, 11, 16, 24, 36, 60, 90, 150, 300, 600};
  for (plInt32 iShot : s_Shots)
  {
    if (m_iFrame == iShot)
    {
      plStringBuilder sName;
      sName.SetFormat("DamBreak_{}", plArgI(iShot, 4, true));
      PL_TEST_BOOL(SaveShot(sName).Succeeded());
    }
  }

  if (m_uiLastTick >= s_uiDamTicks || m_iFrame >= static_cast<plInt32>(s_uiDamTicks) + 200)
  {
    EvaluateDamBreak();
    return plTestAppRun::Quit;
  }

  return plTestAppRun::Continue;
}

void plGameEngineTestGpuFluidLiquidGallery::EvaluateDamBreak()
{
  PL_TEST_BOOL_MSG(m_uiLastTick >= s_uiDamTicks, "Only %u ticks were read back", m_uiLastTick);
  PL_TEST_BOOL_MSG(m_uiReadbacks > s_uiDamTicks / 2, "Only %u readbacks arrived", m_uiReadbacks);

  plStringBuilder sCsv("tick,alive,seeded,drained,lost,substeps,active_bricks,volume_ratio,front_m,max_speed,mean_speed,residual_ratio,centroid_z,nan,overflow\n");
  float fMinVolume = 1e9f, fMaxVolume = -1e9f;
  plUInt32 uiMaxNaN = 0, uiMaxOverflow = 0, uiMaxLost = 0;
  float fMaxResidualRatio = 0.0f;
  for (auto it : m_Ticks)
  {
    const TickSample& s = it.Value();
    sCsv.AppendFormat("{},{},{},{},{},{},{},{},", it.Key(), s.m_uiAlive, s.m_uiSeeded, s.m_uiDrained, s.m_uiLost, s.m_uiSubsteps, s.m_uiActiveBricks, s.m_fVolumeRatio);
    sCsv.AppendFormat("{},{},{},{},{},{},{}\n", s.m_fFront, s.m_fMaxSpeed, s.m_fMeanSpeed, s.m_fResidualRatio, s.m_fCentroidZ, s.m_uiNaN, s.m_uiOverflow);
    if (it.Key() >= 1 && it.Key() <= s_uiDamTicks)
    {
      fMinVolume = plMath::Min(fMinVolume, s.m_fVolumeRatio);
      fMaxVolume = plMath::Max(fMaxVolume, s.m_fVolumeRatio);
    }
    uiMaxNaN = plMath::Max(uiMaxNaN, s.m_uiNaN);
    uiMaxOverflow = plMath::Max(uiMaxOverflow, s.m_uiOverflow);
    uiMaxLost = plMath::Max(uiMaxLost, s.m_uiLost);
    fMaxResidualRatio = plMath::Max(fMaxResidualRatio, s.m_fResidualRatio);
  }
  WriteText(":imgout/GpuFluidGallery/Liquid/DamBreak_ticks.csv", sCsv);

  // N5. The particle count is the volume and must be exact. The cell measure (full interior cells plus fractional
  // surface cells) is noisy tick to tick while the water splashes, so it is checked as a 1 s (30 tick) moving mean;
  // the single-tick extremes are reported. At the end the pool is near rest, and twice the particle centroid height is
  // its depth: depth * floor area against the seeded volume is a measure independent of the cells.
  float fMinMean = 1e9f, fMaxMean = -1e9f;
  for (auto it : m_Ticks)
  {
    if (it.Key() < 30 || it.Key() > s_uiDamTicks)
      continue;
    float fSum = 0.0f;
    plUInt32 uiCount = 0;
    for (plUInt32 k = it.Key() - 29; k <= it.Key(); ++k)
    {
      if (const TickSample* pSample = m_Ticks.GetValue(k))
      {
        fSum += pSample->m_fVolumeRatio;
        ++uiCount;
      }
    }
    if (uiCount >= 15)
    {
      fMinMean = plMath::Min(fMinMean, fSum / uiCount);
      fMaxMean = plMath::Max(fMaxMean, fSum / uiCount);
    }
  }

  // The pool still sloshes gently at 20 s, so the centroid height is averaged over the last 2 s.
  float fCentroidSum = 0.0f;
  plUInt32 uiCentroidCount = 0;
  for (auto it : m_Ticks)
  {
    if (it.Key() + 60 > m_uiLastTick && it.Key() <= m_uiLastTick)
    {
      fCentroidSum += it.Value().m_fCentroidZ;
      ++uiCentroidCount;
    }
  }
  const float fSeededVolume = m_fColumnWidth * 2.0f * m_fColumnWidth * s_vDamTank.y;
  const float fRestVolume = uiCentroidCount > 0 ? 2.0f * (fCentroidSum / uiCentroidCount) * s_vDamTank.x * s_vDamTank.y / fSeededVolume : 0.0f;

  plTestFramework::Output(plTestOutput::Message,
    "N5: %u readbacks, %u with alive != seeded - drained, lost %u, NaN %u, overflow %u; cell volume measure per tick %.4f .. %.4f, 30-tick mean %.4f .. %.4f (limit +-%.0f%%); depth volume over the 60 ticks up to %u %.4f; max residual/rhs %.2e",
    m_uiReadbacks, m_uiN5Mismatches, uiMaxLost, uiMaxNaN, uiMaxOverflow, fMinVolume, fMaxVolume, fMinMean, fMaxMean, s_fVolumeTolerance * 100.0f, m_uiLastTick, fRestVolume, fMaxResidualRatio);
  PL_TEST_INT(m_uiN5Mismatches, 0);
  PL_TEST_INT(uiMaxLost, 0);
  PL_TEST_INT(uiMaxNaN, 0);
  PL_TEST_BOOL_MSG(fMinMean >= 1.0f - s_fVolumeTolerance && fMaxMean <= 1.0f + s_fVolumeTolerance, "N5: the 30-tick mean of the volume measure ran %.4f .. %.4f", fMinMean, fMaxMean);
  PL_TEST_BOOL_MSG(plMath::Abs(fRestVolume - 1.0f) <= s_fVolumeTolerance, "N5: the pool at rest holds %.4f of the seeded volume", fRestVolume);

  // N7: front against Martin & Moyce. T = t sqrt(2g/a); the sample of tick k is the state at t = k * dt.
  const float fTScale = plMath::Sqrt(2.0f * s_fGravity / m_fColumnWidth);
  plDynamicArray<plVec2> curve; // (T, Z/a)
  curve.PushBack(plVec2(0.0f, 1.0f));
  for (auto it : m_Ticks)
  {
    if (it.Value().m_fFront > 0.0f && it.Key() > 0)
      curve.PushBack(plVec2(static_cast<float>(it.Key()) * s_fTickSeconds * fTScale, it.Value().m_fFront / m_fColumnWidth));
  }

  auto ZAt = [&](float fT) -> float
  {
    for (plUInt32 i = 1; i < curve.GetCount(); ++i)
    {
      if (curve[i].x >= fT)
        return plMath::Lerp(curve[i - 1].y, curve[i].y, (fT - curve[i - 1].x) / plMath::Max(curve[i].x - curve[i - 1].x, 1e-6f));
    }
    return -1.0f;
  };

  // The paper's own alignment: shift T so that the front passes Z/a = 1.44 at T = 1.19, as it did for every run.
  float fTAt144 = -1.0f;
  for (plUInt32 i = 1; i < curve.GetCount(); ++i)
  {
    if (curve[i].y >= 1.44f)
    {
      fTAt144 = plMath::Lerp(curve[i - 1].x, curve[i].x, (1.44f - curve[i - 1].y) / plMath::Max(curve[i].y - curve[i - 1].y, 1e-6f));
      break;
    }
  }
  const float fShift = fTAt144 >= 0.0f ? 1.19f - fTAt144 : 0.0f;

  plStringBuilder sFront("T,sim_Z_over_a,MM_Z_over_a,rel_error,sim_Z_over_a_aligned,rel_error_aligned\n");
  for (float fT : {1.0f, 2.0f, 3.0f})
  {
    const float fSim = ZAt(fT);
    const float fSimAligned = ZAt(fT - fShift);
    const float fRef = MartinMoyceZ(fT);
    const float fErr = (fSim - fRef) / fRef;
    const float fErrAligned = (fSimAligned - fRef) / fRef;
    sFront.AppendFormat("{},{},{},{},{},{}\n", fT, fSim, fRef, fErr, fSimAligned, fErrAligned);
    plTestFramework::Output(plTestOutput::Message, "N7 T=%.0f: Z/a %.3f (aligned at Z/a=1.44: %.3f, shift %.3f), Martin & Moyce %.3f -> error %+.1f%% (aligned %+.1f%%), limit %.0f%%", fT, fSim,
      fSimAligned, fShift, fRef, fErr * 100.0f, fErrAligned * 100.0f, s_fFrontTolerance * 100.0f);
    // Checked the way the table is made: Martin & Moyce shifted the time of every run so that it passes Z/a = 1.44 at
    // the tabulated T (their footnote: "T normalized at this value of Z"), because the instant of release was not
    // measured. The unshifted error is reported; the simulation releases instantly and leads by about 0.06 s.
    PL_TEST_BOOL_MSG(fSimAligned > 0.0f && plMath::Abs(fErrAligned) <= s_fFrontTolerance, "N7 T=%.0f: front Z/a %.3f (aligned), Martin & Moyce %.3f", fT, fSimAligned, fRef);
    // The shift itself stays small: an instant release leads the table, by less than 0.6 in T (0.1 s here).
    PL_TEST_BOOL_MSG(fShift >= 0.0f && fShift < 0.6f, "N7: the simulated front reaches Z/a = 1.44 at T = %.3f, the table at 1.19", 1.19f - fShift);
  }
  WriteText(":imgout/GpuFluidGallery/Liquid/DamBreak_front.csv", sFront);

  plStringBuilder sTimes;
  ReportTimings(sTimes);
  WriteText(":imgout/GpuFluidGallery/Liquid/DamBreak_timings.csv", sTimes);
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunHydrostatic()
{
  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  CollectStats();

  if (m_iFrame == 30 || m_iFrame == 300)
  {
    plStringBuilder sName;
    sName.SetFormat("Hydrostatic_{}", plArgI(m_iFrame, 4, true));
    PL_TEST_BOOL(SaveShot(sName).Succeeded());
  }

  if (m_iFrame >= 60 && m_iFrame < 160)
    AccumulateTimings();

  if (m_uiLastTick >= s_uiHydroTicks || m_iFrame >= static_cast<plInt32>(s_uiHydroTicks) + 100)
  {
    EvaluateHydrostatic();
    return plTestAppRun::Quit;
  }
  return plTestAppRun::Continue;
}

void plGameEngineTestGpuFluidLiquidGallery::EvaluateHydrostatic()
{
  PL_TEST_BOOL_MSG(m_uiLastTick >= s_uiHydroTicks, "Only %u ticks were read back", m_uiLastTick);

  plStringBuilder sCsv("tick,alive,max_speed,mean_speed,volume_ratio,residual_ratio,centroid_z,substeps\n");
  for (auto it : m_Ticks)
  {
    const TickSample& s = it.Value();
    sCsv.AppendFormat("{},{},{},{},{},{},{},{}\n", it.Key(), s.m_uiAlive, s.m_fMaxSpeed, s.m_fMeanSpeed, s.m_fVolumeRatio, s.m_fResidualRatio, s.m_fCentroidZ, s.m_uiSubsteps);
  }
  WriteText(":imgout/GpuFluidGallery/Liquid/Hydrostatic_ticks.csv", sCsv);

  const TickSample* pLast = m_Ticks.GetValue(m_uiLastTick);
  if (!PL_TEST_BOOL(pLast != nullptr))
    return;

  const TickSample* pFirst = m_Ticks.IsEmpty() ? nullptr : &m_Ticks.GetIterator().Value();
  plTestFramework::Output(plTestOutput::Message, "N6 tick %u: max |v| %.4f m/s (limit %.3f), mean %.5f m/s, volume ratio %.4f, centroid z %.4f m (first %.4f m), N5 mismatches %u, lost %u", m_uiLastTick,
    pLast->m_fMaxSpeed, s_fHydroMaxSpeed, pLast->m_fMeanSpeed, pLast->m_fVolumeRatio, pLast->m_fCentroidZ, pFirst != nullptr ? pFirst->m_fCentroidZ : 0.0f, m_uiN5Mismatches, pLast->m_uiLost);
  PL_TEST_INT(m_uiN5Mismatches, 0);
  PL_TEST_INT(pLast->m_uiNaN, 0);
  PL_TEST_BOOL_MSG(pLast->m_fMaxSpeed < s_fHydroMaxSpeed, "N6: max |v| %.4f m/s after %u ticks", pLast->m_fMaxSpeed, m_uiLastTick);

  plStringBuilder sTimes;
  ReportTimings(sTimes);
  WriteText(":imgout/GpuFluidGallery/Liquid/Hydrostatic_timings.csv", sTimes);
}
