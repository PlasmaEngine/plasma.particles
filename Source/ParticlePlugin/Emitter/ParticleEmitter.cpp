#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/DataProcessing/Stream/ProcessingStreamGroup.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Emitter/ParticleEmitter.h>

plCVarFloat cvar_ParticlesSpawnCountScale("Particles.SpawnCountScale", 1.0f, plCVarFlags::Save, "Global multiplier for all particle spawn counts, the main scalability lever (0 = no particles).");

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEmitterFactory, 2, plRTTINoAllocator)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Enabled", m_bEnabled)->AddAttributes(new plDefaultValueAttribute(true), new plHiddenAttribute()),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Emitter", plColorScheme::DarkUI(plColorScheme::Yellow)),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEmitter, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

plParticleEmitter* plParticleEmitterFactory::CreateEmitter(plParticleSystemInstance* pOwner) const
{
  const plRTTI* pRtti = GetEmitterType();

  plParticleEmitter* pEmitter = pRtti->GetAllocator()->Allocate<plParticleEmitter>();
  pEmitter->Reset(pOwner);

  CopyEmitterProperties(pEmitter, true);
  pEmitter->CreateRequiredStreams();

  return pEmitter;
}

bool plParticleEmitter::IsContinuous() const
{
  return false;
}

void plParticleEmitter::Process(plUInt64 uiNumElements) {}
void plParticleEmitter::ProcessEventQueue(plParticleEventQueue queue) {}

float plParticleEmitter::GetSpawnCountScale(const plTempHashedString& sScaleParameter) const
{
  const float fParamScale = plMath::Max(GetOwnerEffect()->GetFloatParameter(sScaleParameter, 1.0f), 0.0f);
  const float fGlobalScale = plMath::Max((float)cvar_ParticlesSpawnCountScale, 0.0f);

  return fParamScale * fGlobalScale * GetOwnerEffect()->GetDynamicSpawnScale();
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Emitter_ParticleEmitter);
