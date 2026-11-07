#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/World/World.h>
#include <Foundation/DataProcessing/Stream/DefaultImplementations/ZeroInitializer.h>
#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/DataProcessing/Stream/ProcessingStreamProcessor.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Emitter/ParticleEmitter.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_OnEvent.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>
#include <ParticlePlugin/Streams/ParticleStream.h>
#include <ParticlePlugin/System/ParticleFrameProgram.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/GPU/ParticleGPULowering.h>
#include <ParticlePlugin/Type/GPU/ParticleTypeGPU.h>
#include <ParticlePlugin/Type/ParticleType.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

bool plParticleSystemInstance::HasActiveParticles() const
{
  if (m_StreamGroup.GetNumActiveElements() > 0)
    return true;

  for (auto pType : m_Types)
  {
    if (pType->HasActiveGPUElements())
      return true;
  }

  return false;
}

bool plParticleSystemInstance::IsEmitterConfigEqual(const plParticleSystemDescriptor* pTemplate) const
{
  const auto& factories = pTemplate->GetEmitterFactories();

  if (factories.GetCount() != m_Emitters.GetCount())
    return false;

  for (plUInt32 i = 0; i < factories.GetCount(); ++i)
  {
    if (factories[i]->GetEmitterType() != m_Emitters[i]->GetDynamicRTTI())
      return false;
  }

  return true;
}

bool plParticleSystemInstance::IsInitializerConfigEqual(const plParticleSystemDescriptor* pTemplate) const
{
  const auto& factories = pTemplate->GetInitializerFactories();

  if (factories.GetCount() != m_Initializers.GetCount())
    return false;

  for (plUInt32 i = 0; i < factories.GetCount(); ++i)
  {
    if (factories[i]->GetInitializerType() != m_Initializers[i]->GetDynamicRTTI())
      return false;
  }

  return true;
}

bool plParticleSystemInstance::IsBehaviorConfigEqual(const plParticleSystemDescriptor* pTemplate) const
{
  const auto& factories = pTemplate->GetBehaviorFactories();

  if (factories.GetCount() != m_Behaviors.GetCount())
    return false;

  for (plUInt32 i = 0; i < factories.GetCount(); ++i)
  {
    if (factories[i]->GetBehaviorType() != m_Behaviors[i]->GetDynamicRTTI())
      return false;
  }

  return true;
}

bool plParticleSystemInstance::IsTypeConfigEqual(const plParticleSystemDescriptor* pTemplate) const
{
  const auto& factories = pTemplate->GetTypeFactories();

  if (factories.GetCount() != m_Types.GetCount())
    return false;

  for (plUInt32 i = 0; i < factories.GetCount(); ++i)
  {
    if (factories[i]->GetTypeType() != m_Types[i]->GetDynamicRTTI())
      return false;
  }

  return true;
}

bool plParticleSystemInstance::IsFinalizerConfigEqual(const plParticleSystemDescriptor* pTemplate) const
{
  const auto& factories = pTemplate->GetFinalizerFactories();

  if (factories.GetCount() != m_Finalizers.GetCount())
    return false;

  for (plUInt32 i = 0; i < factories.GetCount(); ++i)
  {
    if (factories[i]->GetFinalizerType() != m_Finalizers[i]->GetDynamicRTTI())
      return false;
  }

  return true;
}

void plParticleSystemInstance::ConfigureFromTemplate(const plParticleSystemDescriptor* pTemplate)
{
  m_bVisible = pTemplate->m_bVisible;

  for (auto& info : m_StreamInfo)
  {
    info.m_bGetsInitialized = false;
    info.m_bInUse = false;
  }

  const bool bWasLoweredToGPU = m_bLoweredToGPU;

  // decide whether the per-frame simulation is lowered onto the GPU
  {
    bool bLower = false;

    if (pTemplate->m_SimulationTarget != plParticleSimulationTarget::CPU)
    {
      // a system already authored with the GPU type needs no lowering
      bool bAlreadyGPU = false;
      for (const auto* pTypeFactory : pTemplate->GetTypeFactories())
      {
        bAlreadyGPU |= pTypeFactory->GetTypeType() == plGetStaticRTTI<plParticleTypeGPU>();
      }

      if (!bAlreadyGPU)
      {
        const plString sBlocker = plParticleGPULowering::FindLoweringBlocker(pTemplate);

        if (sBlocker.IsEmpty())
        {
          bLower = true;
        }
        else if (pTemplate->m_SimulationTarget == plParticleSimulationTarget::GPU)
        {
          plLog::Warning("Particle system cannot run on the GPU ({0}) - simulating on the CPU instead.", sBlocker);
        }
      }
    }

    m_bLoweredToGPU = bLower;
  }

  // when lowered, the instance module lists never mirror the factory lists, so the equality checks
  // below fail and the processors are fully recreated. Switching from CPU to GPU is the exception:
  // the CPU modules still match the factories, so the switch itself forces the recreation.
  const bool allProcessorsEqual = bWasLoweredToGPU == m_bLoweredToGPU && IsEmitterConfigEqual(pTemplate) && IsInitializerConfigEqual(pTemplate) && IsBehaviorConfigEqual(pTemplate) && IsTypeConfigEqual(pTemplate) && IsFinalizerConfigEqual(pTemplate);

  if (!allProcessorsEqual)
  {
    // recreate emitters, initializers and behaviors
    CreateStreamProcessors(pTemplate);
  }
  else
  {
    // just re-initialize the emitters with the new properties
    ReinitializeStreamProcessors(pTemplate);
  }

  SetupOptionalStreams();

  // setup stream initializers for all streams that have none yet
  CreateStreamZeroInitializers();

  // the per-frame program writes into the modules, so it is set up after they exist
  if (pTemplate->m_FrameProgram.IsEmpty())
  {
    m_pFrameProgram.Clear();
  }
  else
  {
    if (m_pFrameProgram == nullptr)
    {
      m_pFrameProgram = PL_DEFAULT_NEW(plParticleFrameProgramRunner);
    }

    m_pFrameProgram->Configure(pTemplate, this, m_bLoweredToGPU);
  }
}


void plParticleSystemInstance::Finalize()
{
  for (auto& pEmitter : m_Emitters)
  {
    pEmitter->OnFinalize();
  }

  for (auto& pInitializer : m_Initializers)
  {
    pInitializer->OnFinalize();
  }

  for (auto& pBehavior : m_Behaviors)
  {
    pBehavior->OnFinalize();
  }

  for (auto& pFinalizer : m_Finalizers)
  {
    pFinalizer->OnFinalize();
  }

  for (auto& pType : m_Types)
  {
    pType->OnFinalize();
  }
}

void plParticleSystemInstance::CreateStreamProcessors(const plParticleSystemDescriptor* pTemplate)
{
  // all spawners get cleared, so clear this as well
  // this has to be done before any streams get created
  // for (auto& info : m_StreamInfo)
  //{
  //  info.m_pZeroInitializer = nullptr;
  //}


  const plUInt64 uiMaxParticles = m_StreamGroup.GetNumElements();
  m_StreamGroup.Clear();
  m_StreamGroup.SetSize(uiMaxParticles);
  m_StreamInfo.Clear();

  // emitters
  {
    m_Emitters.Clear();

    for (const auto pFactory : pTemplate->GetEmitterFactories())
    {
      plParticleEmitter* pEmitter = pFactory->CreateEmitter(this);
      m_StreamGroup.AddProcessor(pEmitter);
      m_Emitters.PushBack(pEmitter);
    }

    // the Event inputs of the particles On Event spawns
    m_pEventInputs = nullptr;

    for (const plParticleEmitter* pEmitter : m_Emitters)
    {
      if (pEmitter->IsInstanceOf<plParticleEmitter_OnEvent>())
      {
        m_pEventInputs = plGetStaticRTTI<plParticleEventInputs>()->GetAllocator()->Allocate<plParticleEventInputs>();
        m_pEventInputs->Reset(this);
        m_StreamGroup.AddProcessor(m_pEventInputs);
        break;
      }
    }
  }

  // initializers
  {
    m_Initializers.Clear();

    for (const auto pFactory : pTemplate->GetInitializerFactories())
    {
      plParticleInitializer* pInitializer = pFactory->CreateInitializer(this);
      m_StreamGroup.AddProcessor(pInitializer);
      m_Initializers.PushBack(pInitializer);
    }
  }

  // behaviors
  {
    m_Behaviors.Clear();

    // when lowered to the GPU, the behaviors run inside the GPU simulate shader instead
    if (!m_bLoweredToGPU)
    {
      for (const auto pFactory : pTemplate->GetBehaviorFactories())
      {
        plParticleBehavior* pBehavior = pFactory->CreateBehavior(this);
        m_StreamGroup.AddProcessor(pBehavior);
        m_Behaviors.PushBack(pBehavior);
      }
    }
  }

  // finalizers
  {
    m_Finalizers.Clear();

    for (const auto pFactory : pTemplate->GetFinalizerFactories())
    {
      plParticleFinalizer* pFinalizer = pFactory->CreateFinalizer(this);
      m_StreamGroup.AddProcessor(pFinalizer);
      m_Finalizers.PushBack(pFinalizer);
    }
  }

  // types
  {
    m_Types.Clear();

    if (m_bLoweredToGPU)
    {
      // replace the authored render type with a GPU type configured from the behavior modules
      plParticleTypeGPU* pType = plGetStaticRTTI<plParticleTypeGPU>()->GetAllocator()->Allocate<plParticleTypeGPU>();
      pType->Reset(this);
      plParticleGPULowering::ConfigureLoweredType(pType, pTemplate, this);
      pType->CreateRequiredStreams();

      m_StreamGroup.AddProcessor(pType);
      m_Types.PushBack(pType);
    }
    else
    {
      for (const auto pFactory : pTemplate->GetTypeFactories())
      {
        plParticleType* pType = pFactory->CreateType(this);
        m_StreamGroup.AddProcessor(pType);
        m_Types.PushBack(pType);
      }
    }
  }
}


void plParticleSystemInstance::SetupOptionalStreams()
{
  for (auto& pEmitter : m_Emitters)
  {
    pEmitter->QueryOptionalStreams();
  }

  if (m_pEventInputs != nullptr)
  {
    m_pEventInputs->QueryOptionalStreams();
  }

  for (auto& pInitializer : m_Initializers)
  {
    pInitializer->QueryOptionalStreams();
  }

  for (auto& pBehavior : m_Behaviors)
  {
    pBehavior->QueryOptionalStreams();
  }

  for (auto& pFinalizer : m_Finalizers)
  {
    pFinalizer->QueryOptionalStreams();
  }

  for (auto& pType : m_Types)
  {
    pType->QueryOptionalStreams();
  }
}

void plParticleSystemInstance::SetTransform(const plTransform& transform, const plVec3& vParticleStartVelocity)
{
  m_Transform = transform;
  m_vParticleStartVelocity = vParticleStartVelocity;
}

void plParticleSystemInstance::ReinitializeStreamProcessors(const plParticleSystemDescriptor* pTemplate)
{
  // emitters
  {
    const auto& factories = pTemplate->GetEmitterFactories();

    for (plUInt32 i = 0; i < factories.GetCount(); ++i)
    {
      m_Emitters[i]->Reset(this);
      factories[i]->CopyEmitterProperties(m_Emitters[i], false);
      m_Emitters[i]->CreateRequiredStreams();
    }
  }

  // initializers
  {
    const auto& factories = pTemplate->GetInitializerFactories();

    for (plUInt32 i = 0; i < factories.GetCount(); ++i)
    {
      m_Initializers[i]->Reset(this);
      factories[i]->CopyInitializerProperties(m_Initializers[i], false);
      m_Initializers[i]->CreateRequiredStreams();
    }
  }

  // behaviors
  {
    const auto& factories = pTemplate->GetBehaviorFactories();

    for (plUInt32 i = 0; i < factories.GetCount(); ++i)
    {
      m_Behaviors[i]->Reset(this);
      factories[i]->CopyBehaviorProperties(m_Behaviors[i], false);
      m_Behaviors[i]->CreateRequiredStreams();
    }
  }

  // finalizers
  {
    const auto& factories = pTemplate->GetFinalizerFactories();

    for (plUInt32 i = 0; i < factories.GetCount(); ++i)
    {
      m_Finalizers[i]->Reset(this);
      factories[i]->CopyFinalizerProperties(m_Finalizers[i], false);
      m_Finalizers[i]->CreateRequiredStreams();
    }
  }

  // types
  {
    const auto& factories = pTemplate->GetTypeFactories();

    for (plUInt32 i = 0; i < factories.GetCount(); ++i)
    {
      m_Types[i]->Reset(this);
      factories[i]->CopyTypeProperties(m_Types[i], false);
      m_Types[i]->CreateRequiredStreams();
    }
  }
}

plParticleSystemInstance::plParticleSystemInstance()
{
  m_BoundingVolume = plBoundingBoxSphere::MakeInvalid();
}

plParticleSystemInstance::~plParticleSystemInstance() = default;

void plParticleSystemInstance::Construct(plUInt32 uiMaxParticles, plWorld* pWorld, plParticleEffectInstance* pOwnerEffect, float fSpawnCountMultiplier)
{
  m_BoundingVolume = plBoundingBoxSphere::MakeInvalid();
  m_Transform.SetIdentity();
  m_pOwnerEffect = pOwnerEffect;
  m_bEmitterEnabled = true;
  m_bVisible = true;
  m_pWorld = pWorld;
  m_fSpawnCountMultiplier = fSpawnCountMultiplier;

  m_StreamInfo.Clear();
  m_StreamGroup.SetSize(uiMaxParticles);
}

void plParticleSystemInstance::Destruct()
{
  // its factory copies point at the modules cleared below
  m_pFrameProgram.Clear();

  m_StreamGroup.Clear();
  m_Emitters.Clear();
  m_pEventInputs = nullptr;
  m_Initializers.Clear();
  m_Behaviors.Clear();
  m_Finalizers.Clear();
  m_Types.Clear();

  m_StreamInfo.Clear();
}

plParticleSystemState::Enum plParticleSystemInstance::Update(const plTime& diff)
{
  PL_PROFILE_SCOPE("PFX: System Update");

  const plTime tStart = plTime::Now();
  PL_SCOPE_EXIT(m_LastUpdateDuration = plTime::Now() - tStart);

  m_uiLastEventsRaised = m_uiEventsRaised;
  m_uiLastEventsDropped = m_uiEventsDropped;
  m_uiEventsRaised = 0;
  m_uiEventsDropped = 0;

  // per-frame graph inputs: every module of this step sees the values computed from them
  if (m_pFrameProgram != nullptr)
  {
    m_pFrameProgram->Evaluate(diff);
  }

  plUInt32 uiSpawnedParticles = 0;

  if (m_bEmitterEnabled)
  {
    // if all emitters are finished, we deactivate this on the whole system
    bool bAllEmittersInactive = true;

    for (auto pEmitter : m_Emitters)
    {
      if (pEmitter->IsFinished() == plParticleEmitterState::Active)
      {
        bAllEmittersInactive = false;
        const plUInt32 uiSpawn = pEmitter->ComputeSpawnCount(diff);

        if (uiSpawn > 0)
        {
          PL_PROFILE_SCOPE("PFX: System Emit");
          m_StreamGroup.InitializeElements(uiSpawn);
          uiSpawnedParticles += uiSpawn;
        }
      }
    }

    if (bAllEmittersInactive)
    {
      // there is a race condition writing this variable when an effect is used by a plParticleTypeEffect and should be disabled
      // therefore we must never set this variable to 'true' here, but we can set it to 'false' once we are done
      m_bEmitterEnabled = false;
    }
  }

  bool bHasReactingEmitters = false;

  // always check reactive emitters, as long as there are particles alive, they might produce more
  {
    for (auto pEmitter : m_Emitters)
    {
      if (pEmitter->IsFinished() == plParticleEmitterState::OnlyReacting)
      {
        bHasReactingEmitters = true;

        const plUInt32 uiSpawn = pEmitter->ComputeSpawnCount(diff);

        if (uiSpawn > 0)
        {
          PL_PROFILE_SCOPE("PFX: System Emit (React)");
          m_StreamGroup.InitializeElements(uiSpawn);
          uiSpawnedParticles += uiSpawn;
        }

        // per event, the batches are spawned after the processing below
        if (const plParticleEmitter_OnEvent* pOnEvent = plDynamicCast<const plParticleEmitter_OnEvent*>(pEmitter))
        {
          for (const plParticleEmitter_OnEvent::Batch& batch : pOnEvent->GetBatches())
          {
            uiSpawnedParticles += batch.m_uiCount;
          }
        }
      }
    }
  }

  {
    PL_PROFILE_SCOPE("PFX: System Step Behaviors");
    for (auto pBehavior : m_Behaviors)
    {
      pBehavior->StepParticleSystem(diff, uiSpawnedParticles);
    }
  }

  {
    PL_PROFILE_SCOPE("PFX: System Step Finalizers");
    for (auto pFinalizer : m_Finalizers)
    {
      pFinalizer->StepParticleSystem(diff, uiSpawnedParticles);
    }
  }

  {
    PL_PROFILE_SCOPE("PFX: System Step Types");
    for (auto pType : m_Types)
    {
      pType->StepParticleSystem(diff, uiSpawnedParticles);
    }
  }

  {
    PL_PROFILE_SCOPE("PFX: System Process");
    m_StreamGroup.Process();
  }

  // like the step's other spawns, these come after the processing, so they live at least one frame
  SpawnEventBatches();

  if (m_bEmitterEnabled)
    return plParticleSystemState::Active;

  // all emitters are done, but some particles are still alive
  if (HasActiveParticles())
    return plParticleSystemState::EmittersFinished;

  return bHasReactingEmitters ? plParticleSystemState::OnlyReacting : plParticleSystemState::Inactive;
}

plProcessingStream* plParticleSystemInstance::QueryStream(const char* szName, plProcessingStream::DataType type) const
{
  return m_StreamGroup.GetStreamByName(szName);
}

void plParticleSystemInstance::CreateStream(const char* szName, plProcessingStream::DataType type, plProcessingStream** pStream, plParticleStreamBinding& inout_binding, bool bWillInitializeElements)
{
  PL_ASSERT_DEV(pStream != nullptr, "The pointer to the stream pointer must not be null");

  StreamInfo* pInfo = nullptr;

  plProcessingStream* pSubStream = m_StreamGroup.GetStreamByName(szName);
  if (pSubStream == nullptr)
  {
    pSubStream = m_StreamGroup.AddStream(szName, type);

    pInfo = &m_StreamInfo.ExpandAndGetRef();
    pInfo->m_sName = szName;
  }
  else
  {
    // A type mismatch between two creators of the same stream makes every consumer of the smaller type
    // read/write out of bounds (each caller casts the data blindly). Surface it loudly in ALL builds,
    // because in release this otherwise silently corrupts memory.
    if (pSubStream->GetDataType() != type)
    {
      plLog::Error("Particle stream '{}' is requested with data type '{}' but already exists with data type '{}'. Fix the module that requests the wrong type - this would corrupt memory.",
        szName, plProcessingStream::GetDataTypeName(type), plProcessingStream::GetDataTypeName(pSubStream->GetDataType()));
      PL_ASSERT_DEV(false, "Particle stream '{}' type mismatch (see error log)", szName);
    }

    for (auto& info : m_StreamInfo)
    {
      if (info.m_sName == szName)
      {
        pInfo = &info;
        break;
      }
    }

    PL_ASSERT_DEV(pInfo != nullptr, "Could not find info for stream");
  }

  pInfo->m_bInUse = true;
  if (bWillInitializeElements)
    pInfo->m_bGetsInitialized = true;

  PL_ASSERT_DEV(pSubStream != nullptr, "Stream creation failed ('{0}')", szName);
  *pStream = pSubStream;

  {
    auto& bind = inout_binding.m_Bindings.ExpandAndGetRef();
    bind.m_ppStream = pStream;
    bind.m_sName = szName;
  }
}

void plParticleSystemInstance::CreateStreamZeroInitializers()
{
  for (plUInt32 i = 0; i < m_StreamInfo.GetCount();)
  {
    auto& info = m_StreamInfo[i];

    if ((!info.m_bInUse || info.m_bGetsInitialized) && info.m_pDefaultInitializer)
    {
      m_StreamGroup.RemoveProcessor(info.m_pDefaultInitializer);
      info.m_pDefaultInitializer = nullptr;
    }

    if (!info.m_bInUse)
    {
      m_StreamGroup.RemoveStreamByName(info.m_sName.GetData());
      m_StreamInfo.RemoveAtAndSwap(i);
    }
    else
    {
      ++i;
    }
  }

  for (auto& info : m_StreamInfo)
  {
    if (info.m_bGetsInitialized)
      continue;

    PL_ASSERT_DEV(info.m_bInUse, "Invalid state");

    if (info.m_pDefaultInitializer == nullptr)
    {
      plParticleStream* pStream = GetOwnerWorldModule()->CreateStreamDefaultInitializer(this, info.m_sName);

      if (pStream == nullptr)
      {
        // custom attributes from the graph start at zero by design
        if (!info.m_sName.StartsWith("Custom."))
          plLog::Warning("Particle stream '{0}' is zero-initialized.", info.m_sName);

        plProcessingStreamSpawnerZeroInitialized* pZeroInit = PL_DEFAULT_NEW(plProcessingStreamSpawnerZeroInitialized);
        pZeroInit->SetStreamName(info.m_sName);

        // with the stream default initializers, before any initializer writes the new particles
        pZeroInit->m_fPriority = -1000.0f;

        info.m_pDefaultInitializer = pZeroInit;
      }
      else
      {
        // plLog::Debug("Particle stream '{0}' is default-initialized.", info.m_sName);
        info.m_pDefaultInitializer = pStream;
      }

      m_StreamGroup.AddProcessor(info.m_pDefaultInitializer);
    }
  }
}

void plParticleStreamBinding::UpdateBindings(const plProcessingStreamGroup* pGroup) const
{
  for (const auto& bind : m_Bindings)
  {
    plProcessingStream* pStream = pGroup->GetStreamByName(bind.m_sName);
    PL_ASSERT_DEV(pStream != nullptr, "Stream binding '{0}' is invalid now", bind.m_sName);

    *bind.m_ppStream = pStream;
  }
}

void plParticleSystemInstance::ProcessEventQueue(plParticleEventQueue queue)
{
  for (auto pEmitter : m_Emitters)
  {
    pEmitter->ProcessEventQueue(queue);
  }
}

void plParticleSystemInstance::RaiseEvent(const plParticleEvent& e)
{
  ++m_uiEventsRaised;
  ++m_uiTotalEventsRaised;

  if (!m_pOwnerEffect->AddParticleEvent(e))
  {
    ++m_uiEventsDropped;
    ++m_uiTotalEventsDropped;
  }
}

void plParticleSystemInstance::CountDroppedEvents(plUInt32 uiCount)
{
  m_uiEventsRaised += uiCount;
  m_uiEventsDropped += uiCount;
  m_uiTotalEventsRaised += uiCount;
  m_uiTotalEventsDropped += uiCount;
  m_pOwnerEffect->CountDroppedEvents(uiCount);
}

void plParticleSystemInstance::SpawnEventBatches()
{
  for (plParticleEmitter* pEmitter : m_Emitters)
  {
    plParticleEmitter_OnEvent* pOnEvent = plDynamicCast<plParticleEmitter_OnEvent*>(pEmitter);

    if (pOnEvent == nullptr || pOnEvent->GetBatches().IsEmpty())
      continue;

    PL_PROFILE_SCOPE("PFX: System Emit (Events)");

    // the initializers place particles around the system's position, so for each batch that is the event's
    const plTransform transform = m_Transform;
    m_pSpawnEmitter = pOnEvent;

    for (const plParticleEmitter_OnEvent::Batch& batch : pOnEvent->GetBatches())
    {
      m_pSpawnEvent = &batch.m_Event;
      m_Transform.m_vPosition = batch.m_Event.m_vPosition;

      m_StreamGroup.InitializeElements(batch.m_uiCount);
      m_StreamGroup.SpawnPendingElements();
    }

    m_Transform = transform;
    m_pSpawnEvent = nullptr;
    m_pSpawnEmitter = nullptr;

    pOnEvent->ClearBatches();
  }
}

plParticleWorldModule* plParticleSystemInstance::GetOwnerWorldModule() const
{
  return m_pOwnerEffect->GetOwnerWorldModule();
}

void plParticleSystemInstance::ExtractSystemRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const
{
  for (auto pType : m_Types)
  {
    pType->ExtractTypeRenderData(ref_msg, instanceTransform);
  }
}

void plParticleSystemInstance::AddParticleDeathEventHandler(ParticleDeathHandler handler)
{
  m_StreamGroup.m_ElementRemovedEvent.AddEventHandler(handler);
}

void plParticleSystemInstance::RemoveParticleDeathEventHandler(ParticleDeathHandler handler)
{
  m_StreamGroup.m_ElementRemovedEvent.RemoveEventHandler(handler);
}

void plParticleSystemInstance::SetBoundingVolume(const plBoundingBoxSphere& volume, float fMaxParticleSize)
{
  m_BoundingVolume = volume;

  float fExpand = 0;
  for (const auto pType : m_Types)
  {
    fExpand = plMath::Max(fExpand, pType->GetMaxParticleRadius(fMaxParticleSize));
  }

  m_BoundingVolume.m_vBoxHalfExtends += plVec3(fExpand);
  m_BoundingVolume.m_fSphereRadius += fExpand;
}

bool plParticleSystemInstance::IsContinuous() const
{
  for (const plParticleEmitter* pEmitter : m_Emitters)
  {
    if (pEmitter->IsContinuous())
      return true;
  }

  return false;
}
