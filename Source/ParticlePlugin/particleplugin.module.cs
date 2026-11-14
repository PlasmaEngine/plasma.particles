using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class ParticlePluginModule : ModuleRules
{
    public ParticlePluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/ParticlePlugin", "ParticlePluginPCH.h",
            "BUILDSYSTEM_BUILDING_PARTICLEPLUGIN_LIB");

        // The GpuFluid constant headers are shared between C++ and HLSL, so both reach them by the same name:
        // <Shaders/GpuFluid/...> from this package's Assets/, and the engine macros they include from the SDK's Data/Base.
        PrivateIncludePaths.Add(System.IO.Path.Combine(context.ProjectRoot, "Assets"));
        PrivateIncludePaths.Add(System.IO.Path.Combine(PlasmaPackageSdk.Root, "Data", "Base"));
    }
}
