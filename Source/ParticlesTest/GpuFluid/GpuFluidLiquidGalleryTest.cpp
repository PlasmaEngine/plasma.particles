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
  constexpr float s_fHydroDepth = 16.5f * s_fHydroCellSize;
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

  const char* s_szPhases[] = {"GpuFluid Liquid/Begin", "GpuFluid Liquid/Seed", "GpuFluid Liquid/Source", "GpuFluid Liquid/Sort", "GpuFluid Liquid/P2G", "GpuFluid Liquid/Solve",
    "GpuFluid Liquid/G2P", "GpuFluid Liquid/Render", "GpuFluid Liquid/Surface Sort", "GpuFluid Liquid/Surface", "GpuFluid Liquid/Readback", "GpuFluid Liquid Copy Scene Color",
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

  void ApplyTuningOverrides(plGpuFluidLiquidVolumeComponent* pLiquid)
  {
    pLiquid->m_uiPressureCycles = static_cast<plUInt32>(TuningFloat("CYCLES", static_cast<float>(pLiquid->m_uiPressureCycles)));
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
    pLiquid->m_Tuning.m_bDensityTwoSided = TuningFloat("TWO_SIDED", pLiquid->m_Tuning.m_bDensityTwoSided ? 1.0f : 0.0f) != 0.0f;
    pLiquid->m_Tuning.m_fDensityMaxError = TuningFloat("DENSITY_MAX_ERROR", pLiquid->m_Tuning.m_fDensityMaxError);
    pLiquid->m_Tuning.m_fParticleRadius = TuningFloat("PARTICLE_RADIUS", pLiquid->m_Tuning.m_fParticleRadius);
    pLiquid->m_Tuning.m_fLevelSetRadius = TuningFloat("LEVEL_SET_RADIUS", pLiquid->m_Tuning.m_fLevelSetRadius);
    pLiquid->m_Tuning.m_fSurfaceBlend = TuningFloat("SURFACE_BLEND", pLiquid->m_Tuning.m_fSurfaceBlend);
    pLiquid->m_Tuning.m_uiSurfaceSmoothSteps = static_cast<plUInt32>(TuningFloat("SURFACE_SMOOTH", static_cast<float>(pLiquid->m_Tuning.m_uiSurfaceSmoothSteps)));
    pLiquid->m_Tuning.m_uiSubstepHoldTicks = static_cast<plUInt32>(TuningFloat("HOLD", static_cast<float>(pLiquid->m_Tuning.m_uiSubstepHoldTicks)));
    pLiquid->m_Tuning.m_fSubstepDownMargin = TuningFloat("DOWN_MARGIN", pLiquid->m_Tuning.m_fSubstepDownMargin);

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

  // Per-phase GPU times are exported as stats while this is on.
  SetCVarBool(s_szGpuStatsCVar, true);

  switch (iIdentifier)
  {
    case SubTests::DamBreak:
    case SubTests::DamBreakVideo:
      return SetupDamBreak();

    case SubTests::HydrostaticTank:
      return SetupHydrostatic(false);

    case SubTests::HydrostaticTankRandom:
      return SetupHydrostatic(true);

    case SubTests::PourIntoGlass:
    case SubTests::PourIntoGlassVideo:
      return SetupPour();

    case SubTests::MediumRoom:
      return SetupMediumRoom();

    case SubTests::NoLiquid:
      return SetupNoLiquid();
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
      return RunHydrostatic(true);
    case SubTests::PourIntoGlass:
      return RunPour(false);
    case SubTests::PourIntoGlassVideo:
      return RunPour(true);
    case SubTests::MediumRoom:
      return RunMediumRoom();
    case SubTests::NoLiquid:
      return RunNoLiquid();
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
  pLiquid->m_uiPressureCycles = 12;
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

plResult plGameEngineTestGpuFluidLiquidGallery::SetupHydrostatic(bool bRandomSeed)
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
  pLiquid->m_uiPressureCycles = 8;
  pLiquid->m_fDebugTopSpeed = 0.05f;
  ApplyTuningOverrides(pLiquid);

  plGpuFluidLiquidFillBox& fill = pLiquid->m_FillBoxes.ExpandAndGetRef();
  fill.m_vCenter = plVec3(0.0f, 0.0f, -0.5f * s_vHydroTank.z + 0.5f * s_fHydroDepth);
  fill.m_vHalfExtents = plVec3(0.5f * s_vHydroTank.x, 0.5f * s_vHydroTank.y, 0.5f * s_fHydroDepth);
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
  ref_sCsv.AppendFormat("total,{}\n", fTotal);
  ref_sCsv.AppendFormat("draw_total,{}\n", fDraw);

  // Per particle-substep: the substep phases (Sort, P2G, Solve, G2P) over particles times substeps; the solve per
  // active brick and substep; the once-per-frame surface (Surface Sort, Surface) apart.
  const double fFrames = m_uiTimedFrames;
  const double fSubsteps = static_cast<double>(m_uiTimedSubsteps) / fFrames;
  const double fBricks = static_cast<double>(m_uiTimedActiveBricks) / fFrames;
  auto Mean = [&](const char* szPhase) { return m_PhaseMs.GetValueOrDefault(szPhase, 0.0) / fFrames; };
  const double fSubstepMs = Mean("GpuFluid Liquid/Sort") + Mean("GpuFluid Liquid/P2G") + Mean("GpuFluid Liquid/Solve") + Mean("GpuFluid Liquid/G2P");
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
    sName.SetFormat("{}_{}", bRandomSeed ? "HydrostaticRandom" : "Hydrostatic", plArgI(m_iFrame, 4, true));
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
  const char* szName = bRandomSeed ? "HydrostaticRandom" : "Hydrostatic";

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
        plStats::RemoveStat(plStringBuilder("Renderer/GPU/", szPhase, "[ms]"));
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
    const plVariant value = plStats::GetStatValue(plStringBuilder("Renderer/GPU/", szPhase, "[ms]"));
    if (value.IsValid())
    {
      plTestFramework::Output(plTestOutput::Error, "No Liquid: the pass '%s' ran (%s)", szPhase, value.ConvertTo<plString>().GetData());
      ++uiRan;
    }
  }
  plTestFramework::Output(plTestOutput::Message, "No Liquid: %u of the liquid pass's GPU passes ran over 30 frames without a liquid volume", uiRan);
  PL_TEST_INT(uiRan, 0);
  return plTestAppRun::Quit;
}
