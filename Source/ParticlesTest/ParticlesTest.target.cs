using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class ParticlesTestTarget : TargetRules
{
    public ParticlesTestTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputName = "ParticlesTest";
        // Kept out of the package binary directory, which is what installs and publishes draw from. The
        // test finds the engine DLLs through PATH and the package plugin through a plugin search root.
        OutputDirectory = Path.Combine(context.ProjectRoot, "Bin", "Tests", $"{context.Platform}_x64_{context.Configuration}");
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        GenerateDebugInfo = true;

        TargetDependencies.Add("ParticlePlugin");
        ExtraModules.Add("ParticlesTestModule");
    }
}
