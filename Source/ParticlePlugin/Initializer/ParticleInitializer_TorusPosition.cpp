#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamGroup.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_TorusPosition.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializerFactory_TorusPosition, 1, plRTTIDefaultAllocator<plParticleInitializerFactory_TorusPosition>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("PositionOffset", m_vPositionOffset),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.02f, 100.0f)),
    PL_MEMBER_PROPERTY("RingRadius", m_fRingRadius)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.01f, 100.0f)),
    PL_MEMBER_PROPERTY("OnSurface", m_bSpawnOnSurface),
    PL_MEMBER_PROPERTY("SetVelocity", m_bSetVelocity),
    PL_MEMBER_PROPERTY("Speed", m_Speed),
    PL_MEMBER_PROPERTY("ScaleRadiusParam", m_sScaleRadiusParameter),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializer_TorusPosition, 1, plRTTIDefaultAllocator<plParticleInitializer_TorusPosition>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleInitializerFactory_TorusPosition::plParticleInitializerFactory_TorusPosition()
{
  m_vPositionOffset.SetZero();
  m_fRadius = 0.5f;
  m_fRingRadius = 0.1f;
  m_bSpawnOnSurface = false;
  m_bSetVelocity = false;
}

const plRTTI* plParticleInitializerFactory_TorusPosition::GetInitializerType() const
{
  return plGetStaticRTTI<plParticleInitializer_TorusPosition>();
}

void plParticleInitializerFactory_TorusPosition::CopyInitializerProperties(plParticleInitializer* pInitializer0, bool bFirstTime) const
{
  plParticleInitializer_TorusPosition* pInitializer = static_cast<plParticleInitializer_TorusPosition*>(pInitializer0);

  const float fScale = pInitializer->GetOwnerEffect()->GetFloatParameter(plTempHashedString(m_sScaleRadiusParameter.GetData()), 1.0f);

  pInitializer->m_vPositionOffset = m_vPositionOffset;
  pInitializer->m_fRingRadius = plMath::Max(m_fRingRadius * fScale, 0.01f);
  pInitializer->m_fRadius = plMath::Max(m_fRadius * fScale, pInitializer->m_fRingRadius); // the ring may not be thicker than the torus is wide
  pInitializer->m_bSpawnOnSurface = m_bSpawnOnSurface;
  pInitializer->m_bSetVelocity = m_bSetVelocity;
  pInitializer->m_Speed = m_Speed;
}

float plParticleInitializerFactory_TorusPosition::GetSpawnCountMultiplier(const plParticleEffectInstance* pEffect) const
{
  const float fScale = pEffect->GetFloatParameter(plTempHashedString(m_sScaleRadiusParameter.GetData()), 1.0f);

  if (fScale != 1.0f)
  {
    // volume scales with R * r^2, surface with R * r
    return m_bSpawnOnSurface ? fScale * fScale : fScale * fScale * fScale;
  }

  return 1.0f;
}

void plParticleInitializerFactory_TorusPosition::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_vPositionOffset;
  inout_stream << m_fRadius;
  inout_stream << m_fRingRadius;
  inout_stream << m_bSpawnOnSurface;
  inout_stream << m_bSetVelocity;
  inout_stream << m_Speed.m_Value;
  inout_stream << m_Speed.m_fVariance;
  inout_stream << m_sScaleRadiusParameter;
}

void plParticleInitializerFactory_TorusPosition::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  inout_stream >> m_vPositionOffset;
  inout_stream >> m_fRadius;
  inout_stream >> m_fRingRadius;
  inout_stream >> m_bSpawnOnSurface;
  inout_stream >> m_bSetVelocity;
  inout_stream >> m_Speed.m_Value;
  inout_stream >> m_Speed.m_fVariance;
  inout_stream >> m_sScaleRadiusParameter;
}

void plParticleInitializerFactory_TorusPosition::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  if (m_bSetVelocity)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
  }
}

//////////////////////////////////////////////////////////////////////////

void plParticleInitializer_TorusPosition::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, true);

  m_pStreamVelocity = nullptr;

  if (m_bSetVelocity)
  {
    CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, true);
  }
}

void plParticleInitializer_TorusPosition::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Torus Position");

  const plVec3 startVel = GetOwnerSystem()->GetParticleStartVelocity();

  plVec4* pPosition = m_pStreamPosition->GetWritableData<plVec4>();
  plVec3* pVelocity = m_bSetVelocity ? m_pStreamVelocity->GetWritableData<plVec3>() : nullptr;

  plRandom& rng = GetRNG();

  const plTransform trans = GetOwnerSystem()->GetTransform();
  const float fTwoPi = 2.0f * plMath::Pi<float>();

  for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
  {
    float fRho, fSinPhi, fCosPhi;

    // sample the ring cross-section; the rejection on (R + rho * cos(phi)) corrects for the outer
    // half of the ring covering more of the torus than the inner half, giving uniform density
    do
    {
      const float fPhi = (float)rng.DoubleZeroToOneExclusive() * fTwoPi;
      fSinPhi = plMath::Sin(plAngle::MakeFromRadian(fPhi));
      fCosPhi = plMath::Cos(plAngle::MakeFromRadian(fPhi));

      fRho = m_bSpawnOnSurface ? m_fRingRadius : m_fRingRadius * plMath::Sqrt((float)rng.DoubleZeroToOneInclusive());
    } while ((float)rng.DoubleZeroToOneExclusive() * (m_fRadius + m_fRingRadius) > m_fRadius + fRho * fCosPhi);

    const float fU = (float)rng.DoubleZeroToOneExclusive() * fTwoPi;
    const float fSinU = plMath::Sin(plAngle::MakeFromRadian(fU));
    const float fCosU = plMath::Cos(plAngle::MakeFromRadian(fU));

    // direction from the ring center-line outwards through the sampled point
    const plVec3 vOutward(fCosU * fCosPhi, fSinU * fCosPhi, fSinPhi);

    plVec3 pos = plVec3(fCosU, fSinU, 0) * m_fRadius + vOutward * fRho;
    pos += m_vPositionOffset;

    if (m_bSetVelocity)
    {
      const float fSpeed = (float)rng.DoubleVariance(m_Speed.m_Value, m_Speed.m_fVariance);

      pVelocity[i] = startVel + trans.m_qRotation * vOutward * fSpeed;
    }

    pPosition[i] = (trans * pos).GetAsVec4(0);
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Initializer_ParticleInitializer_TorusPosition);
