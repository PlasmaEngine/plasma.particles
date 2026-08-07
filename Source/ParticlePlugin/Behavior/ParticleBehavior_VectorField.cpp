#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_VectorField.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleVectorFieldSource, 1)
  PL_ENUM_CONSTANT(plParticleVectorFieldSource::CurlNoise),
  PL_ENUM_CONSTANT(plParticleVectorFieldSource::File),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleVectorFieldMode, 1)
  PL_ENUM_CONSTANT(plParticleVectorFieldMode::Force),
  PL_ENUM_CONSTANT(plParticleVectorFieldMode::Velocity),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleVectorFieldMapping, 1)
  PL_ENUM_CONSTANT(plParticleVectorFieldMapping::Tile),
  PL_ENUM_CONSTANT(plParticleVectorFieldMapping::Clamp),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_VectorField, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_VectorField>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Source", plParticleVectorFieldSource, m_Source),
    PL_MEMBER_PROPERTY("File", m_sFile)->AddAttributes(new plFileBrowserAttribute("Select Vector Field", "*.fga")),
    PL_MEMBER_PROPERTY("Resolution", m_uiResolution)->AddAttributes(new plDefaultValueAttribute(32), new plClampValueAttribute(4, 128)),
    PL_MEMBER_PROPERTY("Seed", m_uiSeed),
    PL_MEMBER_PROPERTY("Frequency", m_fFrequency)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.1f, 64.0f)),
    PL_MEMBER_PROPERTY("Octaves", m_uiOctaves)->AddAttributes(new plDefaultValueAttribute(1), new plClampValueAttribute(1, 6)),
    PL_ENUM_MEMBER_PROPERTY("Mode", plParticleVectorFieldMode, m_Mode),
    PL_ENUM_MEMBER_PROPERTY("Mapping", plParticleVectorFieldMapping, m_Mapping),
    PL_MEMBER_PROPERTY("FieldSize", m_vFieldSize)->AddAttributes(new plDefaultValueAttribute(plVec3(4.0f))),
    PL_MEMBER_PROPERTY("PositionOffset", m_vPositionOffset),
    PL_MEMBER_PROPERTY("Strength", m_fStrength)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("StrengthParam", m_sStrengthParameter),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plBoxVisualizerAttribute("FieldSize", 1.0f, plColorScheme::LightUI(plColorScheme::Cyan), nullptr, plVisualizerAnchor::Center, plVec3(1.0f), "PositionOffset"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_VectorField, 1, plRTTIDefaultAllocator<plParticleBehavior_VectorField>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleBehaviorFactory_VectorField::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_VectorField>();
}

void plParticleBehaviorFactory_VectorField::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_VectorField* pBehavior = static_cast<plParticleBehavior_VectorField*>(pObject);

  pBehavior->m_hField.Invalidate();

  if (m_Source == plParticleVectorFieldSource::File)
  {
    if (!m_sFile.IsEmpty())
    {
      pBehavior->m_hField = plResourceManager::LoadResource<plVectorFieldResource>(m_sFile);
    }
  }
  else
  {
    // procedural fields are shared across all effects with identical bake parameters
    plStringBuilder sResourceId;
    sResourceId.SetFormat("VectorFieldCurl_{0}_{1}_{2}_{3}", m_uiResolution, m_uiSeed, m_fFrequency, m_uiOctaves);

    pBehavior->m_hField = plResourceManager::GetExistingResource<plVectorFieldResource>(sResourceId);

    if (!pBehavior->m_hField.IsValid())
    {
      plVectorFieldResourceDescriptor desc;
      desc.InitializeCurlNoise(m_uiResolution, m_uiSeed, m_fFrequency, m_uiOctaves);

      pBehavior->m_hField = plResourceManager::CreateResource<plVectorFieldResource>(sResourceId, std::move(desc), "baked particle curl noise");
    }
  }

  pBehavior->m_Mode = m_Mode;
  pBehavior->m_Mapping = m_Mapping;
  pBehavior->m_vFieldSize = m_vFieldSize.CompMax(plVec3(0.01f));
  pBehavior->m_vPositionOffset = m_vPositionOffset;
  pBehavior->m_fStrength = m_fStrength;
  pBehavior->m_sStrengthParameter = plTempHashedString(m_sStrengthParameter.GetData());
}

void plParticleBehaviorFactory_VectorField::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
}

void plParticleBehaviorFactory_VectorField::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_Source;
  inout_stream << m_sFile;
  inout_stream << m_uiResolution;
  inout_stream << m_uiSeed;
  inout_stream << m_fFrequency;
  inout_stream << m_uiOctaves;
  inout_stream << m_Mode;
  inout_stream << m_Mapping;
  inout_stream << m_vFieldSize;
  inout_stream << m_vPositionOffset;
  inout_stream << m_fStrength;
  inout_stream << m_sStrengthParameter;
}

void plParticleBehaviorFactory_VectorField::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_Source;
  inout_stream >> m_sFile;
  inout_stream >> m_uiResolution;
  inout_stream >> m_uiSeed;
  inout_stream >> m_fFrequency;
  inout_stream >> m_uiOctaves;
  inout_stream >> m_Mode;
  inout_stream >> m_Mapping;
  inout_stream >> m_vFieldSize;
  inout_stream >> m_vPositionOffset;
  inout_stream >> m_fStrength;
  inout_stream >> m_sStrengthParameter;
}

//////////////////////////////////////////////////////////////////////////

void plParticleBehavior_VectorField::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
}

void plParticleBehavior_VectorField::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: VectorField");

  const float tDiff = (float)m_TimeDiff.GetSeconds();
  if (tDiff <= 0.0f || !m_hField.IsValid())
    return;

  plResourceLock<plVectorFieldResource> pField(m_hField, plResourceAcquireMode::BlockTillLoaded_NeverFail);
  if (pField.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  const plVectorFieldResourceDescriptor& field = pField->GetDescriptor();
  if (!field.IsValid())
    return;

  const float fStrength = m_fStrength * GetOwnerEffect()->GetFloatParameter(m_sStrengthParameter, 1.0f);

  // the field box is anchored at the effect; for local-space simulations the particle
  // positions are already relative to the effect, so the anchor is the origin
  plTransform anchor = plTransform::MakeIdentity();
  if (!GetOwnerEffect()->IsSimulatedInLocalSpace())
  {
    anchor = GetOwnerEffect()->GetTransform();
  }

  const plTransform invAnchor = anchor.GetInverse();
  const plVec3 vInvSize(1.0f / m_vFieldSize.x, 1.0f / m_vFieldSize.y, 1.0f / m_vFieldSize.z);
  const bool bTile = (m_Mapping == plParticleVectorFieldMapping::Tile);
  const bool bForce = (m_Mode == plParticleVectorFieldMode::Force);

  plProcessingStreamIterator<plSimdVec4f> itPosition(m_pStreamPosition, uiNumElements, 0);
  plProcessingStreamIterator<plVec3> itVelocity(m_pStreamVelocity, uiNumElements, 0);

  while (!itPosition.HasReachedEnd())
  {
    const plSimdVec4f simPos = itPosition.Current();
    const plVec3 pos(simPos.GetComponent<0>(), simPos.GetComponent<1>(), simPos.GetComponent<2>());

    // position -> normalized field coordinates, centered on the anchor
    const plVec3 vLocal = (invAnchor * pos) - m_vPositionOffset;
    plVec3 vNorm = plVec3(vLocal.x * vInvSize.x, vLocal.y * vInvSize.y, vLocal.z * vInvSize.z) + plVec3(0.5f);

    if (bTile)
    {
      vNorm.x = vNorm.x - plMath::Floor(vNorm.x);
      vNorm.y = vNorm.y - plMath::Floor(vNorm.y);
      vNorm.z = vNorm.z - plMath::Floor(vNorm.z);
    }

    const plVec3 vSample = anchor.m_qRotation * field.SampleTrilinear(vNorm);

    if (bForce)
    {
      itVelocity.Current() += vSample * (fStrength * tDiff);
    }
    else
    {
      itVelocity.Current() = vSample * fStrength;
    }

    itPosition.Advance();
    itVelocity.Advance();
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_VectorField);
