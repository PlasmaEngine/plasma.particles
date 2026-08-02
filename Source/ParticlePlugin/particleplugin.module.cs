using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class ParticlePluginModule : ModuleRules
{
    public ParticlePluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/ParticlePlugin", "ParticlePluginPCH.h",
            "BUILDSYSTEM_BUILDING_PARTICLEPLUGIN_LIB");
    }
}
