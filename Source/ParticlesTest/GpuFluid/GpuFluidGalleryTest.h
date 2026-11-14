#pragma once

#include <Core/Graphics/Camera.h>
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

  /// The smoke column of P1: a simulated 4 x 4 x 8 m box standing on the stage's floor, open on every side but the
  /// bottom, fed by a warm sphere source near the floor.
  plResult SetupSmokeColumnScene();

  /// The simulated volume of the smoke scenes.
  plGpuFluidGasVolumeComponent* GetSmokeVolume();

  // P3 scenes (GpuFluidGasSolverTests.cpp).

  /// N3: a 4 m cube at 6.25 cm (64 cubed, 8 x 8 x 8 bricks), every face open, solving the manufactured Poisson problem
  /// every frame instead of simulating.
  plResult SetupPoissonScene();

  /// N4: a closed 2 x 2 x 2 m box (32 cubed), every face a wall, no dissipation, a warm source that stops after
  /// fSourceSeconds.
  plResult SetupClosedBoxScene();

  /// N9 and Smoke Around Obstacles: a 4 x 4 x 8 m plume box with a centred source and a sphere collider above it; with
  /// bMovingBox also a box collider that sweeps through the plume.
  plResult SetupObstacleScene(bool bNoise, bool bMovingBox);

  /// Corridor Smoke: a closed 8 x 3 x 3 m room (walls from the closed faces) split by a partition with a doorway, smoke
  /// from a source near the floor at one end.
  plResult SetupCorridorScene();

  /// A plGpuFluidColliderComponent with a matching mesh (box or sphere), so the solid is seen.
  plGameObject* AddCollider(const char* szName, plUInt8 uiShape, const plVec3& vCenter, const plVec3& vHalfExtents, float fRadius, const plColor& color, bool bStatic, bool bVisible = true);

  /// The moving box of the obstacle scene, if any.
  plGameObject* GetMovingCollider();

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

  /// fFovY > 0: a perspective lens with that vertical field of view in degrees; otherwise the game state's own lens.
  void SetCamera(const plVec3& vPosition, const plVec3& vTarget, float fFovY = 0.0f);

  /// Both volumes glow by temperature, or neither does. World write lock held.
  void SetEmissive(bool bEmissive);

  /// Saves the last screenshot as <output>/GpuFluidGallery/<name>.png.
  plResult SaveShot(plStringView sName);

protected:
  virtual void Init_AddPluginSearchRoots() override;
  virtual void Init_FileSystem_ConfigureDataDirs() override;
  virtual void BeforeHighLevelSystemsShutdown() override;

public:
  /// Scene helpers, shared with the liquid gallery (GpuFluidLiquidGalleryTest.cpp). World write lock held.
  plGameObject* AddBox(const char* szName, const plVec3& vCenter, const plVec3& vSize, const plColor& color);
  void AddSunAndSky(const plVec3& vToSun, float fLux);

private:
  /// Clears the world and builds the shared stage: a grey floor, the sun and the sky; with bProps also a 1 m checker
  /// around the origin and a few boxes for scale and depth.
  void SetupStage(plUInt32 uiSeed, const plVec3& vToSun, bool bProps);

  /// What the VDB importer would make of the sphere: a dense RGBA8 grid split into bricks with a one voxel apron and an
  /// RGBA8 brick table, created as a runtime plVDBVolumeResource.
  plVDBVolumeResourceHandle CreateBakedSphere();

  SphereShape m_Shape;
  plUInt32 m_uiBakedBricks = 0;
  plUInt32 m_uiBakeCounter = 0;
  plMeshResourceHandle m_hBoxMesh;
  plMeshResourceHandle m_hSphereMesh;
  plGameObjectHandle m_hMovingCollider;
  plVDBVolumeResourceHandle m_hBakedSphere;
  plGameObjectHandle m_hGpuObject;
  plGameObjectHandle m_hBakedObject;
  plGameObjectHandle m_hSmokeObject;

  bool m_bCameraDefaultsKnown = false;
  plCameraMode::Enum m_DefaultCameraMode = plCameraMode::PerspectiveFixedFovY;
  float m_fDefaultFov = 90.0f;
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
    SmokeColumn,
    SmokeColumnVideo,
    GasSafety,
    // P3
    N2Divergence,
    N3Poisson,
    N4GasMass,
    N9Colliders,
    CorridorSmokeVideo,
    ObstaclesVideo,
    GasWind,
    N4GasMassVelocitySL,
    N9MovingCollider,
  };

  /// One stats readback of a simulated volume, with the simulation time it was recorded at.
  struct GasSample
  {
    float m_fSeconds = 0.0f;
    plUInt64 m_uiFrame = 0;
    float m_fMass = 0.0f;
    float m_fCentroidZ = 0.0f; ///< metres above the volume's floor
    float m_fMaxDensity = 0.0f;
    float m_fMaxSpeed = 0.0f;
    float m_fDivBefore = 0.0f;
    float m_fDivAfter = 0.0f;
    float m_fMeanDivBefore = 0.0f;
    float m_fMeanDivAfter = 0.0f;
    plUInt32 m_uiNaN = 0;
    plUInt32 m_uiOverflow = 0;
    plUInt32 m_uiSpeedClamped = 0;
    plUInt32 m_uiContentBricks = 0;
    plUInt32 m_uiActiveBricks = 0;
    float m_fCentroidX = 0.0f;    ///< metres from the volume's -X face
    float m_fCentroidY = 0.0f;    ///< metres from the volume's -Y face
    plUInt32 m_uiSolidCells = 0;
    float m_fSolidContact = 0.0f; ///< largest density in a fluid cell against a solid on the frame's last tick
    float m_fSolidDensity = 0.0f; ///< largest density left in a solid cell at the end of the frame
    float m_fSolidCleared = 0.0f; ///< density the solids took out of their cells over the frame's ticks
    float m_fEmitted = 0.0f;      ///< density the sources added over the frame's ticks
  };

  /// One solver configuration of the N3 convergence-per-ms sweep.
  struct PoissonRun
  {
    bool m_bMultigrid = true;
    plUInt32 m_uiCount = 0; ///< V-cycles or SOR iterations
    plDynamicArray<double> m_Milliseconds;
    float m_fResidual = 0.0f; ///< final max |r| / initial
    float m_fError = 0.0f;    ///< final max |e| / initial
    float m_fWhere[3] = {};   ///< final max |r| / initial inside bricks, on brick faces, on the domain's boundary
  };

  virtual void SetupSubTests() override;
  virtual plResult InitializeSubTest(plInt32 iIdentifier) override;
  virtual plResult DeInitializeSubTest(plInt32 iIdentifier) override;
  virtual plTestAppRun RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount) override;

private:
  plTestAppRun RunHandoff();
  plTestAppRun RunHandoffSphereVideo();
  plTestAppRun RunSmokeColumn(bool bVideo);
  plTestAppRun RunGasSafety();

  // P3 (GpuFluidGasSolverTests.cpp).
  plResult InitializeP3SubTest(plInt32 iIdentifier);
  plTestAppRun RunN2Divergence();
  plTestAppRun RunN3Poisson();
  plTestAppRun RunN4GasMass(bool bVelocityMacCormack);
  plTestAppRun RunN9Colliders();
  plTestAppRun RunCorridorSmokeVideo();
  plTestAppRun RunObstaclesVideo(bool bVideo);
  plTestAppRun RunGasWind();

  /// Records the frame of a video sub-test into <out>/GpuFluidGallery/<folder>/frame_%05d.png, with a caption.
  void SaveVideoFrame(const char* szFolder, plInt32 iFrame, const char* szTitle);

  /// Takes the smoke volume's latest readback into m_GasSamples when it is new. World read lock held.
  void CollectGasSample();

  /// The Smoke Column checks over m_GasSamples.
  void EvaluateSmokeColumn();

  /// One row per readback.
  static void WriteGasCsv(const plDynamicArray<GasSample>& samples, plStringView sPath);

  /// Sets a bool, int or float CVar for the running sub-test; DeInitializeSubTest puts the old value back.
  void SetCVarForSubTest(const char* szName, const plVariant& value);

  /// The N0 image checks, once every capture is in.
  void EvaluateHandoffImages();

  plGpuFluidGalleryApplication* m_pOwnApplication = nullptr;
  plInt32 m_iFrame = -1;
  plInt32 m_iPhase = 0;
  plInt32 m_iPhaseFrame = 0;
  bool m_bDepthOfFieldWasEnabled = true;
  bool m_bRestoreDepthOfField = false;
  /// CVars changed for the running sub-test and their values before, restored by DeInitializeSubTest.
  plDynamicArray<plString> m_RestoreCVarNames;
  plDynamicArray<plVariant> m_RestoreCVarValues;
  plUInt64 m_uiFirstRenderFrame = 0;
  plDynamicArray<GasSample> m_GasSamples;
  /// Gas Safety: the render frame the NaN was injected on (0 before).
  plUInt64 m_uiInjectRenderFrame = 0;
  /// N3: the convergence-per-ms sweep, one entry per solver configuration.
  plDynamicArray<PoissonRun> m_PoissonRuns;
  plUInt32 m_uiPoissonRun = 0;
  /// N3: residual after each V-cycle of the probed solve.
  plDynamicArray<float> m_PoissonCycleResiduals;
  float m_fPoissonInitialError = 0.0f;
  /// N3: GPU time of each multigrid stage, one value per measured frame.
  plDynamicArray<plString> m_PoissonStageNames;
  plDynamicArray<plDynamicArray<double>> m_PoissonStageValues;
  plImage m_GpuAlone;
  plImage m_GpuAgain;
  plImage m_GpuEmissive;
  plImage m_BakedAlone;
  plImage m_BakedEmissive;
  plImage m_Empty;
};
