#include <ParticlesTest/ParticlesTestPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/Strings/StringConversion.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/Utilities/Stats.h>
#include <GameEngine/GameState/GameState.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidLiquidVolumeComponent.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <Core/Graphics/Geometry.h>
#include <ParticlesTest/GpuFluid/GpuFluidLiquidGalleryTest.h>
#include <ParticlesTest/GpuFluid/GpuFluidTestSolidComponent.h>
#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidConstants.h>

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

  // N5: the volume measure stays within this of the seeded volume, on every tick.
  constexpr float s_fVolumeTolerance = 0.03f;

  // N5, independent of the volume control: over the last 5 s of the dam break the particle centroid height changes by
  // at most this fraction from one tick to the next (a pool that briefly loses its pressure support sinks by 10-20%).
  constexpr float s_fCentroidStepTolerance = 0.02f;
  constexpr plUInt32 s_uiCentroidStepTicks = 150;

  // N5 rest depth: the depth over the last s_uiRestEnergyTicks with the slosh's energy taken out must match the seeded
  // volume within s_fVolumeTolerance; the raw depth (the centroid as it stands) within this.
  constexpr plUInt32 s_uiRestEnergyTicks = 150;
  constexpr float s_fRestRawTolerance = 0.05f;

  // N2: max |residual| / max |rhs| of the last solve of every tick.
  constexpr float s_fResidualTolerance = 1e-3f;

  // N7: the front within this fraction of Martin & Moyce.
  constexpr float s_fFrontTolerance = 0.10f;

  // Front: particles in the bottom two cells, binned along x in quarter cells; a bin counts with at least this many.
  constexpr float s_fFrontHeight = 2.0f * s_fDamCellSize;
  constexpr float s_fFrontBin = 0.25f * s_fDamCellSize;
  // A full bin holds about 64; the threshold keeps loose spray ahead of the water from counting as the front.
  constexpr plUInt32 s_uiFrontMinCount = 10;
  constexpr plUInt32 s_uiFrontGapBins = 4;

  // Hydrostatic tank: about half full (16.5 cells, so the surface lies half way up a cell; the top layer holds half
  // the particles, anywhere in its lower half), at rest. Stratified, or with every particle anywhere in the box.
  const plVec3 s_vHydroTank(1.12f, 0.64f, 0.64f);
  constexpr float s_fHydroCellSize = 0.02f;
  // The random-seed variants "Quarter" and "Three Quarter" fill to 16.25 and 16.75 cells: tau of the level-set
  // classification was set at the half-full top layer, and a resting surface at another height must stay as calm.
  constexpr plUInt32 s_uiHydroTicks = 300;
  constexpr float s_fHydroMaxSpeed = 0.01f;
  // The substep count is forced up and let down again in this period (5 for the first half, then free down to 2), so
  // the pool goes through the dt changes the dam break sees.
  constexpr plUInt32 s_uiHydroSubstepPeriod = 90;
  // N6: the particle centroid height stays within this fraction of its first value on every tick.
  constexpr float s_fHydroCentroidTolerance = 0.01f;

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
  constexpr const char* s_szVirtualShadowsCVar = "Rendering.Shadows.UseVirtualShadowMaps";
  constexpr const char* s_szTransparencyModeCVar = "Rendering.Transparency.Mode";
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

  // Pour Into Glass: a 20 x 20 cm glass, 26 cm high with 1.5 cm walls, standing on the floor of a 43 x 43 x 48 cm domain
  // of 6 mm cells; an 18 mm nozzle 36 cm up pours at 0.9 m/s (0.92 l/s) for the whole run.
  const plVec3 s_vPourDomain(0.42f, 0.42f, 0.45f);
  constexpr float s_fPourCellSize = 0.006f;
  constexpr float s_fGlassHalfInner = 0.1f;
  constexpr float s_fGlassWall = 0.015f;
  constexpr float s_fGlassHeight = 0.26f;
  const plVec3 s_vNozzle(0.02f, 0.0f, 0.36f); // world, metres
  const plVec3 s_vNozzleVelocity(0.0f, 0.0f, -0.9f);
  constexpr float s_fNozzleRadius = 0.018f;
  constexpr plUInt32 s_uiPourTicks = 240;
  // Pour Into Glass (not the video): the nozzle stops after s_uiPourTicks, the water settles for this long, and its
  // depth (twice the particle centroid's height over the mean of the last s_uiPourFillTicks) must match the poured
  // volume over the glass's inner area within s_fPourFillTolerance. The volume measure counts an interior liquid cell
  // as full whatever its particle count, so it cannot see a jet packing the pool; the depth does.
  // Medium Room: the plan's Medium reference load (8 x 6 m filled to 0.5 m at 6.25 cm, 128 x 96 x 64 cells, ~400K
  // particles), 300 ticks; the 95th percentile of the relative residual (N2) with the tier's pressure iterations.
  const plVec3 s_vRoomDomain(8.0f, 6.0f, 4.0f);
  constexpr float s_fRoomCellSize = 0.0625f;
  constexpr plUInt32 s_uiRoomTicks = 300;
  constexpr float s_fRoomResidualTolerance = 1e-3f;

  // No Liquid: wall time for the content flag of the earlier sub-tests to expire (Rendering.Memory.ContentHoldTime).
  constexpr float s_fNoLiquidSettleSeconds = 3.0f;

  constexpr plUInt32 s_uiPourSettleTicks = 60;
  constexpr plUInt32 s_uiPourFillTicks = 15;
  constexpr float s_fPourFillTolerance = 0.05f;

  // "P2G LevelSet", "Solve Setup" and "Solve Project" are passes of their own only with PL_GPUFLUID_LIQ_PROFILE=1
  // (m_bProfileKernels); otherwise they are part of P2G and Solve.
  const char* s_szPhases[] = {"GpuFluid Liquid/Begin", "GpuFluid Liquid/Seed", "GpuFluid Liquid/Source", "GpuFluid Liquid/Sort", "GpuFluid Liquid/P2G", "GpuFluid Liquid/P2G LevelSet",
    "GpuFluid Liquid/Solve Setup", "GpuFluid Liquid/Solve", "GpuFluid Liquid/Solve Project", "GpuFluid Liquid/G2P", "GpuFluid Liquid/Render", "GpuFluid Liquid/Surface Sort", "GpuFluid Liquid/Surface", "GpuFluid Liquid/Readback", "GpuFluid Liquid Copy Scene Color",
    "GpuFluid Liquid Copy Depth", "GpuFluid Liquid Back Faces", "GpuFluid Liquid Surface"};

  // Phases not summed into the solver total: Seed keeps its first-tick value; the last four draw the surface in the view.
  bool IsSolverPhase(const char* szPhase)
  {
    return !plStringUtils::IsEqual(szPhase, "GpuFluid Liquid/Seed") && plStringUtils::StartsWith(szPhase, "GpuFluid Liquid/");
  }

  /// Tuning overrides for experiments: PL_GPUFLUID_LIQ_<NAME> in the environment.
  float TuningFloat(const char* szName, float fDefault)
  {
    plStringBuilder sVar("PL_GPUFLUID_LIQ_", szName);
    const plString sValue = plEnvironmentVariableUtils::GetValueString(sVar);
    double fValue = 0.0;
    if (!sValue.IsEmpty() && plConversionUtils::StringToFloat(sValue, fValue).Succeeded())
    {
      plTestFramework::Output(plTestOutput::Details, "Tuning override %s = %s", sVar.GetData(), sValue.GetData());
      return static_cast<float>(fValue);
    }
    return fDefault;
  }

  /// bAllowSleep: brick and volume sleep stay on. The scenes that check calm or motion (N6, N7) turn it off, since a
  /// sleeping brick's particles are frozen and would read as at rest whatever the solver does.
  void ApplyTuningOverrides(plGpuFluidLiquidVolumeComponent* pLiquid, bool bAllowSleep = false)
  {
    pLiquid->m_bAllowSleep = TuningFloat("SLEEP", bAllowSleep ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_uiPressureCycles = static_cast<plUInt32>(TuningFloat("CYCLES", static_cast<float>(pLiquid->m_uiPressureCycles)));
    pLiquid->m_Tuning.m_bSurfaceOwnLevelSet = TuningFloat("SURFACE_OWN_LEVEL_SET", pLiquid->m_Tuning.m_bSurfaceOwnLevelSet ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_bProfileKernels =TuningFloat("PROFILE", pLiquid->m_Tuning.m_bProfileKernels ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_uiPreSmooth = static_cast<plUInt32>(TuningFloat("PRE_SMOOTH", static_cast<float>(pLiquid->m_Tuning.m_uiPreSmooth)));
    pLiquid->m_Tuning.m_uiPostSmooth = static_cast<plUInt32>(TuningFloat("POST_SMOOTH", static_cast<float>(pLiquid->m_Tuning.m_uiPostSmooth)));
    pLiquid->m_Tuning.m_uiCoarsestSweeps = static_cast<plUInt32>(TuningFloat("COARSEST_SWEEPS", static_cast<float>(pLiquid->m_Tuning.m_uiCoarsestSweeps)));
    pLiquid->m_Tuning.m_bPcg = TuningFloat("PCG", pLiquid->m_Tuning.m_bPcg ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_fVolumeGain = TuningFloat("VOLUME_GAIN", pLiquid->m_Tuning.m_fVolumeGain);
    pLiquid->m_Tuning.m_fDensityGain = TuningFloat("DENSITY_GAIN", pLiquid->m_Tuning.m_fDensityGain);
    pLiquid->m_Tuning.m_uiSpeedHistory = static_cast<plUInt32>(TuningFloat("SPEED_HISTORY", static_cast<float>(pLiquid->m_Tuning.m_uiSpeedHistory)));
    pLiquid->m_uiMaxSubsteps = static_cast<plUInt32>(TuningFloat("MAX_SUBSTEPS", static_cast<float>(pLiquid->m_uiMaxSubsteps)));
    pLiquid->m_Tuning.m_uiMinSubsteps = static_cast<plUInt32>(TuningFloat("MIN_SUBSTEPS", static_cast<float>(pLiquid->m_Tuning.m_uiMinSubsteps)));
    pLiquid->m_fFlipBlend = TuningFloat("FLIP", pLiquid->m_fFlipBlend);
    pLiquid->m_Tuning.m_fDensityDeadZone = TuningFloat("DENSITY_DEAD_ZONE", pLiquid->m_Tuning.m_fDensityDeadZone);
    pLiquid->m_Tuning.m_uiLevelSetSmoothSteps = static_cast<plUInt32>(TuningFloat("LEVEL_SET_SMOOTH", static_cast<float>(pLiquid->m_Tuning.m_uiLevelSetSmoothSteps)));
    pLiquid->m_RenderMode = static_cast<plGpuFluidLiquidRenderMode::Enum>(static_cast<int>(TuningFloat("RENDER_MODE", static_cast<float>(pLiquid->m_RenderMode.GetValue()))));
    pLiquid->m_fDebugTopSpeed = TuningFloat("TOP_SPEED", pLiquid->m_fDebugTopSpeed);
    pLiquid->m_fRefractionStrength = TuningFloat("REFRACTION", pLiquid->m_fRefractionStrength);
    pLiquid->m_fSurfaceRoughness = TuningFloat("ROUGHNESS", pLiquid->m_fSurfaceRoughness);
    if (auto* pCVar = static_cast<plCVarInt*>(plCVar::FindCVarByName("GpuFluid.Liquid.SurfaceDebug")))
      *pCVar = static_cast<int>(TuningFloat("SURFACE_DEBUG", static_cast<float>(pCVar->GetValue())));
    pLiquid->m_Tuning.m_bLevelSetOnly = TuningFloat("LEVEL_SET_ONLY", pLiquid->m_Tuning.m_bLevelSetOnly ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_bLevelSet = TuningFloat("LEVEL_SET", pLiquid->m_Tuning.m_bLevelSet ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_bLevelSetTheta = TuningFloat("LEVEL_SET_THETA", pLiquid->m_Tuning.m_bLevelSetTheta ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_fThetaMin = TuningFloat("THETA_MIN", pLiquid->m_Tuning.m_fThetaMin);
    pLiquid->m_Tuning.m_fSprayPhi = TuningFloat("SPRAY_PHI", pLiquid->m_Tuning.m_fSprayPhi);
    pLiquid->m_Tuning.m_fLiquidPhi = TuningFloat("LIQUID_PHI", pLiquid->m_Tuning.m_fLiquidPhi);
    pLiquid->m_Tuning.m_uiLiquidMinCount = static_cast<plUInt32>(TuningFloat("MIN_COUNT", static_cast<float>(pLiquid->m_Tuning.m_uiLiquidMinCount)));
    pLiquid->m_Tuning.m_bBallisticAir =TuningFloat("BALLISTIC", pLiquid->m_Tuning.m_bBallisticAir ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_bBallisticClearOfSurface = TuningFloat("BALLISTIC_SURFACE", pLiquid->m_Tuning.m_bBallisticClearOfSurface ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_bDensityTwoSided = TuningFloat("TWO_SIDED", pLiquid->m_Tuning.m_bDensityTwoSided ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_fDensityMaxError = TuningFloat("DENSITY_MAX_ERROR", pLiquid->m_Tuning.m_fDensityMaxError);
    pLiquid->m_Tuning.m_fParticleRadius = TuningFloat("PARTICLE_RADIUS", pLiquid->m_Tuning.m_fParticleRadius);
    pLiquid->m_Tuning.m_fLevelSetRadius = TuningFloat("LEVEL_SET_RADIUS", pLiquid->m_Tuning.m_fLevelSetRadius);
    pLiquid->m_Tuning.m_fSurfaceBlend = TuningFloat("SURFACE_BLEND", pLiquid->m_Tuning.m_fSurfaceBlend);
    pLiquid->m_Tuning.m_uiSurfaceSmoothSteps = static_cast<plUInt32>(TuningFloat("SURFACE_SMOOTH", static_cast<float>(pLiquid->m_Tuning.m_uiSurfaceSmoothSteps)));
    pLiquid->m_Tuning.m_uiSubstepHoldTicks = static_cast<plUInt32>(TuningFloat("HOLD", static_cast<float>(pLiquid->m_Tuning.m_uiSubstepHoldTicks)));
    pLiquid->m_Tuning.m_fSubstepDownMargin = TuningFloat("DOWN_MARGIN", pLiquid->m_Tuning.m_fSubstepDownMargin);
    pLiquid->m_Tuning.m_fCflNumber = TuningFloat("CFL", pLiquid->m_Tuning.m_fCflNumber);
    pLiquid->m_Tuning.m_bBlockSmooth = TuningFloat("BLOCK_SMOOTH", pLiquid->m_Tuning.m_bBlockSmooth ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_uiFineSweeps = static_cast<plUInt32>(TuningFloat("FINE_SWEEPS", static_cast<float>(pLiquid->m_Tuning.m_uiFineSweeps)));
    pLiquid->m_Tuning.m_uiFinePasses = static_cast<plUInt32>(TuningFloat("FINE_PASSES", static_cast<float>(pLiquid->m_Tuning.m_uiFinePasses)));
    pLiquid->m_Tuning.m_bP2GScatter = TuningFloat("P2G_SCATTER", pLiquid->m_Tuning.m_bP2GScatter ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_bBlockSmoothCoarse = TuningFloat("BLOCK_COARSE", pLiquid->m_Tuning.m_bBlockSmoothCoarse ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_uiCoarsePasses = static_cast<plUInt32>(TuningFloat("COARSE_PASSES", static_cast<float>(pLiquid->m_Tuning.m_uiCoarsePasses)));
    pLiquid->m_Tuning.m_uiCoarseSweeps = static_cast<plUInt32>(TuningFloat("COARSE_SWEEPS", static_cast<float>(pLiquid->m_Tuning.m_uiCoarseSweeps)));

    // A fixed substep count (min = max), for checking that the rest packing does not follow it.
    const plUInt32 uiForced = static_cast<plUInt32>(TuningFloat("FORCE_SUBSTEPS", 0.0f));
    if (uiForced > 0)
    {
      pLiquid->m_uiMaxSubsteps = uiForced;
      pLiquid->m_Tuning.m_uiMinSubsteps = uiForced;
    }
  }

  plUInt32 ForcedSubsteps()
  {
    return static_cast<plUInt32>(plMath::Max(0, plEnvironmentVariableUtils::GetValueInt("PL_GPUFLUID_LIQ_FORCE_SUBSTEPS", 0)));
  }

  void WriteText(plStringView sPath, const plStringBuilder& sText)
  {
    plFileWriter file;
    if (file.Open(sPath).Succeeded())
      file.WriteBytes(sText.GetData(), sText.GetElementCount()).IgnoreResult();
  }

  /// Whether a stat name is the GPU time of szPass itself or of one of its repeats: the render graph names the n-th pass
  /// of a name in one graph "<name> #n", and every substep records Sort, P2G, Solve and G2P again. Reading the plain
  /// name alone counted the first substep only, which is why the P8A timings did not grow with the substep count.
  bool IsPassStat(plStringView sStatName, const char* szPass)
  {
    plStringBuilder sPrefix("Renderer/GPU/", szPass);
    if (!sStatName.StartsWith(sPrefix))
      return false;
    plStringView sRest = sStatName.GetSubString(sPrefix.GetCharacterCount(), plInvalidIndex);
    if (sRest == "[ms]")
      return true;
    if (!sRest.StartsWith(" #") || !sRest.EndsWith("[ms]"))
      return false;
    for (const char* p = sRest.GetStartPointer() + 2; p < sRest.GetEndPointer() - 4; ++p)
    {
      if (*p < '0' || *p > '9')
        return false;
    }
    return true;
  }

  /// The GPU time of a pass in the last exported frame, its repeats included. False when no run of it has a stat.
  bool GetPassMs(const char* szPass, double& out_fMs)
  {
    plDynamicArray<plString> names;
    plDynamicArray<plVariant> values;
    plStats::GetStatsWithPrefix(plStringBuilder("Renderer/GPU/", szPass), names, values);
    bool bAny = false;
    out_fMs = 0.0;
    for (plUInt32 i = 0; i < names.GetCount(); ++i)
    {
      if (IsPassStat(names[i], szPass) && values[i].IsValid() && values[i].CanConvertTo<double>())
      {
        out_fMs += values[i].ConvertTo<double>();
        bAny = true;
      }
    }
    return bAny;
  }

  /// Removes the GPU time stats of a pass and its repeats.
  void RemovePassStats(const char* szPass)
  {
    plDynamicArray<plString> names;
    plDynamicArray<plVariant> values;
    plStats::GetStatsWithPrefix(plStringBuilder("Renderer/GPU/", szPass), names, values);
    for (const plString& sName : names)
    {
      if (IsPassStat(sName, szPass))
        plStats::RemoveStat(sName);
    }
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
  AddSubTest("Hydrostatic Tank Random Seed", SubTests::HydrostaticTankRandom);
  AddSubTest("Pour Into Glass", SubTests::PourIntoGlass);
  AddSubTest("Pour Into Glass Video", SubTests::PourIntoGlassVideo);
  AddSubTest("Medium Room", SubTests::MediumRoom);
  AddSubTest("No Liquid", SubTests::NoLiquid);
  AddSubTest("Hydrostatic Tank Random Seed Quarter", SubTests::HydrostaticTankRandomQuarter);
  AddSubTest("Hydrostatic Tank Random Seed Three Quarter", SubTests::HydrostaticTankRandomThreeQuarter);
  AddSubTest("Medium Room Substep Cost", SubTests::MediumRoomSubstepCost);
  AddSubTest("Static Solids", SubTests::StaticSolids);
  AddSubTest("Open Side Outflow", SubTests::OpenSideOutflow);
  AddSubTest("Sleep And Wake", SubTests::SleepAndWake);
  AddSubTest("Flooding Room", SubTests::FloodingRoom);
  AddSubTest("Flooding Room Video", SubTests::FloodingRoomVideo);
}

plResult plGameEngineTestGpuFluidLiquidGallery::InitializeSubTest(plInt32 iIdentifier)
{
  PL_SUCCEED_OR_RETURN(SUPER::InitializeSubTest(iIdentifier));

  m_bDepthOfFieldWasEnabled = GetCVarBool(s_szDepthOfFieldCVar, true);
  m_bGpuStatsWasEnabled = GetCVarBool(s_szGpuStatsCVar, false);
  m_bVirtualShadowsWereEnabled = GetCVarBool(s_szVirtualShadowsCVar, true);
  m_bRestoreDepthOfField = true;

  // Cascaded shadow maps for the liquid stages: the virtual shadow maps draw a dark stair-stepped line down every inner
  // corner where two lit walls meet (the dam break tank's back corners), with or without the liquid. An engine issue.
  SetCVarBool(s_szVirtualShadowsCVar, false);

  // Transparent surfaces sorted per object: the adaptive voxel OIT (the engine's default, mode 4) draws magenta lines
  // along the edges of the glass panes, above the water as much as in front of it. An engine issue; the liquid itself
  // composites the same in both modes (it draws before the transparent pass and writes depth).
  if (auto* pMode = static_cast<plCVarInt*>(plCVar::FindCVarByName(s_szTransparencyModeCVar)))
  {
    m_iTransparencyModeWas = pMode->GetValue();
    *pMode = static_cast<int>(TuningFloat("TRANSPARENCY_MODE", 0.0f));
  }

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
  m_uiTimedQuads = 0;
  m_uiTimedActiveBricks = 0;
  m_LastStats = plGpuFluidLiquidStats();
  m_bPourStopped = false;

  // Per-phase GPU times are exported as stats while this is on. A stat keeps its last value when its pass stops
  // running, so the earlier sub-tests' values are removed first.
  SetCVarBool(s_szGpuStatsCVar, true);
  for (const char* szPhase : s_szPhases)
    RemovePassStats(szPhase);

  switch (iIdentifier)
  {
    case SubTests::DamBreak:
    case SubTests::DamBreakVideo:
      return SetupDamBreak();

    case SubTests::HydrostaticTank:
      return SetupHydrostatic(false);

    case SubTests::HydrostaticTankRandom:
      return SetupHydrostatic(true);

    case SubTests::HydrostaticTankRandomQuarter:
      return SetupHydrostatic(true, 16.25f);

    case SubTests::HydrostaticTankRandomThreeQuarter:
      return SetupHydrostatic(true, 16.75f);

    case SubTests::PourIntoGlass:
    case SubTests::PourIntoGlassVideo:
      return SetupPour();

    case SubTests::MediumRoom:
      return SetupMediumRoom();

    case SubTests::MediumRoomSubstepCost:
    {
      // Free-running frames: with V-sync the wall time of a frame is at least the display interval.
      m_CostSegments.Clear();
      if (auto* pVSync = static_cast<plCVarBool*>(plCVar::FindCVarByName("App.VSync")))
      {
        m_bVSyncWas = pVSync->GetValue();
        *pVSync = false;
        m_bRestoreVSync = true;
      }
      return SetupMediumRoom();
    }

    case SubTests::NoLiquid:
      return SetupNoLiquid();

    case SubTests::StaticSolids:
      return SetupStaticSolids();
    case SubTests::OpenSideOutflow:
      return SetupOpenSideOutflow();
    case SubTests::SleepAndWake:
      return SetupSleepAndWake();
    case SubTests::FloodingRoom:
      return SetupFloodingRoom(false);
    case SubTests::FloodingRoomVideo:
      return SetupFloodingRoom(true);
  }

  return PL_FAILURE;
}

plResult plGameEngineTestGpuFluidLiquidGallery::DeInitializeSubTest(plInt32 iIdentifier)
{
  if (m_bRestoreDepthOfField)
  {
    SetCVarBool(s_szDepthOfFieldCVar, m_bDepthOfFieldWasEnabled);
    SetCVarBool(s_szGpuStatsCVar, m_bGpuStatsWasEnabled);
    SetCVarBool(s_szVirtualShadowsCVar, m_bVirtualShadowsWereEnabled);
    if (auto* pMode = static_cast<plCVarInt*>(plCVar::FindCVarByName(s_szTransparencyModeCVar)))
      *pMode = m_iTransparencyModeWas;
    m_bRestoreDepthOfField = false;
  }
  if (m_bRestoreVSync)
  {
    if (auto* pVSync = static_cast<plCVarBool*>(plCVar::FindCVarByName("App.VSync")))
      *pVSync = m_bVSyncWas;
    m_bRestoreVSync = false;
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
      return RunHydrostatic(false);
    case SubTests::DamBreakVideo:
      return RunDamBreak(true);
    case SubTests::HydrostaticTankRandom:
    case SubTests::HydrostaticTankRandomQuarter:
    case SubTests::HydrostaticTankRandomThreeQuarter:
      return RunHydrostatic(true);
    case SubTests::PourIntoGlass:
      return RunPour(false);
    case SubTests::PourIntoGlassVideo:
      return RunPour(true);
    case SubTests::MediumRoom:
      return RunMediumRoom();
    case SubTests::MediumRoomSubstepCost:
      return RunMediumRoomCost();
    case SubTests::NoLiquid:
      return RunNoLiquid();
    case SubTests::StaticSolids:
      return RunStaticSolids();
    case SubTests::OpenSideOutflow:
      return RunOpenSideOutflow();
    case SubTests::SleepAndWake:
      return RunSleepAndWake();
    case SubTests::FloodingRoom:
      return RunFloodingRoom(false);
    case SubTests::FloodingRoomVideo:
      return RunFloodingRoom(true);
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

  // A checker of 8 cm tiles on the tank's floor shows the refraction; the sun stands high over the camera's left
  // shoulder, so the tank is lit and the waves facing the camera throw their glints at it.
  AddLiquidStage(plVec2(0.0f, 0.0f), plVec2(0.5f * s_vDamTank.x, 0.5f * s_vDamTank.y), 0.08f, plVec3(-0.3f, 0.45f, 0.85f));

  // The tank: a back wall and two end walls drawn just outside the domain; the side facing the camera is left open.
  // The camera looks from +y towards -y, so +x (the direction the water runs) is to the right of the picture.
  const plColor wall(0.32f, 0.36f, 0.40f);
  m_pOwnApplication->AddBox("TankBack", plVec3(0.0f, -0.5f * s_vDamTank.y - 0.02f, 0.5f * s_vDamTank.z), plVec3(s_vDamTank.x + 0.08f, 0.04f, s_vDamTank.z), wall);
  // The end walls reach 4 cm into the back wall, so the inner corners are not two boxes meeting edge to edge.
  m_pOwnApplication->AddBox("TankLeft", plVec3(-0.5f * s_vDamTank.x - 0.02f, -0.02f, 0.5f * s_vDamTank.z), plVec3(0.04f, s_vDamTank.y + 0.04f, s_vDamTank.z), wall);
  m_pOwnApplication->AddBox("TankRight", plVec3(0.5f * s_vDamTank.x + 0.02f, -0.02f, 0.5f * s_vDamTank.z), plVec3(0.04f, s_vDamTank.y + 0.04f, s_vDamTank.z), wall);

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
  pLiquid->m_uiPressureCycles = 8;
  pLiquid->m_fFlipBlend = 0.1f;
  pLiquid->m_fDebugTopSpeed = 4.0f;
  pLiquid->m_Tuning.m_fFrontHeight = s_fFrontHeight;
  pLiquid->m_Tuning.m_fFrontBinSize = s_fFrontBin;
  ApplyTuningOverrides(pLiquid);

  plGpuFluidLiquidFillBox& column = pLiquid->m_FillBoxes.ExpandAndGetRef();
  column.m_vCenter = plVec3(-0.5f * s_vDamTank.x + 0.5f * s_fDamA, 0.0f, -0.5f * s_vDamTank.z + s_fDamA);
  column.m_vHalfExtents = plVec3(0.5f * s_fDamA, 0.5f * s_vDamTank.y, s_fDamA);

  m_fColumnWidth = s_fDamA;

  // From the open side and above, so the floor shows through the water.
  m_pOwnApplication->SetCamera(plVec3(0.25f, 3.05f, 1.75f), plVec3(0.1f, -0.1f, 0.3f), 50.0f);

  return PL_SUCCESS;
}

void plGameEngineTestGpuFluidLiquidGallery::AddLiquidStage(const plVec2& vCheckerCenter, const plVec2& vCheckerHalfSize, float fTile, const plVec3& vToSun)
{
  m_pOwnApplication->AddSunAndSky(vToSun, 10.0f);
  m_pOwnApplication->AddBox("Ground", plVec3(0.0f, 0.0f, -0.1f), plVec3(2000.0f, 2000.0f, 0.2f), plColor(0.10f, 0.10f, 0.11f));
  // Light tiles on the dark floor, 2 mm high, in a checker that starts with a light tile at the area's corner.
  const plColor light(0.50f, 0.49f, 0.46f);
  const plInt32 iTilesX = static_cast<plInt32>(plMath::Round(2.0f * vCheckerHalfSize.x / fTile));
  const plInt32 iTilesY = static_cast<plInt32>(plMath::Round(2.0f * vCheckerHalfSize.y / fTile));
  for (plInt32 y = 0; y < iTilesY; ++y)
  {
    for (plInt32 x = 0; x < iTilesX; ++x)
    {
      if (((x + y) & 1) != 0)
        continue;
      const plVec3 vCenter(vCheckerCenter.x - vCheckerHalfSize.x + (x + 0.5f) * fTile, vCheckerCenter.y - vCheckerHalfSize.y + (y + 0.5f) * fTile, 0.001f);
      m_pOwnApplication->AddBox("Tile", vCenter, plVec3(fTile, fTile, 0.002f), light);
    }
  }
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupPour()
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());
  pWorld->Clear();
  pWorld->GetRandomNumberGenerator().Initialize(0x47465038u);
  pWorld->GetClock().Reset(false);

  SetCVarBool(s_szDepthOfFieldCVar, false);

  AddLiquidStage(plVec2(0.0f, 0.0f), plVec2(0.36f, 0.36f), 0.03f, plVec3(-0.4f, 0.35f, 0.85f));

  // The domain stands on the floor: its centre is half its height up, the extents rounded up to bricks of 8 cells.
  auto Rounded = [](float fExtent) { return 8.0f * s_fPourCellSize * plMath::Ceil(fExtent / (8.0f * s_fPourCellSize) - 1e-4f); };
  const plVec3 vDomain(Rounded(s_vPourDomain.x), Rounded(s_vPourDomain.y), Rounded(s_vPourDomain.z));

  plGameObjectDesc desc;
  desc.m_sName.Assign("Pour");
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * vDomain.z);
  plGameObject* pObject = nullptr;
  m_hVolumeObject = pWorld->CreateObject(desc, pObject);

  plGpuFluidLiquidVolumeComponent* pLiquid = nullptr;
  pWorld->GetOrCreateComponentManager<plGpuFluidLiquidVolumeComponentManager>()->CreateComponent(pObject, pLiquid);
  pLiquid->m_vExtents = s_vPourDomain;
  pLiquid->m_fCellSize = s_fPourCellSize;
  pLiquid->m_uiParticlesPerCell = 4;
  pLiquid->m_uiMaxParticles = 320 * 1024;
  pLiquid->m_uiMaxSubsteps = 14;
  // 12 reached 1.1e-3 on the tick the stream's tail lands once the nozzle stopped (N2 asks for 1e-3 on every tick).
  pLiquid->m_uiPressureCycles = 16;
  pLiquid->m_fFlipBlend = 0.1f;
  pLiquid->m_fDebugTopSpeed = 3.0f;
  PL_TEST_BOOL((pLiquid->GetDomainSize() - vDomain).IsZero(1e-5f));

  // The glass: four walls of analytic boxes on the domain's floor (volume space: metres from the domain's centre).
  const float fFloor = -0.5f * vDomain.z;
  const float fOuter = s_fGlassHalfInner + s_fGlassWall;
  auto AddWall = [&](const plVec3& vCenter, const plVec3& vHalf)
  {
    // The collider reaches 2 cm under the floor, so liquid at the foot of a wall is pushed sideways, not downwards.
    plGpuFluidLiquidObstacle& wall = pLiquid->m_Obstacles.ExpandAndGetRef();
    wall.m_Shape = plGpuFluidObstacleShape::Box;
    wall.m_vCenter = vCenter - plVec3(0.0f, 0.0f, 0.01f);
    wall.m_vHalfExtents = vHalf + plVec3(0.0f, 0.0f, 0.01f);
  };
  const float fWallZ = fFloor + 0.5f * s_fGlassHeight;
  AddWall(plVec3(-s_fGlassHalfInner - 0.5f * s_fGlassWall, 0.0f, fWallZ), plVec3(0.5f * s_fGlassWall, fOuter, 0.5f * s_fGlassHeight));
  AddWall(plVec3(s_fGlassHalfInner + 0.5f * s_fGlassWall, 0.0f, fWallZ), plVec3(0.5f * s_fGlassWall, fOuter, 0.5f * s_fGlassHeight));
  AddWall(plVec3(0.0f, -s_fGlassHalfInner - 0.5f * s_fGlassWall, fWallZ), plVec3(s_fGlassHalfInner, 0.5f * s_fGlassWall, 0.5f * s_fGlassHeight));
  AddWall(plVec3(0.0f, s_fGlassHalfInner + 0.5f * s_fGlassWall, fWallZ), plVec3(s_fGlassHalfInner, 0.5f * s_fGlassWall, 0.5f * s_fGlassHeight));

  // What is seen of the glass: four panes of the base Transparent material and a thin frame along their edges. The
  // liquid draws at BeforeTransparent and writes depth, so the panes behind it are hidden by it, the front pane blends
  // over it, and the transparent pass's own scene copy (for its refraction) already holds the liquid.
  {
    const plColor frame(0.86f, 0.88f, 0.90f);
    const float fBar = 0.006f;
    const float fEdge = fOuter - 0.5f * fBar;
    for (float fX : {-fEdge, fEdge})
    {
      for (float fY : {-fEdge, fEdge})
        m_pOwnApplication->AddBox("GlassPost", plVec3(fX, fY, 0.5f * s_fGlassHeight), plVec3(fBar, fBar, s_fGlassHeight), frame);
    }
    for (float fZ : {0.5f * fBar, s_fGlassHeight - 0.5f * fBar})
    {
      for (float fS : {-fEdge, fEdge})
      {
        m_pOwnApplication->AddBox("GlassRim", plVec3(fS, 0.0f, fZ), plVec3(fBar, 2.0f * fOuter, fBar), frame);
        m_pOwnApplication->AddBox("GlassRim", plVec3(0.0f, fS, fZ), plVec3(2.0f * fOuter, fBar, fBar), frame);
      }
    }

    // The panes: the four walls as boxes of the base Transparent material (PL_GPUFLUID_LIQ_GLASS_PANES=0: the frame only).
    if (TuningFloat("GLASS_PANES", 1.0f) != 0.0f)
    {
      plMaterialResourceHandle hGlass = plResourceManager::LoadResource<plMaterialResource>("{ 785a756f-c69c-4f44-b31f-1743806fd4f5 }"); // BaseMaterials/Transparent
      const plColor tint(0.85f, 0.92f, 0.95f, TuningFloat("GLASS_ALPHA", 0.15f));
      auto AddPane = [&](const plVec3& vCenter, const plVec3& vSize)
      {
        plGameObject* pPane = m_pOwnApplication->AddBox("GlassPane", vCenter, vSize, tint);
        plMeshComponent* pMesh = nullptr;
        if (pPane != nullptr && pPane->TryGetComponentOfBaseType(pMesh))
          pMesh->SetMaterial(0, hGlass);
      };
      const float fMid = s_fGlassHalfInner + 0.5f * s_fGlassWall;
      AddPane(plVec3(-fMid, 0.0f, 0.5f * s_fGlassHeight), plVec3(s_fGlassWall, 2.0f * fOuter, s_fGlassHeight));
      AddPane(plVec3(fMid, 0.0f, 0.5f * s_fGlassHeight), plVec3(s_fGlassWall, 2.0f * fOuter, s_fGlassHeight));
      AddPane(plVec3(0.0f, -fMid, 0.5f * s_fGlassHeight), plVec3(2.0f * s_fGlassHalfInner, s_fGlassWall, s_fGlassHeight));
      AddPane(plVec3(0.0f, fMid, 0.5f * s_fGlassHeight), plVec3(2.0f * s_fGlassHalfInner, s_fGlassWall, s_fGlassHeight));
    }
  }

  // A striped backdrop behind the glass, for the stream and the water to bend.
  m_pOwnApplication->AddBox("Backdrop", plVec3(0.0f, -0.42f, 0.4f), plVec3(1.6f, 0.02f, 0.8f), plColor(0.42f, 0.40f, 0.36f));
  for (plInt32 i = -10; i <= 10; ++i)
    m_pOwnApplication->AddBox("Stripe", plVec3(i * 0.05f, -0.405f, 0.4f), plVec3(0.025f, 0.01f, 0.8f), plColor(0.45f, 0.10f, 0.06f));

  // The inside of the glass in domain cells, with half a cell of slack.
  const plVec3 vHalfDomain = 0.5f * vDomain;
  m_vGlassMin = (plVec3(-s_fGlassHalfInner, -s_fGlassHalfInner, 0.0f) + plVec3(vHalfDomain.x, vHalfDomain.y, 0.0f)) / s_fPourCellSize - plVec3(0.5f);
  m_vGlassMax = (plVec3(s_fGlassHalfInner, s_fGlassHalfInner, s_fGlassHeight) + plVec3(vHalfDomain.x, vHalfDomain.y, 0.0f)) / s_fPourCellSize + plVec3(0.5f);

  // The nozzle: an inflow disc, and a dark spout just above it.
  plGpuFluidLiquidInflow& inflow = pLiquid->m_Inflows.ExpandAndGetRef();
  inflow.m_vCenter = s_vNozzle - plVec3(0.0f, 0.0f, 0.5f * vDomain.z);
  inflow.m_vVelocity = s_vNozzleVelocity;
  inflow.m_fRadius = s_fNozzleRadius;
  m_pOwnApplication->AddBox("Spout", s_vNozzle + plVec3(0.0f, 0.0f, 0.045f), plVec3(2.0f * s_fNozzleRadius + 0.012f, 2.0f * s_fNozzleRadius + 0.012f, 0.08f), plColor(0.12f, 0.12f, 0.13f));

  ApplyTuningOverrides(pLiquid);

  // From the front and above, close: the stream, the glass and the checker through the water.
  m_pOwnApplication->SetCamera(plVec3(0.07f, 0.70f, 0.47f), plVec3(0.0f, -0.02f, 0.17f), 40.0f);
  return PL_SUCCESS;
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupHydrostatic(bool bRandomSeed, float fDepthCells)
{
  const float fHydroDepth = fDepthCells * s_fHydroCellSize;
  m_sHydroName = !bRandomSeed ? "Hydrostatic" : (fDepthCells == 16.5f ? "HydrostaticRandom" : (fDepthCells < 16.5f ? "HydrostaticRandomQuarter" : "HydrostaticRandomThreeQuarter"));

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
  pLiquid->m_uiPressureCycles = 8;
  pLiquid->m_fDebugTopSpeed = 0.05f;
  ApplyTuningOverrides(pLiquid);

  plGpuFluidLiquidFillBox& fill = pLiquid->m_FillBoxes.ExpandAndGetRef();
  fill.m_vCenter = plVec3(0.0f, 0.0f, -0.5f * s_vHydroTank.z + 0.5f * fHydroDepth);
  fill.m_vHalfExtents = plVec3(0.5f * s_vHydroTank.x, 0.5f * s_vHydroTank.y, 0.5f * fHydroDepth);
  fill.m_bStratified = !bRandomSeed;

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
  sample.m_fMeanSquaredSpeed = stats.m_fMeanSquaredSpeed;
  sample.m_fResidualRatio = stats.m_fRhsMax > 0.0f ? stats.m_fResidualMax / stats.m_fRhsMax : 0.0f;
  sample.m_fCentroidZ = stats.m_vCentroid.z;
  sample.m_fSimTime = static_cast<float>(stats.m_fSimulatedTime);
  sample.m_uiNaN = stats.m_uiNaN;
  sample.m_uiOverflow = stats.m_uiOverflow;
  sample.m_uiAbove1cm = stats.m_uiAbove1cm;
  sample.m_uiSurfaceVertices = stats.m_uiSurfaceVertices;
  sample.m_uiSurfaceQuads = stats.m_uiSurfaceQuads;
  sample.m_uiSurfaceOverflow = stats.m_uiSurfaceOverflow;
  sample.m_uiAwakeBricks = stats.m_uiAwakeBricks;
  sample.m_uiSleepingBricks = stats.m_uiSleepingBricks;
  sample.m_uiInSolid = stats.m_uiInSolid;
  sample.m_uiMaxCellParticles = stats.m_uiMaxCellParticles;
  sample.m_vMaxCell = stats.m_vMaxCell;
  sample.m_uiMaxCellType = stats.m_uiMaxCellType;
  sample.m_fMaxCellPhi = stats.m_fMaxCellPhi;
  sample.m_fMaxCellSdf = stats.m_fMaxCellSdf;
  sample.m_uiSolidCells = stats.m_uiSolidCells;
  for (plUInt32 i = 0; i < 4; ++i)
  {
    sample.m_fProbes[i] = stats.m_fProbeLevels[i];
    sample.m_fProbePhi[i] = stats.m_fProbePhiLevels[i];
  }

  m_uiLastTick = plMath::Max(m_uiLastTick, stats.m_uiTicks);
  m_uiTimedParticles = stats.m_uiAlive;
  m_LastStats = stats;
  return true;
}

void plGameEngineTestGpuFluidLiquidGallery::AccumulateTimings()
{
  bool bAny = false;
  for (const char* szPhase : s_szPhases)
  {
    double fMs = 0.0;
    if (GetPassMs(szPhase, fMs))
    {
      m_PhaseMs[szPhase] += fMs;
      bAny = true;
    }
    // Read once: a stat keeps its value after its pass stops running, so "Solve #3" would outlive a drop to two
    // substeps. A frame whose timings have not arrived yet then adds nothing instead of the last frame's.
    RemovePassStats(szPhase);
  }

  // The solve's stages (m_bProfileKernels: GPU scopes inside the pass), read once like the passes.
  {
    plDynamicArray<plString> names;
    plDynamicArray<plVariant> values;
    plStats::GetStatsWithPrefix("Renderer/GPU/GpuFluid LiqProf/", names, values);
    for (plUInt32 i = 0; i < names.GetCount(); ++i)
    {
      plStringBuilder sStage = names[i];
      sStage.TrimWordStart("Renderer/GPU/GpuFluid ");
      if (const char* szUnit = sStage.FindSubString("["))
        sStage.SetSubString_FromTo(sStage.GetData(), szUnit);
      if (values[i].CanConvertTo<double>())
        m_PhaseMs[sStage] += values[i].ConvertTo<double>();
      plStats::RemoveStat(names[i]);
    }
  }

  if (bAny)
  {
    ++m_uiTimedFrames;
    if (const TickSample* pSample = m_Ticks.GetValue(m_uiLastTick))
    {
      m_uiTimedSubsteps += pSample->m_uiSubsteps;
      m_uiTimedQuads += pSample->m_uiSurfaceQuads;
      m_uiTimedActiveBricks += pSample->m_uiActiveBricks;
    }
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
  double fTotal = 0.0, fDraw = 0.0;
  for (const char* szPhase : s_szPhases)
  {
    const double fMean = m_PhaseMs.GetValueOrDefault(szPhase, 0.0) / m_uiTimedFrames;
    // Seed runs on the first tick only; its stat keeps that frame's value, so it is listed but not summed. The view's
    // surface passes are summed apart.
    if (IsSolverPhase(szPhase))
      fTotal += fMean;
    else if (!plStringUtils::IsEqual(szPhase, "GpuFluid Liquid/Seed"))
      fDraw += fMean;
    ref_sCsv.AppendFormat("{},{}\n", szPhase, fMean);
    plTestFramework::Output(plTestOutput::Message, "GPU %s: %.3f ms per frame", szPhase, fMean);
  }
  for (auto it : m_PhaseMs)
  {
    if (it.Key().StartsWith("LiqProf/"))
    {
      ref_sCsv.AppendFormat("{},{}\n", it.Key(), it.Value() / m_uiTimedFrames);
      plTestFramework::Output(plTestOutput::Message, "GPU solve stage %s: %.3f ms per frame", it.Key().GetData(), it.Value() / m_uiTimedFrames);
    }
  }
  ref_sCsv.AppendFormat("total,{}\n", fTotal);
  ref_sCsv.AppendFormat("draw_total,{}\n", fDraw);

  // Per particle-substep: the substep phases (Sort, P2G, Solve, G2P) over particles times substeps; the solve per
  // active brick and substep; the once-per-frame surface (Surface Sort, Surface) apart.
  const double fFrames = m_uiTimedFrames;
  const double fSubsteps = static_cast<double>(m_uiTimedSubsteps) / fFrames;
  const double fBricks = static_cast<double>(m_uiTimedActiveBricks) / fFrames;
  auto Mean = [&](const char* szPhase) { return m_PhaseMs.GetValueOrDefault(szPhase, 0.0) / fFrames; };
  const double fSubstepMs = Mean("GpuFluid Liquid/Sort") + Mean("GpuFluid Liquid/P2G") + Mean("GpuFluid Liquid/P2G LevelSet") + Mean("GpuFluid Liquid/Solve Setup") +
                            Mean("GpuFluid Liquid/Solve") + Mean("GpuFluid Liquid/Solve Project") + Mean("GpuFluid Liquid/G2P");
  const double fSurfaceMs = Mean("GpuFluid Liquid/Surface Sort") + Mean("GpuFluid Liquid/Surface");
  const double fNsPerParticleSubstep = m_uiTimedParticles > 0 && fSubsteps > 0.0 ? fSubstepMs * 1.0e6 / (static_cast<double>(m_uiTimedParticles) * fSubsteps) : 0.0;
  const double fUsPerBrickSubstep = fBricks > 0.0 && fSubsteps > 0.0 ? Mean("GpuFluid Liquid/Solve") * 1.0e3 / (fBricks * fSubsteps) : 0.0;
  ref_sCsv.AppendFormat("substep_phases_ms,{}\n", fSubstepMs);
  ref_sCsv.AppendFormat("surface_ms,{}\n", fSurfaceMs);
  ref_sCsv.AppendFormat("particles,{}\n", m_uiTimedParticles);
  ref_sCsv.AppendFormat("substeps_per_frame,{}\n", fSubsteps);
  ref_sCsv.AppendFormat("active_bricks,{}\n", fBricks);
  ref_sCsv.AppendFormat("ns_per_particle_substep,{}\n", fNsPerParticleSubstep);
  ref_sCsv.AppendFormat("solve_us_per_brick_substep,{}\n", fUsPerBrickSubstep);
  plTestFramework::Output(plTestOutput::Message,
    "GPU liquid per frame: substep phases %.3f ms (%.2f ns per particle-substep, solve %.2f us per active brick-substep over %.0f bricks), surface %.3f ms, total %.3f ms", fSubstepMs,
    fNsPerParticleSubstep, fUsPerBrickSubstep, fBricks, fSurfaceMs, fTotal);
  plTestFramework::Output(plTestOutput::Message, "GPU liquid total %.3f ms per frame (simulation and surface extraction) plus %.3f ms drawing the surface, over %u frames, %u particles, %.2f substeps and %.0f surface quads per frame on average",
    fTotal, fDraw, m_uiTimedFrames, m_uiTimedParticles, static_cast<double>(m_uiTimedSubsteps) / m_uiTimedFrames, static_cast<double>(m_uiTimedQuads) / m_uiTimedFrames);
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
    SaveVideoFrame("DamBreakVideo", "P7 Dam Break (rendered): 230K particles, 2 cm cells, surface nets");
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

void plGameEngineTestGpuFluidLiquidGallery::SaveVideoFrame(plStringView sFolder, plStringView sTitle)
{
  // The screenshot of this Run() is the frame that was just rendered.
  if (m_iFrame < 1)
    return;

  plStringBuilder sName;
  sName.SetFormat("{}/frame_{}", sFolder, plArgU(static_cast<plUInt32>(m_iFrame - 1), 5, true));
  PL_TEST_BOOL(SaveShot(sName).Succeeded());
  if (m_iFrame == 1)
  {
    plStringBuilder sPath, sText(sTitle, "\n");
    sPath.SetFormat(":imgout/GpuFluidGallery/Liquid/{}/title.txt", sFolder);
    WriteText(sPath, sText);
  }
}

void plGameEngineTestGpuFluidLiquidGallery::ReportSurface(plStringView sName, plUInt32 uiFirstTick)
{
  plUInt32 uiMaxVertices = 0, uiMaxQuads = 0, uiEmpty = 0, uiOverflow = 0, uiTicks = 0;
  double fQuadSum = 0.0;
  for (auto it : m_Ticks)
  {
    if (it.Key() < uiFirstTick)
      continue;
    const TickSample& s = it.Value();
    ++uiTicks;
    uiMaxVertices = plMath::Max(uiMaxVertices, s.m_uiSurfaceVertices);
    uiMaxQuads = plMath::Max(uiMaxQuads, s.m_uiSurfaceQuads);
    fQuadSum += s.m_uiSurfaceQuads;
    uiEmpty += s.m_uiSurfaceQuads == 0 ? 1u : 0u;
    uiOverflow = plMath::Max(uiOverflow, s.m_uiSurfaceOverflow);
  }

  plStringBuilder sText(sName);
  plTestFramework::Output(plTestOutput::Message, "%s surface: %u ticks, vertices up to %u, quads up to %u (mean %.0f), %u ticks without a mesh, overflow %u", sText.GetData(), uiTicks,
    uiMaxVertices, uiMaxQuads, uiTicks > 0 ? fQuadSum / uiTicks : 0.0, uiEmpty, uiOverflow);
  PL_TEST_BOOL_MSG(uiTicks > 0 && uiEmpty == 0, "%s: %u of %u ticks had no surface mesh", sText.GetData(), uiEmpty, uiTicks);
  PL_TEST_INT(uiOverflow, 0);
}

void plGameEngineTestGpuFluidLiquidGallery::EvaluateDamBreak()
{
  ReportSurface("Dam Break", 2);

  PL_TEST_BOOL_MSG(m_uiLastTick >= s_uiDamTicks, "Only %u ticks were read back", m_uiLastTick);
  PL_TEST_BOOL_MSG(m_uiReadbacks > s_uiDamTicks / 2, "Only %u readbacks arrived", m_uiReadbacks);

  plStringBuilder sCsv("tick,alive,seeded,drained,lost,substeps,active_bricks,volume_ratio,front_m,max_speed,mean_speed,residual_ratio,centroid_z,nan,overflow,sim_time,mean_sq_speed\n");
  float fMinVolume = 1e9f, fMaxVolume = -1e9f;
  plUInt32 uiMaxNaN = 0, uiMaxOverflow = 0, uiMaxLost = 0;
  float fMaxResidualRatio = 0.0f;
  for (auto it : m_Ticks)
  {
    const TickSample& s = it.Value();
    sCsv.AppendFormat("{},{},{},{},{},{},{},{},", it.Key(), s.m_uiAlive, s.m_uiSeeded, s.m_uiDrained, s.m_uiLost, s.m_uiSubsteps, s.m_uiActiveBricks, s.m_fVolumeRatio);
    sCsv.AppendFormat("{},{},{},{},{},{},{},{},{}\n", s.m_fFront, s.m_fMaxSpeed, s.m_fMeanSpeed, s.m_fResidualRatio, s.m_fCentroidZ, s.m_uiNaN, s.m_uiOverflow, s.m_fSimTime, s.m_fMeanSquaredSpeed);
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
  // surface cells) is checked on every tick against +-3%; its 1 s (30 tick) moving mean is reported. That measure is
  // the one the global volume control steers, so two checks do not depend on it: at the end the pool is near rest and
  // twice the particle centroid height is its depth (depth * floor area against the seeded volume), and over the last
  // 5 s the centroid height may not jump from one tick to the next (a pool that briefly loses its pressure support
  // sinks as a whole and comes back).
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

  // The same depth with the slosh's potential energy taken out. A pool still sloshing at 20 s holds its centroid higher
  // than at rest by its wave potential energy over g, and averaged over whole periods that equals the kinetic energy
  // (equipartition of linear waves): z_rest = <z_c> - <|v|^2> / (2 g). Averaged over the last 5 s, a little more than
  // one period of the tank's first mode (2 L / sqrt(g h) = 4.3 s). What is left is the packing the volume control and
  // the density correction hold; checked against +-3%. The raw value above is reported and held to s_fRestRawTolerance.
  double fRestCentroidSum = 0.0, fRestSquaredSpeedSum = 0.0;
  plUInt32 uiRestCount = 0;
  for (auto it : m_Ticks)
  {
    if (it.Key() + s_uiRestEnergyTicks > m_uiLastTick && it.Key() <= m_uiLastTick)
    {
      fRestCentroidSum += it.Value().m_fCentroidZ;
      fRestSquaredSpeedSum += it.Value().m_fMeanSquaredSpeed;
      ++uiRestCount;
    }
  }
  const double fRestZ = uiRestCount > 0 ? (fRestCentroidSum - fRestSquaredSpeedSum / (2.0 * s_fGravity)) / uiRestCount : 0.0;
  const float fRestVolumeEnergy = static_cast<float>(2.0 * fRestZ * s_vDamTank.x * s_vDamTank.y / fSeededVolume);
  const float fSloshRms = uiRestCount > 0 ? static_cast<float>(plMath::Sqrt(fRestSquaredSpeedSum / uiRestCount)) : 0.0f;

  // Largest tick-to-tick change of the centroid height over the last 5 s, relative.
  float fMaxCentroidStep = 0.0f;
  plUInt32 uiMaxCentroidStepTick = 0;
  for (auto it : m_Ticks)
  {
    if (it.Key() + s_uiCentroidStepTicks <= m_uiLastTick || it.Key() > m_uiLastTick)
      continue;
    const TickSample* pPrev = m_Ticks.GetValue(it.Key() - 1);
    if (pPrev == nullptr || pPrev->m_fCentroidZ <= 0.0f)
      continue;
    const float fStep = plMath::Abs(it.Value().m_fCentroidZ - pPrev->m_fCentroidZ) / pPrev->m_fCentroidZ;
    if (fStep > fMaxCentroidStep)
    {
      fMaxCentroidStep = fStep;
      uiMaxCentroidStepTick = it.Key();
    }
  }

  plTestFramework::Output(plTestOutput::Message,
    "N5: %u readbacks, %u with alive != seeded - drained, lost %u, NaN %u, overflow %u; cell volume measure per tick %.4f .. %.4f (limit +-%.0f%%), 30-tick mean %.4f .. %.4f; depth volume over the 60 ticks up to %u %.4f; largest centroid step over the last %u ticks %.2f%% at tick %u (limit %.0f%%)",
    m_uiReadbacks, m_uiN5Mismatches, uiMaxLost, uiMaxNaN, uiMaxOverflow, fMinVolume, fMaxVolume, s_fVolumeTolerance * 100.0f, fMinMean, fMaxMean, m_uiLastTick, fRestVolume, s_uiCentroidStepTicks,
    fMaxCentroidStep * 100.0f, uiMaxCentroidStepTick, s_fCentroidStepTolerance * 100.0f);
  plTestFramework::Output(plTestOutput::Message,
    "N5 rest depth: raw %.4f of the seeded volume (limit +-%.0f%%), with the slosh's energy taken out over the last %u ticks %.4f (limit +-%.0f%%); slosh rms speed %.3f m/s", fRestVolume,
    s_fRestRawTolerance * 100.0f, s_uiRestEnergyTicks, fRestVolumeEnergy, s_fVolumeTolerance * 100.0f, fSloshRms);
  PL_TEST_INT(m_uiN5Mismatches, 0);
  PL_TEST_INT(uiMaxLost, 0);
  PL_TEST_INT(uiMaxNaN, 0);
  PL_TEST_BOOL_MSG(fMinVolume >= 1.0f - s_fVolumeTolerance && fMaxVolume <= 1.0f + s_fVolumeTolerance, "N5: the volume measure ran %.4f .. %.4f over the ticks", fMinVolume, fMaxVolume);
  PL_TEST_BOOL_MSG(plMath::Abs(fRestVolumeEnergy - 1.0f) <= s_fVolumeTolerance, "N5: the pool, its slosh energy taken out, holds %.4f of the seeded volume", fRestVolumeEnergy);
  PL_TEST_BOOL_MSG(plMath::Abs(fRestVolume - 1.0f) <= s_fRestRawTolerance, "N5: the pool at 20 s holds %.4f of the seeded volume", fRestVolume);
  PL_TEST_BOOL_MSG(fMaxCentroidStep <= s_fCentroidStepTolerance, "N5: the centroid height changed by %.2f%% from tick %u to %u", fMaxCentroidStep * 100.0f, uiMaxCentroidStepTick - 1, uiMaxCentroidStepTick);

  // N2: the pressure solve converges on every tick.
  plTestFramework::Output(plTestOutput::Message, "N2: max residual/rhs over the ticks %.2e (limit %.0e)", fMaxResidualRatio, s_fResidualTolerance);
  PL_TEST_BOOL_MSG(fMaxResidualRatio <= s_fResidualTolerance, "N2: relative residual %.2e", fMaxResidualRatio);

  // Slow motion: how much less time than ticks * dt was simulated (the substep cap was not enough).
  if (const TickSample* pLast = m_Ticks.GetValue(m_uiLastTick))
  {
    plTestFramework::Output(plTestOutput::Message, "Simulated %.3f s in %u ticks of %.4f s (%.3f s of slow motion)", pLast->m_fSimTime, m_uiLastTick, s_fTickSeconds,
      m_uiLastTick * s_fTickSeconds - pLast->m_fSimTime);
  }

  // N7: front against Martin & Moyce. T = t sqrt(2g/a), t the simulated time up to the sample's tick.
  const float fTScale = plMath::Sqrt(2.0f * s_fGravity / m_fColumnWidth);
  plDynamicArray<plVec2> curve; // (T, Z/a)
  curve.PushBack(plVec2(0.0f, 1.0f));
  for (auto it : m_Ticks)
  {
    if (it.Value().m_fFront > 0.0f && it.Key() > 0)
      curve.PushBack(plVec2(it.Value().m_fSimTime * fTScale, it.Value().m_fFront / m_fColumnWidth));
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

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunHydrostatic(bool bRandomSeed)
{
  PL_IGNORE_UNUSED(bRandomSeed);

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  CollectStats();

  // Substeps: forced to at least 5 for the first half of each period, free (down to 2) for the second (unless
  // PL_GPUFLUID_LIQ_FORCE_SUBSTEPS fixes the count).
  if (ForcedSubsteps() == 0)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGpuFluidLiquidVolumeComponent* pVolume = GetVolume())
      pVolume->m_Tuning.m_uiMinSubsteps = (static_cast<plUInt32>(m_iFrame) % s_uiHydroSubstepPeriod) < s_uiHydroSubstepPeriod / 2 ? 5u : 2u;
  }

  if (m_iFrame == 30 || m_iFrame == 300)
  {
    plStringBuilder sName;
    sName.SetFormat("{}_{}", m_sHydroName, plArgI(m_iFrame, 4, true));
    PL_TEST_BOOL(SaveShot(sName).Succeeded());
  }

  if (m_iFrame >= 60 && m_iFrame < 160)
    AccumulateTimings();

  if (m_uiLastTick >= s_uiHydroTicks || m_iFrame >= static_cast<plInt32>(s_uiHydroTicks) + 100)
  {
    EvaluateHydrostatic(bRandomSeed);
    return plTestAppRun::Quit;
  }
  return plTestAppRun::Continue;
}

void plGameEngineTestGpuFluidLiquidGallery::EvaluateHydrostatic(bool bRandomSeed)
{
  PL_TEST_BOOL_MSG(m_uiLastTick >= s_uiHydroTicks, "Only %u ticks were read back", m_uiLastTick);
  PL_IGNORE_UNUSED(bRandomSeed);
  const char* szName = m_sHydroName.GetData();

  plStringBuilder sCsv("tick,alive,max_speed,mean_speed,volume_ratio,residual_ratio,centroid_z,substeps,above_1cm\n");
  for (auto it : m_Ticks)
  {
    const TickSample& s = it.Value();
    sCsv.AppendFormat("{},{},{},{},{},{},{},{},{}\n", it.Key(), s.m_uiAlive, s.m_fMaxSpeed, s.m_fMeanSpeed, s.m_fVolumeRatio, s.m_fResidualRatio, s.m_fCentroidZ, s.m_uiSubsteps, s.m_uiAbove1cm);
  }
  plStringBuilder sPath;
  sPath.SetFormat(":imgout/GpuFluidGallery/Liquid/{}_ticks.csv", szName);
  WriteText(sPath, sCsv);

  const TickSample* pLast = m_Ticks.GetValue(m_uiLastTick);
  if (!PL_TEST_BOOL(pLast != nullptr))
    return;

  const TickSample* pFirst = m_Ticks.IsEmpty() ? nullptr : &m_Ticks.GetIterator().Value();
  if (!PL_TEST_BOOL(pFirst != nullptr && pFirst->m_fCentroidZ > 0.0f))
    return;

  // Over every tick: the volume measure, the centroid height against the first, the residual, the substep range.
  float fMinVolume = 1e9f, fMaxVolume = -1e9f, fMaxDrift = 0.0f, fMaxResidual = 0.0f, fMaxSpeedAfter = 0.0f;
  plUInt32 uiMinSubsteps = 0xFFFFFFFFu, uiMaxSubsteps = 0, uiMaxNaN = 0, uiMaxAbove1cmAfter = 0;
  plUInt32 uiMaxSpeedAfterTick = 0;
  for (auto it : m_Ticks)
  {
    const TickSample& s = it.Value();
    if (it.Key() < 1 || it.Key() > s_uiHydroTicks)
      continue;
    fMinVolume = plMath::Min(fMinVolume, s.m_fVolumeRatio);
    fMaxVolume = plMath::Max(fMaxVolume, s.m_fVolumeRatio);
    fMaxDrift = plMath::Max(fMaxDrift, plMath::Abs(s.m_fCentroidZ - pFirst->m_fCentroidZ) / pFirst->m_fCentroidZ);
    fMaxResidual = plMath::Max(fMaxResidual, s.m_fResidualRatio);
    uiMinSubsteps = plMath::Min(uiMinSubsteps, s.m_uiSubsteps);
    uiMaxSubsteps = plMath::Max(uiMaxSubsteps, s.m_uiSubsteps);
    uiMaxNaN = plMath::Max(uiMaxNaN, s.m_uiNaN);
    if (it.Key() > s_uiHydroTicks - 60)
    {
      if (s.m_fMaxSpeed > fMaxSpeedAfter)
      {
        fMaxSpeedAfter = s.m_fMaxSpeed;
        uiMaxSpeedAfterTick = it.Key();
      }
      uiMaxAbove1cmAfter = plMath::Max(uiMaxAbove1cmAfter, s.m_uiAbove1cm);
    }
  }

  plTestFramework::Output(plTestOutput::Message,
    "N6 tick %u: max |v| %.4f m/s with %u particles above 1 cm/s; over the last 60 ticks max |v| %.4f m/s (tick %u) and up to %u particles above 1 cm/s (limit %.3f m/s); mean %.5f m/s; volume measure per tick %.4f .. %.4f; centroid z %.4f m (first %.4f m, largest drift %.2f%%, limit %.0f%%); substeps %u .. %u; N2 max residual/rhs %.2e; N5 mismatches %u, lost %u",
    m_uiLastTick, pLast->m_fMaxSpeed, pLast->m_uiAbove1cm, fMaxSpeedAfter, uiMaxSpeedAfterTick, uiMaxAbove1cmAfter, s_fHydroMaxSpeed, pLast->m_fMeanSpeed, fMinVolume, fMaxVolume, pLast->m_fCentroidZ,
    pFirst->m_fCentroidZ, fMaxDrift * 100.0f, s_fHydroCentroidTolerance * 100.0f, uiMinSubsteps, uiMaxSubsteps, fMaxResidual, m_uiN5Mismatches, pLast->m_uiLost);
  PL_TEST_INT(m_uiN5Mismatches, 0);
  PL_TEST_INT(uiMaxNaN, 0);
  PL_TEST_BOOL_MSG(pLast->m_fMaxSpeed < s_fHydroMaxSpeed, "N6: max |v| %.4f m/s after %u ticks", pLast->m_fMaxSpeed, m_uiLastTick);
  PL_TEST_BOOL_MSG(fMaxSpeedAfter < s_fHydroMaxSpeed, "N6: max |v| %.4f m/s at tick %u of the last 60 (%u particles above 1 cm/s at most)", fMaxSpeedAfter, uiMaxSpeedAfterTick, uiMaxAbove1cmAfter);
  PL_TEST_BOOL_MSG(fMinVolume >= 1.0f - s_fVolumeTolerance && fMaxVolume <= 1.0f + s_fVolumeTolerance, "N6: the volume measure ran %.4f .. %.4f", fMinVolume, fMaxVolume);
  PL_TEST_BOOL_MSG(fMaxDrift <= s_fHydroCentroidTolerance, "N6: the centroid height drifted by %.2f%%", fMaxDrift * 100.0f);
  PL_TEST_BOOL_MSG(fMaxResidual <= s_fResidualTolerance, "N2: relative residual %.2e", fMaxResidual);
  if (ForcedSubsteps() == 0)
    PL_TEST_BOOL_MSG(uiMinSubsteps <= 2 && uiMaxSubsteps >= 5, "N6: the substep count only ran %u .. %u", uiMinSubsteps, uiMaxSubsteps);

  plStringBuilder sTimes;
  ReportTimings(sTimes);
  sPath.SetFormat(":imgout/GpuFluidGallery/Liquid/{}_timings.csv", szName);
  WriteText(sPath, sTimes);
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunPour(bool bVideo)
{
  if (bVideo && plEnvironmentVariableUtils::GetValueInt("PL_GPUFLUID_VIDEO", 0) != 1)
  {
    plTestFramework::Output(plTestOutput::Details, "Skipped: set PL_GPUFLUID_VIDEO=1 to record the frames.");
    return plTestAppRun::Quit;
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  CollectStats();

  if (bVideo)
  {
    SaveVideoFrame("PourVideo", "P7 Pour Into Glass: 0.9 l/s into a 20 cm glass, 6 mm cells, rendered surface");
    return static_cast<plUInt32>(m_iFrame) >= s_uiPourTicks ? plTestAppRun::Quit : plTestAppRun::Continue;
  }

  if (m_iFrame >= 120 && m_iFrame < 220)
    AccumulateTimings();

  for (plInt32 iShot : {15, 60, 120, 240, 300})
  {
    if (m_iFrame == iShot)
    {
      plStringBuilder sName;
      sName.SetFormat("Pour_{}", plArgI(iShot, 4, true));
      PL_TEST_BOOL(SaveShot(sName).Succeeded());
    }
  }

  // The nozzle stops once tick s_uiPourTicks is read back; the water then settles for the fill-height check.
  if (!m_bPourStopped && m_uiLastTick >= s_uiPourTicks)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGpuFluidLiquidVolumeComponent* pVolume = GetVolume())
      pVolume->m_Inflows.Clear();
    m_bPourStopped = true;
  }

  if (m_uiLastTick >= s_uiPourTicks + s_uiPourSettleTicks || m_iFrame >= static_cast<plInt32>(s_uiPourTicks + s_uiPourSettleTicks) + 100)
  {
    EvaluatePour();
    return plTestAppRun::Quit;
  }
  return plTestAppRun::Continue;
}

void plGameEngineTestGpuFluidLiquidGallery::EvaluatePour()
{
  const plUInt32 uiEndTick = s_uiPourTicks + s_uiPourSettleTicks;
  PL_TEST_BOOL_MSG(m_uiLastTick >= uiEndTick, "Only %u ticks were read back", m_uiLastTick);

  plStringBuilder sCsv("tick,alive,seeded,max_speed,mean_speed,substeps,residual_ratio,surface_vertices,surface_quads,sim_time,centroid_z\n");
  plUInt32 uiMaxNaN = 0;
  float fMaxResidual = 0.0f;
  const TickSample* pPour = nullptr; // the last sample of the pour itself, before the nozzle stopped
  float fCentroidSum = 0.0f;
  plUInt32 uiCentroidCount = 0;
  for (auto it : m_Ticks)
  {
    const TickSample& s = it.Value();
    sCsv.AppendFormat("{},{},{},{},{},{},{},{},{},{},{}\n", it.Key(), s.m_uiAlive, s.m_uiSeeded, s.m_fMaxSpeed, s.m_fMeanSpeed, s.m_uiSubsteps, s.m_fResidualRatio, s.m_uiSurfaceVertices,
      s.m_uiSurfaceQuads, s.m_fSimTime, s.m_fCentroidZ);
    uiMaxNaN = plMath::Max(uiMaxNaN, s.m_uiNaN);
    fMaxResidual = plMath::Max(fMaxResidual, s.m_fResidualRatio);
    if (it.Key() <= s_uiPourTicks)
      pPour = &s;
    if (it.Key() + s_uiPourFillTicks > m_uiLastTick)
    {
      fCentroidSum += s.m_fCentroidZ;
      ++uiCentroidCount;
    }
  }
  WriteText(":imgout/GpuFluidGallery/Liquid/Pour_ticks.csv", sCsv);

  const TickSample* pLast = m_Ticks.GetValue(m_uiLastTick);
  if (!PL_TEST_BOOL(pLast != nullptr && pPour != nullptr && uiCentroidCount > 0))
    return;

  // The inflow up to the stop: the nozzle's area times its speed over the simulated time, in particles (n0 per cell).
  const float fParticleVolume = s_fPourCellSize * s_fPourCellSize * s_fPourCellSize / 4.0f;
  const float fExpected = plMath::Pi<float>() * s_fNozzleRadius * s_fNozzleRadius * s_vNozzleVelocity.GetLength() * pPour->m_fSimTime / fParticleVolume;
  const float fSeededRatio = fExpected > 0.0f ? static_cast<float>(pPour->m_uiSeeded) / fExpected : 0.0f;

  // Fill height: the settled water's depth against all the particles' volume spread over the glass's inner area.
  const float fGlassArea = 4.0f * s_fGlassHalfInner * s_fGlassHalfInner;
  const float fExpectedDepth = static_cast<float>(pLast->m_uiAlive) * fParticleVolume / fGlassArea;
  const float fDepth = 2.0f * fCentroidSum / static_cast<float>(uiCentroidCount);
  const float fFillRatio = fExpectedDepth > 0.0f ? fDepth / fExpectedDepth : 0.0f;

  // Everything is inside the glass's footprint at the end (the stream above it included).
  const plVec3U32 vMin = m_LastStats.m_vBoundsMinCell;
  const plVec3U32 vMax = m_LastStats.m_vBoundsMaxCell;
  const bool bInside = static_cast<float>(vMin.x) >= m_vGlassMin.x && static_cast<float>(vMin.y) >= m_vGlassMin.y && static_cast<float>(vMax.x) + 1.0f <= m_vGlassMax.x &&
                       static_cast<float>(vMax.y) + 1.0f <= m_vGlassMax.y;

  plTestFramework::Output(plTestOutput::Message,
    "Pour: %u ticks (the nozzle on for %u, %.3f s simulated), %u particles (seeded %u by tick %u, %.4f of the nozzle's flow), N5 mismatches %u, lost %u, NaN %u; particle cells x %u..%u y %u..%u z %u..%u, glass inside x/y %.1f..%.1f; settled depth %.4f m against %.4f m from the poured volume (%.4f, limit +-%.0f%%); N2 max residual/rhs %.2e",
    m_uiLastTick, s_uiPourTicks, pPour->m_fSimTime, pLast->m_uiAlive, pPour->m_uiSeeded, s_uiPourTicks, fSeededRatio, m_uiN5Mismatches, pLast->m_uiLost, uiMaxNaN, vMin.x, vMax.x, vMin.y, vMax.y,
    vMin.z, vMax.z, m_vGlassMin.x, m_vGlassMax.x, fDepth, fExpectedDepth, fFillRatio, s_fPourFillTolerance * 100.0f, fMaxResidual);
  PL_TEST_INT(m_uiN5Mismatches, 0);
  PL_TEST_INT(pLast->m_uiLost, 0);
  PL_TEST_INT(uiMaxNaN, 0);
  PL_TEST_BOOL_MSG(plMath::Abs(fSeededRatio - 1.0f) <= 0.02f, "Pour: the nozzle seeded %.4f of its flow", fSeededRatio);
  PL_TEST_BOOL_MSG(plMath::Abs(fFillRatio - 1.0f) <= s_fPourFillTolerance, "Pour: the settled water stands %.4f of the depth its volume fills", fFillRatio);
  PL_TEST_BOOL_MSG(bInside, "Pour: particles outside the glass (cells x %u..%u, y %u..%u)", vMin.x, vMax.x, vMin.y, vMax.y);
  PL_TEST_BOOL_MSG(fMaxResidual <= s_fResidualTolerance, "N2: relative residual %.2e", fMaxResidual);
  ReportSurface("Pour", 2);

  plStringBuilder sTimes;
  ReportTimings(sTimes);
  WriteText(":imgout/GpuFluidGallery/Liquid/Pour_timings.csv", sTimes);
}

void plGameEngineTestGpuFluidLiquidGallery::ReportResiduals(plStringView sName, plUInt32 uiFirstTick, float& out_fMedian, float& out_fP95, float& out_fMax) const
{
  plDynamicArray<float> residuals;
  for (auto it : m_Ticks)
  {
    if (it.Key() >= uiFirstTick)
      residuals.PushBack(it.Value().m_fResidualRatio);
  }

  out_fMedian = out_fP95 = out_fMax = 0.0f;
  if (residuals.IsEmpty())
    return;

  residuals.Sort();
  const plUInt32 n = residuals.GetCount();
  out_fMedian = residuals[n / 2];
  out_fP95 = residuals[plMath::Min(n - 1, (n * 95) / 100)];
  out_fMax = residuals[n - 1];

  plStringBuilder sText(sName);
  plTestFramework::Output(plTestOutput::Message, "%s N2 relative residual over %u ticks: median %.2e, p95 %.2e, max %.2e", sText.GetData(), n, out_fMedian, out_fP95, out_fMax);
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupMediumRoom()
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());
  pWorld->Clear();
  pWorld->GetRandomNumberGenerator().Initialize(0x47465039u);
  pWorld->GetClock().Reset(false);

  SetCVarBool(s_szDepthOfFieldCVar, false);
  AddLiquidStage(plVec2(0.0f, 0.0f), plVec2(4.0f, 3.0f), 0.5f, plVec3(-0.3f, 0.45f, 0.85f));

  // The plan's Medium reference load: a room of 8 x 6 m filled to 0.5 m at 6.25 cm, 128 x 96 x 64 cells, 4 particles
  // per cell. The water starts at 0.75 m over the left half and 0.25 m over the right (0.5 m on average, 393,216
  // particles), so it sloshes across the room and the solver has work on every tick.
  plGameObjectDesc desc;
  desc.m_sName.Assign("MediumRoom");
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * s_vRoomDomain.z);
  plGameObject* pObject = nullptr;
  m_hVolumeObject = pWorld->CreateObject(desc, pObject);

  plGpuFluidLiquidVolumeComponent* pLiquid = nullptr;
  pWorld->GetOrCreateComponentManager<plGpuFluidLiquidVolumeComponentManager>()->CreateComponent(pObject, pLiquid);
  pLiquid->m_vExtents = s_vRoomDomain;
  pLiquid->m_fCellSize = s_fRoomCellSize;
  pLiquid->m_uiParticlesPerCell = 4;
  pLiquid->m_uiMaxParticles = 512 * 1024;
  pLiquid->m_Quality = plGpuFluidQuality::Medium; // the tier's substep cap and pressure iterations
  pLiquid->m_fDebugTopSpeed = 2.0f;
  ApplyTuningOverrides(pLiquid);
  PL_TEST_BOOL(pLiquid->GetCells() == plVec3U32(128, 96, 64));

  const float fFloor = -0.5f * s_vRoomDomain.z;
  for (plUInt32 i = 0; i < 2; ++i)
  {
    const float fDepth = i == 0 ? 0.75f : 0.25f;
    plGpuFluidLiquidFillBox& fill = pLiquid->m_FillBoxes.ExpandAndGetRef();
    fill.m_vCenter = plVec3((i == 0 ? -0.25f : 0.25f) * s_vRoomDomain.x, 0.0f, fFloor + 0.5f * fDepth);
    fill.m_vHalfExtents = plVec3(0.25f * s_vRoomDomain.x, 0.5f * s_vRoomDomain.y, 0.5f * fDepth);
  }

  m_pOwnApplication->SetCamera(plVec3(0.0f, 9.0f, 6.0f), plVec3(0.0f, 0.0f, 0.3f), 55.0f);
  return PL_SUCCESS;
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunMediumRoom()
{
  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  CollectStats();

  // The plan's timing rule: 60 warm-up frames, then 180 measured.
  if (m_iFrame >= 60 && m_iFrame < 240)
    AccumulateTimings();

  for (plInt32 iShot : {30, 120, 240})
  {
    if (m_iFrame == iShot)
    {
      plStringBuilder sName;
      sName.SetFormat("MediumRoom_{}", plArgI(iShot, 4, true));
      PL_TEST_BOOL(SaveShot(sName).Succeeded());
    }
  }

  if (m_uiLastTick >= s_uiRoomTicks || m_iFrame >= static_cast<plInt32>(s_uiRoomTicks) + 100)
  {
    EvaluateMediumRoom();
    return plTestAppRun::Quit;
  }
  return plTestAppRun::Continue;
}

void plGameEngineTestGpuFluidLiquidGallery::EvaluateMediumRoom()
{
  PL_TEST_BOOL_MSG(m_uiLastTick >= s_uiRoomTicks, "Only %u ticks were read back", m_uiLastTick);

  plStringBuilder sCsv("tick,alive,substeps,active_bricks,volume_ratio,max_speed,mean_speed,residual_ratio,sim_time,nan\n");
  plUInt32 uiMaxNaN = 0, uiSubstepHist[17] = {};
  float fMinVolume = 1e9f, fMaxVolume = -1e9f;
  for (auto it : m_Ticks)
  {
    const TickSample& s = it.Value();
    sCsv.AppendFormat("{},{},{},{},{},{},{},{},{},{}\n", it.Key(), s.m_uiAlive, s.m_uiSubsteps, s.m_uiActiveBricks, s.m_fVolumeRatio, s.m_fMaxSpeed, s.m_fMeanSpeed, s.m_fResidualRatio, s.m_fSimTime, s.m_uiNaN);
    uiMaxNaN = plMath::Max(uiMaxNaN, s.m_uiNaN);
    ++uiSubstepHist[plMath::Min<plUInt32>(s.m_uiSubsteps, 16)];
    if (it.Key() >= 1)
    {
      fMinVolume = plMath::Min(fMinVolume, s.m_fVolumeRatio);
      fMaxVolume = plMath::Max(fMaxVolume, s.m_fVolumeRatio);
    }
  }
  WriteText(":imgout/GpuFluidGallery/Liquid/MediumRoom_ticks.csv", sCsv);

  const TickSample* pLast = m_Ticks.GetValue(m_uiLastTick);
  if (!PL_TEST_BOOL(pLast != nullptr))
    return;

  float fMedian, fP95, fMax;
  ReportResiduals("Medium Room", 2, fMedian, fP95, fMax);

  plStringBuilder sSubsteps;
  for (plUInt32 i = 1; i <= 16; ++i)
  {
    if (uiSubstepHist[i] > 0)
      sSubsteps.AppendFormat(" {}:{}", i, uiSubstepHist[i]);
  }
  plTestFramework::Output(plTestOutput::Message,
    "Medium Room: %u particles, %u ticks, %.3f s simulated of %.3f s (slow motion %.1f%%), substeps per tick (count:ticks)%s; volume measure %.4f .. %.4f; N5 mismatches %u, lost %u, NaN %u",
    pLast->m_uiAlive, m_uiLastTick, pLast->m_fSimTime, m_uiLastTick * s_fTickSeconds, 100.0f * (1.0f - pLast->m_fSimTime / plMath::Max(m_uiLastTick * s_fTickSeconds, 1e-6f)), sSubsteps.GetData(),
    fMinVolume, fMaxVolume, m_uiN5Mismatches, pLast->m_uiLost, uiMaxNaN);

  PL_TEST_INT(m_uiN5Mismatches, 0);
  PL_TEST_INT(pLast->m_uiLost, 0);
  PL_TEST_INT(uiMaxNaN, 0);
  PL_TEST_BOOL_MSG(fMinVolume >= 1.0f - s_fVolumeTolerance && fMaxVolume <= 1.0f + s_fVolumeTolerance, "Medium Room: the volume measure ran %.4f .. %.4f", fMinVolume, fMaxVolume);
  // The Medium tier's substep cap (plan amendment A2), with slow motion beyond it.
  PL_TEST_BOOL_MSG(ForcedSubsteps() > 0 || uiSubstepHist[4] + uiSubstepHist[5] + uiSubstepHist[6] + uiSubstepHist[7] + uiSubstepHist[8] == 0, "Medium Room: a tick ran more than 3 substeps");
  PL_TEST_BOOL_MSG(fP95 <= s_fRoomResidualTolerance, "N2 Medium Room: 95th percentile of the relative residual %.2e (limit %.0e)", fP95, s_fRoomResidualTolerance);

  plStringBuilder sTimes;
  ReportTimings(sTimes);
  WriteText(":imgout/GpuFluidGallery/Liquid/MediumRoom_timings.csv", sTimes);
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunMediumRoomCost()
{
  // The Medium room at forced substep counts, 100 frames each (the first 30 discarded: the stats and GPU timings of a
  // frame arrive two to three frames late). Every timed frame then ran the segment's substep count, so the cost per
  // substep is the slope across segments, measured three ways: the wall time of a free-running frame (V-sync off; the
  // frame is GPU bound), the GPU time of the whole frame, and the sum of the substep phases' own pass timings.
  constexpr plUInt32 uiSegmentFrames = 100;
  constexpr plUInt32 uiDiscard = 30;
  static const plUInt32 s_Substeps[] = {1, 2, 3, 2};

  const plUInt32 uiSegment = static_cast<plUInt32>(m_iFrame) / uiSegmentFrames;
  const plUInt32 uiInSegment = static_cast<plUInt32>(m_iFrame) % uiSegmentFrames;
  if (uiSegment < PL_ARRAY_SIZE(s_Substeps) && uiInSegment == 0)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGpuFluidLiquidVolumeComponent* pVolume = GetVolume())
    {
      pVolume->m_uiMaxSubsteps = s_Substeps[uiSegment];
      pVolume->m_Tuning.m_uiMinSubsteps = s_Substeps[uiSegment];
    }
    // A stat keeps its last value once its pass stops running: "Sort #3" from three substeps would still be summed at
    // two. The discarded frames refill what runs now.
    for (const char* szPhase : {"GpuFluid Liquid/Sort", "GpuFluid Liquid/P2G", "GpuFluid Liquid/Solve", "GpuFluid Liquid/G2P"})
      RemovePassStats(szPhase);
    m_CostSegments.ExpandAndGetRef().m_uiSubsteps = s_Substeps[uiSegment];
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;
  CollectStats();

  const plTime now = plTime::Now();
  if (uiSegment < PL_ARRAY_SIZE(s_Substeps) && uiInSegment >= uiDiscard)
  {
    CostSegment& segment = m_CostSegments.PeekBack();
    segment.m_WallMs.PushBack((now - m_LastFrameTime).GetMilliseconds());
    const plVariant frameGpu = plStats::GetStatValue("Renderer/GPU/RENDER FRAME[ms]");
    if (frameGpu.IsValid() && frameGpu.CanConvertTo<double>())
      segment.m_FrameGpuMs.PushBack(frameGpu.ConvertTo<double>());
    double fPhases = 0.0;
    bool bFresh = false;
    for (const char* szPhase : {"GpuFluid Liquid/Sort", "GpuFluid Liquid/P2G", "GpuFluid Liquid/Solve", "GpuFluid Liquid/G2P"})
    {
      double fMs = 0.0;
      if (GetPassMs(szPhase, fMs))
      {
        fPhases += fMs;
        bFresh = true;
      }
      RemovePassStats(szPhase); // read once, see AccumulateTimings
    }
    if (bFresh)
      segment.m_PhaseMs.PushBack(fPhases);
  }
  m_LastFrameTime = now;

  if (uiSegment < PL_ARRAY_SIZE(s_Substeps))
    return plTestAppRun::Continue;

  auto Median = [](plDynamicArray<double>& ref_values) -> double
  {
    if (ref_values.IsEmpty())
      return 0.0;
    ref_values.Sort();
    return ref_values[ref_values.GetCount() / 2];
  };

  plStringBuilder sCsv("substeps,frames,wall_ms_median,frame_gpu_ms_median,substep_phases_ms_median\n");
  double wall[4] = {}, gpu[4] = {}, phases[4] = {};
  for (plUInt32 i = 0; i < m_CostSegments.GetCount(); ++i)
  {
    CostSegment& s = m_CostSegments[i];
    wall[i] = Median(s.m_WallMs);
    gpu[i] = Median(s.m_FrameGpuMs);
    phases[i] = Median(s.m_PhaseMs);
    sCsv.AppendFormat("{},{},{},{},{}\n", s.m_uiSubsteps, s.m_WallMs.GetCount(), wall[i], gpu[i], phases[i]);
    plTestFramework::Output(plTestOutput::Message, "Substep cost, %u substeps: wall %.2f ms, frame GPU %.2f ms, substep phases' pass timings %.2f ms per frame (median of %u frames)", s.m_uiSubsteps,
      wall[i], gpu[i], phases[i], s.m_WallMs.GetCount());
  }

  // Slopes from 1 to 3 substeps, per substep; the repeat at 2 substeps shows the drift over the run.
  const double fParticles = plMath::Max(1u, m_LastStats.m_uiAlive);
  const double fWallSlope = 0.5 * (wall[2] - wall[0]);
  const double fGpuSlope = 0.5 * (gpu[2] - gpu[0]);
  const double fPhaseSlope = 0.5 * (phases[2] - phases[0]);
  plTestFramework::Output(plTestOutput::Message,
    "Substep cost per substep (1 -> 3 substeps): wall %.2f ms (%.2f ns per particle-substep), frame GPU %.2f ms (%.2f ns), pass timings %.2f ms (%.2f ns); fixed part (wall at 1 substep minus one substep) %.2f ms; repeat at 2 substeps: wall %.2f -> %.2f ms; %u particles, %u active bricks",
    fWallSlope, fWallSlope * 1.0e6 / fParticles, fGpuSlope, fGpuSlope * 1.0e6 / fParticles, fPhaseSlope, fPhaseSlope * 1.0e6 / fParticles, wall[0] - fWallSlope, wall[1], wall[3],
    m_LastStats.m_uiAlive, m_LastStats.m_uiActiveBricks);
  sCsv.AppendFormat("wall_ms_per_substep,{}\nframe_gpu_ms_per_substep,{}\npass_timings_ms_per_substep,{}\nparticles,{}\nactive_bricks,{}\n", fWallSlope, fGpuSlope, fPhaseSlope,
    m_LastStats.m_uiAlive, m_LastStats.m_uiActiveBricks);
  WriteText(":imgout/GpuFluidGallery/Liquid/MediumRoom_substep_cost.csv", sCsv);

  // The work grows with the substep count; a clock that does not see that does not measure it.
  PL_TEST_BOOL_MSG(wall[2] > wall[0] * 1.2, "Substep cost: the wall time per frame did not grow from 1 to 3 substeps (%.2f -> %.2f ms)", wall[0], wall[2]);
  PL_TEST_BOOL_MSG(phases[2] >= 1.3 * phases[1], "Substep cost: the substep phases' pass timings at 3 substeps (%.2f ms) are not 1.3 times those at 2 (%.2f ms)", phases[2], phases[1]);
  return plTestAppRun::Quit;
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupNoLiquid()
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());
  pWorld->Clear();
  pWorld->GetClock().Reset(false);
  SetCVarBool(s_szDepthOfFieldCVar, false);

  // The dam break's stage without the liquid volume.
  AddLiquidStage(plVec2(0.0f, 0.0f), plVec2(0.5f * s_vDamTank.x, 0.5f * s_vDamTank.y), 0.08f, plVec3(-0.3f, 0.45f, 0.85f));
  m_pOwnApplication->SetCamera(plVec3(0.25f, 3.05f, 1.75f), plVec3(0.1f, -0.1f, 0.3f), 50.0f);
  m_hVolumeObject.Invalidate();
  return PL_SUCCESS;
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunNoLiquid()
{
  // The liquid pass's own GPU passes must not run in a view without liquid: their stats are removed once the content
  // flag the earlier sub-tests set has expired (Rendering.Memory.ContentHoldTime, 2 s of wall time), and must not come
  // back over the next 30 frames.
  if (m_iFrame == 0)
  {
    m_NoLiquidStart = plTime::Now();
    m_iNoLiquidClearedFrame = -1;
  }
  if (m_iNoLiquidClearedFrame < 0 && plTime::Now() - m_NoLiquidStart > plTime::MakeFromSeconds(s_fNoLiquidSettleSeconds))
  {
    m_iNoLiquidClearedFrame = m_iFrame;
    for (const char* szPhase : s_szPhases)
    {
      if (!plStringUtils::StartsWith(szPhase, "GpuFluid Liquid/"))
        RemovePassStats(szPhase);
    }
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  if (m_iNoLiquidClearedFrame < 0 || m_iFrame < m_iNoLiquidClearedFrame + 30)
    return m_iFrame < 3000 ? plTestAppRun::Continue : plTestAppRun::Quit;

  plUInt32 uiRan = 0;
  for (const char* szPhase : s_szPhases)
  {
    if (plStringUtils::StartsWith(szPhase, "GpuFluid Liquid/"))
      continue;
    double fMs = 0.0;
    if (GetPassMs(szPhase, fMs))
    {
      plTestFramework::Output(plTestOutput::Error, "No Liquid: the pass '%s' ran (%.3f ms)", szPhase, fMs);
      ++uiRan;
    }
  }
  plTestFramework::Output(plTestOutput::Message, "No Liquid: %u of the liquid pass's GPU passes ran over 30 frames without a liquid volume", uiRan);
  PL_TEST_INT(uiRan, 0);
  return plTestAppRun::Quit;
}

//////////////////////////////////////////////////////////////////////////
// P8: static solids baked from collision meshes, drains and open faces, brick and volume sleep, the flooding room.

namespace
{
  // Static Solids: a dam break in a 1.28 x 0.32 x 0.64 m channel of 2 cm cells over baked solids (a step, a ramp tilted
  // 20 degrees, a crate turned 30 degrees) up to a single-sided wall: an open plane mesh facing the water at 59.6 cells.
  const plVec3 s_vSolidsDomain(1.28f, 0.32f, 0.64f);
  constexpr float s_fSolidsCellSize = 0.02f;
  constexpr float s_fSolidsWallCells = 59.6f;
  constexpr plUInt32 s_uiSolidsTicks = 240;
  constexpr plInt32 s_iSolidsRebakeFrame = 120;
  // The bake's solid cells against the boxes' own: equal but for cell centres within float rounding of a surface.
  constexpr float s_fSolidCellTolerance = 0.005f;

  // Flooding Room: two rooms of a 4.16 x 3.2 m floor (the domain's walls), 4 cm cells, a partition with a doorway, a
  // table, chairs, crates, a shelf and a cabinet baked from meshes; a burst pipe high on the back wall pours 200 l/s from
  // 0.5 s to 12.5 s; a floor drain opens at 13 s.
  const plVec3 s_vRoomFloor(4.16f, 3.2f, 1.92f);
  constexpr float s_fFloodCellSize = 0.04f;
  constexpr float s_fFloodFlow = 0.2f; // m^3/s
  constexpr float s_fFloodPipeRadius = 0.14f;
  const plVec3 s_vFloodPipe(-0.1f, -1.2f, 1.3f); // world, the inflow disc
  constexpr plUInt32 s_uiFloodStartFrame = 15;
  constexpr plUInt32 s_uiFloodStopFrame = 375;
  constexpr plUInt32 s_uiFloodDrainFrame = 555;  // 6 s after the pour stops: the level is measured over the 4 s before
  constexpr plUInt32 s_uiFloodTestFrames = 1155; // 20 s of draining
  constexpr plUInt32 s_uiFloodVideoFrames = 750; // 25 s
  const plVec3 s_vFloodDrain(-0.85f, -0.95f, 0.0f); // world, centre of the drain on the floor
  constexpr float s_fFloodDrainHalf = 0.2f;
  // N5 flood level: the water depth at the probes, averaged over the 4 s before the drain opens, against the poured
  // volume over the free floor. The rooms still slosh then (a seiche of about 6 s across 4 m, the probes swing by
  // +-30%), and the time average at a point is the rest level. During the pour the level is reported only.
  constexpr float s_fFloodLevelTolerance = 0.05f;
  // The drain against Torricelli: the discharge coefficient Q / (a sqrt(2 g h)) over 1 to 3 s after it opens (the
  // flow has started, the level has not fallen much). A sharp-edged orifice is about 0.6.
  constexpr float s_fFloodDrainCdMin = 0.4f;
  constexpr float s_fFloodDrainCdMax = 1.0f;
  const plVec2 s_FloodProbes[] = {plVec2(0.25f, 1.15f), plVec2(-1.6f, -0.45f), plVec2(1.6f, -0.25f), plVec2(-0.25f, 1.25f)};

  plVec3 DomainMinOf(const plVec3& vCenter, const plVec3& vExtents) { return vCenter - 0.5f * vExtents; }
} // namespace

float plGameEngineTestGpuFluidLiquidGallery::BoxDistance(const BakedBox& box, const plVec3& vPoint)
{
  const plVec3 vLocal = box.m_qRotation.GetInverse() * (vPoint - box.m_vCenter);
  const plVec3 q = plVec3(plMath::Abs(vLocal.x), plMath::Abs(vLocal.y), plMath::Abs(vLocal.z)) - 0.5f * box.m_vSize;
  const plVec3 vOut(plMath::Max(q.x, 0.0f), plMath::Max(q.y, 0.0f), plMath::Max(q.z, 0.0f));
  return vOut.GetLength() + plMath::Min(plMath::Max(q.x, plMath::Max(q.y, q.z)), 0.0f);
}

plUInt32 plGameEngineTestGpuFluidLiquidGallery::CountExpectedSolidCells(const plDynamicArray<BakedBox>& boxes, const plVec3& vDomainMin, const plVec3U32& vCells, float fCellSize)
{
  // The bake's rule (LiquidSolidsBake): solid where the signed distance is below half the minimum thickness.
  const float fGrow = 0.5f * GPU_FLUID_LIQUID_SDF_MIN_THICKNESS_CELLS * fCellSize;
  plUInt32 uiCount = 0;
  // Per cell over the boxes whose bounding sphere reaches it.
  plHybridArray<float, 32> radii;
  for (const BakedBox& box : boxes)
    radii.PushBack(0.5f * box.m_vSize.GetLength() + fGrow + fCellSize);
  for (plUInt32 z = 0; z < vCells.z; ++z)
  {
    for (plUInt32 y = 0; y < vCells.y; ++y)
    {
      for (plUInt32 x = 0; x < vCells.x; ++x)
      {
        const plVec3 p = vDomainMin + plVec3(x + 0.5f, y + 0.5f, z + 0.5f) * fCellSize;
        for (plUInt32 i = 0; i < boxes.GetCount(); ++i)
        {
          if ((p - boxes[i].m_vCenter).GetLengthSquared() > radii[i] * radii[i])
            continue;
          if (BoxDistance(boxes[i], p) < fGrow)
          {
            ++uiCount;
            break;
          }
        }
      }
    }
  }
  return uiCount;
}

float plGameEngineTestGpuFluidLiquidGallery::FreeVolumeBelow(const plDynamicArray<BakedBox>& boxes, const plVec3& vDomainMin, const plVec2& vFloorSize, float fGrow, float fHeight)
{
  // Columns of 2 cm over the floor; in each the union of the boxes' vertical extents (boxes turned about z only, so a
  // box covers a column over one interval) is taken out of [floor, floor + h].
  constexpr float fStep = 0.02f;
  const plUInt32 uiNx = static_cast<plUInt32>(vFloorSize.x / fStep);
  const plUInt32 uiNy = static_cast<plUInt32>(vFloorSize.y / fStep);
  const float fFloor = vDomainMin.z;
  double fVolume = 0.0;
  plHybridArray<plVec2, 16> intervals;
  for (plUInt32 j = 0; j < uiNy; ++j)
  {
    for (plUInt32 i = 0; i < uiNx; ++i)
    {
      const plVec3 p(vDomainMin.x + (i + 0.5f) * fStep, vDomainMin.y + (j + 0.5f) * fStep, 0.0f);
      intervals.Clear();
      for (const BakedBox& box : boxes)
      {
        const plVec3 vLocal = box.m_qRotation.GetInverse() * plVec3(p.x - box.m_vCenter.x, p.y - box.m_vCenter.y, 0.0f);
        if (plMath::Abs(vLocal.x) > 0.5f * box.m_vSize.x + fGrow || plMath::Abs(vLocal.y) > 0.5f * box.m_vSize.y + fGrow)
          continue;
        const float fLo = plMath::Max(box.m_vCenter.z - 0.5f * box.m_vSize.z - fGrow, fFloor);
        const float fHi = plMath::Min(box.m_vCenter.z + 0.5f * box.m_vSize.z + fGrow, fFloor + fHeight);
        if (fHi > fLo)
          intervals.PushBack(plVec2(fLo, fHi));
      }
      intervals.Sort([](const plVec2& a, const plVec2& b) { return a.x < b.x; });
      float fCovered = 0.0f, fEnd = fFloor;
      for (const plVec2& iv : intervals)
      {
        const float fStart = plMath::Max(iv.x, fEnd);
        if (iv.y > fStart)
        {
          fCovered += iv.y - fStart;
          fEnd = iv.y;
        }
      }
      fVolume += (fHeight - fCovered) * fStep * fStep;
    }
  }
  return static_cast<float>(fVolume);
}

float plGameEngineTestGpuFluidLiquidGallery::ExpectedFloodLevel(float fVolume) const
{
  const float fGrow = 0.5f * GPU_FLUID_LIQUID_SDF_MIN_THICKNESS_CELLS * m_fDomainCellSize;
  const plVec2 vFloor(m_vDomainCells.x * m_fDomainCellSize, m_vDomainCells.y * m_fDomainCellSize);
  float fLo = 0.0f, fHi = m_vDomainCells.z * m_fDomainCellSize;
  for (plUInt32 i = 0; i < 24; ++i)
  {
    const float fMid = 0.5f * (fLo + fHi);
    if (FreeVolumeBelow(m_BakedBoxes, m_vDomainMin, vFloor, fGrow, fMid) < fVolume)
      fLo = fMid;
    else
      fHi = fMid;
  }
  return 0.5f * (fLo + fHi);
}

void plGameEngineTestGpuFluidLiquidGallery::AddBakedBox(const BakedBox& box, const plColor& color)
{
  m_BakedBoxes.PushBack(box);

  plWorld* pWorld = m_pOwnApplication->GetWorld();
  plGameObject* pSolids = nullptr;
  if (!pWorld->TryGetObject(m_hSolidsObject, pSolids))
  {
    plGameObjectDesc desc;
    desc.m_sName.Assign("BakedSolids");
    m_hSolidsObject = pWorld->CreateObject(desc, pSolids);
    plGpuFluidTestSolidComponent* pComponent = nullptr;
    pWorld->GetOrCreateComponentManager<plGpuFluidTestSolidComponentManager>()->CreateComponent(pSolids, pComponent);
  }

  plGpuFluidTestSolidComponent* pComponent = nullptr;
  if (pSolids->TryGetComponentOfBaseType(pComponent))
  {
    plGpuFluidTestSolidComponent::Part& part = pComponent->m_Parts.ExpandAndGetRef();
    part.m_LocalTransform = plTransform(box.m_vCenter, box.m_qRotation, box.m_vSize);
    part.m_hMesh = plGpuFluidTestSolidComponent::GetUnitBox();
  }

  if (color.a > 0.0f)
  {
    // The rotation goes in at creation: the box is static, and turning it afterwards counts as moving it.
    m_pOwnApplication->AddBox("Solid", box.m_vCenter, box.m_vSize, plColor(color.r, color.g, color.b), box.m_qRotation);
  }
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupStaticSolids()
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());
  pWorld->Clear();
  pWorld->GetRandomNumberGenerator().Initialize(0x4746503Au);
  pWorld->GetClock().Reset(false);
  SetCVarBool(s_szDepthOfFieldCVar, false);

  AddLiquidStage(plVec2(0.0f, 0.0f), plVec2(0.64f, 0.16f), 0.04f, plVec3(-0.3f, 0.45f, 0.85f));
  m_pOwnApplication->AddBox("ChannelBack", plVec3(0.0f, -0.5f * s_vSolidsDomain.y - 0.02f, 0.5f * s_vSolidsDomain.z), plVec3(s_vSolidsDomain.x + 0.08f, 0.04f, s_vSolidsDomain.z), plColor(0.5f, 0.5f, 0.52f));

  const plVec3 vCenter(0.0f, 0.0f, 0.5f * s_vSolidsDomain.z);
  m_vDomainMin = DomainMinOf(vCenter, s_vSolidsDomain);
  m_fDomainCellSize = s_fSolidsCellSize;

  // The solids: closed box meshes (a step, a ramp, a crate) through the extraction, and the open plane.
  const plColor stone(0.55f, 0.5f, 0.45f);
  AddBakedBox({plVec3(-0.12f, 0.0f, 0.04f), plVec3(0.16f, 0.5f, 0.14f), plQuat::MakeIdentity()}, stone);
  AddBakedBox({plVec3(0.18f, 0.0f, 0.0f), plVec3(0.3f, 0.5f, 0.08f), plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(-20.0f))}, stone);
  AddBakedBox({plVec3(0.38f, 0.0f, 0.07f), plVec3(0.12f, 0.12f, 0.14f), plQuat::MakeFromAxisAndAngle(s_vUp, plAngle::MakeFromDegree(30.0f))}, plColor(0.6f, 0.45f, 0.3f));
  m_uiExpectedSolidCells = CountExpectedSolidCells(m_BakedBoxes, m_vDomainMin, plVec3U32(64, 16, 32), s_fSolidsCellSize);

  // The single-sided wall: a quad facing -x, larger than the domain's cross section by more than the band. Behind it
  // the bake is solid for the band (3 cells), in front for half the minimum thickness.
  const float fWallX = m_vDomainMin.x + s_fSolidsWallCells * s_fSolidsCellSize;
  {
    plGeometry plane;
    plGeometry::GeoOptions options;
    options.m_MainAxis = plBasisAxis::NegativeX;
    plane.AddRect(plVec2(s_vSolidsDomain.z + 0.4f, s_vSolidsDomain.y + 0.4f), 1, 1, options);
    plGameObject* pSolids = nullptr;
    plGpuFluidTestSolidComponent* pComponent = nullptr;
    if (pWorld->TryGetObject(m_hSolidsObject, pSolids) && pSolids->TryGetComponentOfBaseType(pComponent))
    {
      plGpuFluidTestSolidComponent::Part& part = pComponent->m_Parts.ExpandAndGetRef();
      part.m_LocalTransform = plTransform(plVec3(fWallX, 0.0f, vCenter.z));
      part.m_hMesh = plGpuFluidTestSolidComponent::CreateCpuMesh("PlaneNegX", plane);
    }
    m_pOwnApplication->AddBox("Wall", plVec3(fWallX + 0.002f, 0.0f, 0.5f * s_vSolidsDomain.z), plVec3(0.004f, s_vSolidsDomain.y, s_vSolidsDomain.z), plColor(0.7f, 0.7f, 0.72f));
    const float fBand = GPU_FLUID_LIQUID_SDF_BAND_CELLS;
    const float fHalfThickness = 0.5f * GPU_FLUID_LIQUID_SDF_MIN_THICKNESS_CELLS;
    plUInt32 uiColumns = 0;
    for (plUInt32 x = 0; x < 64; ++x)
    {
      const float fCentre = x + 0.5f;
      if (fCentre > s_fSolidsWallCells - fHalfThickness && fCentre < s_fSolidsWallCells + fBand)
        ++uiColumns;
    }
    m_uiExpectedSolidCells += uiColumns * 16 * 32;
    m_uiWallCell = static_cast<plUInt32>(s_fSolidsWallCells);
  }

  plGameObjectDesc desc;
  desc.m_sName.Assign("StaticSolids");
  desc.m_LocalPosition = vCenter;
  plGameObject* pObject = nullptr;
  m_hVolumeObject = pWorld->CreateObject(desc, pObject);

  plGpuFluidLiquidVolumeComponent* pLiquid = nullptr;
  pWorld->GetOrCreateComponentManager<plGpuFluidLiquidVolumeComponentManager>()->CreateComponent(pObject, pLiquid);
  pLiquid->m_vExtents = s_vSolidsDomain;
  pLiquid->m_fCellSize = s_fSolidsCellSize;
  pLiquid->m_uiParticlesPerCell = 4;
  pLiquid->m_uiMaxParticles = 128 * 1024;
  pLiquid->m_uiMaxSubsteps = 8;
  pLiquid->m_uiPressureCycles = 8;
  pLiquid->m_fDebugTopSpeed = 2.0f;
  ApplyTuningOverrides(pLiquid);
  m_vDomainCells = pLiquid->GetCells();
  PL_TEST_BOOL(m_vDomainCells == plVec3U32(64, 16, 32));

  plGpuFluidLiquidFillBox& column = pLiquid->m_FillBoxes.ExpandAndGetRef();
  column.m_vCenter = plVec3(-0.5f * s_vSolidsDomain.x + 0.15f, 0.0f, -0.5f * s_vSolidsDomain.z + 0.225f);
  column.m_vHalfExtents = plVec3(0.15f, 0.5f * s_vSolidsDomain.y, 0.225f);

  m_uiMaxParticleCellX = 0;
  m_uiMaxInSolid = 0;
  m_iRebakeFrame = -1;
  m_pOwnApplication->SetCamera(plVec3(0.1f, 1.6f, 0.9f), plVec3(0.05f, 0.0f, 0.12f), 45.0f);
  return PL_SUCCESS;
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunStaticSolids()
{
  if (m_iFrame == s_iSolidsRebakeFrame)
  {
    // The cvar asks every liquid volume for a new bake; the simulation goes on.
    if (auto* pCVar = static_cast<plCVarInt*>(plCVar::FindCVarByName("GpuFluid.Rebake")))
      *pCVar = pCVar->GetValue() + 1;
    m_iRebakeFrame = m_iFrame;
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  if (CollectStats())
  {
    m_uiMaxParticleCellX = plMath::Max(m_uiMaxParticleCellX, m_LastStats.m_vBoundsMaxCell.x);
    m_uiMaxInSolid = plMath::Max(m_uiMaxInSolid, m_LastStats.m_uiInSolid);
  }

  for (plInt32 iShot : {20, 60, 240})
  {
    if (m_iFrame == iShot)
    {
      plStringBuilder sName;
      sName.SetFormat("StaticSolids_{}", plArgI(iShot, 4, true));
      PL_TEST_BOOL(SaveShot(sName).Succeeded());
    }
  }

  if (m_uiLastTick < s_uiSolidsTicks && m_iFrame < static_cast<plInt32>(s_uiSolidsTicks) + 100)
    return plTestAppRun::Continue;

  PL_TEST_BOOL_MSG(m_uiLastTick >= s_uiSolidsTicks, "Only %u ticks were read back", m_uiLastTick);
  plUInt32 uiBakes = 0, uiTriangles = 0, uiBuilt = 0;
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    if (plGpuFluidLiquidVolumeComponent* pVolume = GetVolume())
    {
      plGpuFluidLiquidStats stats;
      if (pVolume->GetStats(stats))
      {
        uiBakes = stats.m_uiSolidBakes;
        uiTriangles = stats.m_uiSolidTriangles;
      }
      uiBuilt = pVolume->GetStaticSolidsBuilt();
    }
  }

  // The bake's solid cells, the first tick that saw them, and that they stayed the same through the rebake.
  plUInt32 uiFirstSolidTick = 0, uiSolidMin = 0xFFFFFFFFu, uiSolidMax = 0, uiMaxNaN = 0;
  for (auto it : m_Ticks)
  {
    const TickSample& s = it.Value();
    uiMaxNaN = plMath::Max(uiMaxNaN, s.m_uiNaN);
    if (s.m_uiSolidCells == 0)
      continue;
    if (uiFirstSolidTick == 0)
      uiFirstSolidTick = it.Key();
    uiSolidMin = plMath::Min(uiSolidMin, s.m_uiSolidCells);
    uiSolidMax = plMath::Max(uiSolidMax, s.m_uiSolidCells);
  }
  const float fSolidRatio = m_uiExpectedSolidCells > 0 ? static_cast<float>(uiSolidMax) / m_uiExpectedSolidCells : 0.0f;

  plTestFramework::Output(plTestOutput::Message,
    "Static Solids: %u bakes on the GPU (%u built on the CPU, the second asked for by GpuFluid.Rebake at frame %d), %u triangles; solid cells %u .. %u from tick %u against %u from the boxes' own distance (%.4f); particles reached cell x %u, the single-sided wall at cell %.1f; N9 particles left deeper than half a cell in a solid %u; N5 mismatches %u, NaN %u",
    uiBakes, uiBuilt, m_iRebakeFrame, uiTriangles, uiSolidMin, uiSolidMax, uiFirstSolidTick, m_uiExpectedSolidCells, fSolidRatio, m_uiMaxParticleCellX, s_fSolidsWallCells, m_uiMaxInSolid,
    m_uiN5Mismatches, uiMaxNaN);
  PL_TEST_BOOL_MSG(uiBakes == 2 && uiBuilt == 2, "Static Solids: %u bakes on the GPU, %u built, expected 2 (activation and the rebake)", uiBakes, uiBuilt);
  // Three boxes and a quad are 38 triangles; the step's and the ramp's +-y faces (8 triangles) lie at |y| = 0.25 m,
  // beyond the 0.32 m wide domain and its margin, so the bake culls them.
  PL_TEST_BOOL_MSG(uiTriangles == 3 * 12 + 2 - 8, "Static Solids: %u triangles on the GPU, expected 30 (three boxes and a quad, less the 8 outside the domain)", uiTriangles);
  PL_TEST_BOOL_MSG(uiFirstSolidTick > 0 && uiFirstSolidTick <= 5, "Static Solids: the solids arrived at tick %u", uiFirstSolidTick);
  PL_TEST_BOOL_MSG(plMath::Abs(fSolidRatio - 1.0f) <= s_fSolidCellTolerance && uiSolidMin == uiSolidMax, "Static Solids: %u .. %u solid cells, the boxes' own distance gives %u", uiSolidMin, uiSolidMax, m_uiExpectedSolidCells);
  // The water reaches the wall (the solid side is behind it, not in front) and nothing passes it.
  PL_TEST_BOOL_MSG(m_uiMaxParticleCellX < m_uiWallCell && m_uiMaxParticleCellX + 2 >= m_uiWallCell, "Static Solids: particles reached cell x %u, the wall is at %u", m_uiMaxParticleCellX, m_uiWallCell);
  PL_TEST_INT(m_uiMaxInSolid, 0);
  PL_TEST_INT(m_uiN5Mismatches, 0);
  PL_TEST_INT(uiMaxNaN, 0);
  return plTestAppRun::Quit;
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupOpenSideOutflow()
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());
  pWorld->Clear();
  pWorld->GetRandomNumberGenerator().Initialize(0x4746503Bu);
  pWorld->GetClock().Reset(false);
  SetCVarBool(s_szDepthOfFieldCVar, false);

  // A channel open at +x: a column collapses and runs out of the domain; what leaves counts as drained.
  const plVec3 vDomain(1.28f, 0.32f, 0.48f);
  AddLiquidStage(plVec2(0.0f, 0.0f), plVec2(0.64f, 0.16f), 0.04f, plVec3(-0.3f, 0.45f, 0.85f));

  plGameObjectDesc desc;
  desc.m_sName.Assign("OpenSide");
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * vDomain.z);
  plGameObject* pObject = nullptr;
  m_hVolumeObject = pWorld->CreateObject(desc, pObject);

  plGpuFluidLiquidVolumeComponent* pLiquid = nullptr;
  pWorld->GetOrCreateComponentManager<plGpuFluidLiquidVolumeComponentManager>()->CreateComponent(pObject, pLiquid);
  pLiquid->m_vExtents = vDomain;
  pLiquid->m_fCellSize = 0.02f;
  pLiquid->m_uiParticlesPerCell = 4;
  pLiquid->m_uiMaxParticles = 128 * 1024;
  pLiquid->m_uiMaxSubsteps = 8;
  pLiquid->m_uiPressureCycles = 8;
  pLiquid->m_OpenFaces = plGpuFluidDomainFaces::PosX;
  ApplyTuningOverrides(pLiquid);

  plGpuFluidLiquidFillBox& column = pLiquid->m_FillBoxes.ExpandAndGetRef();
  column.m_vCenter = plVec3(-0.5f * vDomain.x + 0.2f, 0.0f, -0.5f * vDomain.z + 0.2f);
  column.m_vHalfExtents = plVec3(0.2f, 0.5f * vDomain.y, 0.2f);

  m_pOwnApplication->SetCamera(plVec3(0.1f, 1.5f, 0.8f), plVec3(0.1f, 0.0f, 0.1f), 45.0f);
  return PL_SUCCESS;
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunOpenSideOutflow()
{
  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;
  CollectStats();

  constexpr plUInt32 uiTicks = 240;
  if (m_uiLastTick < uiTicks && m_iFrame < static_cast<plInt32>(uiTicks) + 100)
    return plTestAppRun::Continue;

  const TickSample* pLast = m_Ticks.GetValue(m_uiLastTick);
  if (!PL_TEST_BOOL(pLast != nullptr && pLast->m_uiSeeded > 0))
    return plTestAppRun::Quit;
  plUInt32 uiMaxNaN = 0, uiMaxLost = 0;
  for (auto it : m_Ticks)
  {
    uiMaxNaN = plMath::Max(uiMaxNaN, it.Value().m_uiNaN);
    uiMaxLost = plMath::Max(uiMaxLost, it.Value().m_uiLost);
  }
  const float fDrained = static_cast<float>(pLast->m_uiDrained) / pLast->m_uiSeeded;
  plTestFramework::Output(plTestOutput::Message, "Open Side: %u ticks, seeded %u, drained through the open face %u (%.3f), alive %u; N5 mismatches %u, lost %u, NaN %u", m_uiLastTick,
    pLast->m_uiSeeded, pLast->m_uiDrained, fDrained, pLast->m_uiAlive, m_uiN5Mismatches, uiMaxLost, uiMaxNaN);
  PL_TEST_INT(m_uiN5Mismatches, 0);
  PL_TEST_INT(uiMaxLost, 0);
  PL_TEST_INT(uiMaxNaN, 0);
  PL_TEST_BOOL_MSG(fDrained > 0.5f, "Open Side: only %.3f of the water left through the open face", fDrained);
  return plTestAppRun::Quit;
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupSleepAndWake()
{
  // The stratified hydrostatic tank with sleep on: it settles, its bricks and then the volume go to sleep; an inflow
  // starting wakes it.
  PL_SUCCEED_OR_RETURN(SetupHydrostatic(false));
  m_sHydroName = "SleepAndWake";
  PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
  if (plGpuFluidLiquidVolumeComponent* pVolume = GetVolume())
    pVolume->m_bAllowSleep = TuningFloat("SLEEP", 1.0f) != 0.0f;
  m_iAsleepFrame = -1;
  m_iWakeFrame = -1;
  m_uiTicksAsleepAtWake = 0;
  m_uiSleepingBricksSeen = 0;
  m_uiAliveAtSleep = 0;
  m_bWokeUp = false;
  return PL_SUCCESS;
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunSleepAndWake()
{
  constexpr plInt32 iSleepDeadline = 300;
  constexpr plInt32 iAsleepFrames = 30; // how long it stays asleep before the inflow starts
  constexpr plInt32 iAfterWake = 120;
  const char* szSortPass = "GpuFluid Liquid/Sort";

  if (m_iAsleepFrame >= 0 && m_iWakeFrame < 0 && m_iFrame == m_iAsleepFrame + iAsleepFrames)
  {
    // The inflow: a small nozzle over one end of the tank.
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGpuFluidLiquidVolumeComponent* pVolume = GetVolume())
    {
      plGpuFluidLiquidStats stats;
      if (pVolume->GetStats(stats))
        m_uiTicksAsleepAtWake = stats.m_uiTicksAsleep;
      plGpuFluidLiquidInflow& inflow = pVolume->m_Inflows.ExpandAndGetRef();
      inflow.m_vCenter = plVec3(-0.4f, 0.0f, 0.2f);
      inflow.m_vVelocity = plVec3(0.0f, 0.0f, -0.5f);
      inflow.m_fRadius = 0.03f;
    }
    m_iWakeFrame = m_iFrame;
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;
  CollectStats();

  plGpuFluidLiquidStats stats;
  bool bStats = false;
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    if (plGpuFluidLiquidVolumeComponent* pVolume = GetVolume())
      bStats = pVolume->GetStats(stats);
  }
  if (bStats && m_iWakeFrame < 0)
  {
    m_uiSleepingBricksSeen = plMath::Max(m_uiSleepingBricksSeen, stats.m_uiSleepingBricks);
    if (m_iAsleepFrame < 0 && stats.m_bVolumeAsleep)
    {
      m_iAsleepFrame = m_iFrame;
      m_uiAliveAtSleep = stats.m_uiAlive;
    }
    // Asleep, the solver records nothing: its pass timings stop (removed here, they must not come back).
    if (m_iAsleepFrame >= 0 && m_iFrame == m_iAsleepFrame + 5)
      RemovePassStats(szSortPass);
    if (m_iAsleepFrame >= 0 && m_iFrame == m_iAsleepFrame + iAsleepFrames - 1)
    {
      double fSortMs = 0.0;
      PL_TEST_BOOL_MSG(!GetPassMs(szSortPass, fSortMs), "Sleep: the solver's Sort pass ran while the volume was asleep");
      PL_TEST_BOOL_MSG(stats.m_bVolumeAsleep, "Sleep: the volume woke up by itself at frame %d", m_iFrame);
    }
  }
  if (bStats && m_iWakeFrame >= 0 && !m_bWokeUp && !stats.m_bVolumeAsleep)
    m_bWokeUp = true;

  if (m_iAsleepFrame < 0 && m_iFrame < iSleepDeadline)
    return plTestAppRun::Continue;
  if (m_iWakeFrame < 0 && m_iAsleepFrame >= 0)
    return plTestAppRun::Continue;
  if (m_iWakeFrame >= 0 && m_iFrame < m_iWakeFrame + iAfterWake)
    return plTestAppRun::Continue;

  const TickSample* pLast = m_Ticks.GetValue(m_uiLastTick);
  plUInt32 uiAwakeAfter = 0;
  for (auto it : m_Ticks)
  {
    if (m_iWakeFrame >= 0 && it.Key() > static_cast<plUInt32>(m_iWakeFrame) - 30)
      uiAwakeAfter = plMath::Max(uiAwakeAfter, it.Value().m_uiAwakeBricks);
  }
  plTestFramework::Output(plTestOutput::Message,
    "Sleep: the volume fell asleep at frame %d (%u particles, up to %u bricks asleep before), %llu ticks skipped asleep by the inflow's start at frame %d; woke %s; after: %u ticks read back, seeded %u, alive %u, up to %u bricks awake; N5 mismatches %u",
    m_iAsleepFrame, m_uiAliveAtSleep, m_uiSleepingBricksSeen, m_uiTicksAsleepAtWake, m_iWakeFrame, m_bWokeUp ? "yes" : "no", m_uiLastTick, pLast ? pLast->m_uiSeeded : 0, pLast ? pLast->m_uiAlive : 0,
    uiAwakeAfter, m_uiN5Mismatches);
  PL_TEST_BOOL_MSG(m_iAsleepFrame >= 0, "Sleep: the settled tank did not fall asleep in %d frames", iSleepDeadline);
  PL_TEST_BOOL_MSG(m_uiSleepingBricksSeen > 0, "Sleep: no brick was asleep before the volume slept");
  PL_TEST_BOOL_MSG(m_uiTicksAsleepAtWake >= static_cast<plUInt64>(iAsleepFrames) - 5, "Sleep: only %llu ticks skipped while asleep", m_uiTicksAsleepAtWake);
  PL_TEST_BOOL_MSG(m_bWokeUp, "Sleep: the inflow did not wake the volume");
  PL_TEST_BOOL_MSG(pLast != nullptr && pLast->m_uiSeeded > m_uiAliveAtSleep && uiAwakeAfter > 0, "Sleep: after the wake nothing was seeded or no brick was awake");
  PL_TEST_INT(m_uiN5Mismatches, 0);
  return plTestAppRun::Quit;
}

plResult plGameEngineTestGpuFluidLiquidGallery::SetupFloodingRoom(bool bVideo)
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());
  pWorld->Clear();
  pWorld->GetRandomNumberGenerator().Initialize(0x4746503Cu);
  pWorld->GetClock().Reset(false);
  SetCVarBool(s_szDepthOfFieldCVar, false);

  const plVec3 vRoom = s_vRoomFloor;
  const plVec3 vCenter(0.0f, 0.0f, 0.5f * vRoom.z);
  m_vDomainMin = DomainMinOf(vCenter, vRoom);
  m_fDomainCellSize = s_fFloodCellSize;
  const float fX0 = m_vDomainMin.x, fX1 = -m_vDomainMin.x, fY0 = m_vDomainMin.y, fY1 = -m_vDomainMin.y;

  // Sun high over the low front and left walls, from the camera's side, so it lights the floor and the water (from over
  // the back wall it left the rooms in shadow and the water all but invisible). Floor tiles of 32 cm for the
  // refraction to show.
  const plVec3 vToSun = plVec3(TuningFloat("FLOOD_SUN_X", -0.45f), TuningFloat("FLOOD_SUN_Y", 0.55f), TuningFloat("FLOOD_SUN_Z", 1.1f)).GetNormalized();
  m_pOwnApplication->AddSunAndSky(vToSun, TuningFloat("FLOOD_SUN", 3.0f));
  m_pOwnApplication->AddBox("Ground", plVec3(0.0f, 0.0f, -0.1f), plVec3(60.0f, 60.0f, 0.2f), plColor(0.20f, 0.20f, 0.19f));
  m_pOwnApplication->AddBox("Floor", plVec3(0.0f, 0.0f, 0.0005f), plVec3(vRoom.x, vRoom.y, 0.001f), plColor(0.28f, 0.22f, 0.16f));
  {
    const plColor tile(0.62f, 0.58f, 0.50f);
    constexpr float fTile = 0.32f;
    for (plInt32 y = 0; y < 10; ++y)
      for (plInt32 x = 0; x < 13; ++x)
        if (((x + y) & 1) == 0)
          m_pOwnApplication->AddBox("Tile", plVec3(fX0 + (x + 0.5f) * fTile, fY0 + (y + 0.5f) * fTile, 0.0015f), plVec3(fTile, fTile, 0.002f), tile);
  }

  // The rooms' outer walls stand just outside the domain (its closed faces are the walls the water sees): the back and
  // the right wall full height, the left and the front wall low, so the camera looks in from the front left.
  const plColor plaster(0.78f, 0.74f, 0.68f);
  const plColor trim(0.36f, 0.30f, 0.26f);
  constexpr float fWallHeight = 2.6f, fLow = 0.5f, fT = 0.16f;
  {
    // Back wall (y0) with a window, x in [-1.7, -0.9], z in [1.0, 2.0].
    const float fY = fY0 - 0.5f * fT;
    const float fWinX0 = -1.7f, fWinX1 = -0.9f, fWinZ0 = 1.0f, fWinZ1 = 2.0f;
    const float fL = fX0 - fT, fR = fX1 + fT;
    m_pOwnApplication->AddBox("BackWall", plVec3(0.5f * (fL + fWinX0), fY, 0.5f * fWallHeight), plVec3(fWinX0 - fL, fT, fWallHeight), plaster);
    m_pOwnApplication->AddBox("BackWall", plVec3(0.5f * (fWinX1 + fR), fY, 0.5f * fWallHeight), plVec3(fR - fWinX1, fT, fWallHeight), plaster);
    m_pOwnApplication->AddBox("BackWall", plVec3(0.5f * (fWinX0 + fWinX1), fY, 0.5f * fWinZ0), plVec3(fWinX1 - fWinX0, fT, fWinZ0), plaster);
    m_pOwnApplication->AddBox("BackWall", plVec3(0.5f * (fWinX0 + fWinX1), fY, 0.5f * (fWinZ1 + fWallHeight)), plVec3(fWinX1 - fWinX0, fT, fWallHeight - fWinZ1), plaster);
    m_pOwnApplication->AddBox("WindowSill", plVec3(0.5f * (fWinX0 + fWinX1), fY + 0.03f, fWinZ0 - 0.02f), plVec3(fWinX1 - fWinX0 + 0.1f, fT + 0.08f, 0.04f), trim);
    m_pOwnApplication->AddBox("WindowBar", plVec3(0.5f * (fWinX0 + fWinX1), fY, 0.5f * (fWinZ0 + fWinZ1)), plVec3(0.04f, 0.05f, fWinZ1 - fWinZ0), trim);
    m_pOwnApplication->AddBox("WindowBar", plVec3(0.5f * (fWinX0 + fWinX1), fY, 0.5f * (fWinZ0 + fWinZ1)), plVec3(fWinX1 - fWinX0, 0.05f, 0.04f), trim);
    m_pOwnApplication->AddBox("RightWall", plVec3(fX1 + 0.5f * fT, 0.0f, 0.5f * fWallHeight), plVec3(fT, vRoom.y + 2.0f * fT, fWallHeight), plaster);
    m_pOwnApplication->AddBox("LeftWall", plVec3(fX0 - 0.5f * fT, 0.0f, 0.5f * fLow), plVec3(fT, vRoom.y + 2.0f * fT, fLow), plaster);
    m_pOwnApplication->AddBox("FrontWall", plVec3(0.0f, fY1 + 0.5f * fT, 0.5f * fLow), plVec3(vRoom.x + 2.0f * fT, fT, fLow), plaster);
    m_pOwnApplication->AddBox("Skirting", plVec3(0.0f, fY0 + 0.01f, 0.05f), plVec3(vRoom.x, 0.02f, 0.1f), trim);
  }

  // Baked solids. The partition between the rooms (x 0.48 .. 0.64) with a 0.9 m doorway, full height.
  const plQuat qNone = plQuat::MakeIdentity();
  auto Turn = [](float fDegrees) { return plQuat::MakeFromAxisAndAngle(s_vUp, plAngle::MakeFromDegree(fDegrees)); };
  const float fDoorY0 = -0.75f, fDoorY1 = 0.15f;
  AddBakedBox({plVec3(0.56f, 0.5f * (fY0 - 0.2f + fDoorY0), 1.3f), plVec3(0.16f, fDoorY0 - (fY0 - 0.2f), 2.6f), qNone}, plaster);
  AddBakedBox({plVec3(0.56f, 0.5f * (fDoorY1 + fY1 + 0.2f), 1.3f), plVec3(0.16f, fY1 + 0.2f - fDoorY1, 2.6f), qNone}, plaster);
  m_pOwnApplication->AddBox("Lintel", plVec3(0.56f, 0.5f * (fDoorY0 + fDoorY1), 2.3f), plVec3(0.16f, fDoorY1 - fDoorY0, 0.6f), plaster);
  m_pOwnApplication->AddBox("DoorFrame", plVec3(0.56f, fDoorY0 - 0.02f, 1.0f), plVec3(0.2f, 0.04f, 2.0f), trim);
  m_pOwnApplication->AddBox("DoorFrame", plVec3(0.56f, fDoorY1 + 0.02f, 1.0f), plVec3(0.2f, 0.04f, 2.0f), trim);

  // Room A: a table with four legs, two chairs, crates by the back wall, a low shelf on the left wall.
  const plColor wood(0.46f, 0.30f, 0.18f), darkWood(0.30f, 0.19f, 0.12f), crate(0.58f, 0.45f, 0.28f), paint(0.30f, 0.38f, 0.44f);
  {
    const plVec3 vTable(-0.9f, 0.35f, 0.0f);
    AddBakedBox({vTable + plVec3(0.0f, 0.0f, 0.73f), plVec3(1.2f, 0.7f, 0.05f), qNone}, wood);
    for (float fSx : {-0.54f, 0.54f})
      for (float fSy : {-0.29f, 0.29f})
        AddBakedBox({vTable + plVec3(fSx, fSy, 0.355f), plVec3(0.07f, 0.07f, 0.71f), qNone}, darkWood);
  }
  auto AddChair = [&](const plVec3& vSeat, float fDegrees)
  {
    const plQuat q = Turn(fDegrees);
    AddBakedBox({vSeat + plVec3(0.0f, 0.0f, 0.45f), plVec3(0.44f, 0.44f, 0.05f), q}, wood);
    for (float fSx : {-0.19f, 0.19f})
      for (float fSy : {-0.19f, 0.19f})
        AddBakedBox({vSeat + q * plVec3(fSx, fSy, 0.225f), plVec3(0.05f, 0.05f, 0.45f), q}, darkWood);
    AddBakedBox({vSeat + q * plVec3(0.0f, -0.2f, 0.73f), plVec3(0.44f, 0.05f, 0.52f), q}, wood);
  };
  AddChair(plVec3(-0.9f, -0.25f, 0.0f), 8.0f);
  AddChair(plVec3(-1.05f, 0.98f, 0.0f), 172.0f);
  AddBakedBox({plVec3(-1.65f, -1.15f, 0.25f), plVec3(0.5f, 0.5f, 0.5f), Turn(15.0f)}, crate);
  AddBakedBox({plVec3(-1.62f, -1.12f, 0.65f), plVec3(0.32f, 0.32f, 0.3f), Turn(38.0f)}, crate);
  AddBakedBox({plVec3(0.12f, -1.25f, 0.2f), plVec3(0.4f, 0.4f, 0.4f), Turn(-20.0f)}, crate);
  AddBakedBox({plVec3(fX0 + 0.16f, 0.55f, 0.32f), plVec3(0.32f, 1.0f, 0.64f), qNone}, paint);
  // Room B: a cabinet, a crate, a low bench.
  AddBakedBox({plVec3(fX1 - 0.22f, 1.15f, 0.42f), plVec3(0.44f, 0.7f, 0.84f), qNone}, paint);
  AddBakedBox({plVec3(1.25f, -1.1f, 0.2f), plVec3(0.4f, 0.4f, 0.4f), Turn(30.0f)}, crate);
  AddBakedBox({plVec3(1.35f, 0.95f, 0.22f), plVec3(0.8f, 0.4f, 0.44f), Turn(-6.0f)}, wood);

  // Some props on the furniture, drawn only.
  m_pOwnApplication->AddBox("Book", plVec3(-0.7f, 0.4f, 0.78f), plVec3(0.24f, 0.17f, 0.05f), plColor(0.55f, 0.12f, 0.10f));
  m_pOwnApplication->AddBox("Box", plVec3(fX0 + 0.16f, 0.35f, 0.74f), plVec3(0.22f, 0.3f, 0.2f), plColor(0.65f, 0.62f, 0.55f));

  // The burst pipe: out of the back wall high up, a flange at its end, pouring forwards.
  const plColor metal(0.25f, 0.26f, 0.28f);
  m_pOwnApplication->AddBox("Pipe", plVec3(s_vFloodPipe.x, 0.5f * (fY0 + s_vFloodPipe.y), s_vFloodPipe.z), plVec3(2.0f * s_fFloodPipeRadius + 0.04f, s_vFloodPipe.y - fY0 + 0.05f, 2.0f * s_fFloodPipeRadius + 0.04f), metal);
  m_pOwnApplication->AddBox("PipeFlange", plVec3(s_vFloodPipe.x, s_vFloodPipe.y - 0.03f, s_vFloodPipe.z), plVec3(2.0f * s_fFloodPipeRadius + 0.12f, 0.05f, 2.0f * s_fFloodPipeRadius + 0.12f), metal);
  m_pOwnApplication->AddBox("PipeRiser", plVec3(s_vFloodPipe.x, fY0 + 0.1f, 0.5f * (s_vFloodPipe.z + fWallHeight)), plVec3(0.2f, 0.16f, fWallHeight - s_vFloodPipe.z), metal);

  // The floor drain: a dark square under a grate.
  m_pOwnApplication->AddBox("Drain", s_vFloodDrain + plVec3(0.0f, 0.0f, 0.0025f), plVec3(2.0f * s_fFloodDrainHalf, 2.0f * s_fFloodDrainHalf, 0.003f), plColor(0.03f, 0.03f, 0.03f));
  for (plInt32 i = -3; i <= 3; ++i)
    m_pOwnApplication->AddBox("Grate", s_vFloodDrain + plVec3(i * 0.055f, 0.0f, 0.004f), plVec3(0.015f, 2.0f * s_fFloodDrainHalf, 0.004f), plColor(0.45f, 0.45f, 0.47f));

  m_uiExpectedSolidCells = CountExpectedSolidCells(m_BakedBoxes, m_vDomainMin, plVec3U32(104, 80, 48), s_fFloodCellSize);

  plGameObjectDesc desc;
  desc.m_sName.Assign("FloodingRoom");
  desc.m_LocalPosition = vCenter;
  plGameObject* pObject = nullptr;
  m_hVolumeObject = pWorld->CreateObject(desc, pObject);

  plGpuFluidLiquidVolumeComponent* pLiquid = nullptr;
  pWorld->GetOrCreateComponentManager<plGpuFluidLiquidVolumeComponentManager>()->CreateComponent(pObject, pLiquid);
  pLiquid->m_vExtents = vRoom;
  pLiquid->m_fCellSize = s_fFloodCellSize;
  pLiquid->m_uiParticlesPerCell = 4;
  pLiquid->m_uiMaxParticles = 384 * 1024;
  pLiquid->m_Quality = plGpuFluidQuality::Medium;
  // The falling jet reaches 6 m/s: 4 substeps at 4 cm keep it in real time (the tier's 3 would run it in slow motion,
  // and the flood level is checked against the simulated time).
  pLiquid->m_uiMaxSubsteps = 5;
  pLiquid->m_fDebugTopSpeed = 4.0f;
  // Flood water: murky enough that 20 cm of it reads as water over the tiles.
  pLiquid->m_vAbsorption = plVec3(TuningFloat("ABS_R", 2.5f), TuningFloat("ABS_G", 1.0f), TuningFloat("ABS_B", 0.7f));
  pLiquid->m_ScatterColor = plColor(0.035f, 0.075f, 0.075f);
  ApplyTuningOverrides(pLiquid, true);
  m_vDomainCells = pLiquid->GetCells();
  PL_TEST_BOOL(m_vDomainCells == plVec3U32(104, 80, 48));

  plGpuFluidLiquidDrain& drain = pLiquid->m_Drains.ExpandAndGetRef();
  drain.m_vCenter = s_vFloodDrain - vCenter + plVec3(0.0f, 0.0f, 0.02f);
  drain.m_vHalfExtents = plVec3(s_fFloodDrainHalf, s_fFloodDrainHalf, 0.02f);
  drain.m_bOpen = false;

  // Probes: the water depth over open floor in both rooms.
  m_uiFloodProbeCount = 0;
  for (const plVec2& vProbe : s_FloodProbes)
  {
    const plVec2 vCell = (vProbe - plVec2(m_vDomainMin.x, m_vDomainMin.y)) / s_fFloodCellSize;
    pLiquid->m_Tuning.m_Probes.PushBack(plVec2U32(static_cast<plUInt32>(vCell.x), static_cast<plUInt32>(vCell.y)));
    ++m_uiFloodProbeCount;
  }

  m_fFloodFlow = s_fFloodFlow;
  m_bFloodStarted = m_bFloodStopped = m_bDrainOpened = false;
  m_fFloodStartSim = m_fFloodStopSim = -1.0f;
  m_uiFloodStartTick = m_uiFloodStopTick = m_uiDrainTick = m_uiFloodEndTick = 0;
  m_uiPeakAlive = 0;

  PL_IGNORE_UNUSED(bVideo);
  m_pOwnApplication->SetCamera(plVec3(-3.6f, 3.9f, 3.3f), plVec3(0.05f, -0.35f, 0.25f), 46.0f);
  return PL_SUCCESS;
}

plTestAppRun plGameEngineTestGpuFluidLiquidGallery::RunFloodingRoom(bool bVideo)
{
  if (bVideo && plEnvironmentVariableUtils::GetValueInt("PL_GPUFLUID_VIDEO", 0) != 1)
  {
    plTestFramework::Output(plTestOutput::Details, "Skipped: set PL_GPUFLUID_VIDEO=1 to record the frames.");
    return plTestAppRun::Quit;
  }

  const plUInt32 uiFrame = static_cast<plUInt32>(m_iFrame);
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGpuFluidLiquidVolumeComponent* pVolume = GetVolume())
    {
      if (uiFrame == s_uiFloodStartFrame)
      {
        // The pipe bursts: 200 l/s through a 28 cm disc, forwards and a little down.
        const plVec3 vDir = plVec3(0.0f, 1.0f, -0.3f).GetNormalized();
        const float fSpeed = m_fFloodFlow / (plMath::Pi<float>() * s_fFloodPipeRadius * s_fFloodPipeRadius);
        plGpuFluidLiquidInflow& inflow = pVolume->m_Inflows.ExpandAndGetRef();
        inflow.m_vCenter = s_vFloodPipe - (m_vDomainMin + 0.5f * s_vRoomFloor);
        inflow.m_vVelocity = vDir * fSpeed;
        inflow.m_fRadius = s_fFloodPipeRadius;
        inflow.m_fFlowRate = m_fFloodFlow * 1000.0f;
        m_bFloodStarted = true;
      }
      if (uiFrame == s_uiFloodStopFrame)
      {
        pVolume->m_Inflows.Clear();
        m_bFloodStopped = true;
      }
      if (uiFrame == s_uiFloodDrainFrame && !pVolume->m_Drains.IsEmpty())
      {
        pVolume->m_Drains[0].m_bOpen = true;
        m_bDrainOpened = true;
      }
    }
  }

  if (bVideo)
  {
    // A slow move along the front, the view lowering a little as the water rises.
    const float f = plMath::Clamp(static_cast<float>(uiFrame) / s_uiFloodVideoFrames, 0.0f, 1.0f);
    const float s = f * f * (3.0f - 2.0f * f);
    const plVec3 vEye = plMath::Lerp(plVec3(-3.7f, 3.6f, 3.4f), plVec3(-2.4f, 4.3f, 2.7f), s);
    const plVec3 vTarget = plMath::Lerp(plVec3(-0.1f, -0.4f, 0.35f), plVec3(0.15f, -0.3f, 0.2f), s);
    m_pOwnApplication->SetCamera(vEye, vTarget, 46.0f);
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;
  if (CollectStats())
    m_uiPeakAlive = plMath::Max(m_uiPeakAlive, m_LastStats.m_uiAlive);

  if (bVideo)
  {
    SaveVideoFrame("FloodingRoomVideo", "P8 Flooding Room: a burst pipe floods two rooms through a doorway, around furniture baked from meshes; a floor drain opens");
    if (uiFrame < s_uiFloodVideoFrames)
      return plTestAppRun::Continue;
    EvaluateFloodingRoom();
    return plTestAppRun::Quit;
  }

  if (uiFrame >= 240 && uiFrame < 420)
    AccumulateTimings();

  for (plUInt32 uiShot : {120u, 240u, 370u, 600u, 980u})
  {
    if (uiFrame == uiShot)
    {
      plStringBuilder sName;
      sName.SetFormat("FloodingRoom_{}", plArgI(uiShot, 4, true));
      PL_TEST_BOOL(SaveShot(sName).Succeeded());
    }
  }

  if (uiFrame < s_uiFloodTestFrames)
    return plTestAppRun::Continue;
  EvaluateFloodingRoom();
  return plTestAppRun::Quit;
}

void plGameEngineTestGpuFluidLiquidGallery::EvaluateFloodingRoom()
{
  // The pour in ticks and simulated time: from the first tick that seeded to the last.
  plUInt32 uiFirstSeedTick = 0, uiLastSeedTick = 0, uiPrevSeeded = 0, uiMaxNaN = 0, uiMaxInSolid = 0, uiMaxLost = 0, uiSolidCells = 0, uiMaxSubsteps = 0;
  plStringBuilder sCsv("tick,sim_time,alive,seeded,drained,substeps,active_bricks,awake,sleeping,volume_ratio,max_speed,residual_ratio,probe0,probe1,probe2,probe3,phi0,phi1,phi2,phi3,max_cell_particles,max_cell_x,max_cell_y,max_cell_z,max_cell_type,max_cell_count,max_cell_phi,max_cell_sdf\n");
  for (auto it : m_Ticks)
  {
    const TickSample& s = it.Value();
    if (s.m_uiSeeded > uiPrevSeeded)
    {
      if (uiFirstSeedTick == 0)
        uiFirstSeedTick = it.Key();
      uiLastSeedTick = it.Key();
    }
    uiPrevSeeded = s.m_uiSeeded;
    uiMaxNaN = plMath::Max(uiMaxNaN, s.m_uiNaN);
    uiMaxInSolid = plMath::Max(uiMaxInSolid, s.m_uiInSolid);
    uiMaxLost = plMath::Max(uiMaxLost, s.m_uiLost);
    uiSolidCells = plMath::Max(uiSolidCells, s.m_uiSolidCells);
    uiMaxSubsteps = plMath::Max(uiMaxSubsteps, s.m_uiSubsteps);
    sCsv.AppendFormat("{},{},{},{},{},{},{},{},{},{},{},{},", it.Key(), s.m_fSimTime, s.m_uiAlive, s.m_uiSeeded, s.m_uiDrained, s.m_uiSubsteps, s.m_uiActiveBricks, s.m_uiAwakeBricks, s.m_uiSleepingBricks,
      s.m_fVolumeRatio, s.m_fMaxSpeed, s.m_fResidualRatio);
    sCsv.AppendFormat("{},{},{},{},{},{},{},{},", s.m_fProbes[0], s.m_fProbes[1], s.m_fProbes[2], s.m_fProbes[3], s.m_fProbePhi[0], s.m_fProbePhi[1], s.m_fProbePhi[2], s.m_fProbePhi[3]);
    sCsv.AppendFormat("{},{},{},{},{},{},{},{}\n", s.m_uiMaxCellParticles, s.m_vMaxCell.x, s.m_vMaxCell.y, s.m_vMaxCell.z, s.m_uiMaxCellType & 0xFFu, s.m_uiMaxCellType >> 8,
      s.m_fMaxCellPhi, s.m_fMaxCellSdf);
  }
  WriteText(":imgout/GpuFluidGallery/Liquid/FloodingRoom_ticks.csv", sCsv);

  const TickSample* pFirst = m_Ticks.GetValue(uiFirstSeedTick > 0 ? uiFirstSeedTick - 1 : 0);
  const float fStartSim = pFirst != nullptr ? pFirst->m_fSimTime : 0.0f;
  const float fParticleVolume = m_fDomainCellSize * m_fDomainCellSize * m_fDomainCellSize / 4.0f;

  // N5 flood level at 6, 9 and 12 s of pouring: the probes' mean depth over 15 ticks against the level Q t fills.
  plStringBuilder sLevels("pour_seconds,expected_q_t_m,expected_seeded_m,probes_mean_m,phi_mean_m,ratio\n");
  float fWorstLevel = 0.0f;
  for (float fPour : {6.0f, 9.0f, 12.0f})
  {
    double fSum = 0.0, fPhiSum = 0.0, fTime = 0.0, fSeeded = 0.0;
    plUInt32 uiCount = 0;
    for (auto it : m_Ticks)
    {
      const float fT = it.Value().m_fSimTime - fStartSim;
      if (uiFirstSeedTick == 0 || it.Key() < uiFirstSeedTick || plMath::Abs(fT - fPour) > 0.25f)
        continue;
      for (plUInt32 p = 0; p < m_uiFloodProbeCount; ++p)
      {
        fSum += it.Value().m_fProbes[p];
        fPhiSum += it.Value().m_fProbePhi[p];
      }
      fTime += fT;
      fSeeded += it.Value().m_uiSeeded;
      ++uiCount;
    }
    if (uiCount == 0)
    {
      PL_TEST_FAILURE("Flooding Room", "no tick at %.0f s of pouring", fPour);
      continue;
    }
    const float fMeasured = static_cast<float>(fSum / (uiCount * m_uiFloodProbeCount));
    const float fMeasuredPhi = static_cast<float>(fPhiSum / (uiCount * m_uiFloodProbeCount));
    const float fExpected = ExpectedFloodLevel(m_fFloodFlow * static_cast<float>(fTime / uiCount));
    const float fExpectedSeeded = ExpectedFloodLevel(static_cast<float>(fSeeded / uiCount) * fParticleVolume);
    const float fRatio = fExpected > 0.0f ? fMeasured / fExpected : 0.0f;
    fWorstLevel = plMath::Max(fWorstLevel, plMath::Abs(fRatio - 1.0f));
    sLevels.AppendFormat("{},{},{},{},{},{}\n", fPour, fExpected, fExpectedSeeded, fMeasured, fMeasuredPhi, fRatio);
    plTestFramework::Output(plTestOutput::Message,
      "Flood N5 at %.0f s of pouring: depth at the probes %.4f m (level set %.4f m) against Q t over the free floor %.4f m (from the seeded count %.4f m): %.4f (limit +-%.0f%%)", fPour,
      fMeasured, fMeasuredPhi, fExpected, fExpectedSeeded, fRatio, s_fFloodLevelTolerance * 100.0f);
  }
  WriteText(":imgout/GpuFluidGallery/Liquid/FloodingRoom_levels.csv", sLevels);

  // The pour's own volume: seeded against Q t.
  const TickSample* pPourEnd = m_Ticks.GetValue(uiLastSeedTick);
  const float fPourSeconds = pPourEnd != nullptr ? pPourEnd->m_fSimTime - fStartSim : 0.0f;
  const float fSeededRatio = pPourEnd != nullptr && fPourSeconds > 0.0f ? pPourEnd->m_uiSeeded * fParticleVolume / (m_fFloodFlow * fPourSeconds) : 0.0f;

  // The drain: what is left at the end of what there was.
  const TickSample* pLast = m_Ticks.GetValue(m_uiLastTick);
  const float fLeft = pLast != nullptr && m_uiPeakAlive > 0 ? static_cast<float>(pLast->m_uiAlive) / m_uiPeakAlive : 1.0f;
  const float fSolidRatio = m_uiExpectedSolidCells > 0 ? static_cast<float>(uiSolidCells) / m_uiExpectedSolidCells : 0.0f;
  plTestFramework::Output(plTestOutput::Message,
    "Flooding Room: %u ticks, pour from tick %u to %u (%.3f s simulated, seeded %.4f of Q t), peak %u particles, %u left at tick %u (%.4f; drained %u), solid cells %u against %u from the boxes (%.4f), substeps up to %u; N5 mismatches %u, lost %u, NaN %u, N9 %u",
    m_uiLastTick, uiFirstSeedTick, uiLastSeedTick, fPourSeconds, fSeededRatio, m_uiPeakAlive, pLast ? pLast->m_uiAlive : 0, m_uiLastTick, fLeft, pLast ? pLast->m_uiDrained : 0, uiSolidCells,
    m_uiExpectedSolidCells, fSolidRatio, uiMaxSubsteps, m_uiN5Mismatches, uiMaxLost, uiMaxNaN, uiMaxInSolid);
  PL_TEST_INT(m_uiN5Mismatches, 0);
  PL_TEST_INT(uiMaxLost, 0);
  PL_TEST_INT(uiMaxNaN, 0);
  PL_TEST_INT(uiMaxInSolid, 0);
  PL_TEST_BOOL_MSG(plMath::Abs(fSeededRatio - 1.0f) <= 0.02f, "Flooding Room: the pipe seeded %.4f of its flow", fSeededRatio);
  PL_TEST_BOOL_MSG(plMath::Abs(fSolidRatio - 1.0f) <= s_fSolidCellTolerance, "Flooding Room: %u solid cells, the boxes give %u", uiSolidCells, m_uiExpectedSolidCells);

  // The drain opens at the first tick that drained anything.
  float fDrainOpen = -1.0f;
  for (auto it : m_Ticks)
  {
    if (it.Value().m_uiDrained > 0)
    {
      fDrainOpen = it.Value().m_fSimTime;
      break;
    }
  }
  if (PL_TEST_BOOL_MSG(fDrainOpen > 0.0f, "Flooding Room: the drain never drained"))
  {
    // N5 at rest: the probes over the 4 s before the drain opened against the seeded volume.
    double fSum = 0.0, fPhiSum = 0.0;
    plUInt32 uiCount = 0, uiSeeded = 0;
    // Torricelli over 1 to 3 s after the drain opened.
    double fDepthSum = 0.0;
    plUInt32 uiDepthCount = 0;
    const TickSample* pDrainA = nullptr;
    const TickSample* pDrainB = nullptr;
    for (auto it : m_Ticks)
    {
      const TickSample& s = it.Value();
      const float fT = s.m_fSimTime - fDrainOpen;
      double fMean = 0.0, fMeanPhi = 0.0;
      for (plUInt32 p = 0; p < m_uiFloodProbeCount; ++p)
      {
        fMean += s.m_fProbes[p] / m_uiFloodProbeCount;
        fMeanPhi += s.m_fProbePhi[p] / m_uiFloodProbeCount;
      }
      if (fT >= -4.0f && fT < 0.0f)
      {
        fSum += fMean;
        fPhiSum += fMeanPhi;
        uiSeeded = s.m_uiSeeded;
        ++uiCount;
      }
      if (fT >= 1.0f && fT <= 3.0f)
      {
        if (pDrainA == nullptr)
          pDrainA = &s;
        pDrainB = &s;
        fDepthSum += fMeanPhi;
        ++uiDepthCount;
      }
    }
    if (PL_TEST_BOOL_MSG(uiCount > 0, "Flooding Room: no tick in the 4 s before the drain opened"))
    {
      const float fMeasured = static_cast<float>(fSum / uiCount);
      const float fMeasuredPhi = static_cast<float>(fPhiSum / uiCount);
      const float fExpected = ExpectedFloodLevel(uiSeeded * fParticleVolume);
      const float fRatio = fExpected > 0.0f ? fMeasured / fExpected : 0.0f;
      sLevels.AppendFormat("settled,{},{},{},{},{}\n", fExpected, fExpected, fMeasured, fMeasuredPhi, fRatio);
      WriteText(":imgout/GpuFluidGallery/Liquid/FloodingRoom_levels.csv", sLevels);
      plTestFramework::Output(plTestOutput::Message, "Flood N5 settled (mean over the 4 s before the drain opens): depth at the probes %.4f m (level set %.4f m) against the poured volume over the free floor %.4f m: %.4f (limit +-%.0f%%)",
        fMeasured, fMeasuredPhi, fExpected, fRatio, s_fFloodLevelTolerance * 100.0f);
      PL_TEST_BOOL_MSG(plMath::Abs(fRatio - 1.0f) <= s_fFloodLevelTolerance, "Flood N5: the settled water stands %.4f of the level its volume fills", fRatio);
    }
    if (PL_TEST_BOOL_MSG(pDrainA != nullptr && pDrainB != pDrainA && uiDepthCount > 0, "Flooding Room: no ticks 1 to 3 s after the drain opened"))
    {
      const float fSeconds = pDrainB->m_fSimTime - pDrainA->m_fSimTime;
      const float fFlow = (pDrainB->m_uiDrained - pDrainA->m_uiDrained) * fParticleVolume / plMath::Max(fSeconds, 1e-3f);
      const float fDepth = static_cast<float>(fDepthSum / uiDepthCount);
      const float fArea = 4.0f * s_fFloodDrainHalf * s_fFloodDrainHalf;
      const float fCd = fFlow / (fArea * plMath::Sqrt(2.0f * 9.81f * plMath::Max(fDepth, 1e-3f)));
      plTestFramework::Output(plTestOutput::Message, "Flood drain: %.3f m^3/s over 1 to 3 s after opening at a mean depth of %.3f m through %.2f m^2: discharge coefficient %.3f (limits %.2f .. %.2f); %.4f of the water left after 20 s",
        fFlow, fDepth, fArea, fCd, s_fFloodDrainCdMin, s_fFloodDrainCdMax, fLeft);
      PL_TEST_BOOL_MSG(fCd >= s_fFloodDrainCdMin && fCd <= s_fFloodDrainCdMax, "Flood drain: discharge coefficient %.3f", fCd);
    }
  }

  plStringBuilder sTimes;
  ReportTimings(sTimes);
  WriteText(":imgout/GpuFluidGallery/Liquid/FloodingRoom_timings.csv", sTimes);
}
