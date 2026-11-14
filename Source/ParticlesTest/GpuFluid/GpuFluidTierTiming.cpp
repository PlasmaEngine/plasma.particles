#include <ParticlesTest/ParticlesTestPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/Stats.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasVolumeComponent.h>
#include <ParticlesTest/GpuFluid/GpuFluidTierTiming.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

static plGameEngineTestGpuFluidTierTiming s_GameEngineTestGpuFluidTierTiming;

// The command line drops the define's quotes, so it is stringized here.
#if defined(BUILDSYSTEM_BUILDTYPE)
#  define PL_GPUFLUID_TIMING_BUILD PL_PP_STRINGIFY(BUILDSYSTEM_BUILDTYPE)
#else
#  define PL_GPUFLUID_TIMING_BUILD "unknown"
#endif

namespace
{
  constexpr plInt32 s_iWarmupFrames = 60;
  /// The sparse gate scene's plumes need about four seconds to reach the open top and settle.
  constexpr plInt32 s_iSparseWarmupFrames = 150;
  constexpr plInt32 s_iMeasuredFrames = 180;

  const plVec3 s_vCameraPosition(-5.3f, -11.3f, 1.8f);
  const plVec3 s_vCameraTarget(0.0f, 0.0f, 3.7f);

  /// The tier of each sub-test (manual selection; P5 adds High and the automatic scaler). The SOR case runs Medium with
  /// the P1 red-black SOR pressure solver (40 iterations), the reference the multigrid's cost is measured against.
  struct TierCase
  {
    const char* m_szName;
    plGpuFluidQuality::Enum m_Quality;
    bool m_bSOR;
    const char* m_szFile;
    bool m_bSparse = false; ///< the performance gate's sparse scene (SetupGateScene) instead of the Smoke Column
  };

  constexpr TierCase s_Tiers[] = {
    {"Gas Low", plGpuFluidQuality::Low, false, "tier-timing-low.csv"},
    {"Gas Medium", plGpuFluidQuality::Medium, false, "tier-timing-medium.csv"},
    {"Gas Medium SOR", plGpuFluidQuality::Medium, true, "tier-timing-medium-sor.csv"},
    {"Gas Medium Sparse", plGpuFluidQuality::Medium, false, "tier-timing-medium-sparse.csv", true},
  };

  /// The pressure solver settings in force, for the CSV header and the log.
  plString PressureSettings()
  {
    plStringBuilder s;
    for (const char* szName : {"GpuFluid.Gas.PressureSolver", "GpuFluid.Gas.PressureCycles", "GpuFluid.Gas.MGShortCycles", "GpuFluid.Gas.MGPreSmooth", "GpuFluid.Gas.MGPostSmooth",
           "GpuFluid.Gas.VelocityMacCormack", "GpuFluid.Gas.Vorticity"})
    {
      plCVar* pCVar = plCVar::FindCVarByName(szName);
      if (pCVar == nullptr)
        continue;
      switch (pCVar->GetType())
      {
        case plCVarType::Bool:
          s.AppendFormat(" {}={}", plStringView(szName).GetSubString(13, 64), static_cast<plCVarBool*>(pCVar)->GetValue() ? 1 : 0);
          break;
        case plCVarType::Int:
          s.AppendFormat(" {}={}", plStringView(szName).GetSubString(13, 64), static_cast<plCVarInt*>(pCVar)->GetValue());
          break;
        case plCVarType::Float:
          s.AppendFormat(" {}={}", plStringView(szName).GetSubString(13, 64), static_cast<plCVarFloat*>(pCVar)->GetValue());
          break;
        default:
          break;
      }
    }
    return s;
  }

  const char* QualityName(plGpuFluidQuality::Enum quality)
  {
    switch (quality)
    {
      case plGpuFluidQuality::Low:
        return "Low";
      case plGpuFluidQuality::Medium:
        return "Medium";
      case plGpuFluidQuality::High:
        return "High";
      case plGpuFluidQuality::Ultra:
        return "Ultra";
      default:
        return "Unknown";
    }
  }

  /// Gas/Alloc (and its upload, clear and debug passes) and Gas/Finalize run once per frame; every other gas phase once
  /// per tick.
  bool IsPerFramePhase(plStringView sPhase)
  {
    return sPhase == "Gas/Finalize" || sPhase.StartsWith("Gas/Alloc");
  }
} // namespace

const char* plGameEngineTestGpuFluidTierTiming::GetTestName() const
{
  return "GpuFluid Tier Timing";
}

plGameEngineTestApplication* plGameEngineTestGpuFluidTierTiming::CreateApplication()
{
  m_pOwnApplication = PL_DEFAULT_NEW(plGpuFluidGalleryApplication);
  return m_pOwnApplication;
}

void plGameEngineTestGpuFluidTierTiming::SetupSubTests()
{
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_Tiers); ++i)
    AddSubTest(s_Tiers[i].m_szName, static_cast<plInt32>(i));
}

plResult plGameEngineTestGpuFluidTierTiming::InitializeSubTest(plInt32 iIdentifier)
{
  PL_SUCCEED_OR_RETURN(SUPER::InitializeSubTest(iIdentifier));

  if (iIdentifier < 0 || iIdentifier >= static_cast<plInt32>(PL_ARRAY_SIZE(s_Tiers)))
    return PL_FAILURE;

  m_iFrame = -1;
  m_PhaseNames.Clear();
  m_PhaseValues.Clear();
  m_uiActiveBricks = 0;
  m_uiTicksPerFrame = 0;
  m_uiNaN = 0;

  // Per-pass GPU times reach plStats only with this on. The baseline: no injected pipeline passes. The scene turns
  // depth of field off, which the image tests after this one in the process need back.
  SetCVarForSubTest("Rendering.Profiling.GPUStats", true);
  SetCVarForSubTest("Rendering.Pipeline.Injections", false);
  SetCVarForSubTest("Rendering.DepthOfField.Enable", false);
  if (s_Tiers[iIdentifier].m_bSOR)
    SetCVarForSubTest("GpuFluid.Gas.PressureSolver", 0);

  m_iWarmupFrames = s_Tiers[iIdentifier].m_bSparse ? s_iSparseWarmupFrames : s_iWarmupFrames;
  m_uiActiveSum = 0;
  m_uiActiveSamples = 0;
  m_uiActiveMin = 0xFFFFFFFFu;
  m_uiActiveMax = 0;
  m_uiDenied = 0;
  if (s_Tiers[iIdentifier].m_bSparse)
    PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupGateScene());
  else
    PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupSmokeColumnScene());

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
    if (pGas == nullptr)
      return PL_FAILURE;
    pGas->m_Quality = s_Tiers[iIdentifier].m_Quality;
  }

  if (s_Tiers[iIdentifier].m_bSparse)
    m_pOwnApplication->SetCamera(plVec3(-14.0f, -28.0f, 7.0f), plVec3(0.0f, 0.0f, 4.0f), 50.0f);
  else
    m_pOwnApplication->SetCamera(s_vCameraPosition, s_vCameraTarget, 50.0f);
  return PL_SUCCESS;
}

plResult plGameEngineTestGpuFluidTierTiming::DeInitializeSubTest(plInt32 iIdentifier)
{
  for (plUInt32 i = m_RestoreCVarNames.GetCount(); i-- > 0;)
  {
    plCVar* pCVar = plCVar::FindCVarByName(m_RestoreCVarNames[i]);
    if (pCVar == nullptr)
      continue;

    const plVariant& value = m_RestoreCVarValues[i];
    switch (pCVar->GetType())
    {
      case plCVarType::Bool:
        *static_cast<plCVarBool*>(pCVar) = value.Get<bool>();
        break;
      case plCVarType::Int:
        *static_cast<plCVarInt*>(pCVar) = value.Get<plInt32>();
        break;
      case plCVarType::Float:
        *static_cast<plCVarFloat*>(pCVar) = value.Get<float>();
        break;
      default:
        break;
    }
  }
  m_RestoreCVarNames.Clear();
  m_RestoreCVarValues.Clear();

  return SUPER::DeInitializeSubTest(iIdentifier);
}

void plGameEngineTestGpuFluidTierTiming::SetCVarForSubTest(const char* szName, const plVariant& value)
{
  plCVar* pCVar = plCVar::FindCVarByName(szName);
  if (!PL_TEST_BOOL_MSG(pCVar != nullptr, "CVar '%s' not found", szName))
    return;

  plVariant previous;
  switch (pCVar->GetType())
  {
    case plCVarType::Bool:
      previous = static_cast<plCVarBool*>(pCVar)->GetValue();
      *static_cast<plCVarBool*>(pCVar) = value.ConvertTo<bool>();
      break;
    case plCVarType::Int:
      previous = static_cast<plCVarInt*>(pCVar)->GetValue();
      *static_cast<plCVarInt*>(pCVar) = value.ConvertTo<plInt32>();
      break;
    case plCVarType::Float:
      previous = static_cast<plCVarFloat*>(pCVar)->GetValue();
      *static_cast<plCVarFloat*>(pCVar) = value.ConvertTo<float>();
      break;
    default:
      PL_TEST_FAILURE("Unsupported CVar type", "%s", szName);
      return;
  }

  m_RestoreCVarNames.PushBack(szName);
  m_RestoreCVarValues.PushBack(previous);
}

plTestAppRun plGameEngineTestGpuFluidTierTiming::RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount)
{
  ++m_iFrame;

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
    {
      plGpuFluidGasStats stats;
      if (pGas->GetStats(stats) && stats.m_uiTicks > 0)
      {
        m_uiActiveBricks = stats.m_uiActiveBricks;
        m_uiTicksPerFrame = stats.m_uiTicks;
        m_uiNaN = plMath::Max(m_uiNaN, stats.m_uiNaNCount);
        if (m_iFrame >= m_iWarmupFrames)
        {
          m_uiActiveSum += stats.m_uiActiveBricks;
          ++m_uiActiveSamples;
          m_uiActiveMin = plMath::Min(m_uiActiveMin, stats.m_uiActiveBricks);
          m_uiActiveMax = plMath::Max(m_uiActiveMax, stats.m_uiActiveBricks);
          m_uiDenied += stats.m_uiDeniedBricks;
        }
      }
    }
  }

  if (m_iFrame < m_iWarmupFrames)
    return plTestAppRun::Continue;

  // One value per frame of every Gas/* phase. The profiler names the second run of a pass in a frame "<name> #2", so
  // the runs are summed into one value per frame. Since P2, Gas/Alloc runs every frame that ticks.
  {
    plDynamicArray<plString> names;
    plDynamicArray<plVariant> values;
    plStats::GetStatsWithPrefix("Renderer/GPU/Gas/", names, values);

    plHybridArray<double, 16> frameValues;
    frameValues.SetCount(m_PhaseNames.GetCount(), 0.0);

    for (plUInt32 i = 0; i < names.GetCount(); ++i)
    {
      plStringBuilder sPhase = names[i];
      sPhase.TrimWordStart("Renderer/GPU/");
      if (const char* szSuffix = sPhase.FindSubString(" #"))
        sPhase.SetSubString_FromTo(sPhase.GetData(), szSuffix);
      else if (const char* szUnit = sPhase.FindSubString("["))
        sPhase.SetSubString_FromTo(sPhase.GetData(), szUnit);

      plUInt32 uiIndex = m_PhaseNames.IndexOf(sPhase);
      if (uiIndex == plInvalidIndex)
      {
        uiIndex = m_PhaseNames.GetCount();
        m_PhaseNames.PushBack(sPhase);
        m_PhaseValues.ExpandAndGetRef();
        frameValues.PushBack(0.0);
      }
      frameValues[uiIndex] += values[i].ConvertTo<double>();
    }

    for (plUInt32 i = 0; i < frameValues.GetCount(); ++i)
      m_PhaseValues[i].PushBack(frameValues[i]);
  }

  if (m_iFrame + 1 < m_iWarmupFrames + s_iMeasuredFrames)
    return plTestAppRun::Continue;

  WriteResults(iIdentifier);
  return plTestAppRun::Quit;
}

void plGameEngineTestGpuFluidTierTiming::WriteResults(plInt32 iIdentifier)
{
  if (!PL_TEST_BOOL_MSG(!m_PhaseNames.IsEmpty(), "No Renderer/GPU/Gas/* stats: are GPU timestamps available?"))
    return;

  const TierCase& tier = s_Tiers[iIdentifier];
  const plGpuFluidQuality::Enum quality = tier.m_Quality;
  // A sparse scene's brick count moves: its mean over the measured frames.
  const plUInt32 uiBricks = tier.m_bSparse && m_uiActiveSamples > 0 ? static_cast<plUInt32>((m_uiActiveSum + m_uiActiveSamples / 2) / m_uiActiveSamples) : m_uiActiveBricks;
  if (tier.m_bSparse)
  {
    plTestFramework::Output(plTestOutput::Message, "Tier %s: active bricks over the measured frames mean %u, min %u, max %u of the pool of %u (%.0f%%), %u denied", tier.m_szName, uiBricks,
      m_uiActiveMin, m_uiActiveMax, plGpuFluidGasVolumeComponent::GetTierBrickCap(quality), 100.0 * uiBricks / plGpuFluidGasVolumeComponent::GetTierBrickCap(quality), m_uiDenied);
  }
  const plUInt32 uiTicks = plMath::Max(m_uiTicksPerFrame, 1u);
  const char* szTier = tier.m_bSOR ? "MediumSOR" : QualityName(quality);
  const plString sSettings = PressureSettings();
  plTestFramework::Output(plTestOutput::Message, "Tier %s pressure settings:%s", szTier, sSettings.GetData());

  auto Percentile = [](plDynamicArray<double> values, double fFraction)
  {
    values.Sort();
    const plUInt32 uiIndex = plMath::Min(values.GetCount() - 1, static_cast<plUInt32>(fFraction * (values.GetCount() - 1) + 0.5));
    return values[uiIndex];
  };

  plStringBuilder sCsv;
  sCsv.AppendFormat("# GpuFluid tier timing, tier {}, {}, {} active bricks, {} ticks per frame, {} warm-up + {} measured frames, Metal, {} build, "
                    "Rendering.Pipeline.Injections off\n# gas settings:{}\n",
    szTier, tier.m_bSparse ? "sparse gate scene (mean active bricks over the measured frames)" : "Smoke Column (whole box)", uiBricks, uiTicks, m_iWarmupFrames, s_iMeasuredFrames, PL_GPUFLUID_TIMING_BUILD, sSettings);
  sCsv.Append("# runs: how often the phase runs per frame (once per tick, Finalize once per frame); per_run = per_frame / runs\n");
  sCsv.Append("tier,phase,runs_per_frame,median_ms_per_frame,p90_ms_per_frame,median_ms_per_run,us_per_brick_run,samples\n");

  const plUInt32 uiFrames = m_PhaseValues[0].GetCount();
  plDynamicArray<double> totals;
  plDynamicArray<double> tickTotals;
  totals.SetCount(uiFrames, 0.0);
  tickTotals.SetCount(uiFrames, 0.0);
  double fFrameOnceMedian = 0.0;

  for (plUInt32 i = 0; i < m_PhaseNames.GetCount(); ++i)
  {
    const plDynamicArray<double>& values = m_PhaseValues[i];
    const bool bPerFrame = IsPerFramePhase(m_PhaseNames[i]);
    const plUInt32 uiRuns = bPerFrame ? 1 : uiTicks;

    for (plUInt32 j = 0; j < plMath::Min(values.GetCount(), uiFrames); ++j)
    {
      totals[j] += values[j];
      if (!bPerFrame)
        tickTotals[j] += values[j];
    }

    const double fMedian = Percentile(values, 0.5);
    const double fP90 = Percentile(values, 0.9);
    const double fPerRun = fMedian / uiRuns;
    const double fPerBrickRun = uiBricks > 0 ? fPerRun * 1000.0 / uiBricks : 0.0;
    if (bPerFrame)
      fFrameOnceMedian += fMedian;

    sCsv.AppendFormat("{},{},{},{},{},{},{},{}\n", szTier, m_PhaseNames[i], uiRuns, plArgF(fMedian, 4), plArgF(fP90, 4), plArgF(fPerRun, 4), plArgF(fPerBrickRun, 4), values.GetCount());
    plTestFramework::Output(plTestOutput::Message, "Tier %s, %s: median %.3f ms, p90 %.3f ms per frame, %.3f ms per run (%u per frame)", szTier, m_PhaseNames[i].GetData(), fMedian, fP90, fPerRun, uiRuns);
    if (m_PhaseNames[i] == "Gas/Pressure")
      plTestFramework::Output(plTestOutput::Message, "Tier %s pressure: %.3f ms per tick (p90 %.3f), %.3f us per brick-tick, %u bricks", szTier, fPerRun, fP90 / uiRuns, fPerBrickRun, uiBricks);
  }

  // The per-tick phases together, per frame and per tick; and the whole gas frame, which adds Finalize once.
  {
    const double fTickMedian = Percentile(tickTotals, 0.5);
    const double fTickP90 = Percentile(tickTotals, 0.9);
    const double fPerTick = fTickMedian / uiTicks;
    sCsv.AppendFormat("{},Gas tick phases,{},{},{},{},{},{}\n", szTier, uiTicks, plArgF(fTickMedian, 4), plArgF(fTickP90, 4), plArgF(fPerTick, 4), plArgF(uiBricks > 0 ? fPerTick * 1000.0 / uiBricks : 0.0, 4), uiFrames);

    const double fMedian = Percentile(totals, 0.5);
    const double fP90 = Percentile(totals, 0.9);
    sCsv.AppendFormat("{},Gas frame total,1,{},{},{},{},{}\n", szTier, plArgF(fMedian, 4), plArgF(fP90, 4), plArgF(fMedian, 4), plArgF(uiBricks > 0 ? fMedian * 1000.0 / uiBricks : 0.0, 4), uiFrames);

    plTestFramework::Output(plTestOutput::Message, "Tier %s: gas frame median %.3f ms, p90 %.3f ms; %.3f ms per tick (%.2f us per brick-tick) x %u ticks + %.3f ms once per frame; %u bricks", szTier, fMedian, fP90, fPerTick,
      uiBricks > 0 ? fPerTick * 1000.0 / uiBricks : 0.0, uiTicks, fFrameOnceMedian, uiBricks);
  }

  // For scale: every pass the GPU timed in the last frame.
  {
    plDynamicArray<plString> names;
    plDynamicArray<plVariant> values;
    plStats::GetStatsWithPrefix("Renderer/GPU/", names, values);
    sCsv.Append("# other passes of the last frame (ms)\n");
    for (plUInt32 i = 0; i < names.GetCount(); ++i)
    {
      const double fValue = values[i].ConvertTo<double>();
      if (fValue >= 0.05)
        sCsv.AppendFormat("# {},{}\n", names[i], plArgF(fValue, 3));
    }
  }

  const plStringBuilder sFile = tier.m_szFile;

  // The plan's place, <sdk>/Output/GpuFluid/, and the test output folder.
  {
    plStringBuilder sDirectory = plFileSystem::GetSdkRootDirectory();
    sDirectory.AppendPath("Output", "GpuFluid");
    plStringBuilder sPath = sDirectory;
    sPath.AppendPath(sFile);

    plOSFile file;
    if (PL_TEST_BOOL_MSG(plOSFile::CreateDirectoryStructure(sDirectory).Succeeded() && file.Open(sPath, plFileOpenMode::Write).Succeeded(), "Could not write %s", sPath.GetData()))
    {
      file.Write(sCsv.GetData(), sCsv.GetElementCount()).IgnoreResult();
      file.Close();
      plTestFramework::Output(plTestOutput::Message, "Tier timing CSV: %s", sPath.GetData());
    }
  }
  {
    plStringBuilder sPath(":imgout/GpuFluid/", sFile);
    plFileWriter csv;
    if (csv.Open(sPath).Succeeded())
      csv.WriteBytes(sCsv.GetData(), sCsv.GetElementCount()).IgnoreResult();
  }

  if (tier.m_bSparse)
    PL_TEST_BOOL_MSG(uiBricks > 0, "Tier %s: no active bricks", tier.m_szName);
  else
    PL_TEST_INT(uiBricks, 8 * 8 * 16);
  PL_TEST_INT(m_uiNaN, 0);
  PL_TEST_INT(uiTicks, quality == plGpuFluidQuality::Low ? 1u : 2u);
}
