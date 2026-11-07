#include <ParticlePlugin/ParticlePluginPCH.h>

#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>

#include <Core/World/World.h>
#include <Foundation/Math/Color16f.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_LastPosition.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererFoundation/Shader/ShaderUtils.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plQuadParticleOrientation, 2)
  PL_ENUM_CONSTANTS(plQuadParticleOrientation::Billboard)
  PL_ENUM_CONSTANTS(plQuadParticleOrientation::Rotating_OrthoEmitterDir, plQuadParticleOrientation::Rotating_EmitterDir)
  PL_ENUM_CONSTANTS(plQuadParticleOrientation::Fixed_EmitterDir, plQuadParticleOrientation::Fixed_RandomDir, plQuadParticleOrientation::Fixed_WorldUp)
  PL_ENUM_CONSTANTS(plQuadParticleOrientation::FixedAxis_EmitterDir, plQuadParticleOrientation::FixedAxis_ParticleDir)
  PL_ENUM_CONSTANTS(plQuadParticleOrientation::Fixed_StartAxis)
  PL_ENUM_CONSTANTS(plQuadParticleOrientation::Custom_Facing)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plQuadParticleEmitterDirSampling, 1)
  PL_ENUM_CONSTANTS(plQuadParticleEmitterDirSampling::Live, plQuadParticleEmitterDirSampling::AtSpawn)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTypeQuadFactory, 4, plRTTIDefaultAllocator<plParticleTypeQuadFactory>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Orientation", plQuadParticleOrientation, m_Orientation),
    PL_ENUM_MEMBER_PROPERTY("EmitterDirSampling", plQuadParticleEmitterDirSampling, m_EmitterDirSampling),
    PL_MEMBER_PROPERTY("MinSpeed", m_fMinSpeed)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("TurnSmoothing", m_fTurnSmoothing)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 0.95f)),
    PL_MEMBER_PROPERTY("Deviation", m_MaxDeviation)->AddAttributes(new plClampValueAttribute(plAngle::MakeFromDegree(0), plAngle::MakeFromDegree(90))),
    PL_ENUM_MEMBER_PROPERTY("RenderMode", plParticleTypeRenderMode, m_RenderMode),
    PL_ENUM_MEMBER_PROPERTY("LightingMode", plParticleLightingMode, m_LightingMode),
    PL_MEMBER_PROPERTY("NormalCurvature", m_fNormalCurvature)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0, 1)),
    PL_MEMBER_PROPERTY("LightDirectionality", m_fLightDirectionality)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0, 1)),
    PL_MEMBER_PROPERTY("UseCustomMaterial", m_bUseCustomMaterial),
    PL_MEMBER_PROPERTY("CustomMaterial", m_sCustomMaterial)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material", "QuadParticle")),
    PL_MEMBER_PROPERTY("Texture", m_sTexture)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D"), new plDefaultValueAttribute(plStringView("{ e00262e8-58f5-42f5-880d-569257047201 }"))),// wrap in plStringView to prevent a memory leak report
    PL_ENUM_MEMBER_PROPERTY("TextureAtlas", plParticleTextureAtlasType, m_TextureAtlasType),
    PL_MEMBER_PROPERTY("NumSpritesX", m_uiNumSpritesX)->AddAttributes(new plDefaultValueAttribute(1), new plClampValueAttribute(1, 16)),
    PL_MEMBER_PROPERTY("NumSpritesY", m_uiNumSpritesY)->AddAttributes(new plDefaultValueAttribute(1), new plClampValueAttribute(1, 16)),
    PL_MEMBER_PROPERTY("TintColorParam", m_sTintColorParameter),
    PL_MEMBER_PROPERTY("DistortionTexture", m_sDistortionTexture)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D")),
    PL_MEMBER_PROPERTY("SixWayMapA", m_sSixWayMapA)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D")),
    PL_MEMBER_PROPERTY("SixWayMapB", m_sSixWayMapB)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D")),
    PL_MEMBER_PROPERTY("SixWayAbsorption", m_fSixWayAbsorption)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.05f, 8.0f)),
    PL_MEMBER_PROPERTY("NormalMap", m_sNormalMap)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D")),
    PL_MEMBER_PROPERTY("NormalMapStrength", m_fNormalMapStrength)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("OrmMap", m_sOrmMap)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D")),
    PL_MEMBER_PROPERTY("Roughness", m_fRoughness)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Metallic", m_fMetallic)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Specular", m_fSpecular)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("DistortionStrength", m_fDistortionStrength)->AddAttributes(new plDefaultValueAttribute(100.0f), new plClampValueAttribute(0.0f, 500.0f)),
    PL_MEMBER_PROPERTY("ParticleStretch", m_fStretch)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(-100.0f, 100.0f)),
    PL_PARTICLE_LOOK_PROPERTIES,
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTypeQuad, 1, plRTTIDefaultAllocator<plParticleTypeQuad>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleTypeQuadFactory::GetTypeType() const
{
  return plGetStaticRTTI<plParticleTypeQuad>();
}

void plParticleTypeQuadFactory::CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const
{
  plParticleTypeQuad* pType = static_cast<plParticleTypeQuad*>(pObject);

  pType->m_Orientation = m_Orientation;
  pType->m_EmitterDirSampling = m_EmitterDirSampling;
  pType->m_fMinSpeed = plMath::Max(m_fMinSpeed, 0.0f);
  pType->m_fTurnSmoothing = plMath::Clamp(m_fTurnSmoothing, 0.0f, 0.95f);
  pType->m_MaxDeviation = m_MaxDeviation;
  pType->m_hTexture.Invalidate();
  pType->m_RenderMode = m_RenderMode;
  pType->m_uiNumSpritesX = m_uiNumSpritesX;
  pType->m_uiNumSpritesY = m_uiNumSpritesY;
  pType->m_sTintColorParameter = plTempHashedString(m_sTintColorParameter.GetData());
  pType->m_hDistortionTexture.Invalidate();
  pType->m_fDistortionStrength = m_fDistortionStrength;
  pType->m_TextureAtlasType = m_TextureAtlasType;
  pType->m_fStretch = m_fStretch;
  pType->m_LightingMode = m_LightingMode;
  pType->m_fNormalCurvature = m_fNormalCurvature;
  pType->m_fLightDirectionality = m_fLightDirectionality;
  pType->m_hCustomMaterial.Invalidate();
  pType->m_Look.Configure(m_Look);

  if (m_bUseCustomMaterial)
  {
    if (!m_sCustomMaterial.IsEmpty())
      pType->m_hCustomMaterial = plResourceManager::LoadResource<plMaterialResource>(m_sCustomMaterial);
  }
  else
  {
    if (!m_sTexture.IsEmpty())
      pType->m_hTexture = plResourceManager::LoadResource<plTexture2DResource>(m_sTexture);

    if (!m_sDistortionTexture.IsEmpty())
      pType->m_hDistortionTexture = plResourceManager::LoadResource<plTexture2DResource>(m_sDistortionTexture);

    if (!m_sSixWayMapA.IsEmpty())
      pType->m_hSixWayMapA = plResourceManager::LoadResource<plTexture2DResource>(m_sSixWayMapA);

    if (!m_sSixWayMapB.IsEmpty())
      pType->m_hSixWayMapB = plResourceManager::LoadResource<plTexture2DResource>(m_sSixWayMapB);

    pType->m_fSixWayAbsorption = m_fSixWayAbsorption;
    pType->m_fNormalMapStrength = m_fNormalMapStrength;

    pType->m_hNormalMap.Invalidate();
    if (!m_sNormalMap.IsEmpty())
      pType->m_hNormalMap = plResourceManager::LoadResource<plTexture2DResource>(m_sNormalMap);

    pType->m_hOrmMap.Invalidate();
    if (!m_sOrmMap.IsEmpty())
      pType->m_hOrmMap = plResourceManager::LoadResource<plTexture2DResource>(m_sOrmMap);

    pType->m_fRoughness = m_fRoughness;
    pType->m_fMetallic = m_fMetallic;
    pType->m_fSpecular = m_fSpecular;
  }
}

enum class TypeQuadVersion
{
  Version_0 = 0,
  Version_1,
  Version_2, // sprite deviation
  Version_3, // distortion
  Version_4, // added texture atlas type
  Version_5, // added particle stretch
  Version_6, // added particle lighting
  Version_7, // added custom material support
  Version_8, // added six-way lighting response maps
  Version_9, // added six-way absorption
  Version_10, // added per-pixel normal map
  Version_11, // added emissive map and depth blending; six-way reads its emissive from the emissive map
  Version_12, // added emitter direction sampling, particle direction min speed / smoothing, surface lighting inputs

  // insert new version numbers above
  Version_Count,
  Version_Current = Version_Count - 1
};

void plParticleTypeQuadFactory::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = (int)TypeQuadVersion::Version_Current;
  inout_stream << uiVersion;

  inout_stream << m_Orientation;
  inout_stream << m_RenderMode;
  inout_stream << m_sTexture;
  inout_stream << m_uiNumSpritesX;
  inout_stream << m_uiNumSpritesY;
  inout_stream << m_sTintColorParameter;
  inout_stream << m_MaxDeviation;
  inout_stream << m_sDistortionTexture;
  inout_stream << m_fDistortionStrength;
  inout_stream << m_TextureAtlasType;

  // Version 5
  inout_stream << m_fStretch;

  // Version 6
  inout_stream << m_LightingMode;
  inout_stream << m_fNormalCurvature;
  inout_stream << m_fLightDirectionality;

  // Version 7
  inout_stream << m_bUseCustomMaterial;
  inout_stream << m_sCustomMaterial;

  // Version 8
  inout_stream << m_sSixWayMapA;
  inout_stream << m_sSixWayMapB;

  // Version 9
  inout_stream << m_fSixWayAbsorption;

  // Version 10
  inout_stream << m_sNormalMap;
  inout_stream << m_fNormalMapStrength;

  // Version 11
  m_Look.Save(inout_stream);

  // Version 12
  inout_stream << m_EmitterDirSampling;
  inout_stream << m_fMinSpeed;
  inout_stream << m_fTurnSmoothing;
  inout_stream << m_sOrmMap;
  inout_stream << m_fRoughness;
  inout_stream << m_fMetallic;
  inout_stream << m_fSpecular;
}

void plParticleTypeQuadFactory::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= (int)TypeQuadVersion::Version_Current, "Invalid version {0}", uiVersion);

  inout_stream >> m_Orientation;
  inout_stream >> m_RenderMode;
  inout_stream >> m_sTexture;
  inout_stream >> m_uiNumSpritesX;
  inout_stream >> m_uiNumSpritesY;
  inout_stream >> m_sTintColorParameter;

  if (uiVersion >= 2)
  {
    inout_stream >> m_MaxDeviation;
  }

  if (uiVersion >= 3)
  {
    inout_stream >> m_sDistortionTexture;
    inout_stream >> m_fDistortionStrength;
  }

  if (uiVersion >= 4)
  {
    inout_stream >> m_TextureAtlasType;

    if (m_TextureAtlasType == plParticleTextureAtlasType::None)
    {
      m_uiNumSpritesX = 1;
      m_uiNumSpritesY = 1;
    }
  }

  if (uiVersion >= 5)
  {
    inout_stream >> m_fStretch;
  }

  if (uiVersion >= 6)
  {
    inout_stream >> m_LightingMode;
    inout_stream >> m_fNormalCurvature;
    inout_stream >> m_fLightDirectionality;
  }

  if (uiVersion >= 7)
  {
    inout_stream >> m_bUseCustomMaterial;
    inout_stream >> m_sCustomMaterial;
  }

  if (uiVersion >= 8)
  {
    inout_stream >> m_sSixWayMapA;
    inout_stream >> m_sSixWayMapB;
  }

  if (uiVersion >= 9)
  {
    inout_stream >> m_fSixWayAbsorption;
  }

  if (uiVersion >= 10)
  {
    inout_stream >> m_sNormalMap;
    inout_stream >> m_fNormalMapStrength;
  }

  if (uiVersion >= 11)
  {
    m_Look.Load(inout_stream);
  }
  else
  {
    m_Look = plParticleLookSettings();

    // six-way used to add the Texture slot as its emissive
    if (m_LightingMode == plParticleLightingMode::SixWay)
      m_Look.m_sEmissiveMap = m_sTexture;
  }

  if (uiVersion >= 12)
  {
    inout_stream >> m_EmitterDirSampling;
    inout_stream >> m_fMinSpeed;
    inout_stream >> m_fTurnSmoothing;
    inout_stream >> m_sOrmMap;
    inout_stream >> m_fRoughness;
    inout_stream >> m_fMetallic;
    inout_stream >> m_fSpecular;
  }
  else
  {
    // older effects keep reading the emitter direction every frame
    m_EmitterDirSampling = plQuadParticleEmitterDirSampling::Live;
    m_fMinSpeed = 0.1f;
    m_fTurnSmoothing = 0.0f;
    m_sOrmMap.Clear();
    m_fRoughness = 0.5f;
    m_fMetallic = 0.0f;
    m_fSpecular = 0.5f;
  }
}

void plParticleTypeQuadFactory::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  if (m_Orientation == plQuadParticleOrientation::FixedAxis_ParticleDir)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_LastPosition>());
  }
}

plParticleTypeQuad::plParticleTypeQuad() = default;
plParticleTypeQuad::~plParticleTypeQuad() = default;

void plParticleTypeQuad::CreateRequiredStreams()
{
  CreateStream("LifeTime", plProcessingStream::DataType::Float2, &m_pStreamLifeTime, false);
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Size", plProcessingStream::DataType::Half, &m_pStreamSize, false);
  CreateStream("Color", plProcessingStream::DataType::Half4, &m_pStreamColor, false);
  CreateStream("RotationSpeed", plProcessingStream::DataType::Half, &m_pStreamRotationSpeed, false);
  CreateStream("RotationOffset", plProcessingStream::DataType::Half, &m_pStreamRotationOffset, false);

  m_pStreamAxis = nullptr;
  m_pStreamVariation = nullptr;
  m_pStreamLastPosition = nullptr;

  if (m_Orientation == plQuadParticleOrientation::Fixed_RandomDir || m_Orientation == plQuadParticleOrientation::Fixed_EmitterDir || m_Orientation == plQuadParticleOrientation::Fixed_WorldUp)
  {
    CreateStream("Axis", plProcessingStream::DataType::Float3, &m_pStreamAxis, true);
  }

  if (m_Orientation == plQuadParticleOrientation::Fixed_StartAxis || m_Orientation == plQuadParticleOrientation::Custom_Facing)
  {
    // an initializer fills the axis at spawn time (e.g. Mesh Position writes the surface normal);
    // without one, the stream's default initializer provides a fixed (1, 0, 0) axis
    CreateStream("Axis", plProcessingStream::DataType::Float3, &m_pStreamAxis, false);
  }

  if (m_Orientation == plQuadParticleOrientation::FixedAxis_ParticleDir || UsesSpawnEmitterDir())
  {
    // Axis: Particle Dir keeps its facing here; the emitter modes keep the direction captured at spawn
    CreateStream("Axis", plProcessingStream::DataType::Float3, &m_pStreamAxis, true);
  }

  if (m_TextureAtlasType == plParticleTextureAtlasType::RandomVariations || m_TextureAtlasType == plParticleTextureAtlasType::RandomYAnimatedX)
  {
    CreateStream("Variation", plProcessingStream::DataType::Int, &m_pStreamVariation, false);
  }

  if (m_Orientation == plQuadParticleOrientation::FixedAxis_ParticleDir)
  {
    CreateStream("LastPosition", plProcessingStream::DataType::Float3, &m_pStreamLastPosition, false);
  }
}

void plParticleTypeQuad::QueryOptionalStreams()
{
  // created by Random Size, Size Curve or Set Attribute when they write per-axis sizes
  m_pStreamSizeScale = GetOwnerSystem()->QueryStream("SizeScale", plProcessingStream::DataType::Half3);

  // created by Set Attribute's Emissive target
  m_pStreamEmissive = GetOwnerSystem()->QueryStream("Emissive", plProcessingStream::DataType::Half);

  // created by the velocity initializers
  m_pStreamVelocity = m_Orientation == plQuadParticleOrientation::FixedAxis_ParticleDir ? GetOwnerSystem()->QueryStream("Velocity", plProcessingStream::DataType::Float3) : nullptr;
}

bool plParticleTypeQuad::UsesSpawnEmitterDir() const
{
  if (m_EmitterDirSampling != plQuadParticleEmitterDirSampling::AtSpawn)
    return false;

  return m_Orientation == plQuadParticleOrientation::FixedAxis_EmitterDir || m_Orientation == plQuadParticleOrientation::Rotating_EmitterDir || m_Orientation == plQuadParticleOrientation::Rotating_OrthoEmitterDir;
}

void plParticleTypeQuad::Process(plUInt64 uiNumElements)
{
  if (m_Orientation == plQuadParticleOrientation::FixedAxis_ParticleDir)
  {
    UpdateParticleDirFacing(uiNumElements);
  }
}

void plParticleTypeQuad::UpdateParticleDirFacing(plUInt64 uiNumElements)
{
  if (m_pStreamAxis == nullptr || m_pStreamLastPosition == nullptr)
    return;

  const float fDeltaSeconds = m_TimeDiff.AsFloatInSeconds();
  if (fDeltaSeconds <= 0.0f)
    return;

  PL_PROFILE_SCOPE("PFX: Quad Particle Dir");

  const plVec4* pPosition = m_pStreamPosition->GetData<plVec4>();
  const plVec3* pLastPosition = m_pStreamLastPosition->GetData<plVec3>();
  plVec3* pAxis = m_pStreamAxis->GetWritableData<plVec3>();

  // a particle that moved less than this in the step keeps the facing it has, e.g. when it comes to rest
  const float fMinDistance = plMath::Max(m_fMinSpeed * fDeltaSeconds, 1.0e-5f);

  // the share of the old facing kept per step, normalized to 30 steps per second
  const float fKeep = m_fTurnSmoothing > 0.0f ? plMath::Pow(m_fTurnSmoothing, fDeltaSeconds * 30.0f) : 0.0f;

  for (plUInt64 i = 0; i < uiNumElements; ++i)
  {
    const plVec3 vDelta = pPosition[i].GetAsVec3() - pLastPosition[i];
    const float fDistance = vDelta.GetLength();

    if (fDistance < fMinDistance)
      continue;

    const plVec3 vTarget = vDelta / fDistance;

    if (fKeep <= 0.0f || pAxis[i].IsZero(1.0e-6f))
    {
      pAxis[i] = vTarget;
      continue;
    }

    plVec3 vFacing = plMath::Lerp(vTarget, pAxis[i], fKeep);
    vFacing.NormalizeIfNotZero(vTarget).IgnoreResult();
    pAxis[i] = vFacing;
  }
}

struct sodComparer
{
  // sort farther particles to the front, so that they get rendered first (back to front)
  PL_ALWAYS_INLINE bool Less(const plParticleTypeQuad::sod& a, const plParticleTypeQuad::sod& b) const { return a.dist > b.dist; }
  PL_ALWAYS_INLINE bool Equal(const plParticleTypeQuad::sod& a, const plParticleTypeQuad::sod& b) const { return a.dist == b.dist; }
};

void plParticleTypeQuad::ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const
{
  // distortion works without a texture: the shader masks the quad with a soft disc
  if (!m_hTexture.IsValid() && !m_hCustomMaterial.IsValid() && m_RenderMode != plParticleTypeRenderMode::Distortion)
    return;

  if (plParticleSkipView(ref_msg, CastsShadows()))
    return;

  const plUInt32 numParticles = (plUInt32)GetOwnerSystem()->GetNumActiveParticles();
  if (numParticles == 0)
    return;

  const bool bNeedsSorting = (m_RenderMode == plParticleTypeRenderMode::Blended) || (m_RenderMode == plParticleTypeRenderMode::BlendedForeground) || (m_RenderMode == plParticleTypeRenderMode::BlendedBackground) || (m_RenderMode == plParticleTypeRenderMode::BlendAdd);

  if (bNeedsSorting)
  {
    // The sort order depends on the camera and the instance transform, so it cannot be cached across
    // views or shared instances - re-extract per call. All memory comes from the frame allocator
    // (thread-safe linear allocator, per-allocation Deallocate is a no-op) and lives until the frame
    // is fully rendered - the same lifetime as the render data pointing into it. Extraction calls are
    // serialized by the particle world module mutex, so the mutable members are race-free.
    plHybridArray<sod, 64> sorted(plFrameAllocator::GetCurrentAllocator());
    sorted.SetCountUninitialized(numParticles);

    // Positions are in "particle space": world space normally, effect space when the transform is
    // applied at render time (local-space simulation / shared instances). Move the camera into that
    // space instead of transforming every particle into world space.
    plVec3 vCameraPos = ref_msg.m_pView->GetCullingCamera()->GetCenterPosition();
    if (GetOwnerEffect()->NeedsToApplyTransform())
    {
      vCameraPos = instanceTransform.GetInverse() * vCameraPos;
    }

    const plVec4* pPosition = m_pStreamPosition->GetData<plVec4>();

    for (plUInt32 p = 0; p < numParticles; ++p)
    {
      sorted[p].dist = (pPosition[p].GetAsVec3() - vCameraPos).GetLengthSquared();
      sorted[p].index = p;
    }

    sorted.Sort(sodComparer());

    CreateExtractedData(&sorted);
  }
  else if (m_uiLastExtractedFrame != plRenderWorld::GetFrameCounter())
  {
    // unsorted data is view- and instance-independent -> copy it only once per frame,
    // no matter how many views or shared instances extract this effect
    m_uiLastExtractedFrame = plRenderWorld::GetFrameCounter();

    CreateExtractedData(nullptr);
  }

  AddParticleRenderData(ref_msg, instanceTransform);
}

PL_ALWAYS_INLINE plUInt32 noRedirect(plUInt32 uiIdx, const plHybridArray<plParticleTypeQuad::sod, 64>* pSorted)
{
  return uiIdx;
}

PL_ALWAYS_INLINE plUInt32 sortedRedirect(plUInt32 uiIdx, const plHybridArray<plParticleTypeQuad::sod, 64>* pSorted)
{
  return (*pSorted)[uiIdx].index;
}

void plParticleTypeQuad::CreateExtractedData(const plHybridArray<sod, 64>* pSorted) const
{
  auto redirect = (pSorted != nullptr) ? sortedRedirect : noRedirect;

  const plUInt32 numParticles = (plUInt32)GetOwnerSystem()->GetNumActiveParticles();

  const bool bNeedsBillboardData = m_Orientation == plQuadParticleOrientation::Billboard;
  const bool bNeedsTangentData = !bNeedsBillboardData;

  const plVec3 vEmitterPos = GetOwnerSystem()->GetTransform().m_vPosition;
  const plVec3 vEmitterDir = GetOwnerSystem()->GetTransform().m_qRotation * plVec3(0, 0, 1); // Z axis
  const plVec3 vEmitterDirOrtho = vEmitterDir.GetOrthogonalVector();

  const plTime tCur = GetOwnerEffect()->GetTotalEffectLifeTime();
  const plColor tintColor = GetOwnerEffect()->GetColorParameter(m_sTintColorParameter, plColor::White);

  const plVec2* pLifeTime = m_pStreamLifeTime->GetData<plVec2>();
  const plVec4* pPosition = m_pStreamPosition->GetData<plVec4>();
  const plFloat16* pSize = m_pStreamSize->GetData<plFloat16>();
  const plColorLinear16f* pColor = m_pStreamColor->GetData<plColorLinear16f>();
  const plFloat16* pRotationSpeed = m_pStreamRotationSpeed->GetData<plFloat16>();
  const plFloat16* pRotationOffset = m_pStreamRotationOffset->GetData<plFloat16>();
  const plVec3* pAxis = m_pStreamAxis ? m_pStreamAxis->GetData<plVec3>() : nullptr;
  const plUInt32* pVariation = m_pStreamVariation ? m_pStreamVariation->GetData<plUInt32>() : nullptr;
  const plVec3* pLastPosition = m_pStreamLastPosition ? m_pStreamLastPosition->GetData<plVec3>() : nullptr;
  const plFloat16Vec3* pSizeScale = m_pStreamSizeScale ? m_pStreamSizeScale->GetData<plFloat16Vec3>() : nullptr;
  const plFloat16* pEmissive = m_pStreamEmissive ? m_pStreamEmissive->GetData<plFloat16>() : nullptr;

  // this will automatically be deallocated at the end of the frame
  m_BaseParticleData = PL_NEW_ARRAY(plFrameAllocator::GetCurrentAllocator(), plBaseParticleShaderData, numParticles);

  AllocateParticleData(numParticles, bNeedsBillboardData, bNeedsTangentData);

  auto SetBaseData = [&](plUInt32 uiDstIdx, plUInt32 uiSrcIdx)
  {
    m_BaseParticleData[uiDstIdx].Size = pSize[uiSrcIdx];
    m_BaseParticleData[uiDstIdx].Color = pColor[uiSrcIdx].ToLinearFloat() * tintColor;
    m_BaseParticleData[uiDstIdx].Life = pLifeTime[uiSrcIdx].x * pLifeTime[uiSrcIdx].y;

    plUInt32 uiVariation = (pVariation != nullptr) ? (pVariation[uiSrcIdx] & 0xFFu) : 0u;
    if (pEmissive != nullptr)
      uiVariation |= PARTICLE_VARIATION_EMISSIVE_BIT | ((plUInt32)pEmissive[uiSrcIdx].GetRawData() << 16);

    m_BaseParticleData[uiDstIdx].Variation = uiVariation;
  };

  auto SetBillboardData = [&](plUInt32 uiDstIdx, plUInt32 uiSrcIdx)
  {
    m_BillboardParticleData[uiDstIdx].Position = pPosition[uiSrcIdx].GetAsVec3();
    m_BillboardParticleData[uiDstIdx].RotationOffset = pRotationOffset[uiSrcIdx];
    m_BillboardParticleData[uiDstIdx].RotationSpeed = pRotationSpeed[uiSrcIdx];
    m_BillboardParticleData[uiDstIdx].ScaleX = pSizeScale ? pSizeScale[uiSrcIdx].x : plFloat16(1.0f);
    m_BillboardParticleData[uiDstIdx].ScaleY = pSizeScale ? pSizeScale[uiSrcIdx].y : plFloat16(1.0f);
  };

  // per-axis size multipliers fold directly into the tangent lengths
  auto ApplyAxisScale = [&](plUInt32 uiDstIdx, plUInt32 uiSrcIdx)
  {
    if (pSizeScale != nullptr)
    {
      m_TangentParticleData[uiDstIdx].TangentX *= (float)pSizeScale[uiSrcIdx].x;
      m_TangentParticleData[uiDstIdx].TangentZ *= (float)pSizeScale[uiSrcIdx].y;
    }
  };

  // axis-aligned quads: X stretches along the axis, Y is the width (the shader reads TangentZ.y, 0 meaning 1)
  auto ApplyAlignedScale = [&](plUInt32 uiDstIdx, plUInt32 uiSrcIdx)
  {
    if (pSizeScale != nullptr)
    {
      m_TangentParticleData[uiDstIdx].TangentZ.x *= (float)pSizeScale[uiSrcIdx].x;
      m_TangentParticleData[uiDstIdx].TangentZ.y = plMath::Max((float)pSizeScale[uiSrcIdx].y, 0.001f);
    }
  };

  // the emitter direction at spawn (captured per particle) or now
  const bool bSpawnEmitterDir = UsesSpawnEmitterDir() && pAxis != nullptr;
  auto GetEmitterDir = [&](plUInt32 uiSrcIdx) -> plVec3
  {
    return bSpawnEmitterDir ? pAxis[uiSrcIdx] : vEmitterDir;
  };

  auto SetTangentDataEmitterDir = [&](plUInt32 uiDstIdx, plUInt32 uiSrcIdx)
  {
    const plVec3 vDir = GetEmitterDir(uiSrcIdx);
    const plVec3 vDirOrtho = bSpawnEmitterDir ? vDir.GetOrthogonalVector() : vEmitterDirOrtho;

    plMat3 mRotation = plMat3::MakeAxisRotation(vDir, plAngle::MakeFromRadian((float)(tCur.GetSeconds() * pRotationSpeed[uiSrcIdx]) + pRotationOffset[uiSrcIdx]));

    m_TangentParticleData[uiDstIdx].Position = pPosition[uiSrcIdx].GetAsVec3();
    m_TangentParticleData[uiDstIdx].TangentX = mRotation * vDirOrtho;
    m_TangentParticleData[uiDstIdx].TangentZ = vDir;
    ApplyAxisScale(uiDstIdx, uiSrcIdx);
  };

  auto SetTangentDataEmitterDirOrtho = [&](plUInt32 uiDstIdx, plUInt32 uiSrcIdx)
  {
    const plVec3 vDir = GetEmitterDir(uiSrcIdx);
    const plVec3 vDirToParticle = (pPosition[uiSrcIdx].GetAsVec3() - vEmitterPos);
    plVec3 vOrthoDir = vDir.CrossRH(vDirToParticle);
    vOrthoDir.NormalizeIfNotZero(plVec3(1, 0, 0)).IgnoreResult();

    plMat3 mRotation = plMat3::MakeAxisRotation(vOrthoDir, plAngle::MakeFromRadian((float)(tCur.GetSeconds() * pRotationSpeed[uiSrcIdx]) + pRotationOffset[uiSrcIdx]));

    m_TangentParticleData[uiDstIdx].Position = pPosition[uiSrcIdx].GetAsVec3();
    m_TangentParticleData[uiDstIdx].TangentX = vOrthoDir;
    m_TangentParticleData[uiDstIdx].TangentZ = mRotation * vDir;
    ApplyAxisScale(uiDstIdx, uiSrcIdx);
  };

  auto SetTangentDataFromAxis = [&](plUInt32 uiDstIdx, plUInt32 uiSrcIdx)
  {
    PL_ASSERT_DEBUG(pAxis != nullptr, "Axis must be valid");
    plVec3 vNormal = pAxis[uiSrcIdx];
    vNormal.Normalize();

    const plVec3 vTangentStart = vNormal.GetOrthogonalVector().GetNormalized();

    plMat3 mRotation = plMat3::MakeAxisRotation(vNormal, plAngle::MakeFromRadian((float)(tCur.GetSeconds() * pRotationSpeed[uiSrcIdx]) + pRotationOffset[uiSrcIdx]));

    const plVec3 vTangentX = mRotation * vTangentStart;

    m_TangentParticleData[uiDstIdx].Position = pPosition[uiSrcIdx].GetAsVec3();
    m_TangentParticleData[uiDstIdx].TangentX = vTangentX;
    m_TangentParticleData[uiDstIdx].TangentZ = vTangentX.CrossRH(vNormal);
    ApplyAxisScale(uiDstIdx, uiSrcIdx);
  };

  auto SetTangentDataAligned_Emitter = [&](plUInt32 uiDstIdx, plUInt32 uiSrcIdx)
  {
    m_TangentParticleData[uiDstIdx].Position = pPosition[uiSrcIdx].GetAsVec3();
    m_TangentParticleData[uiDstIdx].TangentX = GetEmitterDir(uiSrcIdx);
    m_TangentParticleData[uiDstIdx].TangentZ.x = m_fStretch;
    ApplyAlignedScale(uiDstIdx, uiSrcIdx);
  };

  auto SetTangentDataAligned_ParticleDir = [&](plUInt32 uiDstIdx, plUInt32 uiSrcIdx)
  {
    const plVec3 vCurPos = pPosition[uiSrcIdx].GetAsVec3();

    // the facing is kept while the particle rests; a particle that has not been stepped yet has none
    plVec3 vDir = pAxis != nullptr ? pAxis[uiSrcIdx] : plVec3::MakeZero();
    if (vDir.IsZero(1.0e-6f))
      vDir = vCurPos - pLastPosition[uiSrcIdx];
    if (vDir.IsZero(1.0e-6f))
      vDir = vEmitterDir;

    m_TangentParticleData[uiDstIdx].Position = vCurPos;
    m_TangentParticleData[uiDstIdx].TangentX = vDir;
    m_TangentParticleData[uiDstIdx].TangentZ.x = m_fStretch;
    ApplyAlignedScale(uiDstIdx, uiSrcIdx);
  };

  for (plUInt32 p = 0; p < numParticles; ++p)
  {
    SetBaseData(p, redirect(p, pSorted));
  }

  if (bNeedsBillboardData)
  {
    for (plUInt32 p = 0; p < numParticles; ++p)
    {
      SetBillboardData(p, redirect(p, pSorted));
    }
  }

  if (bNeedsTangentData)
  {
    if (m_Orientation == plQuadParticleOrientation::Rotating_EmitterDir)
    {
      for (plUInt32 p = 0; p < numParticles; ++p)
      {
        SetTangentDataEmitterDir(p, redirect(p, pSorted));
      }
    }
    else if (m_Orientation == plQuadParticleOrientation::Rotating_OrthoEmitterDir)
    {
      for (plUInt32 p = 0; p < numParticles; ++p)
      {
        SetTangentDataEmitterDirOrtho(p, redirect(p, pSorted));
      }
    }
    else if (m_Orientation == plQuadParticleOrientation::Fixed_EmitterDir || m_Orientation == plQuadParticleOrientation::Fixed_RandomDir || m_Orientation == plQuadParticleOrientation::Fixed_WorldUp || m_Orientation == plQuadParticleOrientation::Fixed_StartAxis || m_Orientation == plQuadParticleOrientation::Custom_Facing)
    {
      for (plUInt32 p = 0; p < numParticles; ++p)
      {
        SetTangentDataFromAxis(p, redirect(p, pSorted));
      }
    }
    else if (m_Orientation == plQuadParticleOrientation::FixedAxis_EmitterDir)
    {
      for (plUInt32 p = 0; p < numParticles; ++p)
      {
        SetTangentDataAligned_Emitter(p, redirect(p, pSorted));
      }
    }
    else if (m_Orientation == plQuadParticleOrientation::FixedAxis_ParticleDir)
    {
      PL_ASSERT_DEBUG(pLastPosition != nullptr, "FixedAxis_ParticleDir needs the last position attribute");
      for (plUInt32 p = 0; p < numParticles; ++p)
      {
        SetTangentDataAligned_ParticleDir(p, redirect(p, pSorted));
      }
    }
    else
    {
      PL_ASSERT_NOT_IMPLEMENTED;
    }
  }
}

void plParticleTypeQuad::AddParticleRenderData(plMsgExtractRenderData& msg, const plTransform& instanceTransform) const
{
  auto pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plParticleQuadRenderData>(nullptr);

  if (m_hCustomMaterial.IsValid())
  {
    pRenderData->m_uiSortingKey = ComputeSortingKey(m_RenderMode, m_hCustomMaterial.GetResourceIDHash(), 0);
  }
  else
  {
    pRenderData->m_uiSortingKey = ComputeSortingKey(m_RenderMode, m_hTexture.GetResourceIDHash(), m_hDistortionTexture.GetResourceIDHash());
  }

  pRenderData->m_vGlobalPosition = instanceTransform.m_vPosition;

  pRenderData->m_GlobalTransform = GetOwnerEffect()->NeedsToApplyTransform() ? instanceTransform : plTransform::MakeIdentity();
  pRenderData->m_TotalEffectLifeTime = GetOwnerEffect()->GetTotalEffectLifeTime();
  pRenderData->m_RenderMode = m_RenderMode;
  pRenderData->m_hTexture = m_hTexture;
  pRenderData->m_BaseParticleData = m_BaseParticleData;
  pRenderData->m_BillboardParticleData = m_BillboardParticleData;
  pRenderData->m_TangentParticleData = m_TangentParticleData;
  pRenderData->m_uiNumVariationsX = 1;
  pRenderData->m_uiNumVariationsY = 1;
  pRenderData->m_uiNumFlipbookAnimationsX = 1;
  pRenderData->m_uiNumFlipbookAnimationsY = 1;
  pRenderData->m_hDistortionTexture = m_hDistortionTexture;
  pRenderData->m_hSixWayMapA = m_hSixWayMapA;
  pRenderData->m_hSixWayMapB = m_hSixWayMapB;
  pRenderData->m_fSixWayAbsorption = m_fSixWayAbsorption;
  pRenderData->m_hNormalMap = m_hNormalMap;
  pRenderData->m_fNormalMapStrength = m_hNormalMap.IsValid() ? m_fNormalMapStrength : 0.0f; // a strength without a map samples nothing useful
  pRenderData->m_fDistortionStrength = m_fDistortionStrength;
  pRenderData->m_LightingMode = m_LightingMode;
  pRenderData->m_fNormalCurvature = m_fNormalCurvature;
  pRenderData->m_fLightDirectionality = m_fLightDirectionality;
  pRenderData->m_hCustomMaterial = m_hCustomMaterial;
  pRenderData->m_Look = m_Look.GetRenderData(GetOwnerEffect(), m_pStreamEmissive != nullptr);
  pRenderData->m_Look.m_bDistortionTexture = m_hDistortionTexture.IsValid();

  pRenderData->m_hOrmMap = m_hOrmMap;
  pRenderData->m_Look.m_bOrmMap = m_hOrmMap.IsValid();
  pRenderData->m_Look.m_fSurfaceRoughness = m_fRoughness;
  pRenderData->m_Look.m_fSurfaceMetallic = m_fMetallic;
  pRenderData->m_Look.m_fSurfaceSpecular = m_fSpecular;

  switch (m_Orientation)
  {
    case plQuadParticleOrientation::Billboard:
      pRenderData->m_QuadModePermutation = "PARTICLE_QUAD_MODE_BILLBOARD";
      break;
    case plQuadParticleOrientation::Rotating_OrthoEmitterDir:
    case plQuadParticleOrientation::Rotating_EmitterDir:
    case plQuadParticleOrientation::Fixed_EmitterDir:
    case plQuadParticleOrientation::Fixed_WorldUp:
    case plQuadParticleOrientation::Fixed_RandomDir:
    case plQuadParticleOrientation::Fixed_StartAxis:
    case plQuadParticleOrientation::Custom_Facing:
      pRenderData->m_QuadModePermutation = "PARTICLE_QUAD_MODE_TANGENTS";
      break;
    case plQuadParticleOrientation::FixedAxis_EmitterDir:
    case plQuadParticleOrientation::FixedAxis_ParticleDir:
      pRenderData->m_QuadModePermutation = "PARTICLE_QUAD_MODE_AXIS_ALIGNED";
      break;
  }

  switch (m_TextureAtlasType)
  {
    case plParticleTextureAtlasType::None:
      break;

    case plParticleTextureAtlasType::RandomVariations:
      pRenderData->m_uiNumVariationsX = m_uiNumSpritesX;
      pRenderData->m_uiNumVariationsY = m_uiNumSpritesY;
      break;

    case plParticleTextureAtlasType::FlipbookAnimation:
      pRenderData->m_uiNumFlipbookAnimationsX = m_uiNumSpritesX;
      pRenderData->m_uiNumFlipbookAnimationsY = m_uiNumSpritesY;
      break;

    case plParticleTextureAtlasType::RandomYAnimatedX:
      pRenderData->m_uiNumFlipbookAnimationsX = m_uiNumSpritesX;
      pRenderData->m_uiNumVariationsY = m_uiNumSpritesY;
      break;
  }

  plParticleAddRenderData(msg, pRenderData, m_RenderMode);
}

void plParticleTypeQuad::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  if (m_pStreamAxis == nullptr)
    return;

  const plVec3* pVelocity = m_pStreamVelocity != nullptr ? m_pStreamVelocity->GetData<plVec3>() : nullptr;
  plParticleInitializeQuadAxis(GetOwnerSystem(), m_Orientation, m_EmitterDirSampling, m_MaxDeviation, GetRNG(), pVelocity, m_pStreamAxis->GetWritableData<plVec3>(), uiStartIndex, uiNumElements);
}

bool plParticleQuadOrientationNeedsSpawnAxis(plQuadParticleOrientation::Enum orientation, plQuadParticleEmitterDirSampling::Enum sampling)
{
  switch (orientation)
  {
    case plQuadParticleOrientation::Fixed_RandomDir:
    case plQuadParticleOrientation::Fixed_EmitterDir:
    case plQuadParticleOrientation::Fixed_WorldUp:
    case plQuadParticleOrientation::FixedAxis_ParticleDir:
      return true;

    case plQuadParticleOrientation::FixedAxis_EmitterDir:
    case plQuadParticleOrientation::Rotating_EmitterDir:
    case plQuadParticleOrientation::Rotating_OrthoEmitterDir:
      return sampling == plQuadParticleEmitterDirSampling::AtSpawn;

    default:
      return false;
  }
}

void plParticleInitializeQuadAxis(plParticleSystemInstance* pSystem, plQuadParticleOrientation::Enum orientation, plQuadParticleEmitterDirSampling::Enum sampling, plAngle maxDeviation, plRandom& ref_rng, const plVec3* pVelocity, plVec3* pAxis, plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  if (!plParticleQuadOrientationNeedsSpawnAxis(orientation, sampling))
    return;

  PL_PROFILE_SCOPE("PFX: Init Quad Axis");

  const plUInt64 uiEnd = uiStartIndex + uiNumElements;
  const plVec3 vEmitterDir = pSystem->GetTransform().m_qRotation * plVec3(0, 0, 1); // Z axis

  if (orientation == plQuadParticleOrientation::FixedAxis_ParticleDir)
  {
    // the first facing is the start velocity; the initializers run before the types
    for (plUInt64 i = uiStartIndex; i < uiEnd; ++i)
    {
      plVec3 vFacing = pVelocity != nullptr ? pVelocity[i] : vEmitterDir;
      vFacing.NormalizeIfNotZero(vEmitterDir).IgnoreResult();
      pAxis[i] = vFacing;
    }

    return;
  }

  if (orientation == plQuadParticleOrientation::Fixed_RandomDir)
  {
    for (plUInt64 i = uiStartIndex; i < uiEnd; ++i)
    {
      pAxis[i] = plVec3::MakeRandomDirection(ref_rng);
    }

    return;
  }

  // the emitter direction modes capture the direction; Fixed Emitter Dir and World Up may deviate from it
  plVec3 vNormal = vEmitterDir;
  plAngle deviation = plAngle::MakeFromDegree(0.0f);

  if (orientation == plQuadParticleOrientation::Fixed_EmitterDir || orientation == plQuadParticleOrientation::Fixed_WorldUp)
  {
    deviation = maxDeviation;
  }

  if (orientation == plQuadParticleOrientation::Fixed_WorldUp)
  {
    plCoordinateSystem coord;
    pSystem->GetWorld()->GetCoordinateSystem(pSystem->GetOwnerEffect()->GetTransform().m_vPosition, coord);

    // the world's up, in the simulation's space
    vNormal = pSystem->GetOwnerEffect()->NeedsToApplyTransform() ? pSystem->GetOwnerEffect()->GetTransform().m_qRotation.GetInverse() * coord.m_vUpDir : coord.m_vUpDir;
  }

  if (deviation > plAngle::MakeFromDegree(1.0f))
  {
    // how to get from the X axis to the desired normal
    const plQuat qRotToDir = plQuat::MakeShortestRotation(plVec3(1, 0, 0), vNormal);

    for (plUInt64 i = uiStartIndex; i < uiEnd; ++i)
    {
      pAxis[i] = qRotToDir * plVec3::MakeRandomDeviationX(ref_rng, deviation);
    }
  }
  else
  {
    for (plUInt64 i = uiStartIndex; i < uiEnd; ++i)
    {
      pAxis[i] = vNormal;
    }
  }
}

void plParticleTypeQuad::AllocateParticleData(const plUInt32 numParticles, const bool bNeedsBillboardData, const bool bNeedsTangentData) const
{
  m_BillboardParticleData = nullptr;
  if (bNeedsBillboardData)
  {
    m_BillboardParticleData = PL_NEW_ARRAY(plFrameAllocator::GetCurrentAllocator(), plBillboardQuadParticleShaderData, numParticles);
  }

  m_TangentParticleData = nullptr;
  if (bNeedsTangentData)
  {
    m_TangentParticleData = PL_NEW_ARRAY(plFrameAllocator::GetCurrentAllocator(), plTangentQuadParticleShaderData, numParticles);
  }
}

//////////////////////////////////////////////////////////////////////////

#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Serialization/GraphPatch.h>

// Enum values are serialized as string properties on the OWNING object's node - enum types never
// appear as their own nodes in the graph, so patches registered on the enum type never fire.
// The rename of the old plQuadParticleOrientation values is therefore done by the factory patch below.
class plParticleTypeQuadFactory_1_2 final : public plGraphPatch
{
public:
  plParticleTypeQuadFactory_1_2()
    : plGraphPatch("plParticleTypeQuadFactory", 2)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    plAbstractObjectNode::Property* pProp = pNode->FindProperty("Orientation");
    if (pProp == nullptr || !pProp->m_Value.IsA<plString>())
      return;

    const plStringBuilder sOri = pProp->m_Value.Get<plString>();

    if (sOri == "plQuadParticleOrientation::FragmentOrthogonalEmitterDirection")
      pProp->m_Value = "plQuadParticleOrientation::Rotating_OrthoEmitterDir";

    if (sOri == "plQuadParticleOrientation::FragmentEmitterDirection")
      pProp->m_Value = "plQuadParticleOrientation::Rotating_EmitterDir";

    if (sOri == "plQuadParticleOrientation::SpriteEmitterDirection")
      pProp->m_Value = "plQuadParticleOrientation::Fixed_EmitterDir";

    if (sOri == "plQuadParticleOrientation::SpriteRandom")
      pProp->m_Value = "plQuadParticleOrientation::Fixed_RandomDir";

    if (sOri == "plQuadParticleOrientation::SpriteWorldUp")
      pProp->m_Value = "plQuadParticleOrientation::Fixed_WorldUp";

    if (sOri == "plQuadParticleOrientation::AxisAligned_Emitter")
      pProp->m_Value = "plQuadParticleOrientation::FixedAxis_EmitterDir";
  }
};

plParticleTypeQuadFactory_1_2 g_plParticleTypeQuadFactory_1_2;

// Six-way lighting added the Texture slot as its emissive; the emissive map now carries it.
class plParticleTypeQuadFactory_2_3 final : public plGraphPatch
{
public:
  plParticleTypeQuadFactory_2_3()
    : plGraphPatch("plParticleTypeQuadFactory", 3)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    const plAbstractObjectNode::Property* pLighting = pNode->FindProperty("LightingMode");
    if (pLighting == nullptr || !pLighting->m_Value.IsA<plString>() || pLighting->m_Value.Get<plString>() != "plParticleLightingMode::SixWay")
      return;

    if (pNode->FindProperty("EmissiveMap") != nullptr)
      return;

    // an absent Texture is the property's default
    const plAbstractObjectNode::Property* pTexture = pNode->FindProperty("Texture");
    const plString sTexture = (pTexture != nullptr && pTexture->m_Value.IsA<plString>()) ? pTexture->m_Value.Get<plString>() : plString("{ e00262e8-58f5-42f5-880d-569257047201 }");

    pNode->AddProperty("EmissiveMap", sTexture);
  }
};

plParticleTypeQuadFactory_2_3 g_plParticleTypeQuadFactory_2_3;

// Effects authored before the emitter direction could be captured at spawn keep reading it every frame.
class plParticleTypeQuadFactory_3_4 final : public plGraphPatch
{
public:
  plParticleTypeQuadFactory_3_4()
    : plGraphPatch("plParticleTypeQuadFactory", 4)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    if (pNode->FindProperty("EmitterDirSampling") == nullptr)
    {
      pNode->AddProperty("EmitterDirSampling", plString("plQuadParticleEmitterDirSampling::Live"));
    }
  }
};

plParticleTypeQuadFactory_3_4 g_plParticleTypeQuadFactory_3_4;


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_Quad_ParticleTypeQuad);
