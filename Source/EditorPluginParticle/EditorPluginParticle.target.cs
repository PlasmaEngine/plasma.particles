using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginParticleTarget : TargetRules
{
    public EditorPluginParticleTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEditorPluginParticle";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        TargetDependencies.Add("ParticlePlugin");
        TargetDependencies.Add("EnginePluginParticle");
        ExtraModules.Add("EditorPluginParticleModule");
    }
}
