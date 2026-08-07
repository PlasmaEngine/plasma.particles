#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamGroup.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_ConePosition.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializerFactory_ConePosition, 1, plRTTIDefaultAllocator<plParticleInitializerFactory_ConePosition>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("PositionOffset", m_vPositionOffset),
    PL_MEMBER_PROPERTY("Angle", m_Angle)->AddAttributes(new plDefaultValueAttribute(plAngle::MakeFromDegree(25)), new plClampValueAttribute(plAngle::MakeFromDegree(1), plAngle::MakeFromDegree(89))),
    PL_MEMBER_PROPERTY("Height", m_fHeight)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.01f, 100.0f)),
    PL_MEMBER_PROPERTY("OnSurface", m_bSpawnOnSurface),
    PL_MEMBER_PROPERTY("SetVelocity", m_bSetVelocity),
    PL_MEMBER_PROPERTY("Speed", m_Speed),
    PL_MEMBER_PROPERTY("ScaleHeightParam", m_sScaleHeightParameter),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plConeVisualizerAttribute(plBasisAxis::PositiveZ, "Angle", 1.0f, "Height", plColor::MediumVioletRed)
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializer_ConePosition, 1, plRTTIDefaultAllocator<plParticleInitializer_ConePosition>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleInitializerFactory_ConePosition::plParticleInitializerFactory_ConePosition()
{
  m_vPositionOffset.SetZero();
  m_Angle = plAngle::MakeFromDegree(25);
  m_fHeight = 1.0f;
  m_bSpawnOnSurface = false;
  m_bSetVelocity = false;
}

const plRTTI* plParticleInitializerFactory_ConePosition::GetInitializerType() const
{
  return plGetStaticRTTI<plParticleInitializer_ConePosition>();
}

void plParticleInitializerFactory_ConePosition::CopyInitializerProperties(plParticleInitializer* pInitializer0, bool bFirstTime) const
{
  plParticleInitializer_ConePosition* pInitializer = static_cast<plParticleInitializer_ConePosition*>(pInitializer0);

  const float fScaleHeight = pInitializer->GetOwnerEffect()->GetFloatParameter(plTempHashedString(m_sScaleHeightParameter.GetData()), 1.0f);

  pInitializer->m_vPositionOffset = m_vPositionOffset;
  pInitializer->m_Angle = plMath::Clamp(m_Angle, plAngle::MakeFromDegree(1), plAngle::MakeFromDegree(89));
  pInitializer->m_fHeight = plMath::Max(m_fHeight * fScaleHeight, 0.01f);
  pInitializer->m_bSpawnOnSurface = m_bSpawnOnSurface;
  pInitializer->m_bSetVelocity = m_bSetVelocity;
  pInitializer->m_Speed = m_Speed;
}

void plParticleInitializerFactory_ConePosition::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_vPositionOffset;
  inout_stream << m_Angle;
  inout_stream << m_fHeight;
  inout_stream << m_bSpawnOnSurface;
  inout_stream << m_bSetVelocity;
  inout_stream << m_Speed.m_Value;
  inout_stream << m_Speed.m_fVariance;
  inout_stream << m_sScaleHeightParameter;
}

void plParticleInitializerFactory_ConePosition::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  inout_stream >> m_vPositionOffset;
  inout_stream >> m_Angle;
  inout_stream >> m_fHeight;
  inout_stream >> m_bSpawnOnSurface;
  inout_stream >> m_bSetVelocity;
  inout_stream >> m_Speed.m_Value;
  inout_stream >> m_Speed.m_fVariance;
  inout_stream >> m_sScaleHeightParameter;
}

void plParticleInitializerFactory_ConePosition::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  if (m_bSetVelocity)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
  }
}

//////////////////////////////////////////////////////////////////////////

void plParticleInitializer_ConePosition::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, true);

  m_pStreamVelocity = nullptr;

  if (m_bSetVelocity)
  {
    CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, true);
  }
}

void plParticleInitializer_ConePosition::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Cone Position");

  const plVec3 startVel = GetOwnerSystem()->GetParticleStartVelocity();

  plVec4* pPosition = m_pStreamPosition->GetWritableData<plVec4>();
  plVec3* pVelocity = m_bSetVelocity ? m_pStreamVelocity->GetWritableData<plVec3>() : nullptr;

  plRandom& rng = GetRNG();

  const plTransform trans = GetOwnerSystem()->GetTransform();
  const float fTanAngle = plMath::Tan(m_Angle);
  const float fTwoPi = 2.0f * plMath::Pi<float>();

  for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
  {
    float fZ, fRho;

    if (m_bSpawnOnSurface)
    {
      // lateral surface area grows linearly with the distance from the apex -> z = H * sqrt(u)
      fZ = m_fHeight * plMath::Sqrt((float)rng.DoubleZeroToOneInclusive());
      fRho = fZ * fTanAngle;
    }
    else
    {
      // volume of a cone slice grows with z^2 -> z = H * cbrt(u), uniform inside the disc at that height
      fZ = m_fHeight * plMath::Pow((float)rng.DoubleZeroToOneInclusive(), 1.0f / 3.0f);
      fRho = fZ * fTanAngle * plMath::Sqrt((float)rng.DoubleZeroToOneInclusive());
    }

    const float fTheta = (float)rng.DoubleZeroToOneExclusive() * fTwoPi;
    const float fSinTheta = plMath::Sin(plAngle::MakeFromRadian(fTheta));
    const float fCosTheta = plMath::Cos(plAngle::MakeFromRadian(fTheta));

    plVec3 pos(fCosTheta * fRho, fSinTheta * fRho, fZ);

    if (m_bSetVelocity)
    {
      // away from the apex, through the spawn position; at the apex itself the cone axis is used
      plVec3 vDir = pos;
      vDir.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();

      const float fSpeed = (float)rng.DoubleVariance(m_Speed.m_Value, m_Speed.m_fVariance);

      pVelocity[i] = startVel + trans.m_qRotation * vDir * fSpeed;
    }

    pos += m_vPositionOffset;

    pPosition[i] = (trans * pos).GetAsVec4(0);
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Initializer_ParticleInitializer_ConePosition);
