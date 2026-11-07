using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class ParticlesTestModule : ModuleRules
{
    public ParticlesTestModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/ParticlesTest", "ParticlesTestPCH.h",
            "BUILDSYSTEM_BUILDING_PARTICLESTEST");

        // test framework headers and import library from the SDK; the game engine test harness
        // (GameEngineTest/TestClass) is compiled into this executable from the SDK's sources
        var unitTestsRoot = Path.Combine(PlasmaPackageSdk.Root, "Code", "UnitTests");
        PublicIncludePaths.Add(unitTestsRoot);
        PublicIncludePaths.Add(Path.Combine(unitTestsRoot, "TestFramework"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "UnitTests"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "UnitTests", "TestFramework"));

        if (context.Platform == TargetPlatform.Windows)
        {
            PublicIncludePaths.Add(Path.Combine(unitTestsRoot, "TestFramework", "Platform", "Win"));
        }

        PlasmaPackageSdk.AddImportLibrary(this, context, "plTestFramework");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/ParticlePlugin", "plParticlePlugin");

        // the test data and reference images live in the SDK; the test mounts it as its SDK root.
        // Passed unquoted (the command line drops the quotes) and stringized in ParticlesTest.cpp,
        // so these paths must not contain spaces.
        PrivateDefinitions.Add($"PL_PARTICLESTEST_SDK_ROOT={PlasmaPackageSdk.Root.Replace('\\', '/')}");
        PrivateDefinitions.Add($"PL_PARTICLESTEST_PLUGIN_DIR={PlasmaPackageSdk.PackageBinaryDirectory(context).Replace('\\', '/')}");
    }
}
