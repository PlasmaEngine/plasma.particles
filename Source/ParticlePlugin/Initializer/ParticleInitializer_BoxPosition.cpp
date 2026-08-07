#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamGroup.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/SimdMath/SimdConversion.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_BoxPosition.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializerFactory_BoxPosition, 1, plRTTIDefaultAllocator<plParticleInitializerFactory_BoxPosition>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("PositionOffset", m_vPositionOffset),
    PL_MEMBER_PROPERTY("Size", m_vSize)->AddAttributes(new plDefaultValueAttribute(plVec3(0, 0, 0))),
    PL_MEMBER_PROPERTY("OnSurface", m_bSpawnOnSurface),
    PL_MEMBER_PROPERTY("ScaleXParam", m_sScaleXParameter),
    PL_MEMBER_PROPERTY("ScaleYParam", m_sScaleYParameter),
    PL_MEMBER_PROPERTY("ScaleZParam", m_sScaleZParameter),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plBoxVisualizerAttribute("Size", 1.0f, plColor::MediumVioletRed, nullptr, plVisualizerAnchor::Center, plVec3(1.0f), "PositionOffset")
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializer_BoxPosition, 1, plRTTIDefaultAllocator<plParticleInitializer_BoxPosition>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleInitializerFactory_BoxPosition::plParticleInitializerFactory_BoxPosition()
{
  m_vPositionOffset.SetZero();
  m_vSize.Set(0, 0, 0);
}

const plRTTI* plParticleInitializerFactory_BoxPosition::GetInitializerType() const
{
  return plGetStaticRTTI<plParticleInitializer_BoxPosition>();
}

void plParticleInitializerFactory_BoxPosition::CopyInitializerProperties(plParticleInitializer* pInitializer0, bool bFirstTime) const
{
  plParticleInitializer_BoxPosition* pInitializer = static_cast<plParticleInitializer_BoxPosition*>(pInitializer0);

  const float fScaleX = pInitializer->GetOwnerEffect()->GetFloatParameter(plTempHashedString(m_sScaleXParameter.GetData()), 1.0f);
  const float fScaleY = pInitializer->GetOwnerEffect()->GetFloatParameter(plTempHashedString(m_sScaleYParameter.GetData()), 1.0f);
  const float fScaleZ = pInitializer->GetOwnerEffect()->GetFloatParameter(plTempHashedString(m_sScaleZParameter.GetData()), 1.0f);

  plVec3 vSize = m_vSize;
  vSize.x *= fScaleX;
  vSize.y *= fScaleY;
  vSize.z *= fScaleZ;

  pInitializer->m_vPositionOffset = m_vPositionOffset;
  pInitializer->m_vSize = vSize;
  pInitializer->m_bSpawnOnSurface = m_bSpawnOnSurface;
}

float plParticleInitializerFactory_BoxPosition::GetSpawnCountMultiplier(const plParticleEffectInstance* pEffect) const
{
  const float fScaleX = pEffect->GetFloatParameter(plTempHashedString(m_sScaleXParameter.GetData()), 1.0f);
  const float fScaleY = pEffect->GetFloatParameter(plTempHashedString(m_sScaleYParameter.GetData()), 1.0f);
  const float fScaleZ = pEffect->GetFloatParameter(plTempHashedString(m_sScaleZParameter.GetData()), 1.0f);

  float fSpawnMultiplier = 1.0f;

  if (m_vSize.x != 0.0f)
    fSpawnMultiplier *= fScaleX;

  if (m_vSize.y != 0.0f)
    fSpawnMultiplier *= fScaleY;

  if (m_vSize.z != 0.0f)
    fSpawnMultiplier *= fScaleZ;

  return fSpawnMultiplier;
}

void plParticleInitializerFactory_BoxPosition::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 4;
  inout_stream << uiVersion;

  inout_stream << m_vSize;

  // version 2
  inout_stream << m_vPositionOffset;

  // version 3
  inout_stream << m_sScaleXParameter;
  inout_stream << m_sScaleYParameter;
  inout_stream << m_sScaleZParameter;

  // version 4
  inout_stream << m_bSpawnOnSurface;
}

void plParticleInitializerFactory_BoxPosition::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  inout_stream >> m_vSize;

  if (uiVersion >= 2)
  {
    inout_stream >> m_vPositionOffset;
  }

  if (uiVersion >= 3)
  {
    inout_stream >> m_sScaleXParameter;
    inout_stream >> m_sScaleYParameter;
    inout_stream >> m_sScaleZParameter;
  }

  if (uiVersion >= 4)
  {
    inout_stream >> m_bSpawnOnSurface;
  }
}

void plParticleInitializer_BoxPosition::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, true);
}

void plParticleInitializer_BoxPosition::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Box Position");

  plSimdVec4f* pPosition = m_pStreamPosition->GetWritableData<plSimdVec4f>();

  plRandom& rng = GetRNG();

  if (m_vSize.IsZero())
  {
    plSimdVec4f pos = plSimdConversion::ToVec4((GetOwnerSystem()->GetTransform() * m_vPositionOffset).GetAsVec4(0));

    for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
    {
      pPosition[i] = pos;
    }
  }
  else
  {
    plSimdVec4f pos;
    plSimdTransform transform = plSimdConversion::ToTransform(GetOwnerSystem()->GetTransform());

    float p0[4];
    p0[3] = 0;

    // face areas for area-weighted surface sampling; a box that is flat in one axis
    // degenerates to a plane / line, where only the non-zero faces have weight
    const float fAreaXY = m_vSize.x * m_vSize.y; // +/- Z faces
    const float fAreaXZ = m_vSize.x * m_vSize.z; // +/- Y faces
    const float fAreaYZ = m_vSize.y * m_vSize.z; // +/- X faces
    const float fTotalArea = fAreaXY + fAreaXZ + fAreaYZ;

    const bool bOnSurface = m_bSpawnOnSurface && fTotalArea > 0.0f;

    for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
    {
      p0[0] = (float)(rng.DoubleMinMax(-m_vSize.x, m_vSize.x) * 0.5);
      p0[1] = (float)(rng.DoubleMinMax(-m_vSize.y, m_vSize.y) * 0.5);
      p0[2] = (float)(rng.DoubleMinMax(-m_vSize.z, m_vSize.z) * 0.5);

      if (bOnSurface)
      {
        // pick the axis to clamp by face area, then a random side
        const float fPick = (float)rng.DoubleZeroToOneExclusive() * fTotalArea;
        const float fSign = rng.Bool() ? 0.5f : -0.5f;

        if (fPick < fAreaXY)
          p0[2] = m_vSize.z * fSign;
        else if (fPick < fAreaXY + fAreaXZ)
          p0[1] = m_vSize.y * fSign;
        else
          p0[0] = m_vSize.x * fSign;
      }

      p0[0] += m_vPositionOffset.x;
      p0[1] += m_vPositionOffset.y;
      p0[2] += m_vPositionOffset.z;

      pos.Load<4>(p0);

      pPosition[i] = transform.TransformPosition(pos);
    }
  }
}



PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Initializer_ParticleInitializer_BoxPosition);
