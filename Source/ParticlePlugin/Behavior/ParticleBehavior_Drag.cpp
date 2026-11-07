#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Drag.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_Drag, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_Drag>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Drag", m_fDrag)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 100.0f), new plParticlePerParticleAttribute()),
    PL_MEMBER_PROPERTY("SizeRelative", m_bSizeRelative),
    PL_MEMBER_PROPERTY("DragParam", m_sDragParameter),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_Drag, 1, plRTTIDefaultAllocator<plParticleBehavior_Drag>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_Drag::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_Drag>();
}

void plParticleBehaviorFactory_Drag::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_Drag* pBehavior = static_cast<plParticleBehavior_Drag*>(pObject);

  const float fDragScale = plMath::Max(pBehavior->GetOwnerEffect()->GetFloatParameter(plTempHashedString(m_sDragParameter.GetData()), 1.0f), 0.0f);

  pBehavior->m_fDrag = m_fDrag * fDragScale;
  pBehavior->m_fDragScale = fDragScale;
  pBehavior->m_bSizeRelative = m_bSizeRelative;
  pBehavior->m_pProgram = &m_Program;
}

void plParticleBehaviorFactory_Drag::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 2;
  inout_stream << uiVersion;

  inout_stream << m_fDrag;
  inout_stream << m_bSizeRelative;
  inout_stream << m_sDragParameter;
  m_Program.Save(inout_stream);
}

void plParticleBehaviorFactory_Drag::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 2, "Invalid version {0}", uiVersion);

  inout_stream >> m_fDrag;
  inout_stream >> m_bSizeRelative;
  inout_stream >> m_sDragParameter;

  if (uiVersion >= 2)
    m_Program.Load(inout_stream);
}

//////////////////////////////////////////////////////////////////////////

void plParticleBehavior_Drag::CreateRequiredStreams()
{
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);

  m_Runner.Configure(m_pProgram);
  m_Runner.CreateRequiredStreams([this](const char* szName, plProcessingStream::DataType type, plProcessingStream** ppStream)
    { CreateStream(szName, type, ppStream, false); });
}

void plParticleBehavior_Drag::QueryOptionalStreams()
{
  m_pStreamSize = m_bSizeRelative ? GetOwnerSystem()->QueryStream("Size", plProcessingStream::DataType::Half) : nullptr;
}

void plParticleBehavior_Drag::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Drag");

  const float tDiff = (float)m_TimeDiff.GetSeconds();
  if (tDiff <= 0.0f)
    return;

  // a per-particle wire gives every particle its own drag, scaled by the parameter like the literal
  plUInt32 uiComponents = 0;
  if (const float* pDrag = m_Runner.RunProperty(GetOwnerSystem(), uiNumElements, m_TimeDiff, "Drag", uiComponents))
  {
    plVec3* pVelocity = m_pStreamVelocity->GetWritableData<plVec3>();
    const plFloat16* pSize = m_pStreamSize != nullptr ? m_pStreamSize->GetData<plFloat16>() : nullptr;

    for (plUInt64 i = 0; i < uiNumElements; ++i)
    {
      const float fDrag = plMath::Max(pDrag[i * uiComponents], 0.0f) * m_fDragScale * (pSize != nullptr ? (float)pSize[i] : 1.0f);
      pVelocity[i] *= plMath::Exp(-fDrag * tDiff);
    }

    return;
  }

  if (m_fDrag <= 0.0f)
    return;

  plProcessingStreamIterator<plVec3> itVelocity(m_pStreamVelocity, uiNumElements, 0);

  if (m_pStreamSize != nullptr)
  {
    // drag scales with the particle size; frame-rate independent exponential damping per particle
    const plFloat16* pSize = m_pStreamSize->GetData<plFloat16>();

    plUInt32 i = 0;
    while (!itVelocity.HasReachedEnd())
    {
      itVelocity.Current() *= plMath::Exp(-m_fDrag * (float)pSize[i] * tDiff);

      ++i;
      itVelocity.Advance();
    }
  }
  else
  {
    const float fFactor = plMath::Exp(-m_fDrag * tDiff);

    while (!itVelocity.HasReachedEnd())
    {
      itVelocity.Current() *= fFactor;

      itVelocity.Advance();
    }
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_Drag);
