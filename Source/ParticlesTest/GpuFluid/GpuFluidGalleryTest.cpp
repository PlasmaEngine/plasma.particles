#include <ParticlesTest/ParticlesTestPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/Types/TagRegistry.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <Foundation/Utilities/Stats.h>
#include <GameEngine/Effects/Wind/SimpleWindWorldModule.h>
#include <GameEngine/GameState/GameState.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasVolumeComponent.h>
#include <ParticlesTest/GpuFluid/GpuFluidGalleryTest.h>
#include <RendererCore/Lights/DirectionalLightComponent.h>
#include <RendererCore/Lights/SkyAtmosphereComponent.h>
#include <RendererCore/Lights/SkyLightComponent.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Volumes/VDBVolumeComponent.h>
#include <Texture/Image/ImageConversion.h>

static plGameEngineTestGpuFluidGallery s_GameEngineTestGpuFluidGallery;

// Size of the gallery's window, and so of every shot and video frame.
#define PL_GPUFLUID_GALLERY_WIDTH 1280
#define PL_GPUFLUID_GALLERY_HEIGHT 720

namespace
{
  const plVec3 s_vUp(0.0f, 0.0f, 1.0f);

  // Smoke Column: a 4 x 4 x 8 m box at 6.25 cm voxels (8 x 8 x 16 = 1024 bricks, inside Medium's 1536), standing on the
  // floor, 300 ticks (150 frames at the harness's 1/30 s, two ticks each).
  const plVec3 s_vSmokeExtents(4.0f, 4.0f, 8.0f);
  constexpr float s_fSmokeVoxel = 0.0625f;
  constexpr plUInt32 s_uiSmokeBricks = 8 * 8 * 16;
  constexpr plInt32 s_iSmokeFrames = 150;
  constexpr plInt32 s_iSmokeVideoFrames = 300;

  // Gas Safety (N12 stress): the source drives its faces at 48 m/s, above the 32 m/s clamp, for 90 frames; a NaN is
  // injected on frames 45 to 47.
  constexpr plInt32 s_iSafetyFrames = 90;
  constexpr plInt32 s_iSafetyInjectFrame = 45;
  constexpr float s_fSafetySourceSpeed = 48.0f;
  const plVec3 s_vSmokeTarget(0.0f, 0.0f, 3.7f);
  constexpr float s_fSmokeFovY = 50.0f;

  /// The camera's slow arc around the column: 50 degrees over the clip, from the front-left to the front-right.
  plVec3 SmokeOrbit(float t)
  {
    const plAngle angle = plAngle::MakeFromDegree(-115.0f + 50.0f * t);
    return s_vSmokeTarget + plVec3(12.5f * plMath::Cos(angle), 12.5f * plMath::Sin(angle), -1.9f + 0.8f * t);
  }

  /// Sun for the smoke scenes: low and from the side, so the column's lit and shadowed halves read.
  const plVec3 s_vSmokeToSun(-0.85f, -0.25f, 0.42f);

  /// A look value of the smoke scenes, overridable for tuning runs as PL_GPUFLUID_TUNE_<NAME>=<value>.
  float Tune(const char* szName, float fDefault)
  {
    plStringBuilder sVariable("PL_GPUFLUID_TUNE_", szName);
    const plString sValue = plEnvironmentVariableUtils::GetValueString(sVariable, "");
    double fValue = 0.0;
    if (!sValue.IsEmpty() && plConversionUtils::StringToFloat(sValue, fValue).Succeeded())
      return static_cast<float>(fValue);
    return fDefault;
  }

  /// Height of the spheres' centre: the 4 m boxes stand 0.2 m above the ground.
  constexpr float s_fSphereHeight = 2.2f;

  constexpr plInt32 s_iFirstSettleFrames = 120;
  constexpr plInt32 s_iSettleFrames = 60;
  constexpr plUInt32 s_uiVideoFrames = 150;

  // N0 thresholds. A pixel's difference is |a - b| summed over RGB, 0..765. Everything is measured inside the volume's
  // footprint (pixels the GPU or the baked sphere changes by more than s_uiFootprintThreshold against the empty
  // scene), against two references taken in the same run:
  // - signal: how much the volume changes its footprint at all (GPU alone vs empty);
  // - floor: what two captures of the same GPU sphere, frames apart, differ by (the march's jitter after TAA, dither).
  // The hand-off may differ from the bake by the floor plus 1% of the signal: an 8 bit bake quantises density and
  // temperature to 1/255 and brick means to 1/255 (floored), the GPU keeps them in half precision.
  constexpr plUInt32 s_uiFootprintThreshold = 12;
  constexpr float s_fHandoffMinFootprint = 0.02f;        // the volume must cover at least this much of the image
  constexpr float s_fHandoffMaxExcessOfSignal = 0.01f;   // (handoff - floor) / signal, mean over the footprint
  constexpr plUInt32 s_uiHandoffMaxPixel = 32;           // largest single-pixel difference inside the footprint

  struct ImageDiff
  {
    float m_fMean = 0.0f;    ///< mean pixel difference / 765 over the counted pixels
    float m_fChanged = 0.0f; ///< fraction of the counted pixels above s_uiFootprintThreshold
    plUInt32 m_uiMax = 0;    ///< largest pixel difference, 0..765
    plUInt32 m_uiCounted = 0;
  };

  bool ToRGBA8(const plImage& image, plImage& out_image)
  {
    return plImageConversion::Convert(image, out_image, plImageFormat::R8G8B8A8_UNORM).Succeeded() && out_image.GetWidth() > 0;
  }

  plUInt32 PixelDifference(const plImage& a, const plImage& b, plUInt32 x, plUInt32 y)
  {
    const plUInt8* pa = a.GetPixelPointer<plUInt8>(0, 0, 0, x, y);
    const plUInt8* pb = b.GetPixelPointer<plUInt8>(0, 0, 0, x, y);
    plUInt32 uiPixel = 0;
    for (plUInt32 c = 0; c < 3; ++c)
      uiPixel += static_cast<plUInt32>(plMath::Abs(static_cast<plInt32>(pa[c]) - static_cast<plInt32>(pb[c])));
    return uiPixel;
  }

  /// Difference of two RGBA8 images of equal size, over the pixels the mask selects (all of them without a mask).
  ImageDiff CompareImages(const plImage& a, const plImage& b, const plDynamicArray<bool>* pMask = nullptr)
  {
    ImageDiff result;
    plUInt64 uiSum = 0;
    plUInt64 uiChanged = 0;

    for (plUInt32 y = 0; y < a.GetHeight(); ++y)
    {
      for (plUInt32 x = 0; x < a.GetWidth(); ++x)
      {
        if (pMask != nullptr && !(*pMask)[y * a.GetWidth() + x])
          continue;

        const plUInt32 uiPixel = PixelDifference(a, b, x, y);
        uiSum += uiPixel;
        uiChanged += uiPixel > s_uiFootprintThreshold ? 1 : 0;
        result.m_uiMax = plMath::Max(result.m_uiMax, uiPixel);
        ++result.m_uiCounted;
      }
    }

    if (result.m_uiCounted > 0)
    {
      result.m_fMean = static_cast<float>(static_cast<double>(uiSum) / (static_cast<double>(result.m_uiCounted) * 765.0));
      result.m_fChanged = static_cast<float>(static_cast<double>(uiChanged) / static_cast<double>(result.m_uiCounted));
    }
    return result;
  }

  void SetCVarBool(const char* szName, bool bValue)
  {
    if (auto* pCVar = static_cast<plCVarBool*>(plCVar::FindCVarByName(szName)))
      *pCVar = bValue;
    else
      PL_TEST_FAILURE("CVar not found", "%s", szName);
  }

  bool GetCVarBool(const char* szName, bool bDefault)
  {
    if (auto* pCVar = static_cast<plCVarBool*>(plCVar::FindCVarByName(szName)))
      return pCVar->GetValue();
    return bDefault;
  }

  /// Switched off by the gallery scenes and restored after every sub-test: the CVar is process wide, and the image tests
  /// that run after this one in the same process were captured with depth of field on.
  constexpr const char* s_szDepthOfFieldCVar = "Rendering.DepthOfField.Enable";

  /// The sphere of GasAnalyticSphere.plShader, at the centre of a voxel of the virtual grid. Density in x, temperature in y.
  plVec2 EvaluateSphere(const plVec3& vVoxelCenter, const plVec3& vCenter, float fRadius, float fSoftness, float fPeakDensity, float fPeakTemperature)
  {
    const float fDistance = (vVoxelCenter - vCenter).GetLength();
    const float x = plMath::Saturate((fRadius - fDistance) / plMath::Max(fSoftness, 1e-3f));
    const float fDensity = fPeakDensity * x * x * (3.0f - 2.0f * x);
    const float fCore = plMath::Saturate(1.0f - fDistance / plMath::Max(fRadius, 1e-3f));
    return plVec2(fDensity, fPeakTemperature * fCore * x);
  }

  plUInt8 ToUNorm8(float f)
  {
    return static_cast<plUInt8>(plMath::Clamp(f * 255.0f + 0.5f, 0.0f, 255.0f));
  }
} // namespace

//////////////////////////////////////////////////////////////////////////

static plString s_sPackageAssetsDirectory;

plGpuFluidGalleryApplication::plGpuFluidGalleryApplication()
  : plGameEngineTestApplication("FluidGallery")
{
}

void plGpuFluidGalleryApplication::SetPackageAssetsDirectory(plStringView sDirectory)
{
  s_sPackageAssetsDirectory = sDirectory;
}

void plGpuFluidGalleryApplication::Init_AddPluginSearchRoots()
{
  plGameEngineTestApplication::Init_AddPluginSearchRoots();

#if defined(PL_PARTICLESTEST_PLUGIN_DIR)
#  define PL_GPUFLUIDTEST_STRINGIZE_IMPL(x) #x
#  define PL_GPUFLUIDTEST_STRINGIZE(x) PL_GPUFLUIDTEST_STRINGIZE_IMPL(x)
  plPlugin::AddPluginSearchRoot(PL_GPUFLUIDTEST_STRINGIZE(PL_PARTICLESTEST_PLUGIN_DIR));
#endif
}

void plGpuFluidGalleryApplication::Init_FileSystem_ConfigureDataDirs()
{
  plGameEngineTestApplication::Init_FileSystem_ConfigureDataDirs();

  // The GpuFluid shaders live in the package (Assets/Shaders/GpuFluid), mounted the way an installed package is.
  if (!s_sPackageAssetsDirectory.IsEmpty())
  {
    plFileSystem::AddDataDirectory(s_sPackageAssetsDirectory, "GpuFluidTest", "plasma.particles", plDataDirUsage::ReadOnly).AssertSuccess();
  }

  // No pipeline prewarm manifest: it lives in the shader cache per project, and other test applications of the
  // FluidGallery project (the liquid tests) record pipelines of shaders this one does not mount, which then fail to
  // load here. Shaders load synchronously in tests anyway.
  if (auto* pManifest = static_cast<plCVarBool*>(plCVar::FindCVarByName("Rendering.Pipelines.Manifest")))
    *pManifest = false;

  // 1280 x 720 instead of the project's 960 x 540: the per-run appdata folder (set up just before) is searched first
  // for the window config, so this changes only this test application, not the shared FluidGallery project.
  plFileWriter window;
  if (window.Open(":appdata/RuntimeConfigs/Window.ddl").Succeeded())
  {
    const char* szWindow = "WindowDesc\n{\n\tstring %Title{\"GameEngineTest - GpuFluid Gallery\"}\n\tstring %Mode{\"Window\"}\n\tVec2u %Resolution{uint32{" PL_PP_STRINGIFY(PL_GPUFLUID_GALLERY_WIDTH) "," PL_PP_STRINGIFY(
      PL_GPUFLUID_GALLERY_HEIGHT) "}}\n\tbool %ClipMouseCursor{false}\n\tbool %ShowMouseCursor{true}\n\tbool %SetForegroundOnInit{true}\n\tbool %CenterWindowOnDisplay{true}\n}\n";
    window.WriteBytes(szWindow, plStringUtils::GetStringElementCount(szWindow)).IgnoreResult();
  }
}

void plGpuFluidGalleryApplication::BeforeHighLevelSystemsShutdown()
{
  m_hBoxMesh.Invalidate();
  m_hSphereMesh.Invalidate();
  m_hBakedSphere.Invalidate();
  plGameEngineTestApplication::BeforeHighLevelSystemsShutdown();
}

plGameObject* plGpuFluidGalleryApplication::AddBox(const char* szName, const plVec3& vCenter, const plVec3& vSize, const plColor& color)
{
  if (!m_hBoxMesh.IsValid())
  {
    plGeometry geometry;
    geometry.AddBox(plVec3(1.0f), true);
    geometry.ComputeTangents();

    plMeshResourceDescriptor descriptor;
    plMeshBufferResourceDescriptor& meshBuffer = descriptor.MeshBufferDesc();
    meshBuffer.AddCommonStreams();
    meshBuffer.AllocateStreamsFromGeometry(geometry, plGALPrimitiveTopology::Triangles);
    descriptor.AddSubMesh(meshBuffer.GetPrimitiveCount(), 0, 0);
    descriptor.SetMaterial(0, "{ 7c71fc2d-2a1a-4938-9305-f3a0f26d73cc }"); // LitStandard
    descriptor.ComputeBounds();
    m_hBoxMesh = plResourceManager::GetOrCreateResource<plMeshResource>("GameEngineTest/GpuFluidGallery/Box", std::move(descriptor), "GpuFluid gallery box");
  }

  plGameObjectDesc desc;
  desc.m_sName.Assign(szName);
  desc.m_LocalPosition = vCenter;
  desc.m_LocalScaling = vSize;
  desc.m_Tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow"));
  plGameObject* pObject = nullptr;
  m_pWorld->CreateObject(desc, pObject);

  plMeshComponent* pMesh = nullptr;
  m_pWorld->GetOrCreateComponentManager<plMeshComponentManager>()->CreateComponent(pObject, pMesh);
  pMesh->SetMesh(m_hBoxMesh);
  pMesh->SetColor(color);
  return pObject;
}

void plGpuFluidGalleryApplication::AddSunAndSky(const plVec3& vToSun, float fLux)
{
  plGameObjectDesc sunDesc;
  sunDesc.m_sName.Assign("Sun");
  sunDesc.m_LocalRotation = plQuat::MakeShortestRotation(plVec3(-1.0f, 0.0f, 0.0f), vToSun.GetNormalized());
  plGameObject* pSunObject = nullptr;
  m_pWorld->CreateObject(sunDesc, pSunObject);
  plDirectionalLightComponent* pSun = nullptr;
  plDirectionalLightComponent::CreateComponent(pSunObject, pSun);
  pSun->SetIntensity(fLux);
  pSun->SetCastShadows(true);

  plGameObjectDesc skyDesc;
  skyDesc.m_sName.Assign("Sky");
  plGameObject* pSkyObject = nullptr;
  m_pWorld->CreateObject(skyDesc, pSkyObject);
  plSkyAtmosphereComponent* pAtmosphere = nullptr;
  plSkyAtmosphereComponent::CreateComponent(pSkyObject, pAtmosphere);
  plSkyLightComponent* pSkyLight = nullptr;
  plSkyLightComponent::CreateComponent(pSkyObject, pSkyLight);
}

void plGpuFluidGalleryApplication::SetupStage(plUInt32 uiSeed, const plVec3& vToSun, bool bProps)
{
  m_pWorld->Clear();
  m_pWorld->GetRandomNumberGenerator().Initialize(uiSeed);
  m_pWorld->GetClock().Reset(false);

  // World modules outlive Clear(): a scene with wind (Gas Wind) must not blow on the next one.
  if (plSimpleWindWorldModule* pWind = m_pWorld->GetModule<plSimpleWindWorldModule>())
    pWind->SetFallbackWind(plVec3::MakeZero());

  // The camera's default lens is wide open, which blurs everything but the focus point.
  SetCVarBool(s_szDepthOfFieldCVar, false);

  AddSunAndSky(vToSun, 10.0f);

  // A mid grey floor, and a 1 m checker of a darker grey around the origin: something for the eye to measure size and
  // motion against, and an edge for every shadow to fall across.
  const plColor floorLight(0.46f, 0.46f, 0.47f);
  const plColor floorDark(0.27f, 0.27f, 0.28f);
  AddBox("Ground", plVec3(0.0f, 0.0f, -0.1f), plVec3(2000.0f, 2000.0f, 0.2f), floorLight);

  if (!bProps)
    return;

  constexpr plInt32 iCheckerHalf = 10;
  for (plInt32 y = -iCheckerHalf; y < iCheckerHalf; ++y)
  {
    for (plInt32 x = -iCheckerHalf; x < iCheckerHalf; ++x)
    {
      if (((x + y) & 1) == 0)
        continue;
      AddBox("Tile", plVec3(x + 0.5f, y + 0.5f, 0.0025f), plVec3(1.0f, 1.0f, 0.005f), floorDark);
    }
  }

  // Props for depth, all clear of the 4 x 4 m footprint the volumes stand on.
  AddBox("Crate", plVec3(3.6f, 1.8f, 0.5f), plVec3(1.0f), plColor(0.62f, 0.60f, 0.56f));
  AddBox("Crate Small", plVec3(3.4f, 3.1f, 0.3f), plVec3(0.6f), plColor(0.55f, 0.52f, 0.47f));
  AddBox("Pillar", plVec3(-3.8f, 2.6f, 2.0f), plVec3(0.6f, 0.6f, 4.0f), plColor(0.70f, 0.70f, 0.72f));
  AddBox("Slab", plVec3(-3.2f, -3.4f, 0.2f), plVec3(2.0f, 1.0f, 0.4f), plColor(0.50f, 0.50f, 0.52f));
  AddBox("Wall", plVec3(0.0f, 7.5f, 1.5f), plVec3(12.0f, 0.4f, 3.0f), plColor(0.58f, 0.57f, 0.55f));
}

plResult plGpuFluidGalleryApplication::SetupSmokeColumnScene()
{
  PL_LOCK(m_pWorld->GetWriteMarker());
  SetupStage(0x47465032u, s_vSmokeToSun, true);

  plGameObjectDesc desc;
  desc.m_sName.Assign("SmokeColumn");
  desc.m_bDynamic = true;
  desc.m_LocalPosition = plVec3(0.0f, 0.0f, 0.5f * s_vSmokeExtents.z);
  plGameObject* pObject = nullptr;
  m_hSmokeObject = m_pWorld->CreateObject(desc, pObject);

  plGpuFluidGasVolumeComponent* pGas = nullptr;
  m_pWorld->GetOrCreateComponentManager<plGpuFluidGasVolumeComponentManager>()->CreateComponent(pObject, pGas);
  pGas->m_vExtents = s_vSmokeExtents;
  pGas->m_fVoxelSize = s_fSmokeVoxel;
  pGas->m_uiMaxBricks = 1536;
  pGas->m_DebugSource = plGpuFluidDebugSource::None;
  pGas->m_OpenFaces = plGpuFluidFaces::Default; // open but the floor

  // Warm smoke from a sphere just above the floor, a little off the box's centre so the plume does not start
  // symmetric. The values below were tuned by eye on the stills and the video; every one can be overridden for a
  // tuning run as PL_GPUFLUID_TUNE_<NAME>=<value> without rebuilding.
  pGas->m_fBuoyancy = Tune("BUOYANCY", 6.0f);
  pGas->m_fDensityWeight = Tune("DENSITYWEIGHT", 0.1f);
  pGas->m_fCooling = Tune("COOLING", 0.5f);
  pGas->m_fDissipation = Tune("DISSIPATION", 0.03f);
  pGas->m_fVelocityDissipation = Tune("VELOCITYDISSIPATION", 0.0f);
  pGas->m_uiPressureIterations = static_cast<plUInt32>(Tune("ITERATIONS", 40.0f));
  pGas->m_fEdgeFade = Tune("EDGEFADE", 0.9f);

  pGas->m_EmitterShape = plGpuFluidEmitterShape::Sphere;
  pGas->m_fEmitterRadius = Tune("RADIUS", 0.5f);
  pGas->m_fEmitterSoftness = Tune("SOFTNESS", 0.2f);
  pGas->m_vEmitterOffset = plVec3(Tune("EMITX", 0.37f), Tune("EMITY", 0.21f), -0.5f * s_vSmokeExtents.z + 0.1f + pGas->m_fEmitterRadius);
  pGas->m_fEmissionRate = Tune("RATE", 5.0f);
  pGas->m_fEmitterTemperature = 1.0f;
  pGas->m_vEmitterVelocity = plVec3(0.0f, 0.0f, Tune("VELOCITY", 2.0f));
  pGas->m_fEmitterNoise = Tune("NOISE", 0.75f);
  pGas->m_fEmitterNoiseScale = Tune("NOISESCALE", 0.3f);

  pGas->m_fDensityScale = Tune("DENSITYSCALE", 12.0f);
  const float fAlbedo = Tune("ALBEDO", 0.95f);
  pGas->m_Albedo = plColor(fAlbedo, fAlbedo, fAlbedo * 1.02f);
  pGas->m_fAnisotropy = Tune("ANISOTROPY", 0.4f);
  pGas->m_fEmissionScale = 0.0f;
  pGas->m_RenderMode = plVDBVolumeRenderMode::RayMarch;
  pGas->ResetClock();

  return PL_SUCCESS;
}

plGpuFluidGasVolumeComponent* plGpuFluidGalleryApplication::GetSmokeVolume()
{
  plGameObject* pObject = nullptr;
  plGpuFluidGasVolumeComponent* pGas = nullptr;
  if (m_pWorld->TryGetObject(m_hSmokeObject, pObject))
    pObject->TryGetComponentOfBaseType(pGas);
  return pGas;
}

plVDBVolumeResourceHandle plGpuFluidGalleryApplication::CreateBakedSphere()
{
  // The dense grid the importer would rasterise the source into, RGBA8: density in R, temperature in G.
  const plVec3U32 vGrid = m_Shape.m_vBrickGrid;
  const plVec3U32 vDim = vGrid * 8u;
  const plVec3 vCenter = plVec3(static_cast<float>(vDim.x), static_cast<float>(vDim.y), static_cast<float>(vDim.z)) * 0.5f;
  const float fRadius = m_Shape.m_fRadius / m_Shape.m_fVoxelSize;
  const float fSoftness = m_Shape.m_fSoftness / m_Shape.m_fVoxelSize;

  plDynamicArray<plUInt8> dense;
  dense.SetCount(vDim.x * vDim.y * vDim.z * 4);
  for (plUInt32 z = 0; z < vDim.z; ++z)
  {
    for (plUInt32 y = 0; y < vDim.y; ++y)
    {
      for (plUInt32 x = 0; x < vDim.x; ++x)
      {
        const plVec2 v = EvaluateSphere(plVec3(x + 0.5f, y + 0.5f, z + 0.5f), vCenter, fRadius, fSoftness, m_Shape.m_fPeakDensity, m_Shape.m_fPeakTemperature);
        plUInt8* pTexel = &dense[((z * vDim.y + y) * vDim.x + x) * 4];
        pTexel[0] = ToUNorm8(v.x);
        pTexel[1] = ToUNorm8(v.y);
        pTexel[2] = 0;
        pTexel[3] = 0;
      }
    }
  }

  auto DenseAt = [&](plInt32 x, plInt32 y, plInt32 z) -> const plUInt8*
  {
    x = plMath::Clamp<plInt32>(x, 0, vDim.x - 1);
    y = plMath::Clamp<plInt32>(y, 0, vDim.y - 1);
    z = plMath::Clamp<plInt32>(z, 0, vDim.z - 1);
    return &dense[((z * vDim.y + y) * vDim.x + x) * 4];
  };

  // From here on what VDBVolumeAsset.cpp's BuildBrickAtlas does, with its default threshold of 0: bricks whose 8 bit
  // peak is above it are kept, their mean is floored at 1/255, and they are packed into a roughly cubic atlas.
  const plUInt32 uiBrickCount = vGrid.x * vGrid.y * vGrid.z;
  plDynamicArray<plUInt32> brickSlot;
  plDynamicArray<plUInt8> brickMean;
  brickSlot.SetCount(uiBrickCount, plInvalidIndex);
  brickMean.SetCount(uiBrickCount, 0);

  plUInt32 uiOccupied = 0;
  for (plUInt32 bz = 0; bz < vGrid.z; ++bz)
  {
    for (plUInt32 by = 0; by < vGrid.y; ++by)
    {
      for (plUInt32 bx = 0; bx < vGrid.x; ++bx)
      {
        plUInt32 uiPeak = 0;
        plUInt32 uiSum = 0;
        for (plUInt32 z = 0; z < 8; ++z)
          for (plUInt32 y = 0; y < 8; ++y)
            for (plUInt32 x = 0; x < 8; ++x)
            {
              const plUInt32 uiDensity = DenseAt(bx * 8 + x, by * 8 + y, bz * 8 + z)[0];
              uiPeak = plMath::Max(uiPeak, uiDensity);
              uiSum += uiDensity;
            }

        if (uiPeak > 0)
        {
          const plUInt32 uiIndex = (bz * vGrid.y + by) * vGrid.x + bx;
          brickSlot[uiIndex] = uiOccupied++;
          brickMean[uiIndex] = static_cast<plUInt8>(plMath::Max(1u, uiSum / 512u));
        }
      }
    }
  }

  m_uiBakedBricks = uiOccupied;

  plUInt32 uiSide = 1;
  while (uiSide * uiSide * uiSide < uiOccupied)
    ++uiSide;
  const plVec3U32 vAtlasBricks(uiSide, uiSide, plMath::Max(1u, (uiOccupied + uiSide * uiSide - 1) / (uiSide * uiSide)));
  const plVec3U32 vAtlasDim = vAtlasBricks * 10u;

  plDynamicArray<plUInt8> atlas;
  atlas.SetCount(vAtlasDim.x * vAtlasDim.y * vAtlasDim.z * 4, 0);
  plDynamicArray<plUInt8> table;
  table.SetCount(uiBrickCount * 4, 0);

  for (plUInt32 bz = 0; bz < vGrid.z; ++bz)
  {
    for (plUInt32 by = 0; by < vGrid.y; ++by)
    {
      for (plUInt32 bx = 0; bx < vGrid.x; ++bx)
      {
        const plUInt32 uiIndex = (bz * vGrid.y + by) * vGrid.x + bx;
        const plUInt32 uiSlot = brickSlot[uiIndex];
        if (uiSlot == plInvalidIndex)
          continue;

        const plVec3U32 vAtlasBrick(uiSlot % vAtlasBricks.x, (uiSlot / vAtlasBricks.x) % vAtlasBricks.y, uiSlot / (vAtlasBricks.x * vAtlasBricks.y));
        plUInt8* pTable = &table[uiIndex * 4];
        pTable[0] = static_cast<plUInt8>(vAtlasBrick.x);
        pTable[1] = static_cast<plUInt8>(vAtlasBrick.y);
        pTable[2] = static_cast<plUInt8>(vAtlasBrick.z);
        pTable[3] = brickMean[uiIndex];

        for (plUInt32 z = 0; z < 10; ++z)
          for (plUInt32 y = 0; y < 10; ++y)
            for (plUInt32 x = 0; x < 10; ++x)
            {
              // The apron reaches one voxel outside the brick, clamped at the volume's edge.
              const plUInt8* pSource = DenseAt(bx * 8 + x - 1, by * 8 + y - 1, bz * 8 + z - 1);
              const plVec3U32 vDest = vAtlasBrick * 10u + plVec3U32(x, y, z);
              plMemoryUtils::Copy(&atlas[((vDest.z * vAtlasDim.y + vDest.y) * vAtlasDim.x + vDest.x) * 4], pSource, 4);
            }
      }
    }
  }

  plVDBVolumeResourceDescriptor desc;
  const plVec3 vExtents = plVec3(static_cast<float>(vDim.x), static_cast<float>(vDim.y), static_cast<float>(vDim.z)) * m_Shape.m_fVoxelSize;
  desc.m_Volume.m_LocalBounds = plBoundingBox::MakeFromCenterAndHalfExtents(plVec3::MakeZero(), vExtents * 0.5f);
  desc.m_Volume.m_vVoxelSize = plVec3(m_Shape.m_fVoxelSize);
  desc.m_Volume.m_Channels = plVDBVolumeChannel::Density | plVDBVolumeChannel::Temperature;
  desc.m_Volume.m_fMinDensity = 0.0f;
  desc.m_Volume.m_fMaxDensity = m_Shape.m_fPeakDensity;
  desc.m_Volume.m_vResolution = vDim;
  desc.m_Volume.m_uiFrameCount = 1;
  desc.m_Volume.m_vBrickGrid = vGrid;
  desc.m_Volume.m_vAtlasBricks = vAtlasBricks;

  desc.m_DescGAL.m_Type = plGALTextureType::Texture3D;
  desc.m_DescGAL.m_Format = plGALResourceFormat::RGBAUByteNormalized;
  desc.m_DescGAL.m_uiWidth = vAtlasDim.x;
  desc.m_DescGAL.m_uiHeight = vAtlasDim.y;
  desc.m_DescGAL.m_uiDepth = vAtlasDim.z;
  desc.m_DescGAL.m_uiMipLevelCount = 1;
  desc.m_DescGAL.m_uiArraySize = 1;
  desc.m_DescGAL.m_ResourceAccess.m_bImmutable = true;

  desc.m_DescIndirectionGAL = desc.m_DescGAL;
  desc.m_DescIndirectionGAL.m_uiWidth = vGrid.x;
  desc.m_DescIndirectionGAL.m_uiHeight = vGrid.y;
  desc.m_DescIndirectionGAL.m_uiDepth = vGrid.z;

  desc.m_SamplerDesc.m_MinFilter = plGALTextureFilterMode::Linear;
  desc.m_SamplerDesc.m_MagFilter = plGALTextureFilterMode::Linear;
  desc.m_SamplerDesc.m_MipFilter = plGALTextureFilterMode::Linear;
  desc.m_SamplerDesc.m_AddressU = plImageAddressMode::Clamp;
  desc.m_SamplerDesc.m_AddressV = plImageAddressMode::Clamp;
  desc.m_SamplerDesc.m_AddressW = plImageAddressMode::Clamp;

  plGALSystemMemoryDescription atlasMemory;
  atlasMemory.m_pData = plConstByteBlobPtr(atlas.GetData(), atlas.GetCount());
  atlasMemory.m_uiRowPitch = vAtlasDim.x * 4;
  atlasMemory.m_uiSlicePitch = vAtlasDim.x * vAtlasDim.y * 4;

  plGALSystemMemoryDescription tableMemory;
  tableMemory.m_pData = plConstByteBlobPtr(table.GetData(), table.GetCount());
  tableMemory.m_uiRowPitch = vGrid.x * 4;
  tableMemory.m_uiSlicePitch = vGrid.x * vGrid.y * 4;

  desc.m_FrameContent.PushBack(plArrayPtr<plGALSystemMemoryDescription>(&atlasMemory, 1));
  desc.m_IndirectionContent.PushBack(plArrayPtr<plGALSystemMemoryDescription>(&tableMemory, 1));

  plStringBuilder sName;
  sName.SetFormat("GameEngineTest/GpuFluidGallery/BakedSphere{}", m_uiBakeCounter++);
  return plResourceManager::CreateResource<plVDBVolumeResource>(sName, std::move(desc), "GpuFluid gallery baked sphere");
}

plResult plGpuFluidGalleryApplication::SetupHandoffScene(float fSeparation, bool bPulse)
{
  if (!m_hBakedSphere.IsValid())
    m_hBakedSphere = CreateBakedSphere();

  PL_LOCK(m_pWorld->GetWriteMarker());

  // N0 compares single pixels against the noise of two captures, so its stills keep a plain floor: the checker's edges
  // flicker under TAA jitter by more than the hand-off may differ. The video gets the full stage.
  SetupStage(0x47465031u, plVec3(-0.45f, -0.6f, 0.65f), bPulse);

  // Shared look: both volumes are lit and composited by the same march with the same parameters.
  const float fDensityScale = 4.0f;
  const plColor albedo = plColor(0.92f, 0.92f, 0.95f);
  const float fAnisotropy = 0.2f;

  {
    plGameObjectDesc desc;
    desc.m_sName.Assign("GpuSphere");
    desc.m_bDynamic = true;
    desc.m_LocalPosition = plVec3(-0.5f * fSeparation, 0.0f, s_fSphereHeight);
    plGameObject* pObject = nullptr;
    m_hGpuObject = m_pWorld->CreateObject(desc, pObject);

    plGpuFluidGasVolumeComponent* pGas = nullptr;
    m_pWorld->GetOrCreateComponentManager<plGpuFluidGasVolumeComponentManager>()->CreateComponent(pObject, pGas);
    pGas->m_fVoxelSize = m_Shape.m_fVoxelSize;
    pGas->m_vExtents = plVec3(static_cast<float>(m_Shape.m_vBrickGrid.x), static_cast<float>(m_Shape.m_vBrickGrid.y), static_cast<float>(m_Shape.m_vBrickGrid.z)) * (8.0f * m_Shape.m_fVoxelSize);
    pGas->m_uiMaxBricks = 512;
    pGas->m_DebugSource = plGpuFluidDebugSource::AnalyticSphere;
    pGas->m_fSphereRadius = m_Shape.m_fRadius;
    pGas->m_fSphereSoftness = m_Shape.m_fSoftness;
    pGas->m_fPeakDensity = m_Shape.m_fPeakDensity;
    pGas->m_fPeakTemperature = m_Shape.m_fPeakTemperature;
    pGas->m_fPulseAmplitude = bPulse ? 0.15f : 0.0f;
    pGas->m_fPulsePeriod = 2.5f;

    // What an 8 bit bake with threshold 0 keeps: any brick whose density rounds to at least 1/255.
    pGas->m_fAllocThreshold = 0.5f / 255.0f;

    pGas->m_fDensityScale = fDensityScale;
    pGas->m_Albedo = albedo;
    pGas->m_fAnisotropy = fAnisotropy;
    pGas->m_fEmissionScale = 0.0f;
    pGas->m_RenderMode = plVDBVolumeRenderMode::RayMarch;
    pGas->ResetClock();
  }

  {
    plGameObjectDesc desc;
    desc.m_sName.Assign("BakedSphere");
    desc.m_bDynamic = true;
    desc.m_LocalPosition = plVec3(0.5f * fSeparation, 0.0f, s_fSphereHeight);
    plGameObject* pObject = nullptr;
    m_hBakedObject = m_pWorld->CreateObject(desc, pObject);

    plVDBVolumeComponent* pVolume = nullptr;
    plVDBVolumeComponent::CreateComponent(pObject, pVolume);
    pVolume->SetVolume(m_hBakedSphere);
    pVolume->SetDensityScale(fDensityScale);
    pVolume->SetAlbedo(albedo);
    pVolume->SetAnisotropy(fAnisotropy);
    pVolume->SetEmissionScale(0.0f);
    pVolume->SetRenderMode(plVDBVolumeRenderMode::RayMarch);
    pVolume->SetDetailEnabled(false);
  }

  return PL_SUCCESS;
}

plGameObject* plGpuFluidGalleryApplication::GetGpuObject()
{
  plGameObject* pObject = nullptr;
  m_pWorld->TryGetObject(m_hGpuObject, pObject);
  return pObject;
}

plGameObject* plGpuFluidGalleryApplication::GetBakedObject()
{
  plGameObject* pObject = nullptr;
  m_pWorld->TryGetObject(m_hBakedObject, pObject);
  return pObject;
}

plGpuFluidGasVolumeComponent* plGpuFluidGalleryApplication::GetGpuVolume()
{
  plGameObject* pObject = GetGpuObject();
  plGpuFluidGasVolumeComponent* pGas = nullptr;
  if (pObject != nullptr)
    pObject->TryGetComponentOfBaseType(pGas);
  return pGas;
}

void plGpuFluidGalleryApplication::SetCamera(const plVec3& vPosition, const plVec3& vTarget, float fFovY)
{
  if (plGameState* pGameState = plDynamicCast<plGameState*>(GetActiveGameState()))
  {
    plCamera* pCamera = pGameState->GetMainCamera();

    // The lens the game state set up, so a scene without its own field of view gets it back.
    if (!m_bCameraDefaultsKnown)
    {
      m_DefaultCameraMode = pCamera->GetCameraMode();
      m_fDefaultFov = pCamera->GetFovOrDim();
      m_bCameraDefaultsKnown = true;
    }

    if (fFovY > 0.0f)
      pCamera->SetCameraMode(plCameraMode::PerspectiveFixedFovY, fFovY, pCamera->GetNearPlane(), pCamera->GetFarPlane());
    else
      pCamera->SetCameraMode(m_DefaultCameraMode, m_fDefaultFov, pCamera->GetNearPlane(), pCamera->GetFarPlane());

    pCamera->LookAt(vPosition, vTarget, s_vUp);
  }
}

plResult plGpuFluidGalleryApplication::SaveShot(plStringView sName)
{
  plStringBuilder sPath;
  sPath.SetFormat(":imgout/GpuFluidGallery/{}.png", sName);
  return GetLastScreenshot().SaveTo(sPath);
}

//////////////////////////////////////////////////////////////////////////

const char* plGameEngineTestGpuFluidGallery::GetTestName() const
{
  return "GpuFluid Gallery";
}

plGameEngineTestApplication* plGameEngineTestGpuFluidGallery::CreateApplication()
{
  m_pOwnApplication = PL_DEFAULT_NEW(plGpuFluidGalleryApplication);
  return m_pOwnApplication;
}

void plGameEngineTestGpuFluidGallery::SetupSubTests()
{
  AddSubTest("N0 Handoff", SubTests::Handoff);
  AddSubTest("Handoff Sphere Video", SubTests::HandoffSphereVideo);
  AddSubTest("Smoke Column", SubTests::SmokeColumn);
  AddSubTest("Smoke Column Video", SubTests::SmokeColumnVideo);
  AddSubTest("Gas Safety", SubTests::GasSafety);
  AddSubTest("N2 Divergence", SubTests::N2Divergence);
  AddSubTest("N3 Poisson", SubTests::N3Poisson);
  AddSubTest("N4 Gas Mass", SubTests::N4GasMass);
  AddSubTest("N9 Colliders", SubTests::N9Colliders);
  AddSubTest("Corridor Smoke Video", SubTests::CorridorSmokeVideo);
  AddSubTest("Smoke Around Obstacles Video", SubTests::ObstaclesVideo);
  AddSubTest("Gas Wind", SubTests::GasWind);
}

plResult plGameEngineTestGpuFluidGallery::InitializeSubTest(plInt32 iIdentifier)
{
  PL_SUCCEED_OR_RETURN(SUPER::InitializeSubTest(iIdentifier));

  m_bDepthOfFieldWasEnabled = GetCVarBool(s_szDepthOfFieldCVar, true);
  m_bRestoreDepthOfField = true;

  m_iFrame = -1;
  m_iPhase = 0;
  m_iPhaseFrame = 0;

  switch (iIdentifier)
  {
    case SubTests::Handoff:
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupHandoffScene(5.0f, false));
      // From above, so the horizon (a high-contrast edge that TAA jitter flickers) is out of the picture.
      m_pOwnApplication->SetCamera(plVec3(0.0f, -7.0f, 9.5f), plVec3(0.0f, 0.0f, 1.6f));
      return PL_SUCCESS;

    case SubTests::HandoffSphereVideo:
    {
      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupHandoffScene(5.0f, true));

      // Glowing by temperature, so the clip shows both channels the GPU writes.
      PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
      m_pOwnApplication->SetEmissive(true);
      return PL_SUCCESS;
    }

    case SubTests::SmokeColumn:
    case SubTests::SmokeColumnVideo:
    case SubTests::GasSafety:
    {
      m_GasSamples.Clear();
      m_uiFirstRenderFrame = plRenderWorld::GetFrameCounter();
      m_uiInjectRenderFrame = 0;

      // The march's lighting for the smoke scenes, all put back after the sub-test:
      // - self-shadow taps read the full-resolution atlas (lod 0) instead of the per-brick means, and twelve of them;
      // - no ambient occlusion: the march samples it from the per-brick means without filtering, which draws every
      //   brick face that lines up with the view as a seam through the smoke; the sky term is halved instead, so the
      //   shadowed side of the plume still reads darker than the lit one.
      SetCVarForSubTest("Rendering.VDBVolumes.RayMarchShadowSteps", static_cast<plInt32>(Tune("SHADOWSTEPS", 12.0f)));
      SetCVarForSubTest("Rendering.VDBVolumes.SelfShadowLod", Tune("SHADOWLOD", 0.0f));
      SetCVarForSubTest("Rendering.VDBVolumes.AmbientOcclusion", Tune("AO", 0.0f));
      SetCVarForSubTest("Rendering.VDBVolumes.AmbientStrength", Tune("AMBIENT", 0.5f));
      if (const float fStrength = Tune("SHADOWSTRENGTH", -1.0f); fStrength >= 0.0f)
        SetCVarForSubTest("Rendering.VDBVolumes.SelfShadowStrength", fStrength);
      if (const float fBlend = Tune("TAABLEND", -1.0f); fBlend >= 0.0f)
        SetCVarForSubTest("Rendering.TAA.BaseBlend", fBlend);

      PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupSmokeColumnScene());
      m_pOwnApplication->SetCamera(SmokeOrbit(0.5f), s_vSmokeTarget, s_fSmokeFovY);

      if (iIdentifier == SubTests::GasSafety)
      {
        PL_LOCK(m_pOwnApplication->GetWorld()->GetWriteMarker());
        plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
        if (pGas == nullptr)
          return PL_FAILURE;
        pGas->m_vEmitterVelocity = plVec3(0.0f, 0.0f, s_fSafetySourceSpeed);
      }
      return PL_SUCCESS;
    }

    default:
      return InitializeP3SubTest(iIdentifier);
  }
}

plResult plGameEngineTestGpuFluidGallery::DeInitializeSubTest(plInt32 iIdentifier)
{
  if (m_bRestoreDepthOfField)
  {
    SetCVarBool(s_szDepthOfFieldCVar, m_bDepthOfFieldWasEnabled);
    PL_TEST_BOOL_MSG(GetCVarBool(s_szDepthOfFieldCVar, !m_bDepthOfFieldWasEnabled) == m_bDepthOfFieldWasEnabled, "Depth of field was not restored");
    m_bRestoreDepthOfField = false;
  }

  // Last set, first restored, so a CVar set twice ends at its original value.
  for (plUInt32 i = m_RestoreCVarNames.GetCount(); i-- > 0;)
  {
    plCVar* pCVar = plCVar::FindCVarByName(m_RestoreCVarNames[i]);
    const plVariant& value = m_RestoreCVarValues[i];
    if (pCVar == nullptr)
      continue;

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

void plGameEngineTestGpuFluidGallery::SetCVarForSubTest(const char* szName, const plVariant& value)
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

plTestAppRun plGameEngineTestGpuFluidGallery::RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount)
{
  ++m_iFrame;

  switch (iIdentifier)
  {
    case SubTests::Handoff:
      return RunHandoff();
    case SubTests::HandoffSphereVideo:
      return RunHandoffSphereVideo();
    case SubTests::SmokeColumn:
      return RunSmokeColumn(false);
    case SubTests::SmokeColumnVideo:
      return RunSmokeColumn(true);
    case SubTests::GasSafety:
      return RunGasSafety();
    case SubTests::N2Divergence:
      return RunN2Divergence();
    case SubTests::N3Poisson:
      return RunN3Poisson();
    case SubTests::N4GasMass:
      return RunN4GasMass();
    case SubTests::N9Colliders:
      return RunN9Colliders();
    case SubTests::CorridorSmokeVideo:
      return RunCorridorSmokeVideo();
    case SubTests::ObstaclesVideo:
      return RunObstaclesVideo();
    case SubTests::GasWind:
      return RunGasWind();
  }

  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunHandoff()
{
  // N0: a compute-written analytic sphere (plVDBVolumeGpuRenderData, RGBA16F brick table) must render like the same
  // sphere baked the way the VDB importer bakes it (plVDBVolumeResource, RGBA8 atlas and brick table).
  //
  // Phase 0: both side by side, for the picture and the GPU's own numbers.
  // Phase 1: the GPU sphere alone, moved to the middle.   Phase 2: the baked sphere alone, in the same place.
  // Phase 3: neither, to measure how much of the image the volume is at all.
  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  // The first frame only creates the GPU resources; nothing may be handed to the march before they were written.
  if (m_iPhase == 0 && m_iPhaseFrame == 0)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetGpuVolume())
      PL_TEST_BOOL_MSG(!pGas->WasPublished(), "The GPU volume was published on its fresh-resource frame");
  }

  const plInt32 iSettle = m_iPhase == 0 ? s_iFirstSettleFrames : s_iSettleFrames;
  if (++m_iPhaseFrame < iSettle)
    return plTestAppRun::Continue;

  m_iPhaseFrame = 0;
  plWorld* pWorld = m_pOwnApplication->GetWorld();

  switch (m_iPhase)
  {
    case 0:
    {
      PL_TEST_BOOL(m_pOwnApplication->SaveShot("Handoff_SideBySide").Succeeded());

      PL_LOCK(pWorld->GetWriteMarker());
      plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetGpuVolume();
      if (PL_TEST_BOOL_MSG(pGas != nullptr, "The GPU volume is gone"))
      {
        PL_TEST_BOOL_MSG(pGas->WasPublished(), "The GPU volume was not handed to the march");

        plGpuFluidGasStats stats;
        if (PL_TEST_BOOL_MSG(pGas->GetStats(stats), "No stats readback arrived in %d frames", s_iFirstSettleFrames))
        {
          const plUInt32 uiExpected = m_pOwnApplication->GetBakedBrickCount();
          plTestFramework::Output(plTestOutput::Message, "N0 stats: frame %llu, allocated %u (baked %u), requested %u, denied %u, NaN %u, max density %.4f, mass %.1f, bricks [%u %u %u]..[%u %u %u]",
            stats.m_uiFrame, stats.m_uiAllocatedBricks, uiExpected, stats.m_uiRequestedBricks, stats.m_uiDeniedBricks, stats.m_uiNaNCount, stats.m_fMaxDensity, stats.m_fMass,
            stats.m_vBoundsMinBrick.x, stats.m_vBoundsMinBrick.y, stats.m_vBoundsMinBrick.z, stats.m_vBoundsMaxBrick.x, stats.m_vBoundsMaxBrick.y, stats.m_vBoundsMaxBrick.z);

          // The GPU keeps a brick when its float peak is above 0.5/255, the bake when its 8 bit peak is above 0: the
          // same set up to the float rounding of a voxel exactly on that edge.
          PL_TEST_BOOL_MSG(plMath::Abs(static_cast<plInt32>(stats.m_uiAllocatedBricks) - static_cast<plInt32>(uiExpected)) <= 2, "GPU kept %u bricks, the bake %u", stats.m_uiAllocatedBricks, uiExpected);
          PL_TEST_INT(stats.m_uiRequestedBricks, stats.m_uiAllocatedBricks);
          PL_TEST_INT(stats.m_uiDeniedBricks, 0);
          PL_TEST_INT(stats.m_uiNaNCount, 0);
          PL_TEST_FLOAT(stats.m_fMaxDensity, m_pOwnApplication->GetSphereShape().m_fPeakDensity, 0.02f);
          PL_TEST_BOOL(stats.HasBounds());

          // Centred sphere of radius 1.4 m in a 4 m box of 8 bricks: allocation must stay away from the corners.
          const plVec3U32 vGrid = pGas->GetBrickGrid();
          PL_TEST_BOOL(stats.m_vBoundsMaxBrick.x < vGrid.x && stats.m_vBoundsMaxBrick.y < vGrid.y && stats.m_vBoundsMaxBrick.z < vGrid.z);
          PL_TEST_BOOL(stats.m_uiAllocatedBricks < vGrid.x * vGrid.y * vGrid.z);
        }
      }

      // GPU alone, in the middle.
      if (plGameObject* pGpu = m_pOwnApplication->GetGpuObject())
        pGpu->SetLocalPosition(plVec3(0.0f, 0.0f, s_fSphereHeight));
      if (plGameObject* pBaked = m_pOwnApplication->GetBakedObject())
      {
        pBaked->SetActiveFlag(false);
        pBaked->SetLocalPosition(plVec3(0.0f, 0.0f, s_fSphereHeight));
      }
      break;
    }

    case 1:
    {
      m_GpuAlone.ResetAndCopy(m_pOwnApplication->GetLastScreenshot());
      PL_TEST_BOOL(m_pOwnApplication->SaveShot("Handoff_GpuAlone").Succeeded());
      break;
    }

    case 2:
    {
      // The same volume again, frames later: what the march's jitter alone changes between two captures.
      m_GpuAgain.ResetAndCopy(m_pOwnApplication->GetLastScreenshot());

      // The same, glowing by temperature: what the G channel carries only shows through emission.
      PL_LOCK(pWorld->GetWriteMarker());
      m_pOwnApplication->SetEmissive(true);
      break;
    }

    case 3:
    {
      m_GpuEmissive.ResetAndCopy(m_pOwnApplication->GetLastScreenshot());
      PL_TEST_BOOL(m_pOwnApplication->SaveShot("Handoff_GpuEmissive").Succeeded());

      // Baked alone, same place. Deactivating the GPU volume retires its resources.
      PL_LOCK(pWorld->GetWriteMarker());
      m_pOwnApplication->SetEmissive(false);
      if (plGameObject* pGpu = m_pOwnApplication->GetGpuObject())
        pGpu->SetActiveFlag(false);
      if (plGameObject* pBaked = m_pOwnApplication->GetBakedObject())
        pBaked->SetActiveFlag(true);
      break;
    }

    case 4:
    {
      m_BakedAlone.ResetAndCopy(m_pOwnApplication->GetLastScreenshot());
      PL_TEST_BOOL(m_pOwnApplication->SaveShot("Handoff_BakedAlone").Succeeded());

      PL_LOCK(pWorld->GetWriteMarker());
      m_pOwnApplication->SetEmissive(true);
      break;
    }

    case 5:
    {
      m_BakedEmissive.ResetAndCopy(m_pOwnApplication->GetLastScreenshot());
      PL_TEST_BOOL(m_pOwnApplication->SaveShot("Handoff_BakedEmissive").Succeeded());

      PL_LOCK(pWorld->GetWriteMarker());
      m_pOwnApplication->SetEmissive(false);
      if (plGameObject* pBaked = m_pOwnApplication->GetBakedObject())
        pBaked->SetActiveFlag(false);
      break;
    }

    case 6:
    {
      m_Empty.ResetAndCopy(m_pOwnApplication->GetLastScreenshot());
      PL_TEST_BOOL(m_pOwnApplication->SaveShot("Handoff_Empty").Succeeded());
      EvaluateHandoffImages();
      return plTestAppRun::Quit;
    }
  }

  ++m_iPhase;
  return plTestAppRun::Continue;
}

void plGameEngineTestGpuFluidGallery::EvaluateHandoffImages()
{
  plImage gpu, gpuAgain, gpuEmissive, baked, bakedEmissive, empty;
  if (!PL_TEST_BOOL(ToRGBA8(m_GpuAlone, gpu) && ToRGBA8(m_GpuAgain, gpuAgain) && ToRGBA8(m_GpuEmissive, gpuEmissive) && ToRGBA8(m_BakedAlone, baked) &&
                    ToRGBA8(m_BakedEmissive, bakedEmissive) && ToRGBA8(m_Empty, empty)))
    return;

  const plUInt32 uiWidth = gpu.GetWidth();
  const plUInt32 uiHeight = gpu.GetHeight();

  // The footprint: wherever both spheres, plain or both glowing, change the empty scene visibly. Asking both keeps out
  // the pixels a single capture's jitter flips (a high-contrast edge such as the horizon).
  plDynamicArray<bool> footprint;
  footprint.SetCount(uiWidth * uiHeight, false);
  plUInt32 uiFootprint = 0;
  for (plUInt32 y = 0; y < uiHeight; ++y)
  {
    for (plUInt32 x = 0; x < uiWidth; ++x)
    {
      const plUInt32 uiChange = plMath::Max(plMath::Min(PixelDifference(gpu, empty, x, y), PixelDifference(baked, empty, x, y)),
        plMath::Min(PixelDifference(gpuEmissive, empty, x, y), PixelDifference(bakedEmissive, empty, x, y)));
      if (uiChange > s_uiFootprintThreshold)
      {
        footprint[y * uiWidth + x] = true;
        ++uiFootprint;
      }
    }
  }

  const float fFootprint = static_cast<float>(uiFootprint) / static_cast<float>(uiWidth * uiHeight);
  PL_TEST_BOOL_MSG(fFootprint >= s_fHandoffMinFootprint, "The spheres cover only %.4f of the image", fFootprint);
  if (uiFootprint == 0)
    return;

  // Same volume, same place, two captures: the march's jitter after TAA, plus any per-frame dither.
  const ImageDiff floor = CompareImages(gpu, gpuAgain, &footprint);
  const ImageDiff wholeFloor = CompareImages(gpu, gpuAgain);

  plStringBuilder sCsv("case,signal_mean,handoff_mean,floor_mean,excess_of_signal,handoff_max,handoff_changed,whole_image_handoff_mean,whole_image_floor_mean\n");

  auto Check = [&](const char* szCase, const plImage& gpuImage, const plImage& bakedImage)
  {
    const ImageDiff signal = CompareImages(gpuImage, empty, &footprint);
    const ImageDiff handoff = CompareImages(gpuImage, bakedImage, &footprint);
    const ImageDiff wholeHandoff = CompareImages(gpuImage, bakedImage);
    const float fExcess = (handoff.m_fMean - floor.m_fMean) / plMath::Max(signal.m_fMean, 1e-6f);

    plTestFramework::Output(plTestOutput::Message,
      "N0 %s: footprint %.4f of the image; in it GPU vs baked mean %.5f, noise floor %.5f, volume signal %.5f -> excess %.4f of the signal (limit %.2f); max pixel %u/765 (limit %u), changed %.4f; whole image %.5f vs floor %.5f",
      szCase, fFootprint, handoff.m_fMean, floor.m_fMean, signal.m_fMean, fExcess, s_fHandoffMaxExcessOfSignal, handoff.m_uiMax, s_uiHandoffMaxPixel, handoff.m_fChanged, wholeHandoff.m_fMean, wholeFloor.m_fMean);

    sCsv.AppendFormat("{},{},{},{},{},{},{},{},{}\n", szCase, signal.m_fMean, handoff.m_fMean, floor.m_fMean, fExcess, handoff.m_uiMax, handoff.m_fChanged, wholeHandoff.m_fMean, wholeFloor.m_fMean);

    PL_TEST_BOOL_MSG(signal.m_fMean > 4.0f * floor.m_fMean, "N0 %s: the volume (%.5f) barely stands out of the noise (%.5f)", szCase, signal.m_fMean, floor.m_fMean);
    PL_TEST_BOOL_MSG(fExcess <= s_fHandoffMaxExcessOfSignal, "N0 %s: GPU and baked differ by %.4f of the volume's signal beyond the noise floor (limit %.2f)", szCase, fExcess, s_fHandoffMaxExcessOfSignal);
    PL_TEST_BOOL_MSG(handoff.m_uiMax <= s_uiHandoffMaxPixel, "N0 %s: a pixel differs by %u/765 (limit %u)", szCase, handoff.m_uiMax, s_uiHandoffMaxPixel);
  };

  Check("Density", gpu, baked);
  Check("Emissive", gpuEmissive, bakedEmissive);

  // The check must be able to fail: the plain GPU sphere against the glowing baked one (the temperature channel used
  // where it should not be) has to land far outside the limit.
  {
    const ImageDiff signal = CompareImages(gpu, empty, &footprint);
    const ImageDiff mismatch = CompareImages(gpu, bakedEmissive, &footprint);
    const float fExcess = (mismatch.m_fMean - floor.m_fMean) / plMath::Max(signal.m_fMean, 1e-6f);
    plTestFramework::Output(plTestOutput::Message, "N0 control (plain GPU vs glowing bake): excess %.4f of the signal, max pixel %u/765", fExcess, mismatch.m_uiMax);
    sCsv.AppendFormat("Control,{},{},{},{},{},{},,\n", signal.m_fMean, mismatch.m_fMean, floor.m_fMean, fExcess, mismatch.m_uiMax, mismatch.m_fChanged);
    PL_TEST_BOOL_MSG(fExcess > 4.0f * s_fHandoffMaxExcessOfSignal, "N0 control: a temperature mismatch shows as only %.4f of the signal", fExcess);
  }

  plFileWriter csv;
  if (csv.Open(":imgout/GpuFluidGallery/N0_Handoff.csv").Succeeded())
    csv.WriteBytes(sCsv.GetData(), sCsv.GetElementCount()).IgnoreResult();
}

void plGpuFluidGalleryApplication::SetEmissive(bool bEmissive)
{
  // Glow by temperature (VDBBlackbody of G, times G), the same colour and scale on both.
  const plColor emission = bEmissive ? plColor(1.0f, 1.0f, 1.0f) : plColor::Black;
  const float fScale = bEmissive ? 3.0f : 0.0f;

  if (plGpuFluidGasVolumeComponent* pGas = GetGpuVolume())
  {
    pGas->m_EmissionColor = emission;
    pGas->m_fEmissionScale = fScale;
  }

  plGameObject* pBakedObject = GetBakedObject();
  plVDBVolumeComponent* pBaked = nullptr;
  if (pBakedObject != nullptr && pBakedObject->TryGetComponentOfBaseType(pBaked))
  {
    pBaked->SetEmissionColor(emission);
    pBaked->SetEmissionScale(fScale);
  }
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunHandoffSphereVideo()
{
  if (plEnvironmentVariableUtils::GetValueInt("PL_GPUFLUID_VIDEO", 0) != 1)
  {
    plTestFramework::Output(plTestOutput::Details, "Skipped: set PL_GPUFLUID_VIDEO=1 to record the frames.");
    return plTestAppRun::Quit;
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  // Camera: a slow arc of 60 degrees in front of the pair (from -y, looking +y), so the GPU sphere at -x stays on the
  // right of the picture: screen right is -x here.
  const plVec3 vTarget(0.0f, 0.0f, s_fSphereHeight - 0.2f);
  auto Orbit = [&](float t)
  {
    const plAngle angle = plAngle::MakeFromDegree(-120.0f + 60.0f * t);
    return vTarget + plVec3(10.0f * plMath::Cos(angle), 10.0f * plMath::Sin(angle), 1.6f);
  };

  if (m_iPhase == 0)
  {
    m_pOwnApplication->SetCamera(Orbit(0.0f), vTarget);
    if (++m_iPhaseFrame < s_iFirstSettleFrames)
      return plTestAppRun::Continue;

    m_iPhase = 1;
    m_iPhaseFrame = 0;
    return plTestAppRun::Continue;
  }

  // The screenshot of this Run() shows the camera set before it.
  if (m_iPhaseFrame > 0)
  {
    plStringBuilder sName;
    sName.SetFormat("HandoffSphere/frame_{}", plArgU(static_cast<plUInt32>(m_iPhaseFrame - 1), 5, true));
    PL_TEST_BOOL(m_pOwnApplication->SaveShot(sName).Succeeded());

    // The caption make_video.swift puts on the clip, once the folder exists.
    if (m_iPhaseFrame == 1)
    {
      plFileWriter title;
      if (title.Open(":imgout/GpuFluidGallery/HandoffSphere/title.txt").Succeeded())
      {
        const char* szTitle = "P0 Handoff: GPU-written pulsing sphere (right), the same sphere baked like an import (left), one VDB march\n";
        title.WriteBytes(szTitle, plStringUtils::GetStringElementCount(szTitle)).IgnoreResult();
      }
    }
  }

  if (static_cast<plUInt32>(m_iPhaseFrame) >= s_uiVideoFrames)
  {
    plWorld* pWorld = m_pOwnApplication->GetWorld();
    PL_LOCK(pWorld->GetReadMarker());
    if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetGpuVolume())
    {
      plGpuFluidGasStats stats;
      if (PL_TEST_BOOL(pGas->GetStats(stats)))
      {
        plTestFramework::Output(plTestOutput::Message, "Video end stats: allocated %u, denied %u, NaN %u, max density %.4f", stats.m_uiAllocatedBricks, stats.m_uiDeniedBricks, stats.m_uiNaNCount, stats.m_fMaxDensity);
        PL_TEST_INT(stats.m_uiNaNCount, 0);
        PL_TEST_INT(stats.m_uiDeniedBricks, 0);
      }
    }
    return plTestAppRun::Quit;
  }

  m_pOwnApplication->SetCamera(Orbit(static_cast<float>(m_iPhaseFrame) / static_cast<float>(s_uiVideoFrames - 1)), vTarget);
  ++m_iPhaseFrame;
  return plTestAppRun::Continue;
}

//////////////////////////////////////////////////////////////////////////
// P1: Smoke Column

void plGameEngineTestGpuFluidGallery::CollectGasSample()
{
  plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
  plGpuFluidGasStats stats;
  if (pGas == nullptr || !pGas->GetStats(stats) || stats.m_uiTicks == 0)
    return;

  if (!m_GasSamples.IsEmpty() && m_GasSamples.PeekBack().m_uiFrame == stats.m_uiFrame)
    return;

  // Every frame runs two ticks of 1/60 s under the harness's fixed 1/30 s step, the first one included.
  const plUInt64 uiFrames = stats.m_uiFrame >= m_uiFirstRenderFrame ? stats.m_uiFrame - m_uiFirstRenderFrame + 1 : 0;

  GasSample& sample = m_GasSamples.ExpandAndGetRef();
  sample.m_fSeconds = static_cast<float>(uiFrames) * (2.0f / 60.0f);
  sample.m_uiFrame = stats.m_uiFrame;
  sample.m_fMass = stats.m_fMass;
  sample.m_fCentroidZ = stats.m_vCentroid.z * pGas->GetGridExtents().z;
  sample.m_fMaxDensity = stats.m_fMaxDensity;
  sample.m_fMaxSpeed = stats.m_fMaxSpeed;
  sample.m_fDivBefore = stats.m_fMaxDivergenceBefore;
  sample.m_fDivAfter = stats.m_fMaxDivergenceAfter;
  sample.m_fMeanDivBefore = stats.m_fMeanDivergenceBefore;
  sample.m_fMeanDivAfter = stats.m_fMeanDivergenceAfter;
  sample.m_uiNaN = stats.m_uiNaNCount;
  sample.m_uiOverflow = stats.m_uiOverflowCount;
  sample.m_uiSpeedClamped = stats.m_uiSpeedClamped;
  sample.m_uiContentBricks = stats.m_uiAllocatedBricks;
  sample.m_uiActiveBricks = stats.m_uiActiveBricks;
  sample.m_fCentroidX = stats.m_vCentroid.x * pGas->GetGridExtents().x;
  sample.m_fCentroidY = stats.m_vCentroid.y * pGas->GetGridExtents().y;
  sample.m_uiSolidCells = stats.m_uiSolidCells;
  sample.m_fSolidContact = stats.m_fSolidContactDensity;
  sample.m_fSolidDensity = stats.m_fSolidDensity;
}

plTestAppRun plGameEngineTestGpuFluidGallery::RunSmokeColumn(bool bVideo)
{
  if (bVideo && plEnvironmentVariableUtils::GetValueInt("PL_GPUFLUID_VIDEO", 0) != 1)
  {
    plTestFramework::Output(plTestOutput::Details, "Skipped: set PL_GPUFLUID_VIDEO=1 to record the frames.");
    return plTestAppRun::Quit;
  }

  // PL_GPUFLUID_TUNE_FRAMES lengthens the checked run for tuning (the checks only look at its first five seconds).
  const plInt32 iFrames = bVideo ? s_iSmokeVideoFrames : plMath::Max(s_iSmokeFrames, static_cast<plInt32>(Tune("FRAMES", static_cast<float>(s_iSmokeFrames))));
  // The video's first frame is the fresh-resource frame, whose screenshot is still black: it runs but is not saved, so
  // the clip is frames 1 to iFrames.
  const plInt32 iVideoFrame = m_iFrame - 1;
  if (bVideo)
    m_pOwnApplication->SetCamera(SmokeOrbit(plMath::Max(0.0f, static_cast<float>(iVideoFrame) / static_cast<float>(iFrames - 1))), s_vSmokeTarget, s_fSmokeFovY);

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());

    // N12: the first frame only creates the GPU resources and allocates the grid; nothing may reach the march.
    if (m_iFrame == 0)
    {
      if (plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume())
        PL_TEST_BOOL_MSG(!pGas->WasPublished(), "The smoke volume was published on its fresh-resource frame");
    }

    CollectGasSample();
  }

  if (bVideo && iVideoFrame >= 0)
  {
    plStringBuilder sName;
    sName.SetFormat("SmokeColumn/frame_{}", plArgU(static_cast<plUInt32>(iVideoFrame), 5, true));
    PL_TEST_BOOL(m_pOwnApplication->SaveShot(sName).Succeeded());

    if (iVideoFrame == 0)
    {
      plFileWriter title;
      if (title.Open(":imgout/GpuFluidGallery/SmokeColumn/title.txt").Succeeded())
      {
        const char* szTitle = "P1 Smoke Column: GPU gas solver, 4 x 4 x 8 m at 6.25 cm (1024 bricks), 60 Hz ticks, VDB march with self-shadowing\n";
        title.WriteBytes(szTitle, plStringUtils::GetStringElementCount(szTitle)).IgnoreResult();
      }
    }
  }
  else if (!bVideo && (m_iFrame == 30 || m_iFrame == 60 || m_iFrame == 90 || m_iFrame == 120 || m_iFrame == 149 || m_iFrame == 210 || m_iFrame == iFrames - 1))
  {
    plStringBuilder sName;
    sName.SetFormat("SmokeColumn_f{}", plArgU(static_cast<plUInt32>(m_iFrame), 3, true));
    PL_TEST_BOOL(m_pOwnApplication->SaveShot(sName).Succeeded());
  }

  if (m_iFrame + 1 < iFrames + (bVideo ? 1 : 0))
    return plTestAppRun::Continue;

  if (bVideo)
  {
    plUInt32 uiNaN = 0;
    for (const GasSample& sample : m_GasSamples)
      uiNaN = plMath::Max(uiNaN, sample.m_uiNaN);
    plTestFramework::Output(plTestOutput::Message, "Smoke video: %u frames, %u readbacks, largest NaN count %u", iFrames, m_GasSamples.GetCount(), uiNaN);
    PL_TEST_INT(uiNaN, 0);
    PL_TEST_BOOL(m_GasSamples.GetCount() > static_cast<plUInt32>(iFrames / 2));
  }
  else
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    EvaluateSmokeColumn();
  }

  return plTestAppRun::Quit;
}

void plGameEngineTestGpuFluidGallery::EvaluateSmokeColumn()
{
  const plDynamicArray<GasSample>& samples = m_GasSamples;

  WriteGasCsv(samples, ":imgout/GpuFluidGallery/SmokeColumn.csv");

  if (!PL_TEST_BOOL_MSG(samples.GetCount() >= static_cast<plUInt32>(s_iSmokeFrames / 2), "Only %u stats readbacks in %d frames", samples.GetCount(), s_iSmokeFrames))
    return;

  // N12 and the grid: no NaN and no fixed-point overflow ever, every brick of the box simulated, density within its
  // clamp. (Gas Safety shows that both counters do count.)
  plUInt32 uiNaN = 0;
  plUInt32 uiOverflow = 0;
  float fMaxDensity = 0.0f;
  for (const GasSample& s : samples)
  {
    uiNaN = plMath::Max(uiNaN, s.m_uiNaN);
    uiOverflow = plMath::Max(uiOverflow, s.m_uiOverflow);
    fMaxDensity = plMath::Max(fMaxDensity, s.m_fMaxDensity);
    PL_TEST_INT(s.m_uiActiveBricks, s_uiSmokeBricks);
  }
  PL_TEST_INT(uiNaN, 0);
  PL_TEST_INT(uiOverflow, 0);

  plGpuFluidGasVolumeComponent* pGas = m_pOwnApplication->GetSmokeVolume();
  const float fDensityMax = pGas != nullptr ? pGas->m_fDensityMax : 4.0f;
  PL_TEST_BOOL_MSG(fMaxDensity <= fDensityMax * 1.001f, "Density %.3f above its clamp %.3f", fMaxDensity, fDensityMax);
  PL_TEST_BOOL_MSG(fMaxDensity > 0.1f, "The source added almost nothing (peak density %.4f)", fMaxDensity);

  auto SampleAt = [&](float fSeconds) -> const GasSample*
  {
    for (const GasSample& s : samples)
    {
      if (s.m_fSeconds >= fSeconds)
        return &s;
    }
    return nullptr;
  };

  // Mass grows while the source runs: every half second over the first four.
  {
    const GasSample* pPrevious = SampleAt(0.5f);
    for (float t = 1.0f; t <= 4.0f + 1e-3f; t += 0.5f)
    {
      const GasSample* pSample = SampleAt(t);
      if (!PL_TEST_BOOL_MSG(pPrevious != nullptr && pSample != nullptr, "No readback near %.1f s", t))
        break;
      PL_TEST_BOOL_MSG(pSample->m_fMass > pPrevious->m_fMass, "Mass fell from %.1f (%.2f s) to %.1f (%.2f s) while emitting", pPrevious->m_fMass, pPrevious->m_fSeconds, pSample->m_fMass, pSample->m_fSeconds);
      pPrevious = pSample;
    }
  }

  // The plume rises: its centroid goes up monotonically over the first two seconds (every quarter second).
  {
    plStringBuilder sRise;
    const GasSample* pPrevious = SampleAt(0.25f);
    for (float t = 0.5f; t <= 2.0f + 1e-3f; t += 0.25f)
    {
      const GasSample* pSample = SampleAt(t);
      if (!PL_TEST_BOOL_MSG(pPrevious != nullptr && pSample != nullptr, "No readback near %.2f s", t))
        break;
      sRise.AppendFormat(" {}:{}", plArgF(pSample->m_fSeconds, 2), plArgF(pSample->m_fCentroidZ, 3));
      PL_TEST_BOOL_MSG(pSample->m_fCentroidZ > pPrevious->m_fCentroidZ, "Centroid fell from %.3f m (%.2f s) to %.3f m (%.2f s)", pPrevious->m_fCentroidZ, pPrevious->m_fSeconds, pSample->m_fCentroidZ, pSample->m_fSeconds);
      pPrevious = pSample;
    }
    plTestFramework::Output(plTestOutput::Message, "Smoke centroid height (s:m):%s", sRise.GetData());
  }

  // The projection removes at least 90% of the largest divergence, on every readback after the first half second.
  {
    float fWorstRatio = plMath::MaxValue<float>();
    float fWorstBefore = 0.0f;
    float fWorstAfter = 0.0f;
    plDynamicArray<float> ratios;
    for (const GasSample& s : samples)
    {
      if (s.m_fSeconds < 0.5f)
        continue;
      const float fRatio = s.m_fDivBefore / plMath::Max(s.m_fDivAfter, 1e-12f);
      ratios.PushBack(fRatio);
      if (fRatio < fWorstRatio)
      {
        fWorstRatio = fRatio;
        fWorstBefore = s.m_fDivBefore;
        fWorstAfter = s.m_fDivAfter;
      }
    }

    if (PL_TEST_BOOL(!ratios.IsEmpty()))
    {
      ratios.Sort();
      const GasSample& last = samples.PeekBack();
      plTestFramework::Output(plTestOutput::Message, "Smoke divergence: max |div| before / after projection, worst %.1fx (%.3f -> %.4f 1/s), median %.1fx; last readback max %.3f -> %.4f, mean %.4f -> %.5f 1/s",
        fWorstRatio, fWorstBefore, fWorstAfter, ratios[ratios.GetCount() / 2], last.m_fDivBefore, last.m_fDivAfter, last.m_fMeanDivBefore, last.m_fMeanDivAfter);
      PL_TEST_BOOL_MSG(fWorstRatio >= 10.0f, "The projection cut the largest divergence only %.1fx (limit 10x)", fWorstRatio);
    }
  }

  const GasSample& last = samples.PeekBack();
  plTestFramework::Output(plTestOutput::Message, "Smoke end (%.2f s): mass %.1f, centroid %.2f m, peak density %.3f, peak speed %.2f m/s, %u of %u bricks with content, NaN %u",
    last.m_fSeconds, last.m_fMass, last.m_fCentroidZ, last.m_fMaxDensity, last.m_fMaxSpeed, last.m_uiContentBricks, last.m_uiActiveBricks, uiNaN);
}

void plGameEngineTestGpuFluidGallery::WriteGasCsv(const plDynamicArray<GasSample>& samples, plStringView sPath)
{
  plStringBuilder sCsv("seconds,frame,mass,centroid_z_m,max_density,max_speed,div_before_max,div_after_max,div_before_mean,div_after_mean,nan,content_bricks,active_bricks,overflow,speed_clamped\n");
  for (const GasSample& s : samples)
  {
    sCsv.AppendFormat("{},{},{},{},{},{},", s.m_fSeconds, s.m_uiFrame, s.m_fMass, s.m_fCentroidZ, s.m_fMaxDensity, s.m_fMaxSpeed);
    sCsv.AppendFormat("{},{},{},{},{},{},{},", s.m_fDivBefore, s.m_fDivAfter, s.m_fMeanDivBefore, s.m_fMeanDivAfter, s.m_uiNaN, s.m_uiContentBricks, s.m_uiActiveBricks);
    sCsv.AppendFormat("{},{}\n", s.m_uiOverflow, s.m_uiSpeedClamped);
  }

  plFileWriter csv;
  if (csv.Open(sPath).Succeeded())
    csv.WriteBytes(sCsv.GetData(), sCsv.GetElementCount()).IgnoreResult();
}

//////////////////////////////////////////////////////////////////////////
// P1: Gas Safety (N12)
//
// The guards must be seen to work, not only to stay quiet:
// - stress: the source drives its faces at 48 m/s, so the 32 m/s clamp fires every tick; the clamp counter must count,
//   and no NaN and no fixed-point overflow may follow, with every speed the readback sees finite;
// - injection: GpuFluid.Debug.InjectNaN writes a NaN into the density on frames 45 to 47; the NaN count must go above
//   0 in the readbacks of those frames, and back to 0 a few frames later, the solver having zeroed it.
// The timing of the gas is GpuFluid Tier Timing's.

plTestAppRun plGameEngineTestGpuFluidGallery::RunGasSafety()
{
  // Three frames in a row, so a readback the ring has to skip cannot hide the injection. The CVar turns itself off.
  if (m_iFrame >= s_iSafetyInjectFrame && m_iFrame < s_iSafetyInjectFrame + 3)
  {
    SetCVarBool("GpuFluid.Debug.InjectNaN", true);
    if (m_iFrame == s_iSafetyInjectFrame)
      m_uiInjectRenderFrame = plRenderWorld::GetFrameCounter();
  }

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    CollectGasSample();
  }

  if (m_iFrame + 1 < s_iSafetyFrames)
    return plTestAppRun::Continue;

  const plDynamicArray<GasSample>& samples = m_GasSamples;
  WriteGasCsv(samples, ":imgout/GpuFluidGallery/GasSafety.csv");

  if (!PL_TEST_BOOL_MSG(samples.GetCount() >= static_cast<plUInt32>(s_iSafetyFrames / 2), "Only %u stats readbacks in %d frames", samples.GetCount(), s_iSafetyFrames))
    return plTestAppRun::Quit;

  // The injecting ticks run in the render frames of the three updates that consumed the CVar, or one later: two frames
  // of slack after the last. From eight frames after the first on, the field must be clean again.
  const plUInt64 uiInjectFirst = m_uiInjectRenderFrame;
  const plUInt64 uiInjectLast = m_uiInjectRenderFrame + 4;
  const plUInt64 uiCleanFrom = m_uiInjectRenderFrame + 10;

  plUInt32 uiNaNBefore = 0;
  plUInt32 uiNaNInjected = 0;
  plUInt32 uiNaNAfter = 0;
  plUInt32 uiCleanReadbacks = 0;
  plUInt32 uiOverflow = 0;
  plUInt32 uiClampedReadbacks = 0;
  plUInt32 uiMaxClamped = 0;
  float fMaxSpeed = 0.0f;
  bool bFiniteSpeed = true;

  for (const GasSample& s : samples)
  {
    uiOverflow = plMath::Max(uiOverflow, s.m_uiOverflow);
    uiMaxClamped = plMath::Max(uiMaxClamped, s.m_uiSpeedClamped);
    uiClampedReadbacks += s.m_uiSpeedClamped > 0 ? 1 : 0;
    bFiniteSpeed = bFiniteSpeed && plMath::IsFinite(s.m_fMaxSpeed);
    fMaxSpeed = plMath::Max(fMaxSpeed, s.m_fMaxSpeed);

    if (s.m_uiFrame < uiInjectFirst)
      uiNaNBefore = plMath::Max(uiNaNBefore, s.m_uiNaN);
    else if (s.m_uiFrame <= uiInjectLast)
      uiNaNInjected = plMath::Max(uiNaNInjected, s.m_uiNaN);
    else if (s.m_uiFrame >= uiCleanFrom)
    {
      uiNaNAfter = plMath::Max(uiNaNAfter, s.m_uiNaN);
      ++uiCleanReadbacks;
    }

    PL_TEST_INT(s.m_uiActiveBricks, s_uiSmokeBricks);
  }

  plTestFramework::Output(plTestOutput::Message,
    "Gas safety: %u readbacks; source at %.0f m/s: clamp fired on %u readbacks (up to %u faces a frame), largest face speed after the projection %.2f m/s, overflow %u; "
    "NaN before the injection %u, at it %u (render frame %llu), after it %u over %u readbacks",
    samples.GetCount(), s_fSafetySourceSpeed, uiClampedReadbacks, uiMaxClamped, fMaxSpeed, uiOverflow, uiNaNBefore, uiNaNInjected,
    static_cast<unsigned long long>(m_uiInjectRenderFrame), uiNaNAfter, uiCleanReadbacks);

  // The stress itself.
  PL_TEST_BOOL_MSG(uiClampedReadbacks > 0, "The source at %.0f m/s never reached the 32 m/s clamp", s_fSafetySourceSpeed);
  PL_TEST_INT(uiOverflow, 0);
  PL_TEST_INT(uiNaNBefore, 0);
  PL_TEST_BOOL_MSG(bFiniteSpeed, "A readback's largest speed is not finite");

  // The injection is seen, and cleaned up.
  PL_TEST_BOOL_MSG(m_uiInjectRenderFrame > 0, "The NaN was never injected");
  PL_TEST_BOOL_MSG(uiNaNInjected > 0, "The injected NaN was not counted");
  PL_TEST_BOOL_MSG(uiCleanReadbacks > 0, "No readback after the injection");
  PL_TEST_INT(uiNaNAfter, 0);

  return plTestAppRun::Quit;
}
