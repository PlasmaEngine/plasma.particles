#include <ParticlesTest/ParticlesTestPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/Types/TagRegistry.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <Foundation/Utilities/Stats.h>
#include <GameEngine/Effects/Wind/SimpleWindWorldModule.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidColliderComponent.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasVolumeComponent.h>
#include <ParticlesTest/GpuFluid/GpuFluidGalleryTest.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

// P3 of the gas solver: multigrid pressure, MacCormack velocity, vorticity confinement, wind and analytic colliders.
//
// Sub-tests of the GpuFluid Gallery (numeric first, each also asserts numbers):
// - N2 Divergence: the Smoke Column with the multigrid's default V-cycles; L_inf(div after) / L_inf(div before) < 1e-2 on
//   every readback after the first half second. A second phase with 12 V-cycles measures the half-precision floor. The
//   same limit is checked in every other gas scene of this file (CheckDivergenceRatio), the wind's inflow faces and the
//   moving solids included.
// - N3 Poisson: a manufactured problem with a known solution on 64 cubed (8 x 8 x 8 bricks), every face open. The
//   residual must fall at least 5x per V-cycle while it is above the float floor; then a sweep of solver settings
//   (multigrid V-cycles, red-black SOR iterations) writes residual and error against the GPU time of Gas/Pressure to
//   N3_Poisson.csv, for the convergence-per-ms comparison. The plan's third column, MGPCG, has no gas solver to time: the
//   design's gas pressure is sparse MG only, MGPCG is the liquid's dense solver (L10, liquid track P7), and its rows
//   belong in that track's N3 sweep.
// - N4 Gas Mass: a closed box (every face a wall, no dissipation), a source for half a second, then 300 ticks in all:
//   the mass may drift by at most s_fN4MassTolerance. N4 Gas Mass Velocity SL: the same with the velocity advected
//   semi-Lagrangian (scalars still MacCormack), against the tighter s_fN4MassToleranceVelocitySL, so a regression in the
//   scalar advection's conservation is not hidden by the larger drift of the defaults.
// - N9 Colliders: a plume rising into a sphere collider centred above its source, no noise: the density-weighted
//   centroid stays within a voxel of the middle in x and y, gas reaches the sphere and gets past it, and the gas the
//   solid deletes from its cells (GPU_FLUID_STAT_SOLID_CLEARED) stays a small fraction of what the source emitted. (No
//   density left inside the solid and no flow through its faces hold by construction, GasSolids writes both every
//   tick; they are logged, not what the test is about.) N9 Moving Collider: the Smoke Around Obstacles scene (noise, the
//   box sweeping and turning through the plume) without recording frames, held to s_fN9MaxClearedMoving.
// - Corridor Smoke Video, Smoke Around Obstacles Video: with PL_GPUFLUID_VIDEO=1, the P3 clips.

namespace
{
  const plVec3 s_vToSun(-0.85f, -0.25f, 0.42f);
  constexpr float s_fVoxel = 0.0625f;

  // N2
  constexpr plInt32 s_iN2Frames = 150;
  constexpr plInt32 s_iN2FloorFrames = 40;
  constexpr float s_fN2MaxRatio = 1e-2f;
  /// Readbacks whose largest divergence before the projection is below this are not held to the ratio: there the result
  /// is the half-precision floor of the stored velocity (about 0.1 1/s), not the solver's convergence.
  constexpr float s_fN2MinDivergence = 10.0f;

  // N3
  constexpr plInt32 s_iN3ProbeFrames = 8;
  constexpr plUInt32 s_uiN3ProbeCycles = 12;
  constexpr plInt32 s_iN3WarmupFrames = 6;
  constexpr plInt32 s_iN3MeasureFrames = 12;
  constexpr float s_fN3MaxFactor = 0.2f;
  constexpr plUInt32 s_uiN3MultigridCycles[] = {1, 2, 3, 4, 6, 8};
  constexpr plUInt32 s_uiN3SORIterations[] = {10, 20, 40, 80, 160, 320};

  // N4: 300 ticks (150 frames of two), the source on for the first half second.
  constexpr plInt32 s_iN4Frames = 150;
  constexpr float s_fN4SourceSeconds = 0.5f;
  constexpr float s_fN4ReferenceSeconds = 0.7f;
  /// Largest relative mass drift from the reference readback to the end. Set from the first measured run plus 50%, so a
  /// change that loses or gains more gas shows: P3 measured 0.4045 with the Medium defaults (MacCormack scalars and
  /// velocity, vorticity confinement 0.3, then a short second V-cycle), 0.4457 with two full V-cycles; the tighter first
  /// value is kept.
  ///
  /// Where it comes from (one setting changed at a time, largest drift): velocity advected semi-Lagrangian 0.114,
  /// confinement off 0.124, both advections semi-Lagrangian 0.178 (a loss), 8 V-cycles 0.377. So it is the MacCormack
  /// velocity together with confinement: each alone gives about 0.12, together they keep the closed box stirred with
  /// strong grid-scale motion, and the scalars' trilinear backtraces (semi-Lagrangian and MacCormack alike, neither is
  /// conservative) gain gas in that flow: with it the largest divergence before the projection reaches 25 1/s, without it
  /// about 7. The pressure solve is not the cause: the mean divergence after it stays near 1e-5 1/s while the mass grows.
  /// The MacCormack velocity stays on at Medium and up (plan amendment A1.8, plume-top streaking); N4 Gas Mass Velocity
  /// SL holds the scalar advection to the tighter limit below.
  constexpr float s_fN4MassTolerance = 0.61f;
  /// The same rule for the run with the velocity advected semi-Lagrangian: measured 0.1155, plus 50%.
  constexpr float s_fN4MassToleranceVelocitySL = 0.173f;

  // N9
  constexpr plInt32 s_iN9Frames = 150;
  constexpr float s_fN9SphereHeight = 2.2f; ///< above the floor
  constexpr float s_fN9SphereRadius = 0.6f;
  /// Gas the solids may delete from their cells, as a fraction of what the sources emitted over the run. Around a static
  /// solid the advection carries nothing in (every face of a solid cell holds the solid's velocity, 0, so a solid cell's
  /// backtrace stays on its own empty texel): P3 measured exactly 0. A moving solid deletes what it sweeps over: 0.0101.
  constexpr float s_fN9MaxClearedStatic = 1e-3f; ///< N9 Colliders' sphere, the corridor's partition and cabinet
  constexpr float s_fN9MaxClearedMoving = 0.02f; ///< N9 Moving Collider: the box sweeps and turns through the plume

  // Gas Wind
  constexpr plInt32 s_iWindFrames = 120;
  constexpr float s_fWindSpeed = 2.0f;

  // Videos
  constexpr plInt32 s_iCorridorFrames = 300;
  constexpr plInt32 s_iObstacleFrames = 240;

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

  /// The GPU time of Gas/Pressure in the last frame, ms (every run of it summed).
  double PressureMilliseconds()
  {
    plDynamicArray<plString> names;
    plDynamicArray<plVariant> values;
    plStats::GetStatsWithPrefix("Renderer/GPU/Gas/Pressure", names, values);
    double fSum = 0.0;
    for (const plVariant& value : values)
      fSum += value.ConvertTo<double>();
    return fSum;
  }

  double Median(plDynamicArray<double> values)
  {
    if (values.IsEmpty())
      return 0.0;
    values.Sort();
    return values[values.GetCount() / 2];
  }

  /// N2's limit in any gas scene: L_inf(div after) / L_inf(div before) < s_fN2MaxRatio on every readback after the first
  /// half second whose divergence before the projection is above s_fN2MinDivergence. bRequireReadbacks: at least one
  /// readback must be that far above the floor (a calm scene may have none).
  template <typename Samples>
  void CheckDivergenceRatio(const Samples& samples, const char* szScene, bool bRequireReadbacks = true)
  {
    float fWorst = 0.0f;
    float fWorstAt = 0.0f;
    float fWorstBefore = 0.0f;
    float fWorstAfter = 0.0f;
    plUInt32 uiChecked = 0;
    for (const auto& s : samples)
    {
      if (s.m_fSeconds < 0.5f || s.m_fDivBefore < s_fN2MinDivergence)
        continue;
      ++uiChecked;
      const float fRatio = s.m_fDivAfter / s.m_fDivBefore;
      if (!(fRatio <= fWorst)) // a NaN counts as the worst
      {
        fWorst = fRatio;
        fWorstAt = s.m_fSeconds;
        fWorstBefore = s.m_fDivBefore;
        fWorstAfter = s.m_fDivAfter;
      }
    }

    plTestFramework::Output(plTestOutput::Message, "%s, N2 divergence ratio: worst %.5f at %.2f s (%.3f -> %.4f 1/s) over %u readbacks; limit %.3f", szScene, fWorst, fWorstAt, fWorstBefore,
      fWorstAfter, uiChecked, s_fN2MaxRatio);
    PL_TEST_BOOL_MSG(!bRequireReadbacks || uiChecked > 0, "%s: no readback above %.1f 1/s to check the divergence ratio on", szScene, s_fN2MinDivergence);
    PL_TEST_BOOL_MSG(fWorst < s_fN2MaxRatio, "%s: the largest divergence after the projection is %.4f of the one before (limit %.3f)", szScene, fWorst, s_fN2MaxRatio);
  }

  /// Gas the solids deleted from their cells over the run, against what the sources emitted: logged, and held to
  /// fMaxFraction.
  template <typename Samples>
  void CheckSolidClearing(const Samples& samples, const char* szScene, float fMaxFraction)
  {
    double fEmitted = 0.0;
    double fCleared = 0.0;
    for (const auto& s : samples)
    {
      fEmitted += s.m_fEmitted;
      fCleared += s.m_fSolidCleared;
    }
    const double fFraction = fCleared / plMath::Max(fEmitted, 1e-6);
    plTestFramework::Output(plTestOutput::Message, "%s, gas lost to the solids: %.3f of %.1f emitted over %u readbacks = %.5f (limit %.3f)", szScene, fCleared, fEmitted, samples.GetCount(),
      fFraction, fMaxFraction);
    PL_TEST_BOOL_MSG(fEmitted > 1.0, "%s: the readbacks saw almost no emission (%.3f)", szScene, fEmitted);
    PL_TEST_BOOL_MSG(fFraction <= fMaxFraction, "%s: the solids deleted %.4f of the emitted gas (limit %.3f)", szScene, fFraction, fMaxFraction);
  }

  /// Every face closed. (A default-constructed plBitflags holds plGpuFluidFaces::Default, open but the floor.)
  plBitflags<plGpuFluidFaces> NoFaces()
  {
    plBitflags<plGpuFluidFaces> faces;
    faces.SetValue(0);
    return faces;
  }

  plBitflags<plGpuFluidFaces> AllFaces()
  {
    return plGpuFluidFaces::NegX | plGpuFluidFaces::PosX | plGpuFluidFaces::NegY | plGpuFluidFaces::PosY | plGpuFluidFaces::NegZ | plGpuFluidFaces::PosZ;
  }

  /// The march settings of a smoke volume, as the Smoke Column has them.
  void SetSmokeLook(plGpuFluidGasVolumeComponent* pGas, float fDensityScale)
  {
    pGas->m_fDensityScale = Tune("DENSITYSCALE", fDensityScale);
    const float fAlbedo = Tune("ALBEDO", 0.95f);
    pGas->m_Albedo = plColor(fAlbedo, fAlbedo, fAlbedo * 1.02f);
    pGas->m_fAnisotropy = Tune("ANISOTROPY", 0.4f);
    pGas->m_fEmissionScale = 0.0f;
    pGas->m_RenderMode = plVDBVolumeRenderMode::RayMarch;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// Scenes

plGameObject* plGpuFluidGalleryApplication::AddCollider(const char* szName, plUInt8 uiShape, const plVec3& vCenter, const plVec3& vHalfExtents, float fRadius, const plColor& color, bool bStatic, bool bVisible)
{
  plGameObject* pObject = nullptr;
  if (uiShape == plGpuFluidColliderShape::Sphere)
  {
    if (!m_hSphereMesh.IsValid())
    {
      plGeometry geometry;
      geometry.AddStackedSphere(0.5f, 48, 32);
      geometry.ComputeTangents();

      plMeshResourceDescriptor descriptor;
      plMeshBufferResourceDescriptor& meshBuffer = descriptor.MeshBufferDesc();
      meshBuffer.AddCommonStreams();
      meshBuffer.AllocateStreamsFromGeometry(geometry, plGALPrimitiveTopology::Triangles);
      descriptor.AddSubMesh(meshBuffer.GetPrimitiveCount(), 0, 0);
      descriptor.SetMaterial(0, "{ 7c71fc2d-2a1a-4938-9305-f3a0f26d73cc }"); // LitStandard
      descriptor.ComputeBounds();
      m_hSphereMesh = plResourceManager::GetOrCreateResource<plMeshResource>("GameEngineTest/GpuFluidGallery/Sphere", std::move(descriptor), "GpuFluid gallery sphere");
    }

    plGameObjectDesc desc;
    desc.m_sName.Assign(szName);
    desc.m_bDynamic = !bStatic;
    desc.m_LocalPosition = vCenter;
    desc.m_Tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow"));
    m_pWorld->CreateObject(desc, pObject);

    if (bVisible)
    {
      // The mesh is a unit-diameter sphere on a child, scaled to the collider; the collider's own object stays unscaled.
      plGameObjectDesc meshDesc;
      meshDesc.m_sName.Assign("Mesh");
      meshDesc.m_bDynamic = !bStatic;
      meshDesc.m_hParent = pObject->GetHandle();
      meshDesc.m_LocalScaling = plVec3(2.0f * fRadius);
      meshDesc.m_Tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow"));
      plGameObject* pMeshObject = nullptr;
      m_pWorld->CreateObject(meshDesc, pMeshObject);

      plMeshComponent* pMesh = nullptr;
      m_pWorld->GetOrCreateComponentManager<plMeshComponentManager>()->CreateComponent(pMeshObject, pMesh);
      pMesh->SetMesh(m_hSphereMesh);
      pMesh->SetColor(color);
    }
  }
  else
  {
    if (bVisible)
    {
      // AddBox scales a unit box to the size; the collider takes half extents in that scaled space, 0.5 per unit.
      pObject = AddBox(szName, vCenter, vHalfExtents * 2.0f, color);
      if (!bStatic)
        pObject->MakeDynamic();
    }
    else
    {
      plGameObjectDesc desc;
      desc.m_sName.Assign(szName);
      desc.m_bDynamic = !bStatic;
      desc.m_LocalPosition = vCenter;
      desc.m_LocalScaling = vHalfExtents * 2.0f;
      m_pWorld->CreateObject(desc, pObject);
    }
  }

  plGpuFluidColliderComponent* pCollider = nullptr;
  m_pWorld->GetOrCreateComponentManager<plGpuFluidColliderComponentManager>()->CreateComponent(pObject, pCollider);
  pCollider->m_Shape = static_cast<plGpuFluidColliderShape::Enum>(uiShape);
  pCollider->m_vHalfExtents = plVec3(0.5f); // of the object's scale (box)
  pCollider->m_fRadius = fRadius;
  pCollider->m_bStatic = bStatic;
  return pObject;
}

plGameObject* plGpuFluidGalleryApplication::GetMovingCollider()
{
  plGameObject* pObject = nullptr;
  m_pWorld->TryGetObject(m_hMovingCollider, pObject);
  return pObject;
}

plResult plGpuFluidGalleryApplication::SetupPoissonScene()
{
  PL_LOCK(m_pWorld->GetWriteMarker());
  SetupStage(0x47465033u, s_vToSun, false);

  plGameObjectDesc desc;
  desc.m_sName.Assign("Poisson");
  desc.m_bDynamic = true;
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 2.0f);
  plGameObject* pObject = nullptr;
  m_hSmokeObject = m_pWorld->CreateObject(desc, pObject);

  plGpuFluidGasVolumeComponent* pGas = nullptr;
  m_pWorld->GetOrCreateComponentManager<plGpuFluidGasVolumeComponentManager>()->CreateComponent(pObject, pGas);
  pGas->m_vExtents = plVec3(4.0f);
  pGas->m_fVoxelSize = s_fVoxel;
  pGas->m_uiMaxBricks = 512;
  pGas->m_DebugSource = plGpuFluidDebugSource::None;
  pGas->m_EmitterShape = plGpuFluidEmitterShape::None;
  pGas->m_OpenFaces = AllFaces();
  pGas->m_DebugPoissonTest.m_bEnabled = true;
  pGas->m_DebugPoissonTest.m_bProbeEachCycle = true;
  pGas->m_DebugPoissonTest.m_uiProbeInterval = 40;
  pGas->m_fDensityScale = 0.0f;
  pGas->ResetClock();
  return PL_SUCCESS;
}

plResult plGpuFluidGalleryApplication::SetupClosedBoxScene()
{
  PL_LOCK(m_pWorld->GetWriteMarker());
  SetupStage(0x47465034u, s_vToSun, true);

  plGameObjectDesc desc;
  desc.m_sName.Assign("ClosedBox");
  desc.m_bDynamic = true;
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 1.0f);
  plGameObject* pObject = nullptr;
  m_hSmokeObject = m_pWorld->CreateObject(desc, pObject);

  plGpuFluidGasVolumeComponent* pGas = nullptr;
  m_pWorld->GetOrCreateComponentManager<plGpuFluidGasVolumeComponentManager>()->CreateComponent(pObject, pGas);
  pGas->m_vExtents = plVec3(2.0f);
  pGas->m_fVoxelSize = s_fVoxel;
  pGas->m_uiMaxBricks = 64;
  pGas->m_DebugSource = plGpuFluidDebugSource::None;
  pGas->m_OpenFaces = NoFaces(); // every face a wall
  pGas->m_fBuoyancy = Tune("N4BUOYANCY", 6.0f);
  pGas->m_fDensityWeight = Tune("N4DENSITYWEIGHT", 0.1f);
  pGas->m_fCooling = 0.5f;
  pGas->m_fDissipation = 0.0f;
  pGas->m_fVelocityDissipation = 0.0f;
  pGas->m_fEdgeFade = 0.0f;

  pGas->m_EmitterShape = plGpuFluidEmitterShape::Sphere;
  pGas->m_fEmitterRadius = 0.25f;
  pGas->m_fEmitterSoftness = 0.1f;
  pGas->m_vEmitterOffset = plVec3(0.1f, -0.05f, -0.6f);
  pGas->m_fEmissionRate = 5.0f;
  pGas->m_vEmitterVelocity = plVec3(0.0f, 0.0f, Tune("N4VELOCITY", 1.0f));
  pGas->m_fEmitterNoise = Tune("N4NOISE", 0.5f);
  SetSmokeLook(pGas, 12.0f);
  pGas->ResetClock();
  return PL_SUCCESS;
}

plResult plGpuFluidGalleryApplication::SetupObstacleScene(bool bNoise, bool bMovingBox)
{
  PL_LOCK(m_pWorld->GetWriteMarker());
  SetupStage(0x47465039u, s_vToSun, true);

  const plVec3 vExtents(4.0f, 4.0f, 8.0f);

  plGameObjectDesc desc;
  desc.m_sName.Assign("Obstacles");
  desc.m_bDynamic = true;
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * vExtents.z);
  plGameObject* pObject = nullptr;
  m_hSmokeObject = m_pWorld->CreateObject(desc, pObject);

  plGpuFluidGasVolumeComponent* pGas = nullptr;
  m_pWorld->GetOrCreateComponentManager<plGpuFluidGasVolumeComponentManager>()->CreateComponent(pObject, pGas);
  pGas->m_vExtents = vExtents;
  pGas->m_fVoxelSize = s_fVoxel;
  pGas->m_uiMaxBricks = 1536;
  pGas->m_DebugSource = plGpuFluidDebugSource::None;
  pGas->m_OpenFaces = plGpuFluidFaces::Default;
  pGas->m_fBuoyancy = Tune("BUOYANCY", 6.0f);
  pGas->m_fDensityWeight = 0.1f;
  pGas->m_fCooling = Tune("COOLING", 0.35f);
  pGas->m_fDissipation = 0.03f;
  pGas->m_fEdgeFade = 0.9f;
  pGas->m_fVorticity = Tune("VORTICITY", 0.3f);

  // A source centred under the sphere: the scene is symmetric in x and y, so is the plume without noise.
  pGas->m_EmitterShape = plGpuFluidEmitterShape::Sphere;
  pGas->m_fEmitterRadius = 0.45f;
  pGas->m_fEmitterSoftness = 0.2f;
  pGas->m_vEmitterOffset = plVec3(0.0f, 0.0f, -0.5f * vExtents.z + 0.1f + 0.45f);
  pGas->m_fEmissionRate = Tune("RATE", 5.0f);
  pGas->m_vEmitterVelocity = plVec3(0.0f, 0.0f, Tune("VELOCITY", 2.0f));
  pGas->m_fEmitterNoise = bNoise ? Tune("NOISE", 0.75f) : 0.0f;
  pGas->m_fEmitterNoiseScale = 0.3f;
  SetSmokeLook(pGas, 12.0f);
  pGas->ResetClock();

  AddCollider("Sphere", plGpuFluidColliderShape::Sphere, plVec3(0.0f, 0.0f, s_fN9SphereHeight), plVec3::MakeZero(), s_fN9SphereRadius, plColor(0.55f, 0.36f, 0.24f), true);

  m_hMovingCollider.Invalidate();
  if (bMovingBox)
  {
    plGameObject* pBox = AddCollider("Moving Box", plGpuFluidColliderShape::Box, plVec3(-1.4f, 0.0f, 4.6f), plVec3(0.35f, 0.35f, 0.35f), 0.0f, plColor(0.25f, 0.42f, 0.62f), false);
    m_hMovingCollider = pBox->GetHandle();
  }

  return PL_SUCCESS;
}

plResult plGpuFluidGalleryApplication::SetupCorridorScene()
{
  PL_LOCK(m_pWorld->GetWriteMarker());
  // The sun shines in through the open front, low, so the smoke inside the room is lit.
  SetupStage(0x47465043u, plVec3(Tune("SUNX", 0.35f), Tune("SUNY", -0.85f), Tune("SUNZ", 0.45f)), false);

  // The room: 8 x 3 x 3 m on the floor (16 x 6 x 6 bricks), every face closed. The back wall, the end walls and the
  // ceiling are drawn; the front stays open for the camera.
  const plVec3 vRoom(8.0f, 3.0f, 3.0f);
  const plColor wall(0.62f, 0.60f, 0.57f);
  AddBox("Back Wall", plVec3(0.0f, 0.5f * vRoom.y + 0.1f, 0.5f * vRoom.z), plVec3(vRoom.x + 0.4f, 0.2f, vRoom.z), wall);
  AddBox("End Wall -X", plVec3(-0.5f * vRoom.x - 0.1f, 0.0f, 0.5f * vRoom.z), plVec3(0.2f, vRoom.y + 0.4f, vRoom.z), wall);
  AddBox("End Wall +X", plVec3(0.5f * vRoom.x + 0.1f, 0.0f, 0.5f * vRoom.z), plVec3(0.2f, vRoom.y + 0.4f, vRoom.z), wall);
  AddBox("Ceiling", plVec3(0.0f, 0.0f, vRoom.z + 0.1f), plVec3(vRoom.x + 0.4f, vRoom.y + 0.4f, 0.2f), plColor(0.70f, 0.69f, 0.66f));
  AddBox("Floor", plVec3(0.0f, 0.0f, 0.01f), plVec3(vRoom.x, vRoom.y, 0.02f), plColor(0.36f, 0.34f, 0.32f));

  plGameObjectDesc desc;
  desc.m_sName.Assign("Corridor");
  desc.m_bDynamic = true;
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * vRoom.z);
  plGameObject* pObject = nullptr;
  m_hSmokeObject = m_pWorld->CreateObject(desc, pObject);

  plGpuFluidGasVolumeComponent* pGas = nullptr;
  m_pWorld->GetOrCreateComponentManager<plGpuFluidGasVolumeComponentManager>()->CreateComponent(pObject, pGas);
  pGas->m_vExtents = vRoom;
  pGas->m_fVoxelSize = s_fVoxel;
  pGas->m_uiMaxBricks = 1536;
  pGas->m_DebugSource = plGpuFluidDebugSource::None;
  pGas->m_OpenFaces = NoFaces();
  // A hot layer that stays hot (little cooling) and stirred only gently (half the confinement), so it stratifies under
  // the ceiling instead of mixing through the room.
  pGas->m_fBuoyancy = Tune("BUOYANCY", 8.0f);
  pGas->m_fDensityWeight = 0.05f;
  pGas->m_fCooling = Tune("COOLING", 0.03f);
  pGas->m_fDissipation = Tune("DISSIPATION", 0.0f);
  pGas->m_fEdgeFade = 0.0f;
  pGas->m_fVorticity = Tune("VORTICITY", 0.15f);

  // A broad warm source near the floor at the far end of the first room.
  pGas->m_EmitterShape = plGpuFluidEmitterShape::Sphere;
  pGas->m_fEmitterRadius = Tune("RADIUS", 0.5f);
  pGas->m_fEmitterSoftness = 0.15f;
  pGas->m_vEmitterOffset = plVec3(Tune("EMITX", -3.1f), 0.1f, -0.5f * vRoom.z + 0.6f);
  pGas->m_fEmissionRate = Tune("RATE", 4.0f);
  pGas->m_vEmitterVelocity = plVec3(0.0f, 0.0f, Tune("VELOCITY", 1.2f));
  pGas->m_fEmitterNoise = Tune("NOISE", 0.6f);
  pGas->m_fEmitterNoiseScale = 0.25f;
  pGas->m_fDensityMax = 4.0f;
  SetSmokeLook(pGas, Tune("DENSITYSCALE", 5.0f));
  pGas->ResetClock();

  // The partition at x = -1, 0.25 m thick, with a 1 m wide, 1.75 m high doorway in the middle; above the doorway a
  // lintel holds the ceiling layer back until it is deeper than 1.25 m, then it spills through into the second room.
  const plColor partition(0.48f, 0.47f, 0.45f);
  const float fX = Tune("PARTITIONX", -1.0f);
  const float fHalfThickness = 0.125f;
  const float fDoorTop = Tune("DOORTOP", 1.75f);
  AddCollider("Partition -Y", plGpuFluidColliderShape::Box, plVec3(fX, -1.0f, 1.5f), plVec3(fHalfThickness, 0.5f, 1.5f), 0.0f, partition, true);
  AddCollider("Partition +Y", plGpuFluidColliderShape::Box, plVec3(fX, 1.0f, 1.5f), plVec3(fHalfThickness, 0.5f, 1.5f), 0.0f, partition, true);
  AddCollider("Lintel", plGpuFluidColliderShape::Box, plVec3(fX, 0.0f, 0.5f * (fDoorTop + vRoom.z)), plVec3(fHalfThickness, 0.5f, 0.5f * (vRoom.z - fDoorTop)), 0.0f, partition, true);

  // A cabinet in the first room the smoke has to rise past.
  AddCollider("Cabinet", plGpuFluidColliderShape::Box, plVec3(-2.0f, 0.95f, 0.6f), plVec3(0.35f, 0.35f, 0.6f), 0.0f, plColor(0.40f, 0.30f, 0.22f), true);

  m_hMovingCollider.Invalidate();
  return PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////
// Sub-tests

plResult plGameEngineTestGpuFluidGallery::InitializeP3SubTest(plInt32 iIdentifier)
{
  m_GasSamples.Clear();
  m_uiFirstRenderFrame = plRenderWorld::GetFrameCounter();
  m_PoissonRuns.Clear();
  m_uiPoissonRun = 0;
  m_PoissonCycleResiduals.Clear();

  // The solver settings this run uses, as the log's record.
  {
    plStringBuilder sSettings;
    for (const char* szName : {"GpuFluid.Gas.PressureSolver", "GpuFluid.Gas.PressureCycles", "GpuFluid.Gas.MGPreSmooth", "GpuFluid.Gas.MGPostSmooth", "GpuFluid.Gas.MGFinalSweeps",
           "GpuFluid.Gas.MGCoarseSweeps", "GpuFluid.Gas.MGOmega","GpuFluid.Gas.MGDenseGroupCells", "GpuFluid.Gas.VelocityMacCormack", "GpuFluid.Gas.Vorticity",
           "GpuFluid.Gas.VorticityRate"})
    {
      if (plCVar* pCVar = plCVar::FindCVarByName(szName))
      {
        plStringBuilder sValue;
        switch (pCVar->GetType())
        {
          case plCVarType::Bool:
            sValue = static_cast<plCVarBool*>(pCVar)->GetValue() ? "true" : "false";
            break;
          case plCVarType::Int:
            sValue.SetFormat("{}", static_cast<plCVarInt*>(pCVar)->GetValue());
            break;
          case plCVarType::Float:
            sValue.SetFormat("{}", static_cast<plCVarFloat*>(pCVar)->GetValue());
            break;
          default:
            break;
        }
        sSettings.AppendFormat(" {}={}", plStringView(szName).GetSubString(13, 64), sValue);
      }
    }
    plTestFramework::Output(plTestOutput::Message, "Gas solver settings:%s", sSettings.GetData());
  }

  // The smoke scenes' march lighting, as the Smoke Column has it.
  SetCVarForSubTest("Rendering.VDBVolumes.RayMarchShadowSteps", static_cast<plInt32>(Tune("SHADOWSTEPS", 12.0f)));
  SetCVarForSubTest("Rendering.VDBVolumes.SelfShadowLod", Tune("SHADOWLOD", 0.0f));
  SetCVarForSubTest("Rendering.VDBVolumes.AmbientOcclusion", Tune("AO", 0.0f));
  SetCVarForSubTest("Rendering.VDBVolumes.AmbientStrength", Tune("AMBIENT", 0.5f));

  switch (iIdentifier)
  {
    case SubTests::N2Divergence:
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupSmokeColumnScene());
      m_pOwnApplication->SetCamera(plVec3(-5.3f, -11.3f, 1.8f), plVec3(0.0f, 0.0f, 3.7f), 50.0f);
      return PL_SUCCESS;

    case SubTests::N3Poisson:
      SetCVarForSubTest("Rendering.Profiling.GPUStats", true);
      SetCVarForSubTest("GpuFluid.Gas.PressureSolver", 1);
      SetCVarForSubTest("GpuFluid.Gas.PressureCycles", static_cast<plInt32>(s_uiN3ProbeCycles));
      SetCVarForSubTest("GpuFluid.Gas.MGShortCycles", false); // N3 measures the full V-cycle
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupPoissonScene());
      m_pOwnApplication->SetCamera(plVec3(-5.3f, -11.3f, 1.8f), plVec3(0.0f, 0.0f, 2.0f), 50.0f);

      for (plUInt32 uiCycles : s_uiN3MultigridCycles)
      {
        PoissonRun& run = m_PoissonRuns.ExpandAndGetRef();
        run.m_bMultigrid = true;
        run.m_uiCount = uiCycles;
      }
      for (plUInt32 uiIterations : s_uiN3SORIterations)
      {
        PoissonRun& run = m_PoissonRuns.ExpandAndGetRef();
        run.m_bMultigrid = false;
        run.m_uiCount = uiIterations;
      }
      return PL_SUCCESS;

    case SubTests::N4GasMassVelocitySL:
      SetCVarForSubTest("GpuFluid.Gas.VelocityMacCormack", false);
      [[fallthrough]];
    case SubTests::N4GasMass:
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupClosedBoxScene());
      m_pOwnApplication->SetCamera(plVec3(-1.2f, -4.8f, 1.6f), plVec3(0.0f, 0.0f, 1.0f), 45.0f);
      return PL_SUCCESS;

    case SubTests::N9Colliders:
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupObstacleScene(false, false));
      m_pOwnApplication->SetCamera(plVec3(-5.3f, -11.3f, 1.8f), plVec3(0.0f, 0.0f, 3.2f), 50.0f);
      return PL_SUCCESS;

    case SubTests::CorridorSmokeVideo:
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupCorridorScene());
      // From the second room's side and a little above, so the doorway in the partition is seen at an angle.
      m_pOwnApplication->SetCamera(plVec3(Tune("CAMX", 3.4f), Tune("CAMY", -6.8f), Tune("CAMZ", 2.3f)), plVec3(Tune("TGTX", -1.1f), 0.2f, Tune("TGTZ", 1.4f)), Tune("FOV", 55.0f));
      return PL_SUCCESS;

    case SubTests::ObstaclesVideo:
    case SubTests::N9MovingCollider:
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupObstacleScene(true, true));
      m_pOwnApplication->SetCamera(plVec3(-5.3f, -11.3f, 1.8f), plVec3(0.0f, 0.0f, 3.4f), 50.0f);
      return PL_SUCCESS;

    case SubTests::GasWind:
    {
      // The Smoke Column under a 2 m/s wind along +X from the world's wind module.
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupSmokeColumnScene());
      m_pOwnApplication->SetCamera(plVec3(-5.3f, -11.3f, 1.8f), plVec3(0.0f, 0.0f, 3.7f), 50.0f);
      PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
      plSimpleWindWorldModule* pWind = m_pOwnApplication->GetWorld()->GetOrCreateModule<plSimpleWindWorldModule>();
      if (pWind == nullptr)
        return PL_FAILURE;
      pWind->SetFallbackWind(plVec3(s_fWindSpeed, 0.0f, 0.0f));
      return PL_SUCCESS;
    }
  }

  return PL_FAILURE;
}

void plGameEngineTestGpuFluidGallery::SaveVideoFrame(const char* szFolder, plInt32 iFrame, const char* szTitle)
{
  plStringBuilder sName;
  sName.SetFormat("{}/frame_{}", szFolder, plArgU(static_cast<plUInt32>(iFrame), 5, true));
  PL_TEST_BOOL(m_pOwnApplication->SaveShot(sName).Succeeded());

  if (iFrame == 0)
  {
    plStringBuilder sPath;
    sPath.SetFormat(":imgout/GpuFluidGallery/{}/title.txt", szFolder);
    plFileWriter title;
    if (title.Open(sPath).Succeeded())
    {
      plStringBuilder sTitle(szTitle, "\n");
      title.WriteBytes(sTitle.GetData(), sTitle.GetElementCount()).IgnoreResult();
    }
  }
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunN2Divergence()
{
  // Phase 0: the default V-cycles. Phase 1: 12 V-cycles, as converged as the solve gets: what is left after the
  // projection then is the half-precision floor of the stored velocity.
  if (m_iFrame == s_iN2Frames)
    SetCVarForSubTest("GpuFluid.Gas.PressureCycles", 12);

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();
  }

  if (m_iFrame + 1 < s_iN2Frames + s_iN2FloorFrames)
    return plTestAppRun::Continue;

  WriteGasCsv(m_GasSamples, ":imgout/GpuFluidGallery/N2_Divergence.csv");

  plUInt32 uiCycles = 0;
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
      uiCycles = plGpuFluidGasVolumeComponent::GetDefaultPressureCycles(pGas->m_Quality);
  }

  // The readbacks of the 12 cycle phase start a few frames after the switch.
  const plUInt64 uiFloorFrom = m_uiFirstRenderFrame + s_iN2Frames + 4;
  const plUInt64 uiDefaultUntil = m_uiFirstRenderFrame + s_iN2Frames;

  float fWorst = 0.0f;
  float fWorstBefore = 0.0f;
  float fWorstAfter = 0.0f;
  plDynamicArray<double> ratios;
  plDynamicArray<double> floors;
  plUInt32 uiNaN = 0;

  for (const GasSample& s : m_GasSamples)
  {
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
    if (s.m_fSeconds < 0.5f)
      continue;

    if (s.m_uiFrame < uiDefaultUntil)
    {
      const float fRatio = s.m_fDivAfter / plMath::Max(s.m_fDivBefore, 1e-12f);
      ratios.PushBack(fRatio);
      if (fRatio > fWorst)
      {
        fWorst = fRatio;
        fWorstBefore = s.m_fDivBefore;
        fWorstAfter = s.m_fDivAfter;
      }
    }
    else if (s.m_uiFrame >= uiFloorFrom)
    {
      floors.PushBack(s.m_fDivAfter);
    }
  }

  PL_TEST_INT(uiNaN, 0);
  if (!PL_TEST_BOOL_MSG(ratios.GetCount() > 50, "Only %u readbacks to check", ratios.GetCount()))
    return plTestAppRun::Quit;

  plTestFramework::Output(plTestOutput::Message, "N2 divergence (%u V-cycles): L_inf(div after) / L_inf(div before), worst %.5f (%.3f -> %.4f 1/s, %.0fx), median %.5f over %u readbacks; limit %.3f",
    uiCycles, fWorst, fWorstBefore, fWorstAfter, 1.0f / plMath::Max(fWorst, 1e-12f), Median(ratios), ratios.GetCount(), s_fN2MaxRatio);
  if (!floors.IsEmpty())
  {
    plDynamicArray<double> sorted = floors;
    sorted.Sort();
    plTestFramework::Output(plTestOutput::Message, "N2 floor (12 V-cycles, half-precision velocity): max |div| after the projection, median %.4f, smallest %.4f, largest %.4f 1/s over %u readbacks",
      Median(floors), sorted[0], sorted.PeekBack(), floors.GetCount());
  }

  PL_TEST_BOOL_MSG(fWorst < s_fN2MaxRatio, "N2: after %u V-cycles the largest divergence is still %.4f of the one before the projection (limit %.3f)", uiCycles, fWorst, s_fN2MaxRatio);
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunN3Poisson()
{
  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  plWorld* pWorld = m_pOwnApplication->GetWorld();
  plGpuFluidGasStats stats;
  bool bStats = false;
  {
    PL_LOCK(pWorld->GetReadMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
      bStats = pGas->GetStats(stats);
  }

  ++m_iPhaseFrame;

  // Phase 0: one probed multigrid solve per frame; its residual after every V-cycle.
  if (m_iPhase == 0)
  {
    if (m_iPhaseFrame < s_iN3ProbeFrames)
      return plTestAppRun::Continue;

    if (!PL_TEST_BOOL_MSG(bStats && stats.m_fProbes[0] > 0.0f, "N3: no probed solve read back"))
      return plTestAppRun::Quit;

    const float r0 = stats.m_fProbes[0];
    m_fPoissonInitialError = stats.m_fProbes[plGpuFluidProbeInitialError];
    plStringBuilder sFactors;
    plStringBuilder sCsv("cycle,max_residual,relative,factor\n");
    sCsv.AppendFormat("0,{},1,\n", r0);
    float fWorstFactor = 0.0f;
    plUInt32 uiChecked = 0;
    m_PoissonCycleResiduals.PushBack(r0);
    for (plUInt32 k = 1; k <= s_uiN3ProbeCycles && k < plGpuFluidProbeFinalResidual; ++k)
    {
      const float rPrev = stats.m_fProbes[k - 1];
      const float r = stats.m_fProbes[k];
      m_PoissonCycleResiduals.PushBack(r);
      const float fFactor = r / plMath::Max(rPrev, 1e-30f);
      sFactors.AppendFormat(" {}", plArgF(fFactor, 3));
      sCsv.AppendFormat("{},{},{},{}\n", k, r, r / r0, fFactor);

      // Above the float floor of the residual (about 1e-5 of the start, approached from 1e-4), every cycle after the
      // first must cut it 5x. The
      // first, from x = 0, is reported: its largest residual sits on the faces of the bricks the block red-black
      // smoother updated first, and is about 0.3 of the start.
      if (k >= 2 && rPrev > 2e-4f * r0)
      {
        fWorstFactor = plMath::Max(fWorstFactor, fFactor);
        ++uiChecked;
      }
    }

    plFileWriter csv;
    if (csv.Open(":imgout/GpuFluidGallery/N3_Poisson_Cycles.csv").Succeeded())
      csv.WriteBytes(sCsv.GetData(), sCsv.GetElementCount()).IgnoreResult();

    plTestFramework::Output(plTestOutput::Message, "N3 multigrid, residual factor per V-cycle (64 cubed, every face open):%s; worst %.3f over %u cycles above the floor (limit %.2f); after %u cycles residual %.3g of the start, error %.3g of the start (%.3g)",
      sFactors.GetData(), fWorstFactor, uiChecked, s_fN3MaxFactor, s_uiN3ProbeCycles, stats.m_fProbes[plGpuFluidProbeFinalResidual] / r0,
      stats.m_fProbes[plGpuFluidProbeFinalError] / plMath::Max(m_fPoissonInitialError, 1e-30f), stats.m_fProbes[plGpuFluidProbeFinalError]);
    PL_TEST_BOOL_MSG(uiChecked >= 3, "N3: only %u V-cycles above the floor", uiChecked);
    PL_TEST_BOOL_MSG(fWorstFactor <= s_fN3MaxFactor, "N3: a V-cycle cut the residual only by %.3f (limit %.2f)", fWorstFactor, s_fN3MaxFactor);
    PL_TEST_BOOL_MSG(stats.m_fProbes[plGpuFluidProbeFinalError] < 1e-3f * m_fPoissonInitialError, "N3: the error after %u cycles is %.3g of the start", s_uiN3ProbeCycles,
      stats.m_fProbes[plGpuFluidProbeFinalError] / plMath::Max(m_fPoissonInitialError, 1e-30f));

    // The sweep: no probes inside the solve, so Gas/Pressure times only the solver.
    PL_LOCK(pWorld->GetWriteMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
      pGas->m_DebugPoissonTest.m_bProbeEachCycle = false;

    m_iPhase = 1;
    m_iPhaseFrame = 0;
    m_uiPoissonRun = 0;
  }

  // Phase 1: one solver setting after the other.
  if (m_uiPoissonRun < m_PoissonRuns.GetCount())
  {
    PoissonRun& run = m_PoissonRuns[m_uiPoissonRun];

    if (m_iPhaseFrame == 1)
    {
      // Applies from the next update on.
      SetCVarForSubTest("GpuFluid.Gas.PressureSolver", run.m_bMultigrid ? 1 : 0);
      if (run.m_bMultigrid)
        SetCVarForSubTest("GpuFluid.Gas.PressureCycles", static_cast<plInt32>(run.m_uiCount));

      PL_LOCK(pWorld->GetWriteMarker());
      if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
        pGas->m_uiPressureIterations = run.m_bMultigrid ? 40 : run.m_uiCount;
      return plTestAppRun::Continue;
    }

    if (m_iPhaseFrame > s_iN3WarmupFrames)
      run.m_Milliseconds.PushBack(PressureMilliseconds());

    if (m_iPhaseFrame < s_iN3WarmupFrames + s_iN3MeasureFrames)
      return plTestAppRun::Continue;

    if (bStats)
    {
      run.m_fResidual = stats.m_fProbes[plGpuFluidProbeFinalResidual] / plMath::Max(stats.m_fProbes[0], 1e-30f);
      run.m_fError = stats.m_fProbes[plGpuFluidProbeFinalError] / plMath::Max(stats.m_fProbes[plGpuFluidProbeInitialError], 1e-30f);
      for (plUInt32 i = 0; i < 3; ++i)
        run.m_fWhere[i] = stats.m_fProbes[9 + i] / plMath::Max(stats.m_fProbes[0], 1e-30f);
    }

    ++m_uiPoissonRun;
    m_iPhaseFrame = 0;
    if (m_uiPoissonRun < m_PoissonRuns.GetCount())
      return plTestAppRun::Continue;

    // Then the profile: 2 V-cycles, each stage in its own pass.
    SetCVarForSubTest("GpuFluid.Gas.PressureSolver", 1);
    SetCVarForSubTest("GpuFluid.Gas.PressureCycles", 2);
    PL_LOCK(pWorld->GetWriteMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
      pGas->m_DebugPoissonTest.m_bProfileStages = true;
    m_PoissonStageNames.Clear();
    m_PoissonStageValues.Clear();
    return plTestAppRun::Continue;
  }

  // Phase 2: the stage profile.
  if (m_iPhaseFrame <= s_iN3WarmupFrames + s_iN3MeasureFrames)
  {
    if (m_iPhaseFrame > s_iN3WarmupFrames)
    {
      plDynamicArray<plString> names;
      plDynamicArray<plVariant> values;
      plStats::GetStatsWithPrefix("Renderer/GPU/Gas/MG ", names, values);
      plHybridArray<double, 8> frame;
      frame.SetCount(m_PoissonStageNames.GetCount(), 0.0);
      for (plUInt32 i = 0; i < names.GetCount(); ++i)
      {
        plStringBuilder sStage = names[i];
        sStage.TrimWordStart("Renderer/GPU/");
        if (const char* szSuffix = sStage.FindSubString(" #"))
          sStage.SetSubString_FromTo(sStage.GetData(), szSuffix);
        else if (const char* szUnit = sStage.FindSubString("["))
          sStage.SetSubString_FromTo(sStage.GetData(), szUnit);
        plUInt32 uiIndex = m_PoissonStageNames.IndexOf(sStage);
        if (uiIndex == plInvalidIndex)
        {
          uiIndex = m_PoissonStageNames.GetCount();
          m_PoissonStageNames.PushBack(sStage);
          m_PoissonStageValues.ExpandAndGetRef();
          frame.PushBack(0.0);
        }
        frame[uiIndex] += values[i].ConvertTo<double>();
      }
      for (plUInt32 i = 0; i < frame.GetCount(); ++i)
        m_PoissonStageValues[i].PushBack(frame[i]);
    }
    return plTestAppRun::Continue;
  }

  {
    plStringBuilder sProfile;
    double fTotal = 0.0;
    for (plUInt32 i = 0; i < m_PoissonStageNames.GetCount(); ++i)
    {
      const double fMedian = Median(m_PoissonStageValues[i]);
      fTotal += fMedian;
      sProfile.AppendFormat(" {} {} ms;", m_PoissonStageNames[i], plArgF(fMedian, 3));
    }
    plTestFramework::Output(plTestOutput::Message, "N3 profile, 2 V-cycles (both summed):%s total %.3f ms", sProfile.GetData(), fTotal);
  }

  // The CSV, and multigrid against SOR at about the same time.
  plStringBuilder sCsv("# GpuFluid N3 Poisson, 64 cubed (8 x 8 x 8 bricks), every face open, cold start each frame; Metal, Development build; ms = median GPU time of Gas/Pressure\n");
  sCsv.Append("solver,count,median_ms,residual_relative,error_relative\n");
  for (const PoissonRun& run : m_PoissonRuns)
  {
    sCsv.AppendFormat("{},{},{},{},{}\n", run.m_bMultigrid ? "multigrid" : "rbsor", run.m_uiCount, plArgF(Median(run.m_Milliseconds), 4), run.m_fResidual, run.m_fError);
    plTestFramework::Output(plTestOutput::Message, "N3 %s %u: %.3f ms, residual %.3g, error %.3g of the start; residual inside bricks %.3g, on brick faces %.3g, on the domain boundary %.3g",
      run.m_bMultigrid ? "multigrid V-cycles" : "red-black SOR iterations", run.m_uiCount, Median(run.m_Milliseconds), run.m_fResidual, run.m_fError, run.m_fWhere[0], run.m_fWhere[1], run.m_fWhere[2]);
  }

  for (const PoissonRun& mg : m_PoissonRuns)
  {
    if (!mg.m_bMultigrid)
      continue;

    // The SOR setting that took at least as long.
    const PoissonRun* pSor = nullptr;
    for (const PoissonRun& sor : m_PoissonRuns)
    {
      if (!sor.m_bMultigrid && Median(sor.m_Milliseconds) >= Median(mg.m_Milliseconds) && (pSor == nullptr || Median(sor.m_Milliseconds) < Median(pSor->m_Milliseconds)))
        pSor = &sor;
    }
    if (pSor != nullptr)
    {
      plTestFramework::Output(plTestOutput::Message, "N3 at equal time: %u V-cycles (%.3f ms) reach residual %.3g; %u SOR iterations (%.3f ms) reach %.3g", mg.m_uiCount, Median(mg.m_Milliseconds),
        mg.m_fResidual, pSor->m_uiCount, Median(pSor->m_Milliseconds), pSor->m_fResidual);
    }
  }

  // GPU times are only a record (another process on the GPU moves them); the check that cannot flake: 3 V-cycles beat
  // 160 SOR iterations, which take about ten times as long.
  {
    const PoissonRun* pMG3 = nullptr;
    const PoissonRun* pSOR160 = nullptr;
    for (const PoissonRun& run : m_PoissonRuns)
    {
      if (run.m_bMultigrid && run.m_uiCount == 3)
        pMG3 = &run;
      if (!run.m_bMultigrid && run.m_uiCount == 160)
        pSOR160 = &run;
    }
    if (PL_TEST_BOOL(pMG3 != nullptr && pSOR160 != nullptr))
      PL_TEST_BOOL_MSG(pMG3->m_fResidual < pSOR160->m_fResidual, "N3: 3 V-cycles (residual %.3g) are no better than 160 SOR iterations (%.3g)", pMG3->m_fResidual, pSOR160->m_fResidual);
  }

  plFileWriter csv;
  if (csv.Open(":imgout/GpuFluidGallery/N3_Poisson.csv").Succeeded())
    csv.WriteBytes(sCsv.GetData(), sCsv.GetElementCount()).IgnoreResult();

  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunN4GasMass(bool bVelocityMacCormack)
{
  const char* szName = bVelocityMacCormack ? "N4_GasMass" : "N4_GasMassVelocitySL";
  const float fTolerance = bVelocityMacCormack ? s_fN4MassTolerance : s_fN4MassToleranceVelocitySL;

  // The source stops after half a second (on the update thread, so the frame after).
  if (m_iFrame == static_cast<plInt32>(s_fN4SourceSeconds * 30.0f))
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
      pGas->m_EmitterShape = plGpuFluidEmitterShape::None;
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();
  }

  if (m_iFrame == 15 || m_iFrame == 30 || m_iFrame == 60 || m_iFrame == 100 || m_iFrame == 149)
  {
    plStringBuilder sName;
    sName.SetFormat("{}_f{}", szName, plArgU(static_cast<plUInt32>(m_iFrame), 3, true));
    PL_TEST_BOOL(m_pOwnApplication->SaveShot(sName).Succeeded());
  }

  if (m_iFrame + 1 < s_iN4Frames)
    return plTestAppRun::Continue;

  {
    plStringBuilder sPath;
    sPath.SetFormat(":imgout/GpuFluidGallery/{}.csv", szName);
    WriteGasCsv(m_GasSamples, sPath);
  }
  // Semi-Lagrangian velocity keeps the box calm: its divergence before the projection stays below 10 1/s after 0.5 s.
  CheckDivergenceRatio(m_GasSamples, bVelocityMacCormack ? "N4 closed box" : "N4 closed box, velocity semi-Lagrangian", bVelocityMacCormack);

  const GasSample* pReference = nullptr;
  for (const GasSample& s : m_GasSamples)
  {
    if (s.m_fSeconds >= s_fN4ReferenceSeconds)
    {
      pReference = &s;
      break;
    }
  }

  if (!PL_TEST_BOOL_MSG(pReference != nullptr && m_GasSamples.GetCount() > 60, "N4: too few readbacks (%u)", m_GasSamples.GetCount()))
    return plTestAppRun::Quit;

  float fWorstDrift = 0.0f;
  plUInt32 uiNaN = 0;
  float fMaxDensity = 0.0f;
  for (const GasSample& s : m_GasSamples)
  {
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
    fMaxDensity = plMath::Max(fMaxDensity, s.m_fMaxDensity);
    if (s.m_fSeconds >= pReference->m_fSeconds)
      fWorstDrift = plMath::Max(fWorstDrift, plMath::Abs(s.m_fMass - pReference->m_fMass) / plMath::Max(pReference->m_fMass, 1e-6f));
  }

  const GasSample& last = m_GasSamples.PeekBack();
  const float fEndDrift = (last.m_fMass - pReference->m_fMass) / plMath::Max(pReference->m_fMass, 1e-6f);
  plTestFramework::Output(plTestOutput::Message, "N4 gas mass (closed box, no dissipation, velocity %s, %.2f s): reference %.2f at %.2f s, end %.2f at %.2f s -> drift %+.5f; largest drift %.5f (tolerance %.4f); peak density %.3f, NaN %u",
    bVelocityMacCormack ? "MacCormack" : "semi-Lagrangian", last.m_fSeconds, pReference->m_fMass, pReference->m_fSeconds, last.m_fMass, last.m_fSeconds, fEndDrift, fWorstDrift, fTolerance, fMaxDensity, uiNaN);

  PL_TEST_INT(uiNaN, 0);
  PL_TEST_BOOL_MSG(pReference->m_fMass > 1.0f, "N4: the source added almost nothing (%.3f)", pReference->m_fMass);
  PL_TEST_BOOL_MSG(fWorstDrift <= fTolerance, "N4: the mass drifted by %.5f (tolerance %.4f)", fWorstDrift, fTolerance);
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunN9Colliders()
{
  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();
  }

  if (m_iFrame == 45 || m_iFrame == 90 || m_iFrame == 149)
  {
    plStringBuilder sName;
    sName.SetFormat("N9_Colliders_f{}", plArgU(static_cast<plUInt32>(m_iFrame), 3, true));
    PL_TEST_BOOL(m_pOwnApplication->SaveShot(sName).Succeeded());
  }

  if (m_iFrame + 1 < s_iN9Frames)
    return plTestAppRun::Continue;

  WriteGasCsv(m_GasSamples, ":imgout/GpuFluidGallery/N9_Colliders.csv");

  float fGridX = 4.0f;
  float fGridY = 4.0f;
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
    {
      fGridX = pGas->GetGridExtents().x;
      fGridY = pGas->GetGridExtents().y;
      PL_TEST_INT(pGas->GetLastSolidCount(), 1);
    }
  }

  float fDriftX = 0.0f;
  float fDriftY = 0.0f;
  float fSolidDensity = 0.0f;
  float fContact = 0.0f;
  plUInt32 uiSolidCellsMin = 0xFFFFFFFFu;
  plUInt32 uiSolidCellsMax = 0;
  plUInt32 uiNaN = 0;
  plUInt32 uiChecked = 0;
  for (const GasSample& s : m_GasSamples)
  {
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
    fSolidDensity = plMath::Max(fSolidDensity, s.m_fSolidDensity);
    fContact = plMath::Max(fContact, s.m_fSolidContact);
    uiSolidCellsMin = plMath::Min(uiSolidCellsMin, s.m_uiSolidCells);
    uiSolidCellsMax = plMath::Max(uiSolidCellsMax, s.m_uiSolidCells);
    if (s.m_fSeconds < 0.5f)
      continue;
    fDriftX = plMath::Max(fDriftX, plMath::Abs(s.m_fCentroidX - 0.5f * fGridX) / s_fVoxel);
    fDriftY = plMath::Max(fDriftY, plMath::Abs(s.m_fCentroidY - 0.5f * fGridY) / s_fVoxel);
    ++uiChecked;
  }

  // The sphere's cells: (4/3) pi r^3 / dx^3.
  const float fSphereCells = 4.0f / 3.0f * plMath::Pi<float>() * plMath::Pow(s_fN9SphereRadius / s_fVoxel, 3.0f);
  const GasSample& last = m_GasSamples.PeekBack();
  plTestFramework::Output(plTestOutput::Message,
    "N9 colliders: centroid drift from the middle, largest %.3f voxels in x, %.3f in y over %u readbacks; solid cells %u..%u (sphere %.0f); density left in the solid %.6f, largest against it %.3f; end centroid height %.2f m, mass %.1f, NaN %u",
    fDriftX, fDriftY, uiChecked, uiSolidCellsMin, uiSolidCellsMax, fSphereCells, fSolidDensity, fContact, last.m_fCentroidZ, last.m_fMass, uiNaN);

  PL_TEST_INT(uiNaN, 0);
  PL_TEST_BOOL_MSG(uiChecked > 50, "N9: only %u readbacks", uiChecked);
  PL_TEST_BOOL_MSG(fDriftX < 1.0f, "N9: the centroid drifted %.3f voxels in x (limit 1)", fDriftX);
  PL_TEST_BOOL_MSG(fDriftY < 1.0f, "N9: the centroid drifted %.3f voxels in y (limit 1)", fDriftY);
  // GasSolids zeroes the solid's cells every tick, so this only guards that clear itself; what the solid costs the gas is
  // the cleared mass below.
  PL_TEST_BOOL_MSG(fSolidDensity == 0.0f, "N9: density %.6f left inside the solid", fSolidDensity);
  CheckSolidClearing(m_GasSamples, "N9 static sphere", s_fN9MaxClearedStatic);
  CheckDivergenceRatio(m_GasSamples, "N9 static sphere");
  PL_TEST_BOOL_MSG(fContact > 0.1f, "N9: the plume never reached the sphere (largest density against it %.4f)", fContact);
  PL_TEST_BOOL_MSG(plMath::Abs(static_cast<float>(uiSolidCellsMax) - fSphereCells) < 0.05f * fSphereCells && uiSolidCellsMin == uiSolidCellsMax,
    "N9: %u..%u solid cells for a sphere of %.0f", uiSolidCellsMin, uiSolidCellsMax, fSphereCells);
  PL_TEST_BOOL_MSG(last.m_fCentroidZ > s_fN9SphereHeight, "N9: the plume did not get past the sphere (centroid at %.2f m)", last.m_fCentroidZ);
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunGasWind()
{
  // The world's wind becomes the gas's ambient velocity: it blows in through the open -X face and carries the plume
  // downwind.
  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  plVec3 vWind = plVec3::MakeZero();
  float fSourceX = 0.0f;
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
    {
      vWind = pGas->GetLastAmbientWind();
      fSourceX = 0.5f * pGas->GetGridExtents().x + pGas->m_vEmitterOffset.x;
    }
  }

  if (m_iFrame == 60 || m_iFrame + 1 == s_iWindFrames)
  {
    plStringBuilder sName;
    sName.SetFormat("GasWind_f{}", plArgU(static_cast<plUInt32>(m_iFrame), 3, true));
    PL_TEST_BOOL(m_pOwnApplication->SaveShot(sName).Succeeded());
  }

  if (m_iFrame + 1 < s_iWindFrames)
    return plTestAppRun::Continue;

  WriteGasCsv(m_GasSamples, ":imgout/GpuFluidGallery/GasWind.csv");
  if (!PL_TEST_BOOL_MSG(m_GasSamples.GetCount() > 30, "Only %u readbacks", m_GasSamples.GetCount()))
    return plTestAppRun::Quit;

  plUInt32 uiNaN = 0;
  for (const GasSample& s : m_GasSamples)
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
  const GasSample& last = m_GasSamples.PeekBack();
  const float fDrift = last.m_fCentroidX - fSourceX;
  plTestFramework::Output(plTestOutput::Message, "Gas wind: ambient (%.2f, %.2f, %.2f) m/s; after %.2f s the centroid is %.2f m downwind of the source (height %.2f m), NaN %u", vWind.x, vWind.y,
    vWind.z, last.m_fSeconds, fDrift, last.m_fCentroidZ, uiNaN);

  // The plume bends over: its gas leans downwind and stays low (without wind the Smoke Column's centroid is near 3 m by
  // then), and what reaches the +X face fades out there, which keeps the downwind shift of the centroid small.
  PL_TEST_INT(uiNaN, 0);
  PL_TEST_FLOAT(vWind.x, s_fWindSpeed, 1e-3f);
  PL_TEST_BOOL_MSG(fDrift > 0.1f, "The wind carried the plume only %.2f m downwind", fDrift);
  PL_TEST_BOOL_MSG(last.m_fCentroidZ < 2.0f, "The plume still rose straight (centroid at %.2f m)", last.m_fCentroidZ);
  // The wind's inflow faces carry a velocity the solve has to take in on every tick.
  CheckDivergenceRatio(m_GasSamples, "Gas wind");
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunCorridorSmokeVideo()
{
  if (!VideoEnabled())
  {
    plTestFramework::Output(plTestOutput::Details, "Skipped: set PL_GPUFLUID_VIDEO=1 to record the frames.");
    return plTestAppRun::Quit;
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();
  }

  // The first frame is the fresh-resource frame, black for the volume: not recorded.
  const plInt32 iVideoFrame = m_iFrame - 1;
  if (iVideoFrame >= 0)
    SaveVideoFrame("CorridorSmoke", iVideoFrame, "P3 Corridor Smoke: closed room, collider partition; the ceiling layer spills through the doorway");

  if (iVideoFrame + 1 < static_cast<plInt32>(Tune("FRAMES", static_cast<float>(s_iCorridorFrames))))
    return plTestAppRun::Continue;

  WriteGasCsv(m_GasSamples, ":imgout/GpuFluidGallery/CorridorSmoke.csv");
  plUInt32 uiNaN = 0;
  float fSolidDensity = 0.0f;
  for (const GasSample& s : m_GasSamples)
  {
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
    fSolidDensity = plMath::Max(fSolidDensity, s.m_fSolidDensity);
  }
  const GasSample& last = m_GasSamples.PeekBack();
  plTestFramework::Output(plTestOutput::Message, "Corridor video: %d frames, %u readbacks, NaN %u, density left in solids %.6f; end mass %.1f, centroid x %.2f m z %.2f m, %u solid cells",
    s_iCorridorFrames, m_GasSamples.GetCount(), uiNaN, fSolidDensity, last.m_fMass, last.m_fCentroidX, last.m_fCentroidZ, last.m_uiSolidCells);
  PL_TEST_INT(uiNaN, 0);
  PL_TEST_BOOL(fSolidDensity == 0.0f);
  CheckSolidClearing(m_GasSamples, "Corridor video", s_fN9MaxClearedStatic);
  CheckDivergenceRatio(m_GasSamples, "Corridor video");
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunObstaclesVideo(bool bVideo)
{
  if (bVideo && !VideoEnabled())
  {
    plTestFramework::Output(plTestOutput::Details, "Skipped: set PL_GPUFLUID_VIDEO=1 to record the frames.");
    return plTestAppRun::Quit;
  }

  // The box sweeps across the plume and back once over the clip, above the sphere.
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
    if (plGameObject* pBox = m_pOwnApplication->GetMovingCollider())
    {
      const float t = static_cast<float>(plMath::Max(m_iFrame, 0)) / static_cast<float>(s_iObstacleFrames);
      const float x = -1.4f + 2.8f * (0.5f - 0.5f * plMath::Cos(plAngle::MakeFromRadian(2.0f * plMath::Pi<float>() * t)));
      pBox->SetLocalPosition(plVec3(x, 0.0f, 4.6f));
      pBox->SetLocalRotation(plQuat::MakeFromAxisAndAngle(plVec3(0.0f, 0.0f, 1.0f), plAngle::MakeFromDegree(90.0f * t)));
    }

    const float t = static_cast<float>(plMath::Max(m_iFrame - 1, 0)) / static_cast<float>(s_iObstacleFrames - 1);
    const plAngle angle = plAngle::MakeFromDegree(-115.0f + 40.0f * t);
    m_pOwnApplication->SetCamera(plVec3(0.0f, 0.0f, 3.3f) + plVec3(10.0f * plMath::Cos(angle), 10.0f * plMath::Sin(angle), -1.5f + 0.6f * t), plVec3(0.0f, 0.0f, 3.3f), 50.0f);
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();
  }

  const plInt32 iVideoFrame = m_iFrame - 1;
  if (iVideoFrame >= 0 && bVideo)
    SaveVideoFrame("SmokeObstacles", iVideoFrame, "P3 Smoke Around Obstacles: static sphere and a moving, turning box collider");

  if (iVideoFrame + 1 < s_iObstacleFrames)
    return plTestAppRun::Continue;

  WriteGasCsv(m_GasSamples, bVideo ? ":imgout/GpuFluidGallery/SmokeObstacles.csv" : ":imgout/GpuFluidGallery/N9_MovingCollider.csv");
  plUInt32 uiNaN = 0;
  float fSolidDensity = 0.0f;
  for (const GasSample& s : m_GasSamples)
  {
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
    fSolidDensity = plMath::Max(fSolidDensity, s.m_fSolidDensity);
  }
  const char* szScene = bVideo ? "Obstacles video" : "N9 moving collider";
  plTestFramework::Output(plTestOutput::Message, "%s: %d frames, %u readbacks, NaN %u, density left in solids %.6f", szScene, s_iObstacleFrames, m_GasSamples.GetCount(), uiNaN, fSolidDensity);
  PL_TEST_INT(uiNaN, 0);
  PL_TEST_BOOL(fSolidDensity == 0.0f);
  CheckSolidClearing(m_GasSamples, szScene, s_fN9MaxClearedMoving);
  CheckDivergenceRatio(m_GasSamples, szScene);
  return plTestAppRun::Quit;
}
