#pragma once

#include <GameEngineTest/TestClass/TestClass.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererCore/Volumes/VDBVolumeResource.h>
#include <Texture/Image/Image.h>

class plGpuFluidGasVolumeComponent;

/// \brief Scenes for looking at GpuFluid, and the checks that need a whole frame.
///
/// Every scene is built in code in the FluidGallery project and rendered through the project's main render pipeline,
/// which no test touches: the VDB march draws the gas because the volume hands it over as GPU render data. Shots are
/// saved as PNGs under the test output folder (GpuFluidGallery/).
class plGpuFluidGalleryApplication : public plGameEngineTestApplication
{
public:
  plGpuFluidGalleryApplication();

  /// Where the package's Assets folder is. Mounted as the data directory "plasma.particles", as an install would.
  static void SetPackageAssetsDirectory(plStringView sDirectory);

  /// Sun, sky and a ground slab; the GPU-written sphere and its baked twin, side by side at +-fSeparation/2.
  plResult SetupHandoffScene(float fSeparation, bool bPulse);

  /// The analytic sphere both volumes share. Matches GasAnalyticSphere.plShader.
  struct SphereShape
  {
    plVec3U32 m_vBrickGrid = plVec3U32(8, 8, 8);
    float m_fVoxelSize = 0.0625f;
    float m_fRadius = 1.4f;
    float m_fSoftness = 0.5f;
    float m_fPeakDensity = 1.0f;
    float m_fPeakTemperature = 0.8f;
  };

  const SphereShape& GetSphereShape() const { return m_Shape; }

  /// Bricks the importer's sparse bake of the sphere kept.
  plUInt32 GetBakedBrickCount() const { return m_uiBakedBricks; }

  plGameObject* GetGpuObject();
  plGameObject* GetBakedObject();
  plGpuFluidGasVolumeComponent* GetGpuVolume();

  void SetCamera(const plVec3& vPosition, const plVec3& vTarget);

  /// Both volumes glow by temperature, or neither does. World write lock held.
  void SetEmissive(bool bEmissive);

  /// Saves the last screenshot as <output>/GpuFluidGallery/<name>.png.
  plResult SaveShot(plStringView sName);

protected:
  virtual void Init_AddPluginSearchRoots() override;
  virtual void Init_FileSystem_ConfigureDataDirs() override;
  virtual void BeforeHighLevelSystemsShutdown() override;

private:
  plGameObject* AddBox(const char* szName, const plVec3& vCenter, const plVec3& vSize, const plColor& color);
  void AddSunAndSky(const plVec3& vToSun, float fLux);

  /// What the VDB importer would make of the sphere: a dense RGBA8 grid split into bricks with a one voxel apron and an
  /// RGBA8 brick table, created as a runtime plVDBVolumeResource.
  plVDBVolumeResourceHandle CreateBakedSphere();

  SphereShape m_Shape;
  plUInt32 m_uiBakedBricks = 0;
  plUInt32 m_uiBakeCounter = 0;
  plMeshResourceHandle m_hBoxMesh;
  plVDBVolumeResourceHandle m_hBakedSphere;
  plGameObjectHandle m_hGpuObject;
  plGameObjectHandle m_hBakedObject;
};

class plGameEngineTestGpuFluidGallery : public plGameEngineTest
{
  using SUPER = plGameEngineTest;

public:
  virtual const char* GetTestName() const override;
  virtual plGameEngineTestApplication* CreateApplication() override;

protected:
  enum SubTests
  {
    Handoff,
    HandoffSphereVideo,
  };

  virtual void SetupSubTests() override;
  virtual plResult InitializeSubTest(plInt32 iIdentifier) override;
  virtual plResult DeInitializeSubTest(plInt32 iIdentifier) override;
  virtual plTestAppRun RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount) override;

private:
  plTestAppRun RunHandoff();
  plTestAppRun RunHandoffSphereVideo();

  /// The N0 image checks, once every capture is in.
  void EvaluateHandoffImages();

  plGpuFluidGalleryApplication* m_pOwnApplication = nullptr;
  plInt32 m_iFrame = -1;
  plInt32 m_iPhase = 0;
  plInt32 m_iPhaseFrame = 0;
  bool m_bDepthOfFieldWasEnabled = true;
  bool m_bRestoreDepthOfField = false;
  plImage m_GpuAlone;
  plImage m_GpuAgain;
  plImage m_GpuEmissive;
  plImage m_BakedAlone;
  plImage m_BakedEmissive;
  plImage m_Empty;
};
