#include <EnginePluginParticle/EnginePluginParticlePCH.h>

#include <Core/Curves/ColorGradientResource.h>
#include <Core/World/World.h>
#include <EditorEngineProcessFramework/EngineProcess/AssetPreviewEvents.h>
#include <Foundation/Configuration/Startup.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ColorGradient.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Velocity.h>
#include <ParticlePlugin/Components/ParticleComponent.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_Continuous.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_RandomSize.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SpherePosition.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_VelocityCone.h>
#include <ParticlePlugin/Resources/ParticleEffectResource.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>

namespace
{
  // Data/Base/Textures/Flare: a soft round sprite
  constexpr plUuid s_SpriteTexture(104150515546721672ull, 4824860388845839080ull);

  template <typename T>
  T* AllocateModule()
  {
    return plGetStaticRTTI<T>()->GetAllocator()->template Allocate<T>();
  }

  /// A small fire-like fountain whose colour over lifetime is the gradient; one effect per gradient resource.
  plParticleEffectResourceHandle GetPreviewEffect(plStringView sGradient)
  {
    plStringBuilder sEffect("ColorGradientPreviewEffect_", sGradient);
    if (plParticleEffectResourceHandle hExisting = plResourceManager::GetExistingResource<plParticleEffectResource>(sEffect); hExisting.IsValid())
      return hExisting;

    plParticleEffectResourceDescriptor desc;
    plParticleSystemDescriptor* pSystem = AllocateModule<plParticleSystemDescriptor>();
    pSystem->m_LifeTime.m_Value = plTime::MakeFromSeconds(1.6);
    pSystem->m_LifeTime.m_fVariance = 0.15f;

    plParticleEmitterFactory_Continuous* pEmitter = AllocateModule<plParticleEmitterFactory_Continuous>();
    pEmitter->m_uiSpawnCountPerSec = 45;
    pEmitter->m_uiSpawnCountPerSecRange = 10;

    // the emitter list has no adder; it is filled through reflection like the asset transform does
    auto pEmitters = static_cast<const plAbstractArrayProperty*>(plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
    plParticleEmitterFactory* pEmitterBase = pEmitter;
    pEmitters->Insert(pSystem, 0, &pEmitterBase);

    plParticleInitializerFactory_SpherePosition* pPosition = AllocateModule<plParticleInitializerFactory_SpherePosition>();
    pPosition->m_fRadius = 0.12f;
    pSystem->AddInitializerFactory(pPosition);

    plParticleInitializerFactory_VelocityCone* pVelocity = AllocateModule<plParticleInitializerFactory_VelocityCone>();
    pVelocity->m_Angle = plAngle::MakeFromDegree(18.0f);
    pVelocity->m_Speed.m_Value = 0.9f;
    pVelocity->m_Speed.m_fVariance = 0.3f;
    pSystem->AddInitializerFactory(pVelocity);

    plParticleInitializerFactory_RandomSize* pSize = AllocateModule<plParticleInitializerFactory_RandomSize>();
    pSize->m_Size.m_Value = 0.32f;
    pSize->m_Size.m_fVariance = 0.3f;
    pSize->m_Aspect.m_Value = 1.0f;
    pSystem->AddInitializerFactory(pSize);

    plParticleBehaviorFactory_Velocity* pRise = AllocateModule<plParticleBehaviorFactory_Velocity>();
    pRise->m_fRiseSpeed = 0.35f;
    pRise->m_fFriction = 0.6f;
    pSystem->AddBehaviorFactory(pRise);

    plParticleBehaviorFactory_ColorGradient* pColor = AllocateModule<plParticleBehaviorFactory_ColorGradient>();
    pColor->m_GradientMode = plParticleColorGradientMode::Age;
    pColor->m_hGradient = plResourceManager::LoadResource<plColorGradientResource>(sGradient);
    pSystem->AddBehaviorFactory(pColor);

    plStringBuilder sTexture;
    plConversionUtils::ToString(s_SpriteTexture, sTexture);

    plParticleTypeQuadFactory* pQuad = AllocateModule<plParticleTypeQuadFactory>();
    pQuad->m_RenderMode = plParticleTypeRenderMode::Additive;
    pQuad->m_sTexture = sTexture;
    pSystem->AddTypeFactory(pQuad);

    desc.m_Effect.AddParticleSystem(pSystem);
    return plResourceManager::CreateResource<plParticleEffectResource>(sEffect, std::move(desc), "Colour gradient preview");
  }

  void OnAssetPreview(const plAssetPreviewEvent& e)
  {
    if (e.m_sAssetType != "ColorGradient" || e.m_pAnchor == nullptr)
      return;

    plParticleComponent* pParticles = nullptr;
    plParticleComponent::CreateComponent(e.m_pAnchor, pParticles);
    pParticles->SetParticleEffect(GetPreviewEffect(e.m_sResourceId));
    pParticles->m_bSpawnAtStart = true;
    e.m_bHandled = true;
  }
} // namespace

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(EnginePluginParticle, ColorGradientPreview)

  BEGIN_SUBSYSTEM_DEPENDENCIES
    "Foundation",
    "Core"
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
    plAssetPreviewEvent::s_Events.AddEventHandler(&OnAssetPreview);
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
    plAssetPreviewEvent::s_Events.RemoveEventHandler(&OnAssetPreview);
  }

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on
