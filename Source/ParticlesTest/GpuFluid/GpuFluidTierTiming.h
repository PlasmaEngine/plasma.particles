#pragma once

#include <ParticlesTest/GpuFluid/GpuFluidGalleryTest.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>

/// \brief GPU cost of the GpuFluid solvers per phase, at a quality tier picked by hand (P5 adds the automatic tiers).
///
/// Each sub-test is one tier on the Smoke Column scene (1024 bricks). The baseline switches the render pipeline's
/// injected passes off (Rendering.Pipeline.Injections), so the frame around the gas is as small and steady as it gets;
/// the gas itself runs from the PreRender graph either way. After 60 warm-up frames, 180 are measured: the median and
/// p90 of every Renderer/GPU/Gas/* phase per frame and per run of the phase (a tick phase runs once per tick, Finalize
/// once per frame), and the µs per brick and run. The CSV goes to <sdk>/Output/GpuFluid/ and to the test output folder.
///
/// The numbers are only comparable from Release dylibs (plan section 11); a Development run still measures the GPU
/// correctly and says which build it was in the CSV header.
class plGameEngineTestGpuFluidTierTiming : public plGameEngineTest
{
  using SUPER = plGameEngineTest;

public:
  virtual const char* GetTestName() const override;
  virtual plGameEngineTestApplication* CreateApplication() override;

protected:
  virtual void SetupSubTests() override;
  virtual plResult InitializeSubTest(plInt32 iIdentifier) override;
  virtual plResult DeInitializeSubTest(plInt32 iIdentifier) override;
  virtual plTestAppRun RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount) override;

private:
  void SetCVarForSubTest(const char* szName, const plVariant& value);
  void WriteResults(plGpuFluidQuality::Enum quality);

  plGpuFluidGalleryApplication* m_pOwnApplication = nullptr;
  plInt32 m_iFrame = -1;

  plDynamicArray<plString> m_RestoreCVarNames;
  plDynamicArray<plVariant> m_RestoreCVarValues;

  /// Per phase: its name, and one value per measured frame (ms, every run of the phase in that frame summed).
  plDynamicArray<plString> m_PhaseNames;
  plDynamicArray<plDynamicArray<double>> m_PhaseValues;

  plUInt32 m_uiActiveBricks = 0;
  plUInt32 m_uiTicksPerFrame = 0;
  plUInt32 m_uiNaN = 0;
};
