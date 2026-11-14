#include <ParticlesTest/ParticlesTestPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameEngine/Effects/Wind/SimpleWindWorldModule.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidColliderComponent.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasVolumeComponent.h>
#include <ParticlesTest/GpuFluid/GpuFluidGalleryTest.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <Shaders/GpuFluid/GpuFluidConstants.h>
#include <Texture/Image/ImageConversion.h>

// P2 of the gas solver: the sparse allocator (design plan section 3).
//
// Sub-tests of the GpuFluid Gallery (each asserts numbers):
// - N1 Allocation: a sparse plume box (12 x 12 x 16 m at 12.5 cm, 2304 virtual bricks, a pool of 1536). Every frame the
//   allocator's state is read back (page table, active list, per-slot brick and meta, the brick table's A) and checked
//   (CheckAllocationInvariants): page table and active list agree and hold each slot once, active + free = pool, A > 0
//   exactly for allocated bricks with content, every neighbour the last Finalize asked for (content within two voxels
//   of a face, or moving into it) is allocated unless the frame denied a request. Then the source stops and the gas is
//   made to fade fast (dissipation and cooling 15 1/s, so the content falls below the allocation threshold within about
//   35 ticks): 60 ticks after the stop no brick may be left.
// - N1 Allocation Cap: the same plume with a 64 brick pool: no NaN or overflow, the pool fills, requests are denied,
//   and denial follows the buckets' order: a bucket is only denied once every request of every later bucket was.
// - N11 Determinism: the plume four times, the state hashed every frame in virtual brick order (GasHash). Twice with the
//   defaults and a pool of 1024: every frame's hash must be equal. Then twice with exact sampling
//   (GpuFluid.Gas.ExactSampling) and pools of 1024 and 512 (both under the cap): equal again. The defaults' hardware
//   filtering rounds the normalised atlas coordinate, whose last bits depend on where a slot lies in the atlas, so runs
//   with different pools differ in the last bits from the first tick on (measured: 0 of 62 frames equal); exact sampling
//   takes its weights from the position in the brick, at about 3.6 times the advection's cost, so it is a mode for
//   tests and replays across pool sizes rather than the default.
// - Moving Emitter Video: with PL_GPUFLUID_VIDEO=1, hero demo (a) (plan amendment A1.4): a source travelling across a
//   64 x 48 m courtyard with walls and pillars, under a wind; the brick overlay (GpuFluid.Debug.Bricks) on for the
//   middle of the clip, the allocation readout (GpuFluid.Debug.BrickStats) throughout.

namespace
{
  const plVec3 s_vToSun(-0.85f, -0.25f, 0.42f);

  // The sparse plume of N1 and N11.
  const plVec3 s_vPlumeExtents(12.0f, 12.0f, 16.0f);
  constexpr float s_fPlumeVoxel = 0.125f;

  // N1
  constexpr plInt32 s_iN1EmitFrames = 75;  ///< 150 ticks with the source on
  constexpr plInt32 s_iN1StopFrames = 45;  ///< then 90 ticks without it
  constexpr plUInt32 s_uiN1StopTicks = 60; ///< no brick left this many ticks after the source stopped
  constexpr float s_fN1FadeRate = 15.0f;   ///< 1/s, dissipation and cooling once the source is off
  constexpr plUInt32 s_uiN1CapBricks = 64;
  constexpr plInt32 s_iN1CapFrames = 90;

  // N11: runs 0 and 1 the defaults (hardware filtering) with the same pool, runs 2 and 3 exact sampling with two pools,
  // run 4 exact sampling with run 2's pool but the free slots handed out in descending order, so every brick has another
  // slot than in run 2 (and every slot freed and taken again is another one).
  constexpr plInt32 s_iN11Frames = 64;
  constexpr plUInt32 s_uiN11Pools[] = {1024, 1024, 1024, 512, 1024};
  constexpr bool s_bN11Exact[] = {false, false, true, true, true};
  constexpr bool s_bN11Reverse[] = {false, false, false, false, true};

  // N1 Allocation Evict: the plume box with a small pool, its source travelling across the box once the pool is full.
  constexpr plUInt32 s_uiEvictPool = 96;
  constexpr plInt32 s_iEvictFrames = 150;      ///< 300 ticks
  constexpr plInt32 s_iEvictMoveFrame = 45;    ///< the source starts to move
  constexpr float s_fEvictSpeed = 2.0f;        ///< m/s along +x
  constexpr float s_fEvictMinEmitted = 0.9f;   ///< every frame's emission after the pool filled, of the median before

  // Smoke Column Sparse: the Smoke Column with the whole box allocated (phase 0) and sparse (phase 1).
  constexpr plInt32 s_iTwinFrames = 150;
  constexpr plInt32 s_iTwinFirstChecked = 30; ///< the mass is compared from this frame on (relative to the phase start)
  constexpr plInt32 s_iTwinTopFrame = 110;    ///< about when the whole box's plume reaches the open top
  // Tolerances: the measured run plus 50%. Measured on Metal: the sparse mass is within 1.6% of the whole box's until
  // the plume reaches the top, then up to 9.3% above it at 5 s; nothing is dropped by freeing (the plume only grows).
  // The difference is the flow, not lost gas: an unallocated neighbour is open air (p = 0, the ambient wind), so the
  // sparse plume's boundary is a free surface a brick or two from the gas rather than simulated still air, and its
  // centroid rises up to 0.18 m slower, so less of it reaches the open top and the edge fade there. The last frames'
  // images differ by 0.48-0.63% of white on average.
  constexpr float s_fTwinMassRisingTolerance = 0.024f;
  constexpr float s_fTwinMassTolerance = 0.14f;
  constexpr float s_fTwinImageTolerance = 0.0095f;
  const plVec3 s_vTwinCameraTarget(0.0f, 0.0f, 3.7f);

  // Moving Emitter
  constexpr plInt32 s_iMovingFrames = 450; ///< 15 s at 30 fps
  constexpr plInt32 s_iMovingBricksOn = 135;
  constexpr plInt32 s_iMovingBricksOff = 300;
  const plVec3 s_vCourtyardExtents(64.0f, 48.0f, 16.0f);
  constexpr float s_fCourtyardVoxel = 0.25f;
  constexpr float s_fCourtyardWind = 1.5f;
  constexpr plUInt32 s_uiCourtyardPool = 640;

  // The performance gate scene.
  const plVec3 s_vGateExtents(16.0f, 16.0f, 8.0f);
  constexpr float s_fGateVoxel = 0.0625f;

  float Tune(const char* szName, float fDefault)
  {
    plStringBuilder sVariable("PL_GPUFLUID_TUNE_", szName);
    const plString sValue = plEnvironmentVariableUtils::GetValueString(sVariable, "");
    double fValue = 0.0;
    if (!sValue.IsEmpty() && plConversionUtils::StringToFloat(sValue, fValue).Succeeded())
      return static_cast<float>(fValue);
    return fDefault;
  }

  bool VideoEnabled()
  {
    return plEnvironmentVariableUtils::GetValueInt("PL_GPUFLUID_VIDEO", 0) == 1;
  }

  void SetSmokeLook(plGpuFluidGasVolumeComponent* pGas, float fDensityScale)
  {
    pGas->m_fDensityScale = Tune("DENSITYSCALE", fDensityScale);
    const float fAlbedo = Tune("ALBEDO", 0.95f);
    pGas->m_Albedo = plColor(fAlbedo, fAlbedo, fAlbedo * 1.02f);
    pGas->m_fAnisotropy = Tune("ANISOTROPY", 0.4f);
    pGas->m_fEmissionScale = 0.0f;
    pGas->m_RenderMode = plVDBVolumeRenderMode::RayMarch;
  }

  plUInt32 PackBrick(const plVec3U32& v)
  {
    return v.x | (v.y << 10) | (v.z << 20);
  }

  plVec3U32 UnpackBrick(plUInt32 uiPacked)
  {
    return plVec3U32(uiPacked & 1023u, (uiPacked >> 10) & 1023u, uiPacked >> 20);
  }

  /// Where the courtyard's source is at time t (s): it drives across the yard from west to east, weaving between the
  /// pillars.
  plVec3 CourtyardSource(float t)
  {
    const float u = plMath::Clamp(t / 15.0f, 0.0f, 1.0f);
    const float x = -26.0f + 52.0f * u;
    const float y = -5.0f + 3.0f * plMath::Sin(plAngle::MakeFromRadian(u * 3.0f * plMath::Pi<float>()));
    return plVec3(x, y, 0.9f);
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// Scenes

plResult plGpuFluidGalleryApplication::SetupSparsePlumeScene(plUInt32 uiMaxBricks)
{
  PL_LOCK(m_pWorld->GetWriteMarker());
  SetupStage(0x47465031u, s_vToSun, true);

  plGameObjectDesc desc;
  desc.m_sName.Assign("SparsePlume");
  desc.m_bDynamic = true;
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * s_vPlumeExtents.z);
  plGameObject* pObject = nullptr;
  m_hSmokeObject = m_pWorld->CreateObject(desc, pObject);

  plGpuFluidGasVolumeComponent* pGas = nullptr;
  m_pWorld->GetOrCreateComponentManager<plGpuFluidGasVolumeComponentManager>()->CreateComponent(pObject, pGas);
  pGas->m_vExtents = s_vPlumeExtents;
  pGas->m_fVoxelSize = s_fPlumeVoxel;
  pGas->m_uiMaxBricks = uiMaxBricks;
  pGas->m_bAllocateAll = false;
  pGas->m_TickRate = plGpuFluidGasTickRate::Hz60; // N1's and N11's frame counts assume two ticks a frame
  pGas->m_DebugSource = plGpuFluidDebugSource::None;
  pGas->m_OpenFaces = plGpuFluidFaces::Default;
  pGas->m_fBuoyancy = 6.0f;
  pGas->m_fDensityWeight = 0.1f;
  pGas->m_fCooling = 0.5f;
  pGas->m_fDissipation = 0.05f;
  pGas->m_fEdgeFade = 0.5f;
  pGas->m_fVorticity = 0.3f;

  // A source a little off centre that also blows sideways, so the plume crosses brick faces on every axis.
  pGas->m_EmitterShape = plGpuFluidEmitterShape::Sphere;
  pGas->m_fEmitterRadius = 0.6f;
  pGas->m_fEmitterSoftness = 0.25f;
  pGas->m_vEmitterOffset = plVec3(1.0f, -0.5f, -0.5f * s_vPlumeExtents.z + 0.8f);
  pGas->m_fEmissionRate = 5.0f;
  pGas->m_vEmitterVelocity = plVec3(0.8f, 0.4f, 2.5f);
  pGas->m_fEmitterNoise = 0.8f;
  pGas->m_fEmitterNoiseScale = 0.5f;
  SetSmokeLook(pGas, 6.0f);
  pGas->ResetClock();

  m_hMovingCollider.Invalidate();
  return PL_SUCCESS;
}

plResult plGpuFluidGalleryApplication::SetupCourtyardScene()
{
  PL_LOCK(m_pWorld->GetWriteMarker());
  // A low sun ahead and to the left of the camera, so the smoke is lit from behind and the walls cast long shadows.
  SetupStage(0x47465061u, plVec3(Tune("SUNX", -0.6f), Tune("SUNY", 0.55f), Tune("SUNZ", 0.4f)), false);

  // The yard's paving: a darker square under the volume, with strips every 4 m for scale.
  AddBox("Paving", plVec3(0.0f, 0.0f, 0.005f), plVec3(s_vCourtyardExtents.x, s_vCourtyardExtents.y, 0.01f), plColor(0.30f, 0.29f, 0.27f));
  for (plInt32 i = -7; i <= 7; ++i)
    AddBox("Strip", plVec3(4.0f * i, 0.0f, 0.012f), plVec3(0.15f, s_vCourtyardExtents.y, 0.01f), plColor(0.18f, 0.17f, 0.16f));

  plGameObjectDesc desc;
  desc.m_sName.Assign("Courtyard");
  desc.m_bDynamic = true;
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * s_vCourtyardExtents.z);
  plGameObject* pObject = nullptr;
  m_hSmokeObject = m_pWorld->CreateObject(desc, pObject);

  plGpuFluidGasVolumeComponent* pGas = nullptr;
  m_pWorld->GetOrCreateComponentManager<plGpuFluidGasVolumeComponentManager>()->CreateComponent(pObject, pGas);
  pGas->m_vExtents = s_vCourtyardExtents;
  pGas->m_fVoxelSize = s_fCourtyardVoxel;
  // Smaller than the Medium cap (1536), so the pool fills during the clip and the readout shows the sources' bricks
  // evicting others while the source keeps emitting.
  pGas->m_uiMaxBricks = static_cast<plUInt32>(Tune("POOL", static_cast<float>(s_uiCourtyardPool)));
  pGas->m_Quality = plGpuFluidQuality::Medium;
  pGas->m_bAllocateAll = false;
  pGas->m_DebugSource = plGpuFluidDebugSource::None;
  pGas->m_OpenFaces = plGpuFluidFaces::Default;
  pGas->m_fBuoyancy = Tune("BUOYANCY", 4.0f);
  pGas->m_fDensityWeight = 0.05f;
  pGas->m_fCooling = Tune("COOLING", 0.35f);
  pGas->m_fDissipation = Tune("DISSIPATION", 0.12f);
  pGas->m_fEdgeFade = 2.0f;
  pGas->m_fVorticity = Tune("VORTICITY", 0.35f);
  pGas->m_fWindInfluence = 1.0f;

  // The travelling source: thick dark smoke, as from a burning cart.
  pGas->m_EmitterShape = plGpuFluidEmitterShape::Sphere;
  pGas->m_fEmitterRadius = Tune("RADIUS", 1.0f);
  pGas->m_fEmitterSoftness = 0.4f;
  pGas->m_vEmitterOffset = CourtyardSource(0.0f) - plVec3(0.0f, 0.0f, 0.5f * s_vCourtyardExtents.z);
  pGas->m_fEmissionRate = Tune("RATE", 18.0f);
  pGas->m_vEmitterVelocity = plVec3(0.0f, 0.0f, Tune("VELOCITY", 2.5f));
  pGas->m_fEmitterNoise = 0.85f;
  pGas->m_fEmitterNoiseScale = 0.8f;
  pGas->m_fDensityMax = 4.0f;
  SetSmokeLook(pGas, Tune("DENSITYSCALE", 4.0f));
  pGas->m_Albedo = plColor(0.55f, 0.54f, 0.53f);
  pGas->ResetClock();

  // The courtyard: a long north wall the wind pushes the smoke against, an arcade of pillars the source weaves
  // between, a gatehouse with two towers, and a low wall to the south.
  const plColor stone(0.66f, 0.63f, 0.58f);
  const plColor darkStone(0.50f, 0.48f, 0.45f);
  AddCollider("North Wall", plGpuFluidColliderShape::Box, plVec3(-6.0f, 9.0f, 3.0f), plVec3(14.0f, 0.4f, 3.0f), 0.0f, stone, true);
  AddCollider("North Wall East", plGpuFluidColliderShape::Box, plVec3(17.0f, 9.0f, 2.0f), plVec3(6.0f, 0.4f, 2.0f), 0.0f, stone, true);
  for (plInt32 i = 0; i < 6; ++i)
  {
    const float x = -14.0f + 6.0f * static_cast<float>(i);
    AddCollider("Pillar", plGpuFluidColliderShape::Box, plVec3(x, 1.5f, 3.5f), plVec3(0.5f, 0.5f, 3.5f), 0.0f, darkStone, true);
  }
  AddCollider("Gate Tower West", plGpuFluidColliderShape::Box, plVec3(-20.0f, -1.0f, 4.0f), plVec3(1.2f, 1.2f, 4.0f), 0.0f, stone, true);
  AddCollider("Gate Tower East", plGpuFluidColliderShape::Box, plVec3(-20.0f, -9.0f, 4.0f), plVec3(1.2f, 1.2f, 4.0f), 0.0f, stone, true);
  AddCollider("Gate Arch", plGpuFluidColliderShape::Box, plVec3(-20.0f, -5.0f, 6.5f), plVec3(0.8f, 2.8f, 0.8f), 0.0f, darkStone, true);
  AddCollider("South Wall", plGpuFluidColliderShape::Box, plVec3(6.0f, -15.0f, 1.0f), plVec3(16.0f, 0.4f, 1.0f), 0.0f, darkStone, true);
  AddCollider("Well", plGpuFluidColliderShape::Sphere, plVec3(14.0f, -6.0f, 0.0f), plVec3::MakeZero(), 1.6f, darkStone, true);

  // The wind blows from the south, across the source's path, onto the north wall.
  plSimpleWindWorldModule* pWind = m_pWorld->GetOrCreateModule<plSimpleWindWorldModule>();
  if (pWind == nullptr)
    return PL_FAILURE;
  pWind->SetFallbackWind(plVec3(Tune("WINDX", 0.4f), Tune("WINDY", s_fCourtyardWind), 0.0f));

  m_hMovingCollider.Invalidate();
  return PL_SUCCESS;
}

plResult plGpuFluidGalleryApplication::SetupGateScene()
{
  PL_LOCK(m_pWorld->GetWriteMarker());
  SetupStage(0x47465067u, s_vToSun, false);

  plGameObjectDesc desc;
  desc.m_sName.Assign("Gate");
  desc.m_bDynamic = true;
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * s_vGateExtents.z);
  plGameObject* pObject = nullptr;
  m_hSmokeObject = m_pWorld->CreateObject(desc, pObject);

  plGpuFluidGasVolumeComponent* pGas = nullptr;
  m_pWorld->GetOrCreateComponentManager<plGpuFluidGasVolumeComponentManager>()->CreateComponent(pObject, pGas);
  pGas->m_vExtents = s_vGateExtents;
  pGas->m_fVoxelSize = s_fGateVoxel;
  pGas->m_uiMaxBricks = 0; // the Medium cap, 1536
  pGas->m_Quality = plGpuFluidQuality::Medium;
  pGas->m_bAllocateAll = false;
  pGas->m_DebugSource = plGpuFluidDebugSource::None;
  pGas->m_OpenFaces = plGpuFluidFaces::Default;
  pGas->m_fBuoyancy = 6.0f;
  pGas->m_fDensityWeight = 0.1f;
  pGas->m_fCooling = Tune("GATECOOLING", 0.5f);
  pGas->m_fDissipation = Tune("GATEDISSIPATION", 0.8f);
  pGas->m_fEdgeFade = 0.5f;
  pGas->m_fVorticity = 0.3f;

  // Two plumes on a diagonal (up to four on a square with PL_GPUFLUID_TUNE_GATESOURCES), sized so the pool holds about
  // 60% of its bricks over the measured frames (Tier Timing logs the mean, min and max).
  const float fSpacing = Tune("GATESPACING", 4.0f);
  const float fRadius = Tune("GATERADIUS", 0.32f);
  const float fFloor = -0.5f * s_vGateExtents.z + 0.7f;
  pGas->m_EmitterShape = plGpuFluidEmitterShape::Sphere;
  pGas->m_fEmitterRadius = fRadius;
  pGas->m_fEmitterSoftness = 0.2f;
  pGas->m_vEmitterOffset = plVec3(-fSpacing, -fSpacing, fFloor);
  pGas->m_fEmissionRate = 5.0f;
  pGas->m_vEmitterVelocity = plVec3(0.0f, 0.0f, 2.0f);
  pGas->m_fEmitterNoise = 0.8f;
  pGas->m_fEmitterNoiseScale = 0.3f;
  pGas->m_DebugExtraSources.Clear();
  const plUInt32 uiSources = static_cast<plUInt32>(plMath::Clamp(Tune("GATESOURCES", 2.0f), 1.0f, 4.0f));
  const plVec2 extraAt[] = {plVec2(fSpacing, fSpacing), plVec2(fSpacing, -fSpacing), plVec2(-fSpacing, fSpacing)};
  for (plUInt32 i = 0; i + 1 < uiSources; ++i)
  {
    const plVec2 vAt = extraAt[i];
    plGpuFluidGasVolumeComponent::ExtraSource& extra = pGas->m_DebugExtraSources.ExpandAndGetRef();
    extra.m_vOffset = plVec3(vAt.x, vAt.y, fFloor);
    extra.m_fRadius = fRadius;
    extra.m_fRate = 5.0f;
    extra.m_vVelocity = plVec3(0.0f, 0.0f, 2.0f);
  }
  SetSmokeLook(pGas, 8.0f);
  pGas->ResetClock();

  m_hMovingCollider.Invalidate();
  return PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////
// Sub-tests

plResult plGameEngineTestGpuFluidGallery::InitializeP2SubTest(plInt32 iIdentifier)
{
  m_GasSamples.Clear();
  m_uiFirstRenderFrame = plRenderWorld::GetFrameCounter();
  m_uiN1Checked = 0;
  m_uiN1Failures = 0;
  m_uiN1LastDebugFrame = 0;
  m_uiStopRenderFrame = 0;
  m_N11Hashes.Clear();
  m_N11MaxActive.Clear();
  m_N11Denied.Clear();
  m_N11Freed.Clear();
  m_TwinMass.Clear();
  m_fTwinDropped = 0.0f;
  m_fTwinEmitted[0] = m_fTwinEmitted[1] = 0.0f;
  m_uiTwinMaxActive = 0;

  SetCVarForSubTest("Rendering.VDBVolumes.RayMarchShadowSteps", static_cast<plInt32>(Tune("SHADOWSTEPS", 12.0f)));
  SetCVarForSubTest("Rendering.VDBVolumes.SelfShadowLod", Tune("SHADOWLOD", 0.0f));
  SetCVarForSubTest("Rendering.VDBVolumes.AmbientOcclusion", Tune("AO", 0.0f));
  SetCVarForSubTest("Rendering.VDBVolumes.AmbientStrength", Tune("AMBIENT", 0.5f));

  switch (iIdentifier)
  {
    case SubTests::N1Allocation:
    case SubTests::N1AllocationCap:
    {
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupSparsePlumeScene(iIdentifier == SubTests::N1AllocationCap ? s_uiN1CapBricks : 1536));
      m_pOwnApplication->SetCamera(plVec3(-9.0f, -21.0f, 6.0f), plVec3(0.5f, 0.0f, 5.0f), 50.0f);
      PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
      plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
      if (pGas == nullptr)
        return PL_FAILURE;
      pGas->m_bDebugGridReadback = true;
      return PL_SUCCESS;
    }

    case SubTests::N11Determinism:
      // Each run builds its own scene and sets the sampling (RunN11Determinism); the CVar is put back afterwards.
      SetCVarForSubTest("GpuFluid.Gas.ExactSampling", false);
      return PL_SUCCESS;

    case SubTests::N1AllocationEvict:
    {
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupSparsePlumeScene(s_uiEvictPool));
      m_pOwnApplication->SetCamera(plVec3(-9.0f, -21.0f, 6.0f), plVec3(0.5f, 0.0f, 5.0f), 50.0f);
      PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
      plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
      if (pGas == nullptr)
        return PL_FAILURE;
      pGas->m_bDebugGridReadback = true;
      // Start at the -x side, so the source has the box to cross. No noise: the emission per frame is then the same
      // wherever the source is, as long as every brick it overlaps has a slot.
      pGas->m_vEmitterOffset.x = -4.5f;
      pGas->m_fEmitterNoise = 0.0f;
      return PL_SUCCESS;
    }

    case SubTests::SmokeColumnSparse:
      // Each phase builds its own scene (RunSmokeColumnSparse).
      return PL_SUCCESS;

    case SubTests::MovingEmitterVideo:
      SetCVarForSubTest("GpuFluid.Debug.BrickStats", true);
      SetCVarForSubTest("GpuFluid.Debug.Bricks", false);
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupCourtyardScene());
      m_pOwnApplication->SetCamera(plVec3(-30.0f, -27.0f, 13.0f), plVec3(-20.0f, 2.0f, 2.5f), 60.0f);
      return PL_SUCCESS;
  }

  return PL_FAILURE;
}

bool plGameEngineTestGpuFluidGallery::CheckAllocationInvariants(const plGpuFluidGridDebug& debug, plUInt32 uiDenied)
{
  const plVec3U32 vGrid = debug.m_vBrickGrid;
  const plUInt32 uiVirtual = vGrid.x * vGrid.y * vGrid.z;
  const plUInt32 uiCapacity = debug.m_uiCapacity;
  if (debug.m_PageTable.GetCount() != uiVirtual || debug.m_SlotBrick.GetCount() != uiCapacity)
    return false;

  plUInt32 uiErrors = 0;
  auto Fail = [&](const char* szWhat, plUInt32 a, plUInt32 b)
  {
    if (uiErrors++ < 4)
      plTestFramework::Output(plTestOutput::Error, "N1 frame %llu: %s (%u, %u)", debug.m_uiFrame, szWhat, a, b);
  };

  // Page table and active list agree, each slot once; active + free = pool.
  plDynamicArray<bool> listed;
  listed.SetCount(uiCapacity, false);
  const plUInt32 uiActive = debug.m_Counters[GPU_FLUID_COUNTER_ACTIVE];
  if (uiActive != debug.m_ActiveList.GetCount())
    Fail("active count and list length differ", uiActive, debug.m_ActiveList.GetCount());

  for (plUInt32 uiSlot : debug.m_ActiveList)
  {
    if (uiSlot >= uiCapacity)
    {
      Fail("active list entry beyond the pool", uiSlot, uiCapacity);
      continue;
    }
    if (listed[uiSlot])
      Fail("slot twice in the active list", uiSlot, 0);
    listed[uiSlot] = true;

    const plUInt32 uiPacked = debug.m_SlotBrick[uiSlot];
    if (uiPacked == 0xFFFFFFFFu)
    {
      Fail("free slot in the active list", uiSlot, 0);
      continue;
    }
    const plVec3U32 b = UnpackBrick(uiPacked);
    const plUInt32 uiLinear = b.x + vGrid.x * (b.y + vGrid.y * b.z);
    if (uiLinear >= uiVirtual || debug.m_PageTable[uiLinear] != uiSlot)
      Fail("active slot's brick does not map back to it", uiSlot, uiLinear < uiVirtual ? debug.m_PageTable[uiLinear] : uiLinear);
  }

  plUInt32 uiFree = 0;
  for (plUInt32 s = 0; s < uiCapacity; ++s)
  {
    if (debug.m_SlotBrick[s] == 0xFFFFFFFFu)
      ++uiFree;
    else if (!listed[s])
      Fail("slot with a brick missing from the active list", s, 0);
  }
  if (uiActive + uiFree != uiCapacity)
    Fail("active + free != pool", uiActive + uiFree, uiCapacity);

  // Every page entry belongs to its slot; A > 0 exactly for allocated bricks with content.
  for (plUInt32 v = 0; v < uiVirtual; ++v)
  {
    const plUInt32 uiSlot = debug.m_PageTable[v];
    const float fA = debug.m_BrickTableA[v];
    if (uiSlot == 0xFFFFFFFFu)
    {
      if (fA != 0.0f)
        Fail("brick without a slot has A > 0", v, 0);
      continue;
    }
    if (uiSlot >= uiCapacity || debug.m_SlotBrick[uiSlot] != PackBrick(plVec3U32(v % vGrid.x, (v / vGrid.x) % vGrid.y, v / (vGrid.x * vGrid.y))))
    {
      Fail("page entry does not match its slot's brick", v, uiSlot);
      continue;
    }
    if ((fA > 0.0f) != (debug.m_SlotAlive[uiSlot] != 0))
      Fail("A > 0 does not match content", v, debug.m_SlotAlive[uiSlot]);
  }

  // Content near a face (or moving into a neighbour) implies the neighbour is allocated, unless requests were denied or
  // slots evicted for a source's bricks.
  if (uiDenied == 0 && debug.m_Counters[GPU_FLUID_COUNTER_EVICTED] == 0)
  {
    for (plUInt32 uiSlot : debug.m_ActiveList)
    {
      if (uiSlot >= uiCapacity || debug.m_SlotBrick[uiSlot] == 0xFFFFFFFFu)
        continue;
      const plUInt32 uiMask = debug.m_SlotMask[uiSlot];
      if (uiMask == 0)
        continue;
      const plVec3U32 b = UnpackBrick(debug.m_SlotBrick[uiSlot]);
      for (plUInt32 n = 0; n < 27; ++n)
      {
        if ((uiMask & (1u << n)) == 0)
          continue;
        const plInt32 x = static_cast<plInt32>(b.x) + static_cast<plInt32>(n % 3) - 1;
        const plInt32 y = static_cast<plInt32>(b.y) + static_cast<plInt32>((n / 3) % 3) - 1;
        const plInt32 z = static_cast<plInt32>(b.z) + static_cast<plInt32>(n / 9) - 1;
        if (x < 0 || y < 0 || z < 0 || x >= static_cast<plInt32>(vGrid.x) || y >= static_cast<plInt32>(vGrid.y) || z >= static_cast<plInt32>(vGrid.z))
          continue;
        const plUInt32 uiLinear = static_cast<plUInt32>(x) + vGrid.x * (static_cast<plUInt32>(y) + vGrid.y * static_cast<plUInt32>(z));
        if (debug.m_PageTable[uiLinear] == 0xFFFFFFFFu)
          Fail("requested neighbour without a slot", uiSlot, n);
      }
    }
  }

  return uiErrors == 0;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunN1Allocation(bool bCap)
{
  const plInt32 iFrames = bCap ? s_iN1CapFrames : s_iN1EmitFrames + s_iN1StopFrames;

  // The source stops, and what is left fades out fast.
  if (!bCap && m_iFrame == s_iN1EmitFrames)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
    {
      pGas->m_EmitterShape = plGpuFluidEmitterShape::None;
      pGas->m_fDissipation = s_fN1FadeRate;
      pGas->m_fCooling = s_fN1FadeRate;
    }
    m_uiStopRenderFrame = plRenderWorld::GetFrameCounter();
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();

    plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
    plGpuFluidGridDebug debug;
    if (pGas != nullptr && pGas->GetGridDebug(debug) && debug.m_uiFrame != m_uiN1LastDebugFrame && !debug.m_PageTable.IsEmpty())
    {
      m_uiN1LastDebugFrame = debug.m_uiFrame;
      ++m_uiN1Checked;
      if (!CheckAllocationInvariants(debug, debug.m_Counters[GPU_FLUID_COUNTER_DENIED]))
        ++m_uiN1Failures;
    }

    // Denial order on every stats readback.
    plGpuFluidGasStats stats;
    if (pGas != nullptr && pGas->GetStats(stats) && stats.m_uiTicks > 0 && !m_GasSamples.IsEmpty() && m_GasSamples.PeekBack().m_uiFrame == stats.m_uiFrame)
    {
      // Denial order: a bucket may only be denied once every request of every later bucket was.
      for (plUInt32 b = 0; b < 4; ++b)
      {
        if (stats.m_uiDeniedPerBucket[b] == 0)
          continue;
        for (plUInt32 later = b + 1; later < 4; ++later)
        {
          PL_TEST_BOOL_MSG(stats.m_uiDeniedPerBucket[later] == stats.m_uiRequestsPerBucket[later], "N1 frame %llu: bucket %u denied %u while bucket %u got %u of %u requests", stats.m_uiFrame, b,
            stats.m_uiDeniedPerBucket[b], later, stats.m_uiRequestsPerBucket[later] - stats.m_uiDeniedPerBucket[later], stats.m_uiRequestsPerBucket[later]);
        }
      }
    }
  }

  if (m_iFrame + 1 < iFrames)
    return plTestAppRun::Continue;

  WriteGasCsv(m_GasSamples, bCap ? ":imgout/GpuFluidGallery/N1_AllocationCap.csv" : ":imgout/GpuFluidGallery/N1_Allocation.csv");

  plUInt32 uiNaN = 0, uiOverflow = 0, uiMaxActive = 0, uiMaxDenied = 0, uiMaxNew = 0;
  plUInt32 uiDeniedBucket3 = 0, uiDeniedBucket2 = 0;
  for (const GasSample& s : m_GasSamples)
  {
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
    uiOverflow = plMath::Max(uiOverflow, s.m_uiOverflow);
    uiMaxActive = plMath::Max(uiMaxActive, s.m_uiAllocActive);
    uiMaxDenied = plMath::Max(uiMaxDenied, s.m_uiAllocDenied);
    uiMaxNew = plMath::Max(uiMaxNew, s.m_uiAllocNew);
    uiDeniedBucket3 += s.m_uiDeniedBucket[3];
    uiDeniedBucket2 += s.m_uiDeniedBucket[2];
  }

  plTestFramework::Output(plTestOutput::Message, "N1%s: %u grid readbacks checked, %u failed; %u stats readbacks, active bricks up to %u, new up to %u a frame, denied up to %u a frame (bucket 3 %u, bucket 2 %u in all), NaN %u, overflow %u",
    bCap ? " cap" : "", m_uiN1Checked, m_uiN1Failures, m_GasSamples.GetCount(), uiMaxActive, uiMaxNew, uiMaxDenied, uiDeniedBucket3, uiDeniedBucket2, uiNaN, uiOverflow);

  PL_TEST_BOOL_MSG(m_uiN1Checked >= static_cast<plUInt32>(iFrames / 2), "N1: only %u grid readbacks arrived", m_uiN1Checked);
  PL_TEST_INT(m_uiN1Failures, 0);
  PL_TEST_INT(uiNaN, 0);
  PL_TEST_INT(uiOverflow, 0);

  if (bCap)
  {
    PL_TEST_INT(uiMaxActive, s_uiN1CapBricks);
    PL_TEST_BOOL_MSG(uiMaxDenied > 0, "N1 cap: a %u brick pool denied nothing", s_uiN1CapBricks);
    PL_TEST_BOOL_MSG(uiDeniedBucket3 > 0, "N1 cap: no proximity request was denied");
    return plTestAppRun::Quit;
  }

  PL_TEST_BOOL_MSG(uiMaxActive > 32 && uiMaxActive < 1536, "N1: active bricks up to %u", uiMaxActive);
  PL_TEST_INT(uiMaxDenied, 0);

  // No brick left s_uiN1StopTicks after the source stopped: the readbacks of frames that far on.
  const plUInt64 uiNoneFrom = m_uiStopRenderFrame + s_uiN1StopTicks / 2;
  plUInt32 uiLate = 0;
  plUInt32 uiLeft = 0;
  plUInt64 uiEmptyAt = 0;
  for (const GasSample& s : m_GasSamples)
  {
    if (uiEmptyAt == 0 && s.m_uiFrame >= m_uiStopRenderFrame && s.m_uiAllocActive == 0 && s.m_uiActiveBricks == 0)
      uiEmptyAt = s.m_uiFrame;
    if (s.m_uiFrame < uiNoneFrom)
      continue;
    ++uiLate;
    uiLeft = plMath::Max(uiLeft, plMath::Max(s.m_uiAllocActive, s.m_uiActiveBricks));
  }
  plTestFramework::Output(plTestOutput::Message, "N1: the source stopped at render frame %llu; no brick left from frame %llu (%lld ticks after); %u readbacks %u or more ticks after the stop, up to %u bricks in them",
    m_uiStopRenderFrame, uiEmptyAt, uiEmptyAt > 0 ? static_cast<long long>(2 * (uiEmptyAt - m_uiStopRenderFrame)) : -1ll, uiLate, s_uiN1StopTicks, uiLeft);
  PL_TEST_BOOL_MSG(uiLate > 0, "N1: no readback %u ticks after the stop", s_uiN1StopTicks);
  PL_TEST_INT(uiLeft, 0);
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunN11Determinism()
{
  constexpr plUInt32 uiRuns = PL_ARRAY_SIZE(s_uiN11Pools);

  if (m_iPhaseFrame == 0)
  {
    if (plCVarBool* pExact = static_cast<plCVarBool*>(plCVar::FindCVarByName("GpuFluid.Gas.ExactSampling")))
      *pExact = s_bN11Exact[m_iPhase];
    if (m_pOwnApplication->SetupSparsePlumeScene(s_uiN11Pools[m_iPhase]).Failed())
      return plTestAppRun::Quit;
    m_pOwnApplication->SetCamera(plVec3(-9.0f, -21.0f, 6.0f), plVec3(0.5f, 0.0f, 5.0f), 50.0f);
    {
      PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
      if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
      {
        pGas->m_bDebugHashState = true;
        pGas->m_bDebugReverseSlots = s_bN11Reverse[m_iPhase];
      }
    }
    m_uiFirstRenderFrame = plRenderWorld::GetFrameCounter();
    m_N11Hashes.ExpandAndGetRef();
    m_N11MaxActive.PushBack(0);
    m_N11Denied.PushBack(0);
    m_N11Freed.PushBack(0);
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
    plGpuFluidGasStats stats;
    if (pGas != nullptr && pGas->GetStats(stats))
    {
      if (stats.m_uiHashFrame >= m_uiFirstRenderFrame && stats.m_uiHashFrame > 0)
        m_N11Hashes.PeekBack().Insert(stats.m_uiHashFrame - m_uiFirstRenderFrame, stats.m_uiStateHash);
      if (stats.m_uiTicks > 0)
      {
        m_N11MaxActive.PeekBack() = plMath::Max(m_N11MaxActive.PeekBack(), stats.m_uiAllocActive);
        m_N11Denied.PeekBack() += stats.m_uiDeniedBricks;
        m_N11Freed.PeekBack() += stats.m_uiFreedBricks;
      }
    }
  }

  ++m_iPhaseFrame;
  if (m_iPhaseFrame < s_iN11Frames)
    return plTestAppRun::Continue;

  m_iPhaseFrame = 0;
  ++m_iPhase;
  if (m_iPhase < static_cast<plInt32>(uiRuns))
    return plTestAppRun::Continue;

  // Runs 0 and 1 (defaults, same pool) and runs 2 and 3 (exact sampling, two pools), on every frame both have a hash for.
  auto Compare = [&](plUInt32 a, plUInt32 b, plUInt32& out_uiCompared, plUInt32& out_uiEqual, plUInt64& out_uiLastHash)
  {
    out_uiCompared = 0;
    out_uiEqual = 0;
    plUInt64 uiLastFrame = 0;
    for (auto it : m_N11Hashes[a])
    {
      const plUInt64* pOther = m_N11Hashes[b].GetValue(it.Key());
      if (pOther == nullptr)
        continue;
      ++out_uiCompared;
      if (*pOther == it.Value())
        ++out_uiEqual;
      if (it.Key() >= uiLastFrame)
      {
        uiLastFrame = it.Key();
        out_uiLastHash = it.Value();
      }
    }
  };

  plUInt32 uiComparedSame = 0, uiEqualSame = 0, uiComparedPools = 0, uiEqualPools = 0, uiComparedSlots = 0, uiEqualSlots = 0;
  plUInt64 uiHashSame = 0, uiHashPools = 0, uiHashSlots = 0;
  Compare(0, 1, uiComparedSame, uiEqualSame, uiHashSame);
  Compare(2, 3, uiComparedPools, uiEqualPools, uiHashPools);
  Compare(2, 4, uiComparedSlots, uiEqualSlots, uiHashSlots);

  plTestFramework::Output(plTestOutput::Message, "N11 determinism, hardware filtering, two runs with pools of %u: %u of %u frames equal (last hash %016llx); exact sampling, pools of %u and %u: %u of %u frames equal (last hash %016llx); exact sampling, ascending and descending slots: %u of %u frames equal (last hash %016llx)",
    s_uiN11Pools[0], uiEqualSame, uiComparedSame, uiHashSame, s_uiN11Pools[2], s_uiN11Pools[3], uiEqualPools, uiComparedPools, uiHashPools, uiEqualSlots, uiComparedSlots, uiHashSlots);
  plTestFramework::Output(plTestOutput::Message, "N11 runs: active up to %u / %u / %u / %u / %u, denied %u / %u / %u / %u / %u, freed %u / %u / %u / %u / %u", m_N11MaxActive[0], m_N11MaxActive[1],
    m_N11MaxActive[2], m_N11MaxActive[3], m_N11MaxActive[4], m_N11Denied[0], m_N11Denied[1], m_N11Denied[2], m_N11Denied[3], m_N11Denied[4], m_N11Freed[0], m_N11Freed[1], m_N11Freed[2],
    m_N11Freed[3], m_N11Freed[4]);

  PL_TEST_BOOL_MSG(uiComparedSame >= static_cast<plUInt32>(s_iN11Frames) / 2 && uiComparedPools >= static_cast<plUInt32>(s_iN11Frames) / 2 && uiComparedSlots >= static_cast<plUInt32>(s_iN11Frames) / 2,
    "N11: only %u / %u / %u frames were hashed in both runs", uiComparedSame, uiComparedPools, uiComparedSlots);
  PL_TEST_INT(uiEqualSame, uiComparedSame);
  PL_TEST_INT(uiEqualPools, uiComparedPools);
  PL_TEST_INT(uiEqualSlots, uiComparedSlots);
  for (plUInt32 i = 0; i < uiRuns; ++i)
  {
    PL_TEST_INT(m_N11Denied[i], 0);
    PL_TEST_BOOL_MSG(m_N11MaxActive[i] > 16 && m_N11MaxActive[i] < s_uiN11Pools[i], "N11 run %u: active up to %u of %u", i, m_N11MaxActive[i], s_uiN11Pools[i]);
  }
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunMovingEmitterVideo()
{
  if (!VideoEnabled())
  {
    plTestFramework::Output(plTestOutput::Details, "Skipped: set PL_GPUFLUID_VIDEO=1 to record the frames.");
    return plTestAppRun::Quit;
  }

  const float t = static_cast<float>(plMath::Max(m_iFrame, 0)) / 30.0f;
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
      pGas->m_vEmitterOffset = CourtyardSource(t) - plVec3(0.0f, 0.0f, 0.5f * s_vCourtyardExtents.z);

    // The camera follows the source from the south, up high and a little behind it, so the trail is seen.
    const plVec3 vSource = CourtyardSource(t);
    const float fFollow = plMath::Clamp(vSource.x * 0.8f, -22.0f, 18.0f);
    m_pOwnApplication->SetCamera(plVec3(fFollow - Tune("CAMBACK", 9.0f), Tune("CAMY", -27.0f), Tune("CAMZ", 13.0f)), plVec3(fFollow + 1.0f, 2.0f, 2.5f), Tune("FOV", 60.0f));
  }

  // The brick overlay for the middle of the clip.
  const bool bBricks = m_iFrame >= s_iMovingBricksOn && m_iFrame < s_iMovingBricksOff;
  if (plCVarBool* pBricks = static_cast<plCVarBool*>(plCVar::FindCVarByName("GpuFluid.Debug.Bricks")))
    *pBricks = bBricks;

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();
  }

  const plInt32 iVideoFrame = m_iFrame - 1;
  if (iVideoFrame >= 0)
    SaveVideoFrame("MovingEmitter", iVideoFrame, "P2 Moving Emitter: 64 m courtyard, sparse bricks, Medium (30 Hz), pool 640; 4.5-10 s brick edges");

  if (iVideoFrame + 1 < static_cast<plInt32>(Tune("FRAMES", static_cast<float>(s_iMovingFrames))))
    return plTestAppRun::Continue;

  WriteGasCsv(m_GasSamples, ":imgout/GpuFluidGallery/MovingEmitter.csv");
  plUInt32 uiNaN = 0, uiMaxActive = 0, uiDenied = 0, uiSourceDenied = 0, uiEvicted = 0;
  for (const GasSample& s : m_GasSamples)
  {
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
    uiMaxActive = plMath::Max(uiMaxActive, s.m_uiAllocActive);
    uiDenied += s.m_uiAllocDenied;
    uiSourceDenied += s.m_uiDeniedBucket[0];
    uiEvicted += s.m_uiAllocEvicted;
  }
  plTestFramework::Output(plTestOutput::Message, "Moving emitter video: %d frames, %u readbacks, active bricks up to %u of %u, %u denied in all (%u of a source's), %u evicted, NaN %u",
    s_iMovingFrames, m_GasSamples.GetCount(), uiMaxActive, s_uiCourtyardPool, uiDenied, uiSourceDenied, uiEvicted, uiNaN);
  PL_TEST_INT(uiNaN, 0);
  PL_TEST_INT(uiSourceDenied, 0);
  PL_TEST_BOOL(uiMaxActive > 0);
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunN1AllocationEvict()
{
  // The source travels along +x once the pool has filled, into bricks that have no slot: the allocator must evict
  // bricks no source overlaps for them, so the source never stops emitting.
  if (m_iFrame >= s_iEvictMoveFrame)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
      pGas->m_vEmitterOffset.x = plMath::Min(-4.5f + s_fEvictSpeed * static_cast<float>(m_iFrame - s_iEvictMoveFrame) / 30.0f, 4.5f);
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();

    plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
    plGpuFluidGridDebug debug;
    if (pGas != nullptr && pGas->GetGridDebug(debug) && debug.m_uiFrame != m_uiN1LastDebugFrame && !debug.m_PageTable.IsEmpty())
    {
      m_uiN1LastDebugFrame = debug.m_uiFrame;
      ++m_uiN1Checked;
      if (!CheckAllocationInvariants(debug, debug.m_Counters[GPU_FLUID_COUNTER_DENIED]))
        ++m_uiN1Failures;
    }
  }

  if (m_iFrame + 1 < s_iEvictFrames)
    return plTestAppRun::Continue;

  WriteGasCsv(m_GasSamples, ":imgout/GpuFluidGallery/N1_AllocationEvict.csv");

  // Emission per frame before the pool first filled, and after.
  plDynamicArray<float> before;
  plUInt64 uiFullFrame = 0;
  plUInt32 uiNaN = 0, uiOverflow = 0, uiMaxActive = 0, uiEvicted = 0, uiSourceDenied = 0, uiDenied = 0;
  float fDropped = 0.0f, fEmitted = 0.0f;
  for (const GasSample& s : m_GasSamples)
  {
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
    uiOverflow = plMath::Max(uiOverflow, s.m_uiOverflow);
    uiMaxActive = plMath::Max(uiMaxActive, s.m_uiAllocActive);
    uiEvicted += s.m_uiAllocEvicted;
    uiSourceDenied += s.m_uiDeniedBucket[0];
    uiDenied += s.m_uiAllocDenied;
    fDropped += s.m_fMassDropped;
    fEmitted += s.m_fEmitted;
    if (uiFullFrame == 0 && s.m_uiAllocActive >= s_uiEvictPool)
      uiFullFrame = s.m_uiFrame;
    if (uiFullFrame == 0 && s.m_fEmitted > 0.0f)
      before.PushBack(s.m_fEmitted);
  }
  before.Sort();
  const float fMedianBefore = before.IsEmpty() ? 0.0f : before[before.GetCount() / 2];

  // Once full, the source keeps emitting on every frame that ran its ticks (stats readbacks with ticks).
  plUInt32 uiAfter = 0, uiLow = 0;
  float fLowest = -1.0f;
  for (const GasSample& s : m_GasSamples)
  {
    if (uiFullFrame == 0 || s.m_uiFrame <= uiFullFrame)
      continue;
    ++uiAfter;
    if (s.m_fEmitted < s_fEvictMinEmitted * fMedianBefore)
      ++uiLow;
    fLowest = fLowest < 0.0f ? s.m_fEmitted : plMath::Min(fLowest, s.m_fEmitted);
  }

  plTestFramework::Output(plTestOutput::Message,
    "N1 evict: pool of %u full from render frame %llu; %u readbacks after, %u of them emitting less than %.0f%% of the median before (%.3f), lowest %.3f; evicted %u, denied %u (sources %u), dropped mass %.2f of %.2f emitted; %u grid readbacks checked, %u failed; NaN %u, overflow %u",
    s_uiEvictPool, uiFullFrame, uiAfter, uiLow, 100.0f * s_fEvictMinEmitted, fMedianBefore, fLowest, uiEvicted, uiDenied, uiSourceDenied, fDropped, fEmitted, m_uiN1Checked, m_uiN1Failures, uiNaN,
    uiOverflow);

  PL_TEST_BOOL_MSG(uiFullFrame > 0, "N1 evict: the pool of %u never filled (largest %u)", s_uiEvictPool, uiMaxActive);
  PL_TEST_BOOL_MSG(uiAfter >= 30, "N1 evict: only %u readbacks after the pool filled", uiAfter);
  PL_TEST_BOOL_MSG(uiEvicted > 0, "N1 evict: nothing was evicted");
  PL_TEST_INT(uiSourceDenied, 0);
  PL_TEST_INT(uiLow, 0);
  PL_TEST_INT(m_uiN1Failures, 0);
  PL_TEST_INT(uiNaN, 0);
  PL_TEST_INT(uiOverflow, 0);
  return plTestAppRun::Quit;
}

namespace
{
  /// Mean absolute difference of two images as a fraction of white (0..1), over the RGB channels.
  float MeanImageDifference(const plImage& a, const plImage& b)
  {
    plImage ca, cb;
    if (plImageConversion::Convert(a, ca, plImageFormat::R8G8B8A8_UNORM).Failed() || plImageConversion::Convert(b, cb, plImageFormat::R8G8B8A8_UNORM).Failed())
      return 1.0f;
    if (ca.GetWidth() != cb.GetWidth() || ca.GetHeight() != cb.GetHeight() || ca.GetWidth() == 0)
      return 1.0f;

    plUInt64 uiSum = 0;
    for (plUInt32 y = 0; y < ca.GetHeight(); ++y)
    {
      for (plUInt32 x = 0; x < ca.GetWidth(); ++x)
      {
        const plUInt8* pa = ca.GetPixelPointer<plUInt8>(0, 0, 0, x, y);
        const plUInt8* pb = cb.GetPixelPointer<plUInt8>(0, 0, 0, x, y);
        for (plUInt32 c = 0; c < 3; ++c)
          uiSum += static_cast<plUInt64>(plMath::Abs(static_cast<plInt32>(pa[c]) - static_cast<plInt32>(pb[c])));
      }
    }
    return static_cast<float>(static_cast<double>(uiSum) / (static_cast<double>(ca.GetWidth()) * ca.GetHeight() * 3.0 * 255.0));
  }
} // namespace

plTestAppRun plGameEngineTestGpuFluidGallery::RunSmokeColumnSparse()
{
  // Phase 0 the Smoke Column with the whole box allocated, phase 1 the same scene sparse. Same source, seed and ticks:
  // the sparse grid may only lose what leaves the allocated bricks.
  if (m_iPhaseFrame == 0)
  {
    if (m_pOwnApplication->SetupSmokeColumnScene(m_iPhase == 0).Failed())
      return plTestAppRun::Quit;
    const plAngle angle = plAngle::MakeFromDegree(-90.0f);
    m_pOwnApplication->SetCamera(s_vTwinCameraTarget + plVec3(12.5f * plMath::Cos(angle), 12.5f * plMath::Sin(angle), -1.5f), s_vTwinCameraTarget, 50.0f);
    m_uiFirstRenderFrame = plRenderWorld::GetFrameCounter();
    m_TwinMass.ExpandAndGetRef();
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
    plGpuFluidGasStats stats;
    if (pGas != nullptr && pGas->GetStats(stats) && stats.m_uiTicks > 0 && stats.m_uiFrame >= m_uiFirstRenderFrame && !m_TwinMass.PeekBack().Contains(stats.m_uiFrame - m_uiFirstRenderFrame))
    {
      m_TwinMass.PeekBack().Insert(stats.m_uiFrame - m_uiFirstRenderFrame, plVec2(stats.m_fMass, stats.m_vCentroid.z * pGas->GetGridExtents().z));
      m_fTwinEmitted[m_iPhase] += stats.m_fEmitted;
      if (m_iPhase == 1)
      {
        m_fTwinDropped += stats.m_fMassDropped;
        m_uiTwinMaxActive = plMath::Max(m_uiTwinMaxActive, stats.m_uiAllocActive);
      }
    }
  }

  ++m_iPhaseFrame;
  if (m_iPhaseFrame < s_iTwinFrames)
    return plTestAppRun::Continue;

  m_TwinImages[m_iPhase].ResetAndCopy(m_pOwnApplication->GetLastScreenshot());
  PL_TEST_BOOL(m_pOwnApplication->SaveShot(m_iPhase == 0 ? "SmokeColumnTwin_Whole" : "SmokeColumnTwin_Sparse").Succeeded());

  m_iPhaseFrame = 0;
  ++m_iPhase;
  if (m_iPhase < 2)
    return plTestAppRun::Continue;

  // The mass curves and the centroids' heights, on every frame both phases have a readback for: before the plume reaches
  // the open top (s_iTwinTopFrame), and over the whole run.
  plStringBuilder sCsv("frame,mass_whole,mass_sparse,centroid_z_whole,centroid_z_sparse\n");
  plUInt32 uiCompared = 0;
  float fLargest = 0.0f, fLargestRising = 0.0f, fLargestCentroid = 0.0f, fSum = 0.0f;
  plUInt64 uiLargestFrame = 0;
  for (auto it : m_TwinMass[0])
  {
    const plVec2* pSparse = m_TwinMass[1].GetValue(it.Key());
    if (pSparse == nullptr)
      continue;
    sCsv.AppendFormat("{},{},{},{},{}\n", it.Key(), it.Value().x, pSparse->x, it.Value().y, pSparse->y);
    if (it.Key() < static_cast<plUInt64>(s_iTwinFirstChecked) || it.Value().x <= 0.0f)
      continue;
    const float fRelative = plMath::Abs(pSparse->x - it.Value().x) / it.Value().x;
    ++uiCompared;
    fSum += fRelative;
    fLargestCentroid = plMath::Max(fLargestCentroid, plMath::Abs(pSparse->y - it.Value().y));
    if (it.Key() < static_cast<plUInt64>(s_iTwinTopFrame))
      fLargestRising = plMath::Max(fLargestRising, fRelative);
    if (fRelative > fLargest)
    {
      fLargest = fRelative;
      uiLargestFrame = it.Key();
    }
  }
  {
    plFileWriter csv;
    if (csv.Open(":imgout/GpuFluidGallery/SmokeColumnSparse.csv").Succeeded())
      csv.WriteBytes(sCsv.GetData(), sCsv.GetElementCount()).IgnoreResult();
  }

  const float fImage = MeanImageDifference(m_TwinImages[0], m_TwinImages[1]);
  plTestFramework::Output(plTestOutput::Message,
    "Smoke Column Sparse: %u frames compared, mass difference sparse vs whole box before frame %d largest %.5f, over the run largest %.5f (frame %llu), mean %.5f; centroid heights differ by up to %.3f m; emitted %.2f / %.2f, dropped by freeing %.3f, active up to %u of 1024; last image mean difference %.5f",
    uiCompared, s_iTwinTopFrame, fLargestRising, fLargest, uiLargestFrame, uiCompared > 0 ? fSum / uiCompared : 0.0f, fLargestCentroid, m_fTwinEmitted[0], m_fTwinEmitted[1], m_fTwinDropped,
    m_uiTwinMaxActive, fImage);

  PL_TEST_BOOL_MSG(uiCompared >= static_cast<plUInt32>(s_iTwinFrames - s_iTwinFirstChecked) / 2, "Smoke Column Sparse: only %u frames compared", uiCompared);
  PL_TEST_BOOL_MSG(fLargestRising <= s_fTwinMassRisingTolerance, "Smoke Column Sparse: mass differs by %.5f before frame %d (tolerance %.5f)", fLargestRising, s_iTwinTopFrame,
    s_fTwinMassRisingTolerance);
  PL_TEST_BOOL_MSG(fLargest <= s_fTwinMassTolerance, "Smoke Column Sparse: mass differs by %.5f (tolerance %.5f)", fLargest, s_fTwinMassTolerance);
  PL_TEST_BOOL_MSG(fImage <= s_fTwinImageTolerance, "Smoke Column Sparse: image differs by %.5f (tolerance %.5f)", fImage, s_fTwinImageTolerance);
  PL_TEST_BOOL_MSG(m_uiTwinMaxActive < 1024, "Smoke Column Sparse: the sparse run allocated the whole box (%u)", m_uiTwinMaxActive);
  return plTestAppRun::Quit;
}
