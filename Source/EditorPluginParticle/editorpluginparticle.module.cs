using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginParticleModule : ModuleRules
{
    public EditorPluginParticleModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginParticle", "EditorPluginParticlePCH.h",
            "BUILDSYSTEM_BUILDING_EDITORPLUGINPARTICLE_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PlasmaPackageSdk.ConfigureAssetsBuiltIn(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/ParticlePlugin", "plParticlePlugin");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/EnginePluginParticle", "plEnginePluginParticle");

        PlasmaPackageQt.ConfigureQtModule(this, context, "EditorPluginParticleModule",
            "Source/EditorPluginParticle", "Core", "Gui", "Widgets");
    }
}
