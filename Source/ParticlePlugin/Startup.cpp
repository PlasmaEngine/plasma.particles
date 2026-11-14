#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/Startup.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGpuManager.h>
#include <ParticlePlugin/Renderer/ParticleDistortionPass.h>
#include <ParticlePlugin/Resources/ParticleEffectResource.h>
#include <RendererCore/Pipeline/RenderPipelineInjection.h>

namespace
{
  const char* const s_szInjectionOwner = "plasma.particles";

  /// Distortion particles bend everything drawn before them, so their pass goes behind the transparent pass and takes
  /// its colour over. The engine places it and keeps the pipeline working if it cannot be placed or fails.
  void RegisterPipelineInjections()
  {
    plRenderPipelineInjectionDesc distortion;
    distortion.m_sName.Assign("plasma.particles/Distortion");
    distortion.m_sOwner.Assign(s_szInjectionOwner);
    distortion.m_sPoint.Assign(plRenderPipelineInjectionPoints::AfterTransparent);
    distortion.m_pPassType = plGetStaticRTTI<plParticleDistortionPass>();
    distortion.Bind("Color", "Color", "Color");
    // Without it the particles are not depth tested, which is what a pipeline without resolved depth gets.
    distortion.Read("ResolvedDepth", "ResolvedDepth", false);
    plRenderPipelineInjections::Register(distortion).IgnoreResult();
  }
} // namespace

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(Particle, ParticlePlugin)

  BEGIN_SUBSYSTEM_DEPENDENCIES
    "Foundation",
    "Core"
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
    plResourceManager::RegisterResourceForAssetType("Particle Effect", plGetStaticRTTI<plParticleEffectResource>());

    plParticleEffectResourceDescriptor desc;
    plParticleEffectResourceHandle hEffect = plResourceManager::CreateResource<plParticleEffectResource>("ParticleEffectMissing", std::move(desc), "Fallback for missing Particle Effects");
    plResourceManager::SetResourceTypeMissingFallback<plParticleEffectResource>(hEffect);

    RegisterPipelineInjections();
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
    plRenderPipelineInjections::UnregisterOwner(s_szInjectionOwner);

    plParticleEffectResource::CleanupDynamicPluginReferences();
  }

  ON_HIGHLEVELSYSTEMS_STARTUP
  {
    plGpuFluidGpuManager::Startup();
  }

  ON_HIGHLEVELSYSTEMS_SHUTDOWN
  {
    // While the device is still up: every GpuFluid texture, buffer and render graph goes here.
    plGpuFluidGpuManager::Shutdown();
  }

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Startup);
