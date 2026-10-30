#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Curves/Curve1DResource.h>
#include <Foundation/DataProcessing/Stream/ProcessingStreamGroup.h>
#include <Foundation/Math/Color16f.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Serialization/GraphPatch.h>
#include <Foundation/Time/Clock.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_OnEvent.h>
#include <ParticlePlugin/Events/ParticleEvent.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleOnEventSpawn, 1)
  PL_ENUM_CONSTANTS(plParticleOnEventSpawn::PerEvent, plParticleOnEventSpawn::OncePerFrame)
PL_END_STATIC_REFLECTED_ENUM;

// version 2: Spawn and the Inherit properties; documents of version 1 are patched to Once Per Frame
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEmitterFactory_OnEvent, 2, plRTTIDefaultAllocator<plParticleEmitterFactory_OnEvent>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("EventName", m_sEventName)->AddAttributes(new plDynamicStringEnumAttribute("ParticleEventNamesEnum")),
    PL_ENUM_MEMBER_PROPERTY("Spawn", plParticleOnEventSpawn, m_SpawnMode),
    PL_MEMBER_PROPERTY("MinSpawnCount", m_uiSpawnCountMin)->AddAttributes(new plDefaultValueAttribute(1)),
    PL_MEMBER_PROPERTY("SpawnCountRange", m_uiSpawnCountRange),
    PL_MEMBER_PROPERTY("SpawnCountScaleParam", m_sSpawnCountScaleParameter),
    PL_MEMBER_PROPERTY("InheritVelocity", m_fInheritVelocity)->AddAttributes(new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("InheritColor", m_bInheritColor),
    PL_MEMBER_PROPERTY("InheritSize", m_bInheritSize),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEmitter_OnEvent, 1, plRTTIDefaultAllocator<plParticleEmitter_OnEvent>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEventInputs, 1, plRTTIDefaultAllocator<plParticleEventInputs>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleEmitterFactory_OnEvent::plParticleEmitterFactory_OnEvent() = default;
plParticleEmitterFactory_OnEvent::~plParticleEmitterFactory_OnEvent() = default;

const plRTTI* plParticleEmitterFactory_OnEvent::GetEmitterType() const
{
  return plGetStaticRTTI<plParticleEmitter_OnEvent>();
}

void plParticleEmitterFactory_OnEvent::CopyEmitterProperties(plParticleEmitter* pEmitter0, bool bFirstTime) const
{
  plParticleEmitter_OnEvent* pEmitter = static_cast<plParticleEmitter_OnEvent*>(pEmitter0);

  pEmitter->m_sEventName = plTempHashedString(m_sEventName.GetData());

  pEmitter->m_uiSpawnCountMin = (plUInt32)(m_uiSpawnCountMin * pEmitter->GetOwnerSystem()->GetSpawnCountMultiplier());
  pEmitter->m_uiSpawnCountRange = (plUInt32)(m_uiSpawnCountRange * pEmitter->GetOwnerSystem()->GetSpawnCountMultiplier());

  pEmitter->m_sSpawnCountScaleParameter = plTempHashedString(m_sSpawnCountScaleParameter.GetData());

  pEmitter->m_SpawnMode = m_SpawnMode;
  pEmitter->m_fInheritVelocity = m_fInheritVelocity;
  pEmitter->m_bInheritColor = m_bInheritColor;
  pEmitter->m_bInheritSize = m_bInheritSize;
}

void plParticleEmitterFactory_OnEvent::QueryMaxParticleCount(plUInt32& out_uiMaxParticlesAbs, plUInt32& out_uiMaxParticlesPerSecond) const
{
  out_uiMaxParticlesAbs = 0;

  // a guess: per event, a dense spray fills a good part of the effect's event queue every second
  const plUInt32 uiEventsPerSecond = m_SpawnMode == plParticleOnEventSpawn::PerEvent ? 256 : 16;
  out_uiMaxParticlesPerSecond = (m_uiSpawnCountMin + m_uiSpawnCountRange) * uiEventsPerSecond;

  // TODO: consider to scale by m_sSpawnCountScaleParameter
}

enum class EmitterOnEventVersion
{
  Version_0 = 0,
  Version_1,
  Version_2,
  Version_3, // Spawn mode and the Inherit properties

  // insert new version numbers above
  Version_Count,
  Version_Current = Version_Count - 1
};


void plParticleEmitterFactory_OnEvent::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = (int)EmitterOnEventVersion::Version_Current;
  inout_stream << uiVersion;

  // Version 1
  inout_stream << m_sEventName;

  // Version 2
  inout_stream << m_uiSpawnCountMin;
  inout_stream << m_uiSpawnCountRange;
  inout_stream << m_sSpawnCountScaleParameter;

  // Version 3
  inout_stream << m_SpawnMode;
  inout_stream << m_fInheritVelocity;
  inout_stream << m_bInheritColor;
  inout_stream << m_bInheritSize;
}

void plParticleEmitterFactory_OnEvent::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= (int)EmitterOnEventVersion::Version_Current, "Invalid version {0}", uiVersion);

  inout_stream >> m_sEventName;

  if (uiVersion >= 2)
  {
    inout_stream >> m_uiSpawnCountMin;
    inout_stream >> m_uiSpawnCountRange;
    inout_stream >> m_sSpawnCountScaleParameter;
  }

  if (uiVersion >= 3)
  {
    inout_stream >> m_SpawnMode;
    inout_stream >> m_fInheritVelocity;
    inout_stream >> m_bInheritColor;
    inout_stream >> m_bInheritSize;
  }
  else
  {
    // effects built before 1.6 spawned once per frame; they keep their look
    m_SpawnMode = plParticleOnEventSpawn::OncePerFrame;
  }
}

plParticleEmitter_OnEvent::plParticleEmitter_OnEvent()
{
  // after the Initialize blocks, so the inherited values build on what they wrote, and before the
  // render types, which take the finished particles (a GPU system packs them there)
  m_fPriority = 999.0f;
}

void plParticleEmitter_OnEvent::CreateRequiredStreams()
{
  m_pStreamVelocity = nullptr;
  m_pStreamColor = nullptr;
  m_pStreamSize = nullptr;

  if (m_SpawnMode != plParticleOnEventSpawn::PerEvent)
    return;

  if (m_fInheritVelocity != 0.0f)
    CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);

  if (m_bInheritColor)
    CreateStream("Color", plProcessingStream::DataType::Half4, &m_pStreamColor, false);

  if (m_bInheritSize)
    CreateStream("Size", plProcessingStream::DataType::Half, &m_pStreamSize, false);
}

void plParticleEmitter_OnEvent::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  const plParticleEvent* pEvent = GetOwnerSystem()->GetSpawnEvent();

  if (pEvent == nullptr || GetOwnerSystem()->GetSpawnEmitter() != this)
    return;

  const plUInt64 uiEnd = uiStartIndex + uiNumElements;

  if (m_pStreamVelocity != nullptr)
  {
    plVec3* pVelocity = m_pStreamVelocity->GetWritableData<plVec3>();
    const plVec3 vInherited = pEvent->m_vVelocity * m_fInheritVelocity;

    for (plUInt64 i = uiStartIndex; i < uiEnd; ++i)
    {
      pVelocity[i] += vInherited;
    }
  }

  if (m_pStreamColor != nullptr)
  {
    plColorLinear16f* pColor = m_pStreamColor->GetWritableData<plColorLinear16f>();

    for (plUInt64 i = uiStartIndex; i < uiEnd; ++i)
    {
      pColor[i] = pColor[i].ToLinearFloat() * pEvent->m_Color;
    }
  }

  if (m_pStreamSize != nullptr)
  {
    plFloat16* pSize = m_pStreamSize->GetWritableData<plFloat16>();

    for (plUInt64 i = uiStartIndex; i < uiEnd; ++i)
    {
      pSize[i] = (float)pSize[i] * pEvent->m_fSize;
    }
  }
}

plParticleEmitterState plParticleEmitter_OnEvent::IsFinished()
{
  return plParticleEmitterState::OnlyReacting;
}

plUInt32 plParticleEmitter_OnEvent::ComputeSpawnCount(const plTime& tDiff)
{
  if (m_SpawnMode == plParticleOnEventSpawn::PerEvent)
  {
    // the system spawns the batches itself, one event at a time, after the step's processing
    m_bSpawn = false;
    ComputeEventSpawnCount();
    return 0;
  }

  m_uiLastEventCount = m_PendingEvents.GetCount();
  m_uiTotalEventCount += m_uiLastEventCount;
  m_PendingEvents.Clear();
  m_uiLastSpawnCount = 0;

  if (!m_bSpawn)
    return 0;

  m_bSpawn = false;

  const float fSpawnFactor = GetSpawnCountScale(m_sSpawnCountScaleParameter);

  m_uiLastSpawnCount = static_cast<plUInt32>((m_uiSpawnCountMin + GetRNG().UIntInRange(1 + m_uiSpawnCountRange)) * fSpawnFactor);
  m_uiTotalSpawnCount += m_uiLastSpawnCount;
  return m_uiLastSpawnCount;
}

plUInt32 plParticleEmitter_OnEvent::ComputeEventSpawnCount()
{
  m_Batches.Clear();

  const float fSpawnFactor = GetSpawnCountScale(m_sSpawnCountScaleParameter);
  plRandom& rng = GetRNG();
  plUInt32 uiTotal = 0;

  for (const plParticleEvent& e : m_PendingEvents)
  {
    const plUInt32 uiCount = static_cast<plUInt32>((m_uiSpawnCountMin + rng.UIntInRange(1 + m_uiSpawnCountRange)) * fSpawnFactor);

    if (uiCount == 0)
      continue;

    Batch& batch = m_Batches.ExpandAndGetRef();
    batch.m_Event = e;
    batch.m_uiCount = uiCount;
    uiTotal += uiCount;
  }

  m_uiLastEventCount = m_PendingEvents.GetCount();
  m_uiLastSpawnCount = uiTotal;
  m_uiTotalEventCount += m_uiLastEventCount;
  m_uiTotalSpawnCount += uiTotal;
  m_PendingEvents.Clear();

  return uiTotal;
}

void plParticleEmitter_OnEvent::ProcessEventQueue(plParticleEventQueue queue)
{
  for (const plParticleEvent& e : queue)
  {
    if (e.m_EventType == m_sEventName) // this is the event type we are waiting for!
    {
      m_bSpawn = true;

      // once per frame only counts them, for the stats
      m_PendingEvents.PushBack(e);
    }
  }
}

plParticleEventInputs::plParticleEventInputs()
{
  // after the default initializers (which zero these streams) and before the Initialize blocks
  m_fPriority = -900.0f;
}

void plParticleEventInputs::QueryOptionalStreams()
{
  // only the streams some program reads exist
  m_pStreamPosition = GetOwnerSystem()->QueryStream("EventPosition", plProcessingStream::DataType::Float3);
  m_pStreamVelocity = GetOwnerSystem()->QueryStream("EventVelocity", plProcessingStream::DataType::Float3);
  m_pStreamNormal = GetOwnerSystem()->QueryStream("EventNormal", plProcessingStream::DataType::Float3);
}

void plParticleEventInputs::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  const plParticleEvent* pEvent = GetOwnerSystem()->GetSpawnEvent();

  if (pEvent == nullptr)
    return;

  auto Fill = [&](plProcessingStream* pStream, const plVec3& vValue)
  {
    if (pStream == nullptr)
      return;

    plVec3* pData = pStream->GetWritableData<plVec3>();

    for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
    {
      pData[i] = vValue;
    }
  };

  Fill(m_pStreamPosition, pEvent->m_vPosition);
  Fill(m_pStreamVelocity, pEvent->m_vVelocity);
  Fill(m_pStreamNormal, pEvent->m_vNormal);
}
//////////////////////////////////////////////////////////////////////////

/// On Event blocks saved before 1.6 spawned once per frame; they keep doing so.
class plParticleEmitterFactory_OnEvent_1_2 : public plGraphPatch
{
public:
  plParticleEmitterFactory_OnEvent_1_2()
    : plGraphPatch("plParticleEmitterFactory_OnEvent", 2)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    pNode->AddProperty("Spawn", "plParticleOnEventSpawn::OncePerFrame");
  }
};

plParticleEmitterFactory_OnEvent_1_2 g_plParticleEmitterFactory_OnEvent_1_2;

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Emitter_ParticleEmitter_OnEvent);
