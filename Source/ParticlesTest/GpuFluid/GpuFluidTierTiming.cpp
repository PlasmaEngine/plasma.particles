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
  constexpr plInt32 s_iMeasuredFrames = 180;

  const plVec3 s_vCameraPosition(-5.3f, -11.3f, 1.8f);
  const plVec3 s_vCameraTarget(0.0f, 0.0f, 3.7f);

  /// The tier of each sub-test. Only the tiers P1 has (manual selection); P5 adds High and the automatic scaler.
  struct TierCase
  {
    const char* m_szName;
    plGpuFluidQuality::Enum m_Quality;
  };

  constexpr TierCase s_Tiers[] = {
    {"Gas Low", plGpuFluidQuality::Low},
    {"Gas Medium", plGpuFluidQuality::Medium},
  };

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

  /// Gas/Finalize runs once per frame; every other gas phase once per tick.
  bool IsPerFramePhase(plStringView sPhase)
  {
    return sPhase == "Gas/Finalize";
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

  PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupSmokeColumnScene());

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
    if (pGas == nullptr)
      return PL_FAILURE;
    pGas->m_Quality = s_Tiers[iIdentifier].m_Quality;
  }

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
      }
    }
  }

  if (m_iFrame < s_iWarmupFrames)
    return plTestAppRun::Continue;

  // One value per frame of every Gas/* phase. The profiler names the second run of a pass in a frame "<name> #2", so
  // the runs are summed into one value per frame. Gas/Alloc only runs on the volume's first frame; its stat merely
  // keeps that value, so it is left out.
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

      if (sPhase == "Gas/Alloc")
        continue;

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

  if (m_iFrame + 1 < s_iWarmupFrames + s_iMeasuredFrames)
    return plTestAppRun::Continue;

  WriteResults(s_Tiers[iIdentifier].m_Quality);
  return plTestAppRun::Quit;
}

void plGameEngineTestGpuFluidTierTiming::WriteResults(plGpuFluidQuality::Enum quality)
{
  if (!PL_TEST_BOOL_MSG(!m_PhaseNames.IsEmpty(), "No Renderer/GPU/Gas/* stats: are GPU timestamps available?"))
    return;

  const plUInt32 uiBricks = m_uiActiveBricks;
  const plUInt32 uiTicks = plMath::Max(m_uiTicksPerFrame, 1u);
  const char* szTier = QualityName(quality);

  auto Percentile = [](plDynamicArray<double> values, double fFraction)
  {
    values.Sort();
    const plUInt32 uiIndex = plMath::Min(values.GetCount() - 1, static_cast<plUInt32>(fFraction * (values.GetCount() - 1) + 0.5));
    return values[uiIndex];
  };

  plStringBuilder sCsv;
  sCsv.AppendFormat("# GpuFluid tier timing, tier {}, Smoke Column, {} active bricks, {} ticks per frame, {} warm-up + {} measured frames, Metal, {} build, "
                    "Rendering.Pipeline.Injections off\n",
    szTier, uiBricks, uiTicks, s_iWarmupFrames, s_iMeasuredFrames, PL_GPUFLUID_TIMING_BUILD);
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

  plStringBuilder sFile;
  sFile.SetFormat("tier-timing-{}.csv", szTier);
  sFile.ToLower();

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

  PL_TEST_INT(uiBricks, 8 * 8 * 16);
  PL_TEST_INT(m_uiNaN, 0);
  PL_TEST_INT(uiTicks, quality == plGpuFluidQuality::Low ? 1u : 2u);
}
