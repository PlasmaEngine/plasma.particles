#include <ParticlesTest/ParticlesTestPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/Types/TagRegistry.h>
#include <GameEngine/GameState/GameState.h>
#include <ParticlePlugin/GpuFluid/Components/GpuFluidGasVolumeComponent.h>
#include <ParticlesTest/GpuFluid/GpuFluidGalleryTest.h>
#include <RendererCore/Lights/DirectionalLightComponent.h>
#include <RendererCore/Lights/SkyAtmosphereComponent.h>
#include <RendererCore/Lights/SkyLightComponent.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/Volumes/VDBVolumeComponent.h>
#include <Texture/Image/ImageConversion.h>

static plGameEngineTestGpuFluidGallery s_GameEngineTestGpuFluidGallery;

namespace
{
  const plVec3 s_vUp(0.0f, 0.0f, 1.0f);

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
}

void plGpuFluidGalleryApplication::BeforeHighLevelSystemsShutdown()
{
  m_hBoxMesh.Invalidate();
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
  m_pWorld->Clear();
  m_pWorld->GetRandomNumberGenerator().Initialize(0x47465031u);
  m_pWorld->GetClock().Reset(false);

  // The camera's default lens is wide open, which blurs everything but the focus point.
  SetCVarBool("Rendering.DepthOfField.Enable", false);

  AddSunAndSky(plVec3(-0.45f, -0.6f, 0.65f), 10.0f);
  AddBox("Ground", plVec3(0.0f, 0.0f, -0.1f), plVec3(2000.0f, 2000.0f, 0.2f), plColor(0.32f, 0.30f, 0.28f));

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

void plGpuFluidGalleryApplication::SetCamera(const plVec3& vPosition, const plVec3& vTarget)
{
  if (plGameState* pGameState = plDynamicCast<plGameState*>(GetActiveGameState()))
  {
    pGameState->GetMainCamera()->LookAt(vPosition, vTarget, s_vUp);
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
}

plResult plGameEngineTestGpuFluidGallery::InitializeSubTest(plInt32 iIdentifier)
{
  PL_SUCCEED_OR_RETURN(SUPER::InitializeSubTest(iIdentifier));

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
  }

  return PL_FAILURE;
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

  // Camera: a slow arc of 60 degrees in front of the pair, so the GPU sphere stays on the left.
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
        const char* szTitle = "P0 Handoff: GPU-written pulsing sphere (left), the same sphere baked like an import (right), one VDB march\n";
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
