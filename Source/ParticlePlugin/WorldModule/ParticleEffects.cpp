#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/World/World.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Utilities/Stats.h>
#include <GameEngine/Animation/Skeletal/AnimatedMeshComponent.h>
#include <ParticlePlugin/Components/ParticleForceFieldComponent.h>
#include <ParticlePlugin/Resources/ParticleEffectResource.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

plCVarInt cvar_ParticlesMaxParticles("Particles.MaxParticles", 0, plCVarFlags::Save, "World particle budget: Cosmetic effects scale their spawn counts down when the total exceeds this (0 = unlimited).");

plParticleEffectHandle plParticleWorldModule::InternalCreateEffectInstance(const plParticleEffectResourceHandle& hResource, plUInt64 uiRandomSeed,
  bool bIsShared, plArrayPtr<plParticleEffectFloatParam> floatParams, plArrayPtr<plParticleEffectColorParam> colorParams)
{
  PL_LOCK(m_Mutex);

  plParticleEffectInstance* pInstance = nullptr;

  if (!m_ParticleEffectsFreeList.IsEmpty())
  {
    pInstance = m_ParticleEffectsFreeList.PeekBack();
    m_ParticleEffectsFreeList.PopBack();
  }
  else
  {
    pInstance = &m_ParticleEffects.ExpandAndGetRef();
  }

  plParticleEffectHandle hEffectHandle(m_ActiveEffects.Insert(pInstance));
  pInstance->Construct(hEffectHandle, hResource, GetWorld(), this, uiRandomSeed, bIsShared, floatParams, colorParams);

  return hEffectHandle;
}

plParticleEffectHandle plParticleWorldModule::InternalCreateSharedEffectInstance(
  const char* szSharedName, const plParticleEffectResourceHandle& hResource, plUInt64 uiRandomSeed, const void* pSharedInstanceOwner)
{
  PL_LOCK(m_Mutex);

  plStringBuilder fullName;
  fullName.SetFormat("{{0}}-{{1}}[{2}]", szSharedName, hResource.GetResourceID(), uiRandomSeed);

  bool bExisted = false;
  auto it = m_SharedEffects.FindOrAdd(fullName, &bExisted);
  plParticleEffectInstance* pEffect = nullptr;

  if (bExisted)
  {
    TryGetEffectInstance(it.Value(), pEffect);
  }

  if (!pEffect)
  {
    it.Value() =
      InternalCreateEffectInstance(hResource, uiRandomSeed, true, plArrayPtr<plParticleEffectFloatParam>(), plArrayPtr<plParticleEffectColorParam>());
    TryGetEffectInstance(it.Value(), pEffect);
  }

  PL_ASSERT_DEBUG(pEffect != nullptr, "Invalid effect pointer");
  pEffect->AddSharedInstance(pSharedInstanceOwner);

  return it.Value();
}


plParticleEffectHandle plParticleWorldModule::CreateEffectInstance(const plParticleEffectResourceHandle& hResource, plUInt64 uiRandomSeed,
  const char* szSharedName, const void*& inout_pSharedInstanceOwner, plArrayPtr<plParticleEffectFloatParam> floatParams,
  plArrayPtr<plParticleEffectColorParam> colorParams)
{
  PL_ASSERT_DEBUG(hResource.IsValid(), "Invalid Particle Effect resource handle");

  bool bIsShared = !plStringUtils::IsNullOrEmpty(szSharedName) && (inout_pSharedInstanceOwner != nullptr);

  if (!bIsShared)
  {
    plResourceLock<plParticleEffectResource> pResource(hResource, plResourceAcquireMode::BlockTillLoaded);
    bIsShared |= pResource->GetDescriptor().m_Effect.m_bAlwaysShared;
  }

  if (!bIsShared)
  {
    inout_pSharedInstanceOwner = nullptr;
    return InternalCreateEffectInstance(hResource, uiRandomSeed, false, floatParams, colorParams);
  }
  else
  {
    return InternalCreateSharedEffectInstance(szSharedName, hResource, uiRandomSeed, inout_pSharedInstanceOwner);
  }
}

void plParticleWorldModule::DestroyEffectInstance(const plParticleEffectHandle& hEffect, bool bInterruptImmediately, const void* pSharedInstanceOwner)
{
  PL_LOCK(m_Mutex);

  plParticleEffectInstance* pInstance = nullptr;
  if (TryGetEffectInstance(hEffect, pInstance))
  {
    if (pSharedInstanceOwner != nullptr)
    {
      pInstance->RemoveSharedInstance(pSharedInstanceOwner);
      return; // never delete these
    }

    if (!pInstance->m_bIsFinishing)
    {
      // make sure not to insert it into m_FinishingEffects twice
      pInstance->m_bIsFinishing = true;
      pInstance->SetEmitterEnabled(false);
      m_FinishingEffects.PushBack(pInstance);

      if (!bInterruptImmediately)
      {
        m_NeedFinisherComponent.PushBack(pInstance);
      }
    }

    if (bInterruptImmediately)
    {
      pInstance->Interrupt();
    }
  }
}

bool plParticleWorldModule::TryGetEffectInstance(const plParticleEffectHandle& hEffect, plParticleEffectInstance*& out_pEffect)
{
  return m_ActiveEffects.TryGetValue(hEffect.GetInternalID(), out_pEffect);
}

bool plParticleWorldModule::TryGetEffectInstance(const plParticleEffectHandle& hEffect, const plParticleEffectInstance*& out_pEffect) const
{
  plParticleEffectInstance* pEffect = nullptr;
  bool bResult = m_ActiveEffects.TryGetValue(hEffect.GetInternalID(), pEffect);
  out_pEffect = pEffect;
  return bResult;
}

void plParticleWorldModule::UpdateEffects(const plWorldModule::UpdateContext& context)
{
  PL_LOCK(m_Mutex);

  DestroyFinishedEffects();
  ReconfigureEffects();

  // main camera position for distance LOD; effects fade their spawn counts with distance
  plVec3 vMainCameraPos = plVec3::MakeZero();
  bool bHasMainCamera = false;

  for (const plViewHandle& hView : plRenderWorld::GetMainViews())
  {
    plView* pView = nullptr;
    if (plRenderWorld::TryGetView(hView, pView) && pView->GetWorld() == GetWorld())
    {
      vMainCameraPos = pView->GetCullingCamera()->GetCenterPosition();
      bHasMainCamera = true;
      break;
    }
  }

  // snapshot the active force fields so the update tasks can sample them lock-free
  {
    m_ForceFieldData.Clear();

    for (const plParticleForceFieldComponent* pField : m_ForceFields)
    {
      if (!pField->IsActiveAndInitialized())
        continue;

      const plGameObject* pOwner = pField->GetOwner();

      auto& data = m_ForceFieldData.ExpandAndGetRef();
      data.m_vCenter = pOwner->GetGlobalPosition();
      data.m_vAxis = pOwner->GetGlobalRotation() * plVec3(0, 0, 1);
      data.m_qInvRotation = pOwner->GetGlobalRotation().GetInverse();
      data.m_vInvBoxHalfExtents = plVec3(2.0f / pField->m_vExtents.x, 2.0f / pField->m_vExtents.y, 2.0f / pField->m_vExtents.z);
      data.m_fRadius = pField->m_fRadius;
      data.m_fStrength = pField->m_fStrength;
      data.m_fFalloffStart = pField->m_fFalloffStart;
      data.m_uiType = (plUInt8)pField->m_Type.GetValue();
      data.m_uiShape = (plUInt8)pField->m_Shape.GetValue();
    }
  }

  m_EffectUpdateTaskGroup = plTaskSystem::CreateTaskGroup(plTaskPriority::LateThisFrame);

  // world particle budget: when last frame's total exceeds the cvar, Cosmetic effects
  // scale their spawn counts down proportionally; Gameplay effects are never throttled
  plUInt64 uiTotalParticles = 0;
  plUInt32 uiActiveEffects = 0;

  for (plUInt32 i = 0; i < m_ParticleEffects.GetCount(); ++i)
  {
    if (!m_ParticleEffects[i].ShouldBeUpdated())
      continue;

    uiTotalParticles += m_ParticleEffects[i].GetNumActiveParticles();
    ++uiActiveEffects;
  }

  float fBudgetScale = 1.0f;
  if (cvar_ParticlesMaxParticles > 0 && uiTotalParticles > (plUInt64)cvar_ParticlesMaxParticles)
  {
    fBudgetScale = (float)cvar_ParticlesMaxParticles / (float)uiTotalParticles;
  }

  plStats::SetStat("Particles/ActiveParticles", uiTotalParticles);
  plStats::SetStat("Particles/ActiveEffects", uiActiveEffects);
  plStats::SetStat("Particles/BudgetSpawnScale", fBudgetScale);

  const plTime tDiff = GetWorld()->GetClock().GetTimeDiff();
  for (plUInt32 i = 0; i < m_ParticleEffects.GetCount(); ++i)
  {
    if (!m_ParticleEffects[i].ShouldBeUpdated())
      continue;

    auto& effect = m_ParticleEffects[i];

    // shared effects are rendered from many positions, a single distance would be wrong
    float fLodScale = 1.0f;
    if (bHasMainCamera && !effect.IsSharedEffect() && effect.GetFadeOutEndDistance() > effect.GetFadeOutStartDistance())
    {
      const float fDistance = (effect.GetTransform().m_vPosition - vMainCameraPos).GetLength();
      const float fFade = (fDistance - effect.GetFadeOutStartDistance()) / (effect.GetFadeOutEndDistance() - effect.GetFadeOutStartDistance());

      fLodScale = 1.0f - plMath::Saturate(fFade);
    }

    const float fEffectBudgetScale = (effect.GetImportance() == plParticleEffectImportance::Gameplay) ? 1.0f : fBudgetScale;

    effect.SetDynamicSpawnScale(fLodScale * fEffectBudgetScale);

    // snapshot the animated mesh pose for skeletal emission; taken on the main thread BEFORE the
    // update tasks start (and before this frame's animation update), so the tasks read a stable
    // copy of last frame's pose
    if (!effect.GetSkinnedMeshComponent().IsInvalidated())
    {
      auto& snapshot = effect.GetSkinningSnapshotForWriting();
      snapshot.m_bValid = false;

      plAnimatedMeshComponent* pAnimMesh = nullptr;
      if (GetWorld()->TryGetComponent(effect.GetSkinnedMeshComponent(), pAnimMesh))
      {
        const plArrayPtr<const plShaderTransform> boneTransforms = pAnimMesh->GetSkinningTransforms();

        if (!boneTransforms.IsEmpty())
        {
          snapshot.m_BoneMatrices.SetCountUninitialized(boneTransforms.GetCount());

          for (plUInt32 b = 0; b < boneTransforms.GetCount(); ++b)
          {
            snapshot.m_BoneMatrices[b] = boneTransforms[b].GetAsMat4();
          }

          snapshot.m_Transform = pAnimMesh->GetOwner()->GetGlobalTransform() * pAnimMesh->GetRootTransform();
          snapshot.m_bValid = true;
        }
      }
    }

    effect.ProcessEventQueues();

    const plSharedPtr<plTask>& pTask = effect.GetUpdateTask();
    static_cast<plParticleEffectUpdateTask*>(pTask.Borrow())->m_UpdateDiff = tDiff;

    plTaskSystem::AddTaskToGroup(m_EffectUpdateTaskGroup, pTask);
  }

  plTaskSystem::StartTaskGroup(m_EffectUpdateTaskGroup);
}

void plParticleWorldModule::DestroyFinishedEffects()
{
  PL_LOCK(m_Mutex);

  for (plUInt32 i = 0; i < m_FinishingEffects.GetCount();)
  {
    plParticleEffectInstance* pEffect = m_FinishingEffects[i];

    if (!pEffect->HasActiveParticles())
    {
      if (m_ActiveEffects.Remove(pEffect->GetHandle().GetInternalID()))
      {
        pEffect->Destruct();

        m_ParticleEffectsFreeList.PushBack(pEffect);
      }

      m_FinishingEffects.RemoveAtAndSwap(i);
    }
    else
    {
      ++i;
    }
  }

  for (plUInt32 i = 0; i < m_NeedFinisherComponent.GetCount(); ++i)
  {
    plParticleEffectInstance* pEffect = m_NeedFinisherComponent[i];

    CreateFinisherComponent(pEffect);
  }

  m_NeedFinisherComponent.Clear();
}

void plParticleWorldModule::ReconfigureEffects()
{
  PL_LOCK(m_Mutex);

  for (auto pEffect : m_EffectsToReconfigure)
  {
    pEffect->Reconfigure(false, plArrayPtr<plParticleEffectFloatParam>(), plArrayPtr<plParticleEffectColorParam>());
  }

  m_EffectsToReconfigure.Clear();
}


