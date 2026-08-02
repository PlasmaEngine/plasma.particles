using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class ParticlePluginTarget : TargetRules
{
    public ParticlePluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plParticlePlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        ExtraModules.Add("ParticlePluginModule");
    }
}
