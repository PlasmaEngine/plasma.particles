#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Interfaces/WindWorldModule.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Core/World/World.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Time/Clock.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Emitter/ParticleEmitter.h>
#include <ParticlePlugin/Events/ParticleEventReaction.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>
#include <ParticlePlugin/Resources/ParticleEffectResource.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

#if PL_ENABLED(PL_COMPILE_FOR_DEVELOPMENT)
plCVarBool cvar_ParticlesDebugWindSamples("Particles.DebugWindSamples", false, plCVarFlags::Default, "Enables debug visualization for wind sampling on particle effects.");
#endif

plParticleEffectInstance::plParticleEffectInstance()
{
  m_pTask = PL_DEFAULT_NEW(plParticleEffectUpdateTask, this);
  m_pTask->ConfigureTask("Particle Effect Update", plTaskNesting::Maybe);

  m_pOwnerModule = nullptr;

  Destruct();
}

plParticleEffectInstance::~plParticleEffectInstance()
{
  Destruct();
}

void plParticleEffectInstance::Construct(plParticleEffectHandle hEffectHandle, const plParticleEffectResourceHandle& hResource, plWorld* pWorld, plParticleWorldModule* pOwnerModule, plUInt64 uiRandomSeed, bool bIsShared, plArrayPtr<plParticleEffectFloatParam> floatParams, plArrayPtr<plParticleEffectColorParam> colorParams)
{
  m_hEffectHandle = hEffectHandle;
  m_pWorld = pWorld;
  m_pOwnerModule = pOwnerModule;
  m_hResource = hResource;
  m_bIsSharedEffect = bIsShared;
  m_OwnerTags.Clear(); // instances are pooled; the new owner sets its tags after creation
  m_fDynamicSpawnScale = 1.0f;
  m_hSkinnedMeshComponent.Invalidate();
  m_SkinningSnapshot.m_bValid = false;
  m_SkinningSnapshot.m_BoneMatrices.Clear();
  m_bEmitterEnabled = true;
  m_bIsFinishing = false;
  m_BoundingVolume = plBoundingBoxSphere::MakeInvalid();
  m_ElapsedTimeSinceUpdate = plTime::MakeZero();
  m_EffectIsVisible = plTime::MakeZero();
  m_iMinSimStepsToDo = 4;
  m_Transform.SetIdentity();
  m_TransformForNextFrame.SetIdentity();
  m_vVelocity.SetZero();
  m_vVelocityForNextFrame.SetZero();
  m_TotalEffectLifeTime = plTime::MakeZero();
  m_pVisibleIf = nullptr;
  m_uiRandomSeed = uiRandomSeed;

  if (uiRandomSeed == 0)
    m_Random.InitializeFromCurrentTime();
  else
    m_Random.Initialize(uiRandomSeed);

  Reconfigure(true, floatParams, colorParams);
}

void plParticleEffectInstance::Destruct()
{
  Interrupt();

  m_SharedInstances.Clear();
  m_hEffectHandle.Invalidate();

  m_Transform.SetIdentity();
  m_TransformForNextFrame.SetIdentity();
  m_bIsSharedEffect = false;
  m_pWorld = nullptr;
  m_hResource.Invalidate();
  m_hEffectHandle.Invalidate();
  m_uiReviveTimeout = 5;

  m_WindSampleGrids[0] = nullptr;
  m_WindSampleGrids[1] = nullptr;

  m_EventQueue.Clear();
}

void plParticleEffectInstance::Interrupt()
{
  ClearParticleSystems();
  ClearEventReactions();
  m_bEmitterEnabled = false;
}

void plParticleEffectInstance::SetEmitterEnabled(bool bEnable)
{
  m_bEmitterEnabled = bEnable;

  for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
  {
    if (m_ParticleSystems[i])
    {
      m_ParticleSystems[i]->SetEmitterEnabled(m_bEmitterEnabled);
    }
  }
}


bool plParticleEffectInstance::HasActiveParticles() const
{
  for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
  {
    if (m_ParticleSystems[i])
    {
      if (m_ParticleSystems[i]->HasActiveParticles())
        return true;
    }
  }

  return false;
}


void plParticleEffectInstance::ClearParticleSystem(plUInt32 index)
{
  if (m_ParticleSystems[index])
  {
    m_pOwnerModule->DestroySystemInstance(m_ParticleSystems[index]);
    m_ParticleSystems[index] = nullptr;
  }
}

void plParticleEffectInstance::ClearParticleSystems()
{
  for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
  {
    ClearParticleSystem(i);
  }

  m_ParticleSystems.Clear();
}


void plParticleEffectInstance::ClearEventReactions()
{
  for (plUInt32 i = 0; i < m_EventReactions.GetCount(); ++i)
  {
    if (m_EventReactions[i])
    {
      m_EventReactions[i]->GetDynamicRTTI()->GetAllocator()->Deallocate(m_EventReactions[i]);
    }
  }

  m_EventReactions.Clear();
}

bool plParticleEffectInstance::IsContinuous() const
{
  for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
  {
    if (m_ParticleSystems[i])
    {
      if (m_ParticleSystems[i]->IsContinuous())
        return true;
    }
  }

  return false;
}

void plParticleEffectInstance::PreSimulate()
{
  if (m_PreSimulateDuration.GetSeconds() == 0.0)
    return;

  PassTransformToSystems();

  // Pre-simulate the effect, if desired, to get it into a 'good looking' state

  // simulate in large steps to get close
  {
    const plTime tDiff = plTime::MakeFromSeconds(0.5);
    while (m_PreSimulateDuration.GetSeconds() > 10.0)
    {
      StepSimulation(tDiff);
      m_PreSimulateDuration -= tDiff;
    }
  }

  // finer steps
  {
    const plTime tDiff = plTime::MakeFromSeconds(0.2);
    while (m_PreSimulateDuration.GetSeconds() > 5.0)
    {
      StepSimulation(tDiff);
      m_PreSimulateDuration -= tDiff;
    }
  }

  // even finer
  {
    const plTime tDiff = plTime::MakeFromSeconds(0.1);
    while (m_PreSimulateDuration.GetSeconds() >= 0.1)
    {
      StepSimulation(tDiff);
      m_PreSimulateDuration -= tDiff;
    }
  }

  // final step if necessary
  if (m_PreSimulateDuration.GetSeconds() > 0.0)
  {
    StepSimulation(m_PreSimulateDuration);
    m_PreSimulateDuration = plTime::MakeFromSeconds(0);
  }

  if (!IsContinuous())
  {
    // Can't check this at the beginning, because the particle systems are only set up during StepSimulation.
    plLog::Warning("Particle pre-simulation is enabled on an effect that is not continuous.");
  }
}

void plParticleEffectInstance::SetIsVisible() const
{
  // if it is visible this frame, also render it the next few frames
  // this has multiple purposes:
  // 1) it fixes the transition when handing off an effect from a
  //    plParticleComponent to a plParticleFinisherComponent
  //    though this would only need one frame overlap
  // 2) The bounding volume for culling is only computed every couple of frames
  //    so it may be too small and culling could be imprecise
  //    by just rendering it the next 100ms, no matter what, the bounding volume
  //    does not need to be updated so frequently
  m_EffectIsVisible = plClock::GetGlobalClock()->GetAccumulatedTime() + plTime::MakeFromSeconds(0.1);
}


void plParticleEffectInstance::SetVisibleIf(plParticleEffectInstance* pOtherVisible)
{
  PL_ASSERT_DEV(pOtherVisible != this, "Invalid effect");
  m_pVisibleIf = pOtherVisible;
}

bool plParticleEffectInstance::IsVisible() const
{
  if (m_pVisibleIf != nullptr)
  {
    return m_pVisibleIf->IsVisible();
  }

  return m_EffectIsVisible >= plClock::GetGlobalClock()->GetAccumulatedTime();
}

void plParticleEffectInstance::Reconfigure(bool bFirstTime, plArrayPtr<plParticleEffectFloatParam> floatParams, plArrayPtr<plParticleEffectColorParam> colorParams)
{
  if (!m_hResource.IsValid())
  {
    plLog::Error("Effect Reconfigure: Effect Resource is invalid");
    return;
  }

  plResourceLock<plParticleEffectResource> pResource(m_hResource, plResourceAcquireMode::BlockTillLoaded);

  const auto& desc = pResource->GetDescriptor().m_Effect;
  const auto& systems = desc.GetParticleSystems();

  m_Transform.SetIdentity();
  m_TransformForNextFrame.SetIdentity();
  m_vVelocity.SetZero();
  m_vVelocityForNextFrame.SetZero();
  m_fApplyInstanceVelocity = desc.m_fApplyInstanceVelocity;
  m_bSimulateInLocalSpace = desc.m_bSimulateInLocalSpace;
  m_InvisibleUpdateRate = desc.m_InvisibleUpdateRate;
  m_fFadeOutStartDistance = desc.m_fFadeOutStartDistance;
  m_fFadeOutEndDistance = desc.m_fFadeOutEndDistance;
  m_Importance = desc.m_Importance;
  m_FixedTickStep = desc.m_fFixedTickHz > 0.0f ? plTime::MakeFromSeconds(1.0 / desc.m_fFixedTickHz) : plTime::MakeZero();
  m_uiMaxTicksPerFrame = plMath::Max<plUInt8>(desc.m_uiMaxTicksPerFrame, 1);

  m_vNumWindSamples.x = desc.m_vNumWindSamples.x;
  m_vNumWindSamples.y = desc.m_vNumWindSamples.y;
  m_vNumWindSamples.z = desc.m_vNumWindSamples.z;
  m_vNumWindSamples.w = desc.m_vNumWindSamples.x * desc.m_vNumWindSamples.y * desc.m_vNumWindSamples.z;
  m_WindSampleGrids[0] = nullptr;
  m_WindSampleGrids[1] = nullptr;

  // parameters
  {
    m_FloatParameters.Clear();
    m_ColorParameters.Clear();

    for (auto it = desc.m_FloatParameters.GetIterator(); it.IsValid(); ++it)
    {
      SetParameter(plTempHashedString(it.Key().GetData()), it.Value());
    }

    for (auto it = desc.m_ColorParameters.GetIterator(); it.IsValid(); ++it)
    {
      SetParameter(plTempHashedString(it.Key().GetData()), it.Value());
    }

    // shared effects do not support per-instance parameters
    if (m_bIsSharedEffect)
    {
      if (!floatParams.IsEmpty() || !colorParams.IsEmpty())
      {
        plLog::Warning("Shared particle effects do not support effect parameters");
      }
    }
    else
    {
      for (plUInt32 p = 0; p < floatParams.GetCount(); ++p)
      {
        SetParameter(floatParams[p].m_sName, floatParams[p].m_Value);
      }

      for (plUInt32 p = 0; p < colorParams.GetCount(); ++p)
      {
        SetParameter(colorParams[p].m_sName, colorParams[p].m_Value);
      }
    }
  }

  if (bFirstTime)
  {
    m_PreSimulateDuration = desc.m_PreSimulateDuration;
  }

  // a system whose max-particle count changed is cleared and recreated below,
  // which re-sizes its stream group and re-initializes all streams

  if (m_ParticleSystems.GetCount() != systems.GetCount())
  {
    // reset everything
    ClearParticleSystems();
  }

  m_ParticleSystems.SetCount(systems.GetCount());

  struct MulCount
  {
    PL_DECLARE_POD_TYPE();

    float m_fMultiplier = 1.0f;
    plUInt32 m_uiCount = 0;
  };

  plHybridArray<MulCount, 8> systemMaxParticles;
  {
    systemMaxParticles.SetCountUninitialized(systems.GetCount());
    for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
    {
      plUInt32 uiMaxParticlesAbs = 0, uiMaxParticlesPerSec = 0;
      for (const plParticleEmitterFactory* pEmitter : systems[i]->GetEmitterFactories())
      {
        plUInt32 uiMaxParticlesAbs0 = 0, uiMaxParticlesPerSec0 = 0;
        pEmitter->QueryMaxParticleCount(uiMaxParticlesAbs0, uiMaxParticlesPerSec0);

        uiMaxParticlesAbs += uiMaxParticlesAbs0;
        uiMaxParticlesPerSec += uiMaxParticlesPerSec0;
      }

      const plTime tLifetime = systems[i]->GetAvgLifetime();

      const plUInt32 uiMaxParticles = plMath::Max(32u, plMath::Max(uiMaxParticlesAbs, (plUInt32)(uiMaxParticlesPerSec * tLifetime.GetSeconds())));

      float fMultiplier = 1.0f;

      for (const plParticleInitializerFactory* pInitializer : systems[i]->GetInitializerFactories())
      {
        fMultiplier *= pInitializer->GetSpawnCountMultiplier(this);
      }

      systemMaxParticles[i].m_fMultiplier = plMath::Max(0.0f, fMultiplier);
      systemMaxParticles[i].m_uiCount = (plUInt32)(uiMaxParticles * systemMaxParticles[i].m_fMultiplier);
    }
  }
  // delete all that have important changes
  {
    for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
    {
      if (m_ParticleSystems[i] != nullptr)
      {
        if (m_ParticleSystems[i]->GetMaxParticles() != systemMaxParticles[i].m_uiCount)
          ClearParticleSystem(i);
      }
    }
  }

  // recreate where necessary
  {
    for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
    {
      if (m_ParticleSystems[i] == nullptr)
      {
        m_ParticleSystems[i] = m_pOwnerModule->CreateSystemInstance(systemMaxParticles[i].m_uiCount, m_pWorld, this, systemMaxParticles[i].m_fMultiplier);
      }
    }
  }

  const plVec3 vStartVelocity = m_vVelocity * m_fApplyInstanceVelocity;

  for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
  {
    m_ParticleSystems[i]->ConfigureFromTemplate(systems[i]);
    m_ParticleSystems[i]->SetTransform(m_Transform, vStartVelocity);
    m_ParticleSystems[i]->SetEmitterEnabled(m_bEmitterEnabled);
    m_ParticleSystems[i]->Finalize();
  }

  // recreate event reactions
  {
    ClearEventReactions();

    m_EventReactions.SetCount(desc.GetEventReactions().GetCount());

    const auto& er = desc.GetEventReactions();
    for (plUInt32 i = 0; i < er.GetCount(); ++i)
    {
      if (m_EventReactions[i] == nullptr)
      {
        m_EventReactions[i] = er[i]->CreateEventReaction(this);
      }
    }
  }
}

bool plParticleEffectInstance::Update(const plTime& diff)
{
  PL_PROFILE_SCOPE("PFX: Effect Update");

  plTime tMinStep = plTime::MakeFromSeconds(0);

  if (!IsVisible() && m_iMinSimStepsToDo == 0)
  {
    // shared effects always get paused when they are invisible
    if (IsSharedEffect())
      return true;

    switch (m_InvisibleUpdateRate)
    {
      case plEffectInvisibleUpdateRate::FullUpdate:
        tMinStep = plTime::MakeFromSeconds(1.0 / 60.0);
        break;

      case plEffectInvisibleUpdateRate::Max20fps:
        tMinStep = plTime::MakeFromMilliseconds(50);
        break;

      case plEffectInvisibleUpdateRate::Max10fps:
        tMinStep = plTime::MakeFromMilliseconds(100);
        break;

      case plEffectInvisibleUpdateRate::Max5fps:
        tMinStep = plTime::MakeFromMilliseconds(200);
        break;

      case plEffectInvisibleUpdateRate::Pause:
      {
        if (m_bEmitterEnabled)
        {
          // during regular operation, pause
          return m_uiReviveTimeout > 0;
        }

        // otherwise do infrequent updates to shut the effect down
        tMinStep = plTime::MakeFromMilliseconds(200);
        break;
      }

      case plEffectInvisibleUpdateRate::Discard:
        Interrupt();
        return false;
    }
  }

  m_ElapsedTimeSinceUpdate += diff;
  PassTransformToSystems();

  // fixed-tick simulation: consume the elapsed time in constant steps, catch up at most
  // MaxTicksPerFrame steps per frame and drop the rest (the Jolt-style death-spiral cap)
  if (m_FixedTickStep.IsPositive())
  {
    if (m_ElapsedTimeSinceUpdate < plMath::Max(m_FixedTickStep, tMinStep))
      return m_uiReviveTimeout > 0;

    plUInt32 uiSteps = plMath::Min((plUInt32)(m_ElapsedTimeSinceUpdate.GetSeconds() / m_FixedTickStep.GetSeconds()), (plUInt32)m_uiMaxTicksPerFrame);

    bool bAlive = m_uiReviveTimeout > 0;
    for (plUInt32 i = 0; i < uiSteps; ++i)
    {
      m_ElapsedTimeSinceUpdate -= m_FixedTickStep;

      bAlive = StepSimulation(m_FixedTickStep);
      if (!bAlive)
        return false;
    }

    // drop backlog beyond one step so a hitch doesn't cause a catch-up spiral
    m_ElapsedTimeSinceUpdate = plMath::Min(m_ElapsedTimeSinceUpdate, m_FixedTickStep);

    return bAlive;
  }

  // if the time step is too big, iterate multiple times
  {
    const plTime tMaxTimeStep = plTime::MakeFromMilliseconds(200); // in sync with Max5fps
    while (m_ElapsedTimeSinceUpdate > tMaxTimeStep)
    {
      m_ElapsedTimeSinceUpdate -= tMaxTimeStep;

      if (!StepSimulation(tMaxTimeStep))
        return false;
    }
  }

  if (m_ElapsedTimeSinceUpdate < tMinStep)
    return m_uiReviveTimeout > 0;

  // do the remainder
  const plTime tUpdateDiff = m_ElapsedTimeSinceUpdate;
  m_ElapsedTimeSinceUpdate = plTime::MakeZero();

  return StepSimulation(tUpdateDiff);
}

bool plParticleEffectInstance::StepSimulation(const plTime& tDiff)
{
  m_TotalEffectLifeTime += tDiff;

  for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
  {
    if (m_ParticleSystems[i] != nullptr)
    {
      auto state = m_ParticleSystems[i]->Update(tDiff);

      if (state == plParticleSystemState::Inactive)
      {
        ClearParticleSystem(i);
      }
      else if (state != plParticleSystemState::OnlyReacting)
      {
        // this is used to delay particle effect death by a couple of frames
        // that way, if an event is in the pipeline that might trigger a reacting emitter,
        // or particles are in the spawn queue, but not yet created, we don't kill the effect too early
        m_uiReviveTimeout = 3;
      }
    }
  }

  m_iMinSimStepsToDo = plMath::Max<plInt8>(m_iMinSimStepsToDo - 1, 0);

  --m_uiReviveTimeout;
  return m_uiReviveTimeout > 0;
}


void plParticleEffectInstance::AddParticleEvent(const plParticleEvent& pe)
{
  // drop events when the capacity is full
  if (m_EventQueue.GetCount() == m_EventQueue.GetCapacity())
    return;

  m_EventQueue.PushBack(pe);
}

void plParticleEffectInstance::RequestWindSamples()
{
  const plUInt32 uiTotalNumSamples = m_vNumWindSamples.w;

  for (auto& grid : m_WindSampleGrids)
  {
    if (grid == nullptr)
    {
      grid = PL_NEW(plFoundation::GetAlignedAllocator(), WindSampleGrid);
      grid->m_vMinPos.Set(1000.0f);
      grid->m_vMaxPos.Set(-1000.0f);
      grid->m_vInvCellSize.SetZero();
      grid->m_Samples.SetCountUninitialized(uiTotalNumSamples);
      plMemoryUtils::ZeroFill(grid->m_Samples.GetData(), uiTotalNumSamples);
    }
  }
}

void plParticleEffectInstance::UpdateWindSamples(plTime diff)
{
  const plUInt64 uiFrameCounter = plRenderWorld::GetFrameCounter();
  const plUInt32 uiDataIdx = uiFrameCounter & 1;
  if (m_WindSampleGrids[uiDataIdx] == nullptr || m_BoundingVolume.IsValid() == false)
    return;

  auto& grid = *m_WindSampleGrids[uiDataIdx];
  const auto& oldGrid = *m_WindSampleGrids[(uiDataIdx + 1) & 1];

  const plUInt32 uiNumSamplesX = m_vNumWindSamples.x;
  const plUInt32 uiNumSamplesY = m_vNumWindSamples.y;
  const plUInt32 uiNumSamplesZ = m_vNumWindSamples.z;
  const plUInt32 uiTotalNumSamples = m_vNumWindSamples.w;
  PL_ASSERT_DEBUG(grid.m_Samples.GetCount() == uiTotalNumSamples && oldGrid.m_Samples.GetCount() == uiTotalNumSamples, "Invalid number of samples");

  const plSimdVec4f interpolationFactor = plSimdVec4f(1.0f - plMath::Pow(0.1f, diff.AsFloatInSeconds()));

  const plSimdBBox boundingBox = plSimdConversion::ToBBox(m_BoundingVolume.GetBox());
  const plSimdVec4f boundsSize = boundingBox.GetExtents();
  const plSimdVec4f gridSize = plSimdVec4i(uiNumSamplesX, uiNumSamplesY, uiNumSamplesZ).ToFloat();
  plSimdVec4f cellSize = boundsSize.CompDiv(gridSize);
  const plSimdVec4f minPos = boundingBox.m_Min + cellSize * 0.5f;
  const plSimdVec4f maxPos = boundingBox.m_Max - cellSize * 0.5f;

  const plSimdVec4b oldGridValid = oldGrid.m_vMinPos < oldGrid.m_vMaxPos;
  grid.m_vMinPos = plSimdVec4f::Select(oldGridValid, plSimdVec4f::Lerp(oldGrid.m_vMinPos, minPos, interpolationFactor), minPos);
  grid.m_vMaxPos = plSimdVec4f::Select(oldGridValid, plSimdVec4f::Lerp(oldGrid.m_vMaxPos, maxPos, interpolationFactor), maxPos);

  const plSimdVec4f finalGridSize = grid.m_vMaxPos - grid.m_vMinPos;
  const plSimdVec4f maxIndices = gridSize - plSimdVec4f(1.0f);
  cellSize = plSimdVec4f::Select(maxIndices != plSimdVec4f::MakeZero(), finalGridSize.CompDiv(maxIndices), plSimdVec4f::MakeZero());
  grid.m_vInvCellSize = plSimdVec4f::Select(cellSize != plSimdVec4f::MakeZero(), plSimdVec4f(1.0f).CompDiv(cellSize), plSimdVec4f::MakeZero());
  PL_ASSERT_DEBUG(grid.m_vInvCellSize.IsValid<3>(), "");

  if (auto pWind = GetWorld()->GetModuleReadOnly<plWindWorldModuleInterface>())
  {
    for (plUInt32 i = 0; i < uiTotalNumSamples; ++i)
    {
      plUInt32 index = i;
      const plUInt32 z = i / (uiNumSamplesX * uiNumSamplesY);
      index -= z * (uiNumSamplesX * uiNumSamplesY);
      const plUInt32 y = index / uiNumSamplesX;
      const plUInt32 x = index - (y * uiNumSamplesX);

      const plSimdVec4f samplePos = grid.m_vMinPos + cellSize.CompMul(plSimdVec4i(x, y, z).ToFloat());

      grid.m_Samples[i] = plSimdVec4f::Lerp(oldGrid.m_Samples[i], pWind->GetWindAtSimd(samplePos), interpolationFactor);

#if PL_ENABLED(PL_COMPILE_FOR_DEVELOPMENT)
      if (cvar_ParticlesDebugWindSamples)
      {
        const plColor c = plColorScheme::GetColor(plColorScheme::Blue, 8);

        const plVec3 samplePos0 = plSimdConversion::ToVec3(samplePos);
        plDebugRenderer::DrawCross(GetWorld(), samplePos0, 0.1f, c);

        const plVec3 vWind = plSimdConversion::ToVec3(grid.m_Samples[i]);
        const float fWindStrength = vWind.GetLength();
        plDebugRenderer::Draw3DText(GetWorld(), plFmt("{} m/s", plArgF(fWindStrength, 2)), samplePos0, c);

        if (fWindStrength > 0.01f)
        {
          const plQuat q = plQuat::MakeShortestRotation(plVec3::MakeAxisX(), vWind);
          plDebugRenderer::DrawArrow(GetWorld(), fWindStrength, c, plTransform::Make(samplePos0, q));
        }
      }
#endif
    }
  }
  else
  {
    for (auto& sample : grid.m_Samples)
    {
      sample.SetZero();
    }
  }
}

plUInt64 plParticleEffectInstance::GetNumActiveParticles() const
{
  plUInt64 num = 0;

  for (auto pSystem : m_ParticleSystems)
  {
    if (pSystem)
    {
      num += pSystem->GetNumActiveParticles();
    }
  }

  return num;
}

void plParticleEffectInstance::SetTransform(const plTransform& transform, const plVec3& vParticleStartVelocity)
{
  m_Transform = transform;
  m_TransformForNextFrame = transform;

  m_vVelocity = vParticleStartVelocity;
  m_vVelocityForNextFrame = vParticleStartVelocity;
}

void plParticleEffectInstance::SetTransformForNextFrame(const plTransform& transform, const plVec3& vParticleStartVelocity)
{
  m_TransformForNextFrame = transform;
  m_vVelocityForNextFrame = vParticleStartVelocity;
}

plSimdVec4f plParticleEffectInstance::GetWindAt(const plSimdVec4f& vPosition) const
{
  const plUInt64 uiFrameCounter = plRenderWorld::GetFrameCounter();
  const plUInt32 uiDataIdx = (uiFrameCounter + 1) & 1;
  if (m_WindSampleGrids[uiDataIdx] == nullptr)
  {
    return plSimdVec4f::MakeZero();
  }

  auto& grid = *m_WindSampleGrids[uiDataIdx];

  const plUInt32 uiTotalNumSamples = m_vNumWindSamples.w;
  PL_ASSERT_DEBUG(grid.m_Samples.GetCount() == uiTotalNumSamples, "Invalid sample count");

  // Sample grid with trilinear interpolation
  plSimdVec4f gridSpacePos = (vPosition - grid.m_vMinPos).CompMul(grid.m_vInvCellSize);
  gridSpacePos = gridSpacePos.CompMax(plSimdVec4f::MakeZero());

  const plSimdVec4f gridSpacePosFloor = gridSpacePos.Floor();
  const plSimdVec4f weights = gridSpacePos - gridSpacePosFloor;

  const plSimdVec4i maxIndices = plSimdConversion::ToVec4i(m_vNumWindSamples) - plSimdVec4i(1);
  const plSimdVec4i pos0 = plSimdVec4i::Truncate(gridSpacePosFloor).CompMin(maxIndices);
  const plSimdVec4i pos1 = (pos0 + plSimdVec4i(1)).CompMin(maxIndices);

  const plInt32 xCount = m_vNumWindSamples.x;
  const plInt32 xyCount = xCount * m_vNumWindSamples.y;
  const plSimdVec4i cXcXYcXcXY = plSimdVec4i(xCount, xyCount, xCount, xyCount);
  const plSimdVec4i y0z0y1z1 = pos0.GetCombined<plSwizzle::YZYZ>(pos1).CompMul(cXcXYcXcXY);
  const plSimdVec4i y0y0y1y1 = y0z0y1z1.Get<plSwizzle::XXZZ>();
  const plSimdVec4i x0x0x1x1 = pos0.GetCombined<plSwizzle::XXXX>(pos1);
  const plSimdVec4i x0x1x0x1 = x0x0x1x1.Get<plSwizzle::XZXZ>();
  const plSimdVec4i y0y0y1y1_plus_x0x1x0x1 = y0y0y1y1 + x0x1x0x1;

  const plSimdVec4f wX = weights.Get<plSwizzle::XXXX>();
  const plSimdVec4f wY = weights.Get<plSwizzle::YYYY>();

  const plSimdVec4f* pSamples = grid.m_Samples.GetData();

  const plSimdVec4i indices_z0 = y0z0y1z1.Get<plSwizzle::YYYY>() + y0y0y1y1_plus_x0x1x0x1;
  const plSimdVec4f sample_z0y0x0 = pSamples[indices_z0.x()];
  const plSimdVec4f sample_z0y0x1 = pSamples[indices_z0.y()];
  const plSimdVec4f res_z0y0 = plSimdVec4f::Lerp(sample_z0y0x0, sample_z0y0x1, wX);

  const plSimdVec4f sample_z0y1x0 = pSamples[indices_z0.z()];
  const plSimdVec4f sample_z0y1x1 = pSamples[indices_z0.w()];
  const plSimdVec4f res_z0y1 = plSimdVec4f::Lerp(sample_z0y1x0, sample_z0y1x1, wX);

  const plSimdVec4f res_z0 = plSimdVec4f::Lerp(res_z0y0, res_z0y1, wY);

  const plSimdVec4i indices_z1 = y0z0y1z1.Get<plSwizzle::WWWW>() + y0y0y1y1_plus_x0x1x0x1;
  const plSimdVec4f sample_z1y0x0 = pSamples[indices_z1.x()];
  const plSimdVec4f sample_z1y0x1 = pSamples[indices_z1.y()];
  const plSimdVec4f res_z1y0 = plSimdVec4f::Lerp(sample_z1y0x0, sample_z1y0x1, wX);

  const plSimdVec4f sample_z1y1x0 = pSamples[indices_z1.z()];
  const plSimdVec4f sample_z1y1x1 = pSamples[indices_z1.w()];
  const plSimdVec4f res_z1y1 = plSimdVec4f::Lerp(sample_z1y1x0, sample_z1y1x1, wX);

  const plSimdVec4f res_z1 = plSimdVec4f::Lerp(res_z1y0, res_z1y1, wY);

  const plSimdVec4f wZ = weights.Get<plSwizzle::ZZZZ>();
  const plSimdVec4f res = plSimdVec4f::Lerp(res_z0, res_z1, wZ);

  return res;
}

void plParticleEffectInstance::PassTransformToSystems()
{
  if (!m_bSimulateInLocalSpace)
  {
    const plVec3 vStartVel = m_vVelocity * m_fApplyInstanceVelocity;

    for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
    {
      if (m_ParticleSystems[i] != nullptr)
      {
        m_ParticleSystems[i]->SetTransform(m_Transform, vStartVel);
      }
    }
  }
}

void plParticleEffectInstance::AddSharedInstance(const void* pSharedInstanceOwner)
{
  m_SharedInstances.Insert(pSharedInstanceOwner);
}

void plParticleEffectInstance::RemoveSharedInstance(const void* pSharedInstanceOwner)
{
  m_SharedInstances.Remove(pSharedInstanceOwner);
}

bool plParticleEffectInstance::ShouldBeUpdated() const
{
  if (m_hEffectHandle.IsInvalidated())
    return false;

  // do not update shared instances when there is no one watching
  if (m_bIsSharedEffect && m_SharedInstances.GetCount() == 0)
    return false;

  return true;
}

void plParticleEffectInstance::GetBoundingVolume(plBoundingBoxSphere& ref_volume) const
{
  if (!m_BoundingVolume.IsValid())
  {
    ref_volume = plBoundingSphere::MakeFromCenterAndRadius(plVec3::MakeZero(), 0.25f);
    return;
  }

  ref_volume = m_BoundingVolume;

  if (!m_bSimulateInLocalSpace)
  {
    // transform the bounding volume to local space, unless it was already created there
    const plMat4 invTrans = GetTransform().GetAsMat4().GetInverse();
    ref_volume.Transform(invTrans);
  }
}

void plParticleEffectInstance::CombineSystemBoundingVolumes()
{
  plBoundingBoxSphere effectVolume = plBoundingBoxSphere::MakeInvalid();

  for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
  {
    if (m_ParticleSystems[i])
    {
      const plBoundingBoxSphere& systemVolume = m_ParticleSystems[i]->GetBoundingVolume();
      if (systemVolume.IsValid())
      {
        effectVolume.ExpandToInclude(systemVolume);
      }
    }
  }

  m_BoundingVolume = effectVolume;
}

void plParticleEffectInstance::ProcessEventQueues()
{
  m_Transform = m_TransformForNextFrame;
  m_vVelocity = m_vVelocityForNextFrame;

  if (m_EventQueue.IsEmpty())
    return;

  PL_PROFILE_SCOPE("PFX: Effect Event Queue");
  for (plUInt32 i = 0; i < m_ParticleSystems.GetCount(); ++i)
  {
    if (m_ParticleSystems[i])
    {
      m_ParticleSystems[i]->ProcessEventQueue(m_EventQueue);
    }
  }

  for (const plParticleEvent& e : m_EventQueue)
  {
    plUInt32 rnd = m_Random.UIntInRange(100);

    for (plParticleEventReaction* pReaction : m_EventReactions)
    {
      if (pReaction->m_sEventName != e.m_EventType)
        continue;

      if (pReaction->m_uiProbability > rnd)
      {
        pReaction->ProcessEvent(e);
        break;
      }

      rnd -= pReaction->m_uiProbability;
    }
  }

  m_EventQueue.Clear();
}

plParticleEffectUpdateTask::plParticleEffectUpdateTask(plParticleEffectInstance* pEffect)
{
  m_pEffect = pEffect;
  m_UpdateDiff = plTime::MakeZero();
}

void plParticleEffectUpdateTask::Execute()
{
  if (HasBeenCanceled())
    return;

  if (m_UpdateDiff.GetSeconds() != 0.0)
  {
    m_pEffect->PreSimulate();

    if (!m_pEffect->Update(m_UpdateDiff))
    {
      const plParticleEffectHandle hEffect = m_pEffect->GetHandle();
      PL_ASSERT_DEBUG(!hEffect.IsInvalidated(), "Invalid particle effect handle");

      m_pEffect->GetOwnerWorldModule()->DestroyEffectInstance(hEffect, true, nullptr);
    }
  }
}

void plParticleEffectInstance::SetParameter(const plTempHashedString& sName, float value)
{
  // shared effects do not support parameters
  if (m_bIsSharedEffect)
    return;

  for (plUInt32 i = 0; i < m_FloatParameters.GetCount(); ++i)
  {
    if (m_FloatParameters[i].m_uiNameHash == sName.GetHash())
    {
      m_FloatParameters[i].m_fValue = value;
      return;
    }
  }

  auto& ref = m_FloatParameters.ExpandAndGetRef();
  ref.m_uiNameHash = sName.GetHash();
  ref.m_fValue = value;
}

void plParticleEffectInstance::SetParameter(const plTempHashedString& sName, const plColor& value)
{
  // shared effects do not support parameters
  if (m_bIsSharedEffect)
    return;

  for (plUInt32 i = 0; i < m_ColorParameters.GetCount(); ++i)
  {
    if (m_ColorParameters[i].m_uiNameHash == sName.GetHash())
    {
      m_ColorParameters[i].m_Value = value;
      return;
    }
  }

  auto& ref = m_ColorParameters.ExpandAndGetRef();
  ref.m_uiNameHash = sName.GetHash();
  ref.m_Value = value;
}

plInt32 plParticleEffectInstance::FindFloatParameter(const plTempHashedString& sName) const
{
  for (plUInt32 i = 0; i < m_FloatParameters.GetCount(); ++i)
  {
    if (m_FloatParameters[i].m_uiNameHash == sName.GetHash())
      return i;
  }

  return -1;
}

float plParticleEffectInstance::GetFloatParameter(const plTempHashedString& sName, float fDefaultValue) const
{
  if (sName.IsEmpty())
    return fDefaultValue;

  for (plUInt32 i = 0; i < m_FloatParameters.GetCount(); ++i)
  {
    if (m_FloatParameters[i].m_uiNameHash == sName.GetHash())
      return m_FloatParameters[i].m_fValue;
  }

  return fDefaultValue;
}

plInt32 plParticleEffectInstance::FindColorParameter(const plTempHashedString& sName) const
{
  for (plUInt32 i = 0; i < m_ColorParameters.GetCount(); ++i)
  {
    if (m_ColorParameters[i].m_uiNameHash == sName.GetHash())
      return i;
  }

  return -1;
}

const plColor& plParticleEffectInstance::GetColorParameter(const plTempHashedString& sName, const plColor& defaultValue) const
{
  if (sName.IsEmpty())
    return defaultValue;

  for (plUInt32 i = 0; i < m_ColorParameters.GetCount(); ++i)
  {
    if (m_ColorParameters[i].m_uiNameHash == sName.GetHash())
      return m_ColorParameters[i].m_Value;
  }

  return defaultValue;
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Effect_ParticleEffectInstance);
