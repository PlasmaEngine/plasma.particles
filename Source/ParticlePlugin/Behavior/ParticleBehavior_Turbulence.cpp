#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Turbulence.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_Turbulence, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_Turbulence>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Strength", m_fStrength)->AddAttributes(new plDefaultValueAttribute(2.0f)),
    PL_MEMBER_PROPERTY("Frequency", m_fFrequency)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.01f, {})),
    PL_MEMBER_PROPERTY("ChangeSpeed", m_vScrollSpeed)->AddAttributes(new plDefaultValueAttribute(plVec3(1.0f))),
    PL_MEMBER_PROPERTY("Octaves", m_uiOctaves)->AddAttributes(new plDefaultValueAttribute(1), new plClampValueAttribute(1, 4)),
    PL_MEMBER_PROPERTY("AffectVelocity", m_bAffectVelocity)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_Turbulence, 1, plRTTIDefaultAllocator<plParticleBehavior_Turbulence>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleBehaviorFactory_Turbulence::plParticleBehaviorFactory_Turbulence() = default;

const plRTTI* plParticleBehaviorFactory_Turbulence::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_Turbulence>();
}

void plParticleBehaviorFactory_Turbulence::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_Turbulence* pBehavior = static_cast<plParticleBehavior_Turbulence*>(pObject);

  pBehavior->m_fStrength = m_fStrength;
  pBehavior->m_fFrequency = m_fFrequency;
  pBehavior->m_vScrollSpeed = m_vScrollSpeed;
  pBehavior->m_uiOctaves = m_uiOctaves;
  pBehavior->m_bAffectVelocity = m_bAffectVelocity;
}

void plParticleBehaviorFactory_Turbulence::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  if (m_bAffectVelocity)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
  }
}

void plParticleBehaviorFactory_Turbulence::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_fStrength;
  inout_stream << m_fFrequency;
  inout_stream << m_vScrollSpeed;
  inout_stream << m_uiOctaves;
  inout_stream << m_bAffectVelocity;
}

void plParticleBehaviorFactory_Turbulence::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_fStrength;
  inout_stream >> m_fFrequency;
  inout_stream >> m_vScrollSpeed;
  inout_stream >> m_uiOctaves;
  inout_stream >> m_bAffectVelocity;
}

void plParticleBehavior_Turbulence::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
}

void plParticleBehavior_Turbulence::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: NoiseForce");

  const float tDiff = (float)m_TimeDiff.GetSeconds();
  if (tDiff <= 0.0f)
    return;

  m_fTotalTime += tDiff;

  const float freq = m_fFrequency;
  const plVec3 scroll = m_vScrollSpeed * m_fTotalTime;

  // Small offsets for curl noise approximation: sample noise at 3 different offsets
  // to get decorrelated X, Y, Z force components
  const float kOffset1 = 31.416f;
  const float kOffset2 = 67.123f;

  plProcessingStreamIterator<plSimdVec4f> itPosition(m_pStreamPosition, uiNumElements, 0);
  plProcessingStreamIterator<plVec3> itVelocity(m_pStreamVelocity, uiNumElements, 0);

  const plSimdVec4f constHalf(0.5f);
  const plSimdVec4f constMul = plSimdVec4f(2.0f * m_fStrength * tDiff);

  if (m_bAffectVelocity)
  {
    while (!itPosition.HasReachedEnd())
    {
      const plSimdVec4f simPos = itPosition.Current();
      const float px = simPos.GetComponent<0>();
      const float py = simPos.GetComponent<1>();
      const float pz = simPos.GetComponent<2>();

      // Noise sampling coordinates with scroll and frequency.
      const float sx = (px + scroll.x) * freq;
      const float sy = (py + scroll.y) * freq;
      const float sz = (pz + scroll.z) * freq;

      // Sample noise at 3 offset positions for curl-noise approximation
      // Each component uses a different spatial offset to get uncorrelated values
      const plSimdVec4f noise = m_Noise.NoiseZeroToOne(plSimdVec4f(sx, sx + kOffset1, sx + kOffset2, 0), plSimdVec4f(sy, sy + kOffset1, sy + kOffset2, 0), plSimdVec4f(sz, sz + kOffset1, sz + kOffset2, 0), m_uiOctaves);

      // Map [0,1] -> [-1,1]
      const plVec3 force = plSimdConversion::ToVec3((noise - constHalf).CompMul(constMul));

      itVelocity.Current() += force;

      itPosition.Advance();
      itVelocity.Advance();
    }
  }
  else
  {
    // Direct position offset mode
    while (!itPosition.HasReachedEnd())
    {
      const plSimdVec4f simPos = itPosition.Current();
      const float px = simPos.GetComponent<0>();
      const float py = simPos.GetComponent<1>();
      const float pz = simPos.GetComponent<2>();

      const float sx = (px + scroll.x) * freq;
      const float sy = (py + scroll.y) * freq;
      const float sz = (pz + scroll.z) * freq;

      const plSimdVec4f noise = m_Noise.NoiseZeroToOne(plSimdVec4f(sx, sx + kOffset1, sx + kOffset2, 0), plSimdVec4f(sy, sy + kOffset1, sy + kOffset2, 0), plSimdVec4f(sz, sz + kOffset1, sz + kOffset2, 0), m_uiOctaves);

      const plSimdVec4f offset = (noise - constHalf).CompMul(constMul);

      itPosition.Current() = simPos + offset;

      itPosition.Advance();
      itVelocity.Advance();
    }
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_Turbulence);