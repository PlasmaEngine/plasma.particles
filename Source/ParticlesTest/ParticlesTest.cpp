#include <ParticlesTest/ParticlesTestPCH.h>

#include <Foundation/Configuration/Plugin.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/Strings/StringBuilder.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <ParticlesTest/ParticleTests.h>
#include <RendererCore/Textures/TextureUtils.h>
#include <TestFramework/Framework/TestFramework.h>
#include <TestFramework/Utilities/TestSetup.h>

// The game engine test harness ships as sources with the SDK; this executable compiles its own copy.
#include <GameEngineTest/TestClass/TestClass.cpp>

#define PL_PARTICLESTEST_STRINGIZE_IMPL(x) #x
#define PL_PARTICLESTEST_STRINGIZE(x) PL_PARTICLESTEST_STRINGIZE_IMPL(x)

// Named after the engine's game engine tests on purpose: the test data and the reference images live
// in the SDK under Data/UnitTests/GameEngineTest, which the test framework derives from this name.
PL_TESTFRAMEWORK_ENTRY_POINT_BEGIN("GameEngineTest", "Particle Tests")
{
  // the executable lives in the package, so the SDK is not found by walking up from it
  plFileSystem::SetSdkRootDirectory(PL_PARTICLESTEST_STRINGIZE(PL_PARTICLESTEST_SDK_ROOT));

  // this package's plugin; PL_PARTICLESTEST_PLUGIN_DIRS adds others the test project loads
  // (plJoltPlugin), separated by ';'
  plGameEngineTestApplication_Particles::AddPluginSearchRoot(PL_PARTICLESTEST_STRINGIZE(PL_PARTICLESTEST_PLUGIN_DIR));

  plStringBuilder sExtraDirs = plEnvironmentVariableUtils::GetValueString("PL_PARTICLESTEST_PLUGIN_DIRS");
  plHybridArray<plStringView, 4> extraDirs;
  sExtraDirs.Split(false, extraDirs, ";");

  for (plStringView sDir : extraDirs)
  {
    plGameEngineTestApplication_Particles::AddPluginSearchRoot(sDir);
  }

  plTextureUtils::s_bForceFullQualityAlways = true; // never allow to use low-res textures
  plTestFramework::GetInstance()->SetTestTimeout(1000 * 60 * 20);
  plTestFramework::s_bCallstackOnAssert = true;
}
PL_TESTFRAMEWORK_ENTRY_POINT_END()
