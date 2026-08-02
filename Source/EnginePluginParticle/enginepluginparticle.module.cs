using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EnginePluginParticleModule : ModuleRules
{
    public EnginePluginParticleModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EnginePluginParticle", "EnginePluginParticlePCH.h",
            "BUILDSYSTEM_BUILDING_ENGINEPLUGINPARTICLE_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PlasmaPackageSdk.ConfigureAssetsBuiltIn(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/ParticlePlugin", "plParticlePlugin");
    }
}
