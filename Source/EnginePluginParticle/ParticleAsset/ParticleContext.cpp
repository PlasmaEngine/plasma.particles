#include <EnginePluginParticle/EnginePluginParticlePCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <EnginePluginParticle/ParticleAsset/ParticleContext.h>
#include <EnginePluginParticle/ParticleAsset/ParticlePreviewModule.h>
#include <EnginePluginParticle/ParticleAsset/ParticleView.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ColorGradient.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_FadeOut.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Raycast.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_OnEvent.h>
#include <ParticlePlugin/Components/ParticleComponent.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SetAttribute.h>
#include <ParticlePlugin/System/ParticleFrameProgram.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/GPU/ParticleGPULowering.h>
#include <ParticlePlugin/Type/GPU/ParticleTypeGPU.h>
#include <RendererCore/Meshes/MeshComponent.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleContext, 1, plRTTIDefaultAllocator<plParticleContext>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_CONSTANT_PROPERTY("DocumentType", (const char*) "Particle Effect"),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleContext::plParticleContext()
  : plEngineProcessDocumentContext(plEngineProcessDocumentContextFlags::CreateWorld)
{
}

plParticleContext::~plParticleContext() = default;

void plParticleContext::HandleMessage(const plEditorEngineDocumentMsg* pMsg)
{
  if (pMsg->GetDynamicRTTI()->IsDerivedFrom<plSimulationSettingsMsgToEngine>())
  {
    // this message comes exactly once per 'update', afterwards there will be 1 to n redraw messages

    auto msg = static_cast<const plSimulationSettingsMsgToEngine*>(pMsg);

    m_pWorld->SetWorldSimulationEnabled(msg->m_bSimulateWorld);
    m_pWorld->GetClock().SetSpeed(msg->m_fSimulationSpeed);
    return;
  }

  if (pMsg->GetDynamicRTTI()->IsDerivedFrom<plEditorEngineRestartSimulationMsg>())
  {
    RestartEffect();
  }

  if (pMsg->GetDynamicRTTI()->IsDerivedFrom<plEditorEngineLoopAnimationMsg>())
  {
    SetAutoRestartEffect(((const plEditorEngineLoopAnimationMsg*)pMsg)->m_bLoop);
  }

  if (const auto* pSettings = plDynamicCast<const plParticlePreviewSettingsMsgToEngine*>(pMsg))
  {
    m_bShowStats = pSettings->m_bShowStats;
    m_bShowEvents = pSettings->m_bShowEvents;
    m_Motion = pSettings->m_Motion;
    m_SystemNames = pSettings->m_SystemNames;
    m_BlackboardEntries = pSettings->m_BlackboardEntries;
    return;
  }

  if (const auto* pTime = plDynamicCast<const plParticleSetPreviewTimeMsgToEngine*>(pMsg))
  {
    SimulateTo(pTime->m_fTime);
    return;
  }

  if (const auto* pSpy = plDynamicCast<const plParticleSpyMsgToEngine*>(pMsg))
  {
    PL_LOCK(m_pWorld->GetWriteMarker());
    m_pPreviewModule->SetSpy(*pSpy);
    return;
  }

  plEngineProcessDocumentContext::HandleMessage(pMsg);
}

namespace
{
  /// The preview always uses this seed, so a re-simulation for the timeline gives what played.
  constexpr plUInt32 s_uiPreviewSeed = 42;

  constexpr double s_fPreviewStep = 1.0 / 60.0;

  /// The longest a timeline scrub re-simulates: one minute of the effect.
  constexpr plUInt32 s_uiMaxPreviewSteps = 60 * 60;

  plUInt32 CountProgramOps(const plParticleSystemDescriptor& system)
  {
    plUInt32 uiOps = system.m_FrameProgram.m_ByteCode.GetNumInstructions();

    for (const plParticleBehaviorFactory* pBehavior : system.GetBehaviorFactories())
    {
      if (const plParticleProgramData* pProgram = pBehavior->GetParticleProgram())
        uiOps += pProgram->m_ByteCode.GetNumInstructions();
    }

    for (const plParticleInitializerFactory* pInitializer : system.GetInitializerFactories())
    {
      if (const auto* pSet = plDynamicCast<const plParticleInitializerFactory_SetAttribute*>(pInitializer))
        uiOps += pSet->m_Settings.m_Program.m_ByteCode.GetNumInstructions();
    }

    return uiOps;
  }

  bool HasUpdateProgram(const plParticleSystemDescriptor& system)
  {
    for (const plParticleBehaviorFactory* pBehavior : system.GetBehaviorFactories())
    {
      if (const plParticleProgramData* pProgram = pBehavior->GetParticleProgram(); pProgram != nullptr && !pProgram->IsEmpty())
        return true;
    }

    return false;
  }

  /// "Gradient, Fade every 4th frame (32 m)"; empty when the system has neither.
  plString DescribeUpdateInterval(const plParticleSystemDescriptor& system, const plParticleEffectInstance& effect)
  {
    plStringBuilder sBlocks;

    for (const plParticleBehaviorFactory* pBehavior : system.GetBehaviorFactories())
    {
      const char* szName = nullptr;
      if (pBehavior->IsInstanceOf<plParticleBehaviorFactory_ColorGradient>())
        szName = "Gradient";
      else if (pBehavior->IsInstanceOf<plParticleBehaviorFactory_FadeOut>())
        szName = "Fade";

      if (szName != nullptr && sBlocks.FindSubString(szName) == nullptr)
        sBlocks.AppendWithSeparator(", ", szName);
    }

    if (sBlocks.IsEmpty())
      return {};

    const plUInt8 uiInterval = effect.GetLodUpdateInterval();
    const char* szEvery = uiInterval == 1 ? "every frame" : (uiInterval == 2 ? "every 2nd frame" : "every 4th frame");

    plStringBuilder s;
    if (effect.GetLodDistance() < 0.0f)
      s.SetFormat("{} {} (no distance)", sBlocks, szEvery);
    else
      s.SetFormat("{} {} ({} m)", sBlocks, szEvery, plArgF(effect.GetLodDistance(), 0));

    return s;
  }
} // namespace

void plParticleContext::UpdateDocumentContext()
{
  plEngineProcessDocumentContext::UpdateDocumentContext();

  MovePreview();
  CollectRecentEvents();

  // the editor's timeline and value spy follow at about ten updates a second
  const plTime tNow = plTime::Now();
  if (tNow - m_LastStatus >= plTime::MakeFromMilliseconds(100))
  {
    m_LastStatus = tNow;
    UpdateSystemStats();
    SendStatus();
  }
}

void plParticleContext::MovePreview()
{
  PL_LOCK(m_pWorld->GetWriteMarker());

  if (m_pComponent == nullptr)
    return;

  const double t = m_pWorld->GetClock().GetAccumulatedTime().GetSeconds();
  m_pComponent->GetOwner()->SetGlobalPosition(GetPreviewPosition(m_Motion, t, &m_fPreviewSpeed));
}

plVec3 plParticleContext::GetPreviewPosition(plParticlePreviewMotion::Enum motion, double t, float* out_pSpeed)
{
  // both motions keep one speed, which the readout shows
  float fSpeed = 0.0f;
  plVec3 vPosition = plVec3::MakeZero();

  switch (motion)
  {
    case plParticlePreviewMotion::Orbit:
    {
      // a 1.5 m circle at 1.4 rad/s
      const float fAngle = (float)(t * 1.4);
      vPosition.Set(plMath::Cos(plAngle::MakeFromRadian(fAngle)) * 1.5f, plMath::Sin(plAngle::MakeFromRadian(fAngle)) * 1.5f, 0.0f);
      fSpeed = 1.5f * 1.4f;
      break;
    }

    case plParticlePreviewMotion::Line:
    {
      // 5 m back and forth along Y, 0.6 lengths per second
      const double u = plMath::Mod(t * 0.6, 2.0);
      vPosition.Set(0.0f, (float)((u < 1.0 ? u : 2.0 - u) * 5.0 - 2.5), 0.0f);
      fSpeed = 5.0f * 0.6f;
      break;
    }

    default:
      break;
  }

  if (out_pSpeed != nullptr)
    *out_pSpeed = fSpeed;

  return vPosition;
}

double plParticleContext::GetPreviewPeriod(plParticlePreviewMotion::Enum motion)
{
  switch (motion)
  {
    case plParticlePreviewMotion::Orbit:
      return 2.0 * plMath::Pi<double>() / 1.4;
    case plParticlePreviewMotion::Line:
      return 2.0 / 0.6;
    default:
      return 0.0;
  }
}

void plParticleContext::SimulateTo(double fTime)
{
  PL_LOCK(m_pWorld->GetWriteMarker());

  if (m_pComponent == nullptr)
    return;

  // the spy's tint must not be simulated
  m_pPreviewModule->RestoreColors();

  m_pComponent->m_uiRandomSeed = s_uiPreviewSeed;
  m_pComponent->InterruptEffect();
  m_pComponent->StartEffect();

  const plUInt32 uiSteps = plMath::Min((plUInt32)plMath::Round(plMath::Max(fTime, 0.0) / s_fPreviewStep), s_uiMaxPreviewSteps);
  for (plUInt32 i = 0; i < uiSteps && m_pComponent->m_EffectController.IsAlive(); ++i)
  {
    m_pComponent->m_EffectController.ForceVisible();
    m_pComponent->m_EffectController.Tick(plTime::MakeFromSeconds(s_fPreviewStep));
  }

  // one world update without time passing hands the bounds to culling, so the result is drawn
  const bool bPaused = m_pWorld->GetClock().GetPaused();
  const bool bSimulating = m_pWorld->GetWorldSimulationEnabled();
  m_pWorld->SetWorldSimulationEnabled(true);
  m_pWorld->GetClock().SetPaused(true);
  m_pWorld->Update();
  m_pWorld->GetClock().SetPaused(bPaused);
  m_pWorld->SetWorldSimulationEnabled(bSimulating);
}

void plParticleContext::CollectRecentEvents()
{
  const plTime tNow = plTime::Now();

  // a marker shows for half a second
  for (plUInt32 i = m_RecentEvents.GetCount(); i-- > 0;)
  {
    if (tNow - m_RecentEvents[i].m_Time > plTime::MakeFromMilliseconds(500))
      m_RecentEvents.RemoveAtAndSwap(i);
  }

  if (!m_bShowEvents)
  {
    m_RecentEvents.Clear();
    return;
  }

  PL_LOCK(m_pWorld->GetReadMarker());

  if (m_pComponent == nullptr || !m_pComponent->m_EffectController.IsAlive())
    return;

  const plParticleEffectInstance* pEffect = m_pComponent->m_EffectController.GetInstance();
  if (pEffect == nullptr)
    return;

  // the preview may update more often than the world steps; each step's events count once
  if (pEffect->GetTotalEffectLifeTime() == m_LastEventCollection)
    return;

  m_LastEventCollection = pEffect->GetTotalEffectLifeTime();

  // events are in simulation space, which for a local-space effect moves with it
  const plTransform transform = pEffect->IsSimulatedInLocalSpace() ? pEffect->GetTransform() : plTransform::MakeIdentity();

  for (const plParticleEvent& e : pEffect->GetLastEvents())
  {
    RecentEvent& recent = m_RecentEvents.ExpandAndGetRef();
    recent.m_vPosition = transform.TransformPosition(e.m_vPosition);
    recent.m_Time = tNow;
  }
}

void plParticleContext::UpdateSystemStats()
{
  PL_LOCK(m_pWorld->GetReadMarker());

  m_SystemStats.Clear();

  if (m_pComponent == nullptr || !m_pComponent->m_EffectController.IsAlive())
    return;

  plResourceLock<plParticleEffectResource> pResource(m_hParticle, plResourceAcquireMode::AllowLoadingFallback);
  if (pResource.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  const auto& descriptors = pResource->GetDescriptor().m_Effect.GetParticleSystems();
  const auto& systems = m_pComponent->m_EffectController.GetInstance()->GetParticleSystems();

  for (plUInt32 i = 0; i < systems.GetCount() && i < descriptors.GetCount(); ++i)
  {
    const plParticleSystemInstance* pSystem = systems[i];
    if (pSystem == nullptr)
      continue;

    const plParticleSystemDescriptor& desc = *descriptors[i];

    SystemStats& stats = m_SystemStats.ExpandAndGetRef();

    plStringBuilder sName;
    if (i < m_SystemNames.GetCount() && !m_SystemNames[i].IsEmpty())
      sName = m_SystemNames[i];
    else
      sName.SetFormat("System {}", i + 1);
    stats.m_sName = sName;

    stats.m_uiParticles = pSystem->GetNumActiveParticles();
    stats.m_uiMaxParticles = pSystem->GetMaxParticles();
    stats.m_uiProgramOps = CountProgramOps(desc);
    stats.m_UpdateDuration = pSystem->GetLastUpdateDuration();

    if (pSystem->GetBoundingVolume().IsValid())
      stats.m_vBoundsSize = pSystem->GetBoundingVolume().m_vBoxHalfExtends * 2.0f;

    if (pSystem->IsLoweredToGPU())
    {
      stats.m_bOnGPU = true;
      stats.m_sRunsOn = "GPU";

      const plParticleTypeGPU* pType = pSystem->GetTypes().IsEmpty() ? nullptr : plDynamicCast<const plParticleTypeGPU*>(pSystem->GetTypes()[0]);
      if (pType != nullptr)
      {
        if (pType->m_Programs.GetNumInstructions() > 0)
        {
          stats.m_sRunsOn = "GPU \xC2\xB7 graph program"; // ·
          stats.m_uiGPURegisters = pType->m_Programs.GetMaxRegisters();
        }

        // the sort covers the alive particles padded to a power of two; the CPU only knows how many were spawned and not timed out
        const bool bBlended = pType->m_RenderMode == plParticleTypeRenderMode::Blended || pType->m_RenderMode == plParticleTypeRenderMode::BlendedBackground ||
                              pType->m_RenderMode == plParticleTypeRenderMode::BlendedForeground || pType->m_RenderMode == plParticleTypeRenderMode::BlendAdd;
        if (bBlended)
        {
          stats.m_uiSortSlots = plMath::PowerOfTwo_Ceil(pType->m_uiMaxGPUParticles);
          stats.m_uiSortedAtMost = plMath::Min(plMath::PowerOfTwo_Ceil(plMath::Max<plUInt32>((plUInt32)stats.m_uiParticles, 1u)), stats.m_uiSortSlots);
        }
      }
    }
    else if (desc.m_SimulationTarget != plParticleSimulationTarget::CPU)
    {
      // the panel has room for the reason's first part; the graph shows all of it on the system
      const plString sReason = plParticleGPULowering::FindLoweringBlocker(&desc);
      plStringView sShort = sReason;
      if (const char* szRest = sReason.FindSubString(";"))
        sShort = plStringView(sReason.GetData(), szRest);

      plStringBuilder sRunsOn;
      sRunsOn.SetFormat("CPU \xC2\xB7 {}", sShort);
      stats.m_sRunsOn = sRunsOn;
    }
    else
    {
      stats.m_bOnCpuByProgram = HasUpdateProgram(desc);
      stats.m_sRunsOn = stats.m_bOnCpuByProgram ? "CPU \xC2\xB7 per-particle program" : "CPU";
    }

    // on the GPU every particle is updated every frame
    if (!stats.m_bOnGPU)
      stats.m_sUpdateInterval = DescribeUpdateInterval(desc, *m_pComponent->m_EffectController.GetInstance());

    GatherEventStats(i, desc, *pSystem, stats);
  }
}

void plParticleContext::GatherEventStats(plUInt32 uiSystem, const plParticleSystemDescriptor& desc, const plParticleSystemInstance& system, SystemStats& ref_stats)
{
  // raised: by the system's deaths or a Raycast's collisions
  bool bRaises = !desc.m_sOnDeathEvent.IsEmpty();
  for (const plParticleBehaviorFactory* pBehavior : desc.GetBehaviorFactories())
  {
    if (const auto* pRaycast = plDynamicCast<const plParticleBehaviorFactory_Raycast*>(pBehavior))
      bRaises |= !pRaycast->m_sOnCollideEvent.IsEmpty();
  }

  // the On Event that spawns on events; the instance's emitters are in the descriptor's order
  const plParticleEmitterFactory_OnEvent* pFactory = nullptr;
  const plParticleEmitter_OnEvent* pOnEvent = nullptr;
  {
    const auto& factories = desc.GetEmitterFactories();
    const plArrayPtr<plParticleEmitter* const> emitters = system.GetEmitters();

    for (plUInt32 i = 0; i < factories.GetCount() && i < emitters.GetCount() && pOnEvent == nullptr; ++i)
    {
      pFactory = plDynamicCast<const plParticleEmitterFactory_OnEvent*>(factories[i]);
      pOnEvent = pFactory != nullptr ? plDynamicCast<const plParticleEmitter_OnEvent*>(emitters[i]) : nullptr;
    }
  }

  if (!bRaises && pOnEvent == nullptr)
    return;

  // a few events a second would mostly read 0 per frame, so the rates are over the last second
  if (uiSystem >= m_EventRates.GetCount())
    m_EventRates.SetCount(uiSystem + 1);

  EventRate& rate = m_EventRates[uiSystem];
  const plUInt64 uiRaised = system.GetTotalEventsRaised();
  const plUInt64 uiDropped = system.GetTotalEventsDropped();
  const plUInt64 uiEvents = pOnEvent != nullptr ? pOnEvent->GetTotalEventCount() : 0;
  const plUInt64 uiSpawned = pOnEvent != nullptr ? pOnEvent->GetTotalSpawnCount() : 0;
  const plTime tNow = plTime::Now();

  // a restarted effect starts its counts again
  if (uiRaised < rate.m_uiRaised || uiEvents < rate.m_uiEvents)
    rate = EventRate();

  if (rate.m_Start.IsZero())
  {
    rate.m_Start = tNow;
    rate.m_uiRaised = uiRaised;
    rate.m_uiDropped = uiDropped;
    rate.m_uiEvents = uiEvents;
    rate.m_uiSpawned = uiSpawned;
  }
  else if (const double fSeconds = (tNow - rate.m_Start).GetSeconds(); fSeconds >= 1.0)
  {
    rate.m_fRaised = (float)((uiRaised - rate.m_uiRaised) / fSeconds);
    rate.m_fDropped = (float)((uiDropped - rate.m_uiDropped) / fSeconds);
    rate.m_fEvents = (float)((uiEvents - rate.m_uiEvents) / fSeconds);
    rate.m_fEach = uiEvents > rate.m_uiEvents ? (float)(uiSpawned - rate.m_uiSpawned) / (float)(uiEvents - rate.m_uiEvents) : 0.0f;
    rate.m_Start = tNow;
    rate.m_uiRaised = uiRaised;
    rate.m_uiDropped = uiDropped;
    rate.m_uiEvents = uiEvents;
    rate.m_uiSpawned = uiSpawned;
  }

  plStringBuilder s;

  if (bRaises)
  {
    const plParticleTypeGPU* pType = system.GetTypes().IsEmpty() ? nullptr : plDynamicCast<const plParticleTypeGPU*>(system.GetTypes()[0]);

    s.SetFormat("{} / s", plArgF(rate.m_fRaised, 0));

    if (rate.m_fDropped > 0.0f || pType == nullptr)
      s.AppendFormat(" \xC2\xB7 {} dropped", plArgF(rate.m_fDropped, 0)); // ·

    if (pType != nullptr && pType->GetEventLatencyFrames() > 0)
      s.AppendFormat(" \xC2\xB7 read back {} frames late", pType->GetEventLatencyFrames());

    ref_stats.m_sRaises = s;
  }

  if (pOnEvent != nullptr)
  {
    if (rate.m_fEvents <= 0.0f)
      s.SetFormat("{} \xC2\xB7 no events", pFactory->m_sEventName); // ·
    else
      s.SetFormat("{} \xC2\xB7 {} events / s \xC2\xB7 {} each", pFactory->m_sEventName, plArgF(rate.m_fEvents, 0), plArgF(rate.m_fEach, 0));

    ref_stats.m_sSpawnedBy = s;
  }
}

void plParticleContext::SendStatus()
{
  plParticlePreviewStatusMsgToEditor msg;
  msg.m_DocumentGuid = GetDocumentGuid();

  {
    PL_LOCK(m_pWorld->GetReadMarker());

    if (m_pComponent != nullptr && m_pComponent->m_EffectController.IsAlive())
    {
      msg.m_bEffectAlive = true;
      msg.m_fEffectTime = m_pComponent->m_EffectController.GetInstance()->GetTotalEffectLifeTime().GetSeconds();
    }

    const plParticlePreviewModule::SpyResult& spy = m_pPreviewModule->GetSpyResult();
    msg.m_SpyNode = spy.m_Node;
    msg.m_uiSpyCount = spy.m_uiCount;
    msg.m_fSpyMin = spy.m_fMin;
    msg.m_fSpyAvg = spy.m_fAvg;
    msg.m_fSpyMax = spy.m_fMax;
    msg.m_SpyBins = spy.m_Bins.GetArrayPtr();
    msg.m_bSpyOnGPU = spy.m_bOnGPU;
  }

  for (const plString& sEntry : m_BlackboardEntries)
  {
    if (!plParticleHasBlackboardEntry(sEntry))
      msg.m_MissingBlackboardEntries.PushBack(sEntry);
  }

  SendProcessMessage(&msg);
}

void plParticleContext::OnInitialize()
{
  auto pWorld = m_pWorld;
  PL_LOCK(pWorld->GetWriteMarker());

  plParticleComponentManager* pCompMan = pWorld->GetOrCreateComponentManager<plParticleComponentManager>();


  // Preview Effect
  {
    plGameObjectDesc obj;
    plGameObject* pObj;
    obj.m_sName.Assign("ParticlePreview");
    pWorld->CreateObject(obj, pObj);

    pCompMan->CreateComponent(pObj, m_pComponent);
    m_pComponent->m_OnFinishedAction = plOnComponentFinishedAction2::Restart;
    m_pComponent->m_MinRestartDelay = plTime::MakeFromSeconds(0.5);
    m_pComponent->m_uiRandomSeed = s_uiPreviewSeed;

    m_pPreviewModule = pWorld->GetOrCreateModule<plParticlePreviewModule>();
    m_pPreviewModule->SetPreviewComponent(m_pComponent);

    plStringBuilder sParticleGuid;
    plConversionUtils::ToString(GetDocumentGuid(), sParticleGuid);
    m_hParticle = plResourceManager::LoadResource<plParticleEffectResource>(sParticleGuid);

    m_pComponent->SetParticleEffect(m_hParticle);
  }

  const char* szMeshName = "ParticlePreviewBackgroundMesh";
  m_hPreviewMeshResource = plResourceManager::GetExistingResource<plMeshResource>(szMeshName);

  if (!m_hPreviewMeshResource.IsValid())
  {
    const char* szMeshBufferName = "ParticlePreviewBackgroundMeshBuffer";

    plMeshBufferResourceHandle hMeshBuffer = plResourceManager::GetExistingResource<plMeshBufferResource>(szMeshBufferName);

    if (!hMeshBuffer.IsValid())
    {
      // Build geometry
      plGeometry geom;

      geom.AddBox(plVec3(4, 4, 4), true);
      geom.ComputeTangents();

      plMeshBufferResourceDescriptor desc;
      desc.AddCommonStreams();
      desc.AllocateStreamsFromGeometry(geom, plGALPrimitiveTopology::Triangles);

      hMeshBuffer = plResourceManager::GetOrCreateResource<plMeshBufferResource>(szMeshBufferName, std::move(desc), szMeshBufferName);
    }
    {
      plResourceLock<plMeshBufferResource> pMeshBuffer(hMeshBuffer, plResourceAcquireMode::AllowLoadingFallback);

      plMeshResourceDescriptor md;
      md.UseExistingMeshBuffer(hMeshBuffer);
      md.AddSubMesh(pMeshBuffer->GetPrimitiveCount(), 0, 0);
      md.SetMaterial(0, "{ 6bd5e7e6-b7be-9801-e032-14226cba1e96 }"); // PrototypeGrey.plMaterialAsset
      md.ComputeBounds();

      m_hPreviewMeshResource = plResourceManager::GetOrCreateResource<plMeshResource>(szMeshName, std::move(md), pMeshBuffer->GetResourceDescription());
    }
  }

  plPhysicsWorldModuleInterface* pPhysicsInterface = GetWorld()->GetOrCreateModule<plPhysicsWorldModuleInterface>();

  // Background Mesh
  {
    plGameObjectDesc obj;
    obj.m_sName.Assign("ParticleBackground");

    const plColor bgColor(0.3f, 0.3f, 0.3f);

    for (int y = -1; y <= 5; ++y)
    {
      for (int x = -5; x <= 5; ++x)
      {
        plGameObject* pObj;
        obj.m_LocalPosition.Set(6, (float)x * 4, 1 + (float)y * 4);
        pWorld->CreateObject(obj, pObj);

        plMeshComponent* pMesh;
        plMeshComponent::CreateComponent(pObj, pMesh);
        pMesh->SetMesh(m_hPreviewMeshResource);
        pMesh->SetColor(bgColor);

        if (pPhysicsInterface)
          pPhysicsInterface->AddStaticCollisionBox(pObj, plVec3(4, 4, 4));
      }
    }

    for (int y = -5; y <= 5; ++y)
    {
      for (int x = -5; x <= 1; ++x)
      {
        plGameObject* pObj;
        obj.m_LocalPosition.Set((float)x * 4, (float)y * 4, -3);
        pWorld->CreateObject(obj, pObj);

        plMeshComponent* pMesh;
        plMeshComponent::CreateComponent(pObj, pMesh);
        pMesh->SetMesh(m_hPreviewMeshResource);
        pMesh->SetColor(bgColor);

        if (pPhysicsInterface)
          pPhysicsInterface->AddStaticCollisionBox(pObj, plVec3(4, 4, 4));
      }
    }

    for (int x = -5; x <= 5; ++x)
    {
      plGameObject* pObj;
      obj.m_LocalPosition.Set(4, (float)x * 4, -2);
      obj.m_LocalRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(45));
      pWorld->CreateObject(obj, pObj);

      plMeshComponent* pMesh;
      plMeshComponent::CreateComponent(pObj, pMesh);
      pMesh->SetMesh(m_hPreviewMeshResource);
      pMesh->SetColor(bgColor);

      if (pPhysicsInterface)
        pPhysicsInterface->AddStaticCollisionBox(pObj, plVec3(4, 4, 4));
    }
  }
}

plEngineProcessViewContext* plParticleContext::CreateViewContext()
{
  return PL_DEFAULT_NEW(plParticleViewContext, this);
}

void plParticleContext::DestroyViewContext(plEngineProcessViewContext* pContext)
{
  PL_DEFAULT_DELETE(pContext);
}

void plParticleContext::OnThumbnailViewContextRequested()
{
  m_ThumbnailBoundingVolume = plBoundingBoxSphere::MakeInvalid();
}

bool plParticleContext::UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext)
{
  plParticleViewContext* pParticleViewContext = static_cast<plParticleViewContext*>(pThumbnailViewContext);

  if (!m_ThumbnailBoundingVolume.IsValid())
  {
    PL_LOCK(m_pWorld->GetWriteMarker());

    // the spy's tint must not be simulated
    m_pPreviewModule->RestoreColors();

    const bool bWorldPaused = m_pWorld->GetClock().GetPaused();

    // make sure the component restarts as soon as possible
    {
      const plTime restartDelay = m_pComponent->m_MinRestartDelay;
      const auto onFinished = m_pComponent->m_OnFinishedAction;
      const double fClockSpeed = m_pWorld->GetClock().GetSpeed();

      m_pComponent->m_MinRestartDelay = plTime::MakeZero();
      m_pComponent->m_OnFinishedAction = plOnComponentFinishedAction2::Restart;

      m_pWorld->SetWorldSimulationEnabled(true);
      m_pWorld->GetClock().SetPaused(false);
      m_pWorld->GetClock().SetSpeed(10);
      m_pWorld->Update();
      m_pWorld->GetClock().SetPaused(bWorldPaused);
      m_pWorld->GetClock().SetSpeed(fClockSpeed);
      m_pWorld->SetWorldSimulationEnabled(false);

      m_pComponent->m_MinRestartDelay = restartDelay;
      m_pComponent->m_OnFinishedAction = onFinished;
    }

    if (m_pComponent && !m_pComponent->m_EffectController.IsAlive())
    {
      // if this happens, the effect has finished and we need to wait for it to restart, so that it can be reconfigured for the screenshot
      // not very clean solution

      pParticleViewContext->PositionThumbnailCamera(m_ThumbnailBoundingVolume);
      return false;
    }

    if (m_pComponent && m_pComponent->m_EffectController.IsAlive())
    {
      // set a fixed random seed
      m_pComponent->m_uiRandomSeed = 11;

      const plUInt32 uiMinSimSteps = 3;
      plUInt32 uiSimStepsNeeded = uiMinSimSteps;

      if (m_pComponent->m_EffectController.IsContinuousEffect())
      {
        uiSimStepsNeeded = 30;
      }
      else
      {
        m_pComponent->InterruptEffect();
        m_pComponent->StartEffect();

        plUInt64 uiMostParticles = 0;
        plUInt64 uiMostParticlesStep = 0;

        for (plUInt32 step = 0; step < 30; ++step)
        {
          // step once, to get the initial bbox out of the way
          m_pComponent->m_EffectController.ForceVisible();
          m_pComponent->m_EffectController.Tick(plTime::MakeFromSeconds(0.05));

          if (!m_pComponent->m_EffectController.IsAlive())
            break;

          const plUInt64 numParticles = m_pComponent->m_EffectController.GetNumActiveParticles();

          if (step == uiMinSimSteps && numParticles > 0)
          {
            uiMostParticles = 0;
          }

          if (numParticles > uiMostParticles)
          {
            // this is the step with the largest number of particles
            // but usually a few steps later is the best step to capture

            uiMostParticles = numParticles;
            uiMostParticlesStep = step;
            uiSimStepsNeeded = step;
          }
          else if ((numParticles > uiMostParticles * 0.8f) && (step < uiMostParticlesStep + 5))
          {
            // if a few steps later we still have a decent amount of particles (so it didn't drop significantly),
            // prefer to use that step
            uiSimStepsNeeded = step;
          }
        }
      }

      m_pComponent->InterruptEffect();
      m_pComponent->StartEffect();

      for (plUInt32 step = 0; step < uiSimStepsNeeded; ++step)
      {
        m_pComponent->m_EffectController.ForceVisible();
        m_pComponent->m_EffectController.Tick(plTime::MakeFromSeconds(0.05));

        if (m_pComponent->m_EffectController.IsAlive())
        {
          m_pComponent->m_EffectController.GetBoundingVolume(m_ThumbnailBoundingVolume);

          // shrink the bbox to zoom in
          m_ThumbnailBoundingVolume.m_fSphereRadius *= 0.7f;
          m_ThumbnailBoundingVolume.m_vBoxHalfExtends *= 0.7f;
        }
      }

      m_pComponent->m_uiRandomSeed = s_uiPreviewSeed;

      // tick the world once more, so that the effect passes on its bounding box to the culling system
      // otherwise the effect is not rendered, when only the thumbnail is updated, but the document is not open
      m_pWorld->SetWorldSimulationEnabled(true);
      m_pWorld->GetClock().SetPaused(true);
      m_pWorld->Update();
      m_pWorld->GetClock().SetPaused(bWorldPaused);
      m_pWorld->SetWorldSimulationEnabled(false);
    }
  }

  pParticleViewContext->PositionThumbnailCamera(m_ThumbnailBoundingVolume);
  return true;
}

void plParticleContext::RestartEffect()
{
  PL_LOCK(m_pWorld->GetWriteMarker());

  if (m_pComponent)
  {
    m_pComponent->InterruptEffect();
    m_pComponent->StartEffect();
  }
}

void plParticleContext::SetAutoRestartEffect(bool loop)
{
  PL_LOCK(m_pWorld->GetWriteMarker());

  if (m_pComponent)
  {
    m_pComponent->m_OnFinishedAction = loop ? plOnComponentFinishedAction2::Restart : plOnComponentFinishedAction2::None;
  }
}
