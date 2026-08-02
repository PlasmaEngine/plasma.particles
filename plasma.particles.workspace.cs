// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaParticlesWorkspace : WorkspaceRules
{
    public PlasmaParticlesWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("ParticlePlugin");
        TargetNames.Add("EnginePluginParticle");
        TargetNames.Add("EditorPluginParticle");
    }
}
