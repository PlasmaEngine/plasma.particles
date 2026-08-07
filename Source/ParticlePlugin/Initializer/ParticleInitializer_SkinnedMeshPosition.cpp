#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStreamGroup.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SkinnedMeshPosition.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializerFactory_SkinnedMeshPosition, 1, plRTTIDefaultAllocator<plParticleInitializerFactory_SkinnedMeshPosition>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Mesh", m_sMesh)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Mesh_Skinned")),
    PL_ENUM_MEMBER_PROPERTY("SpawnMode", plParticleMeshSpawnMode, m_SpawnMode),
    PL_MEMBER_PROPERTY("SetVelocity", m_bSetVelocity),
    PL_MEMBER_PROPERTY("Speed", m_Speed),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleInitializer_SkinnedMeshPosition, 1, plRTTIDefaultAllocator<plParticleInitializer_SkinnedMeshPosition>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleInitializerFactory_SkinnedMeshPosition::plParticleInitializerFactory_SkinnedMeshPosition() = default;

const plRTTI* plParticleInitializerFactory_SkinnedMeshPosition::GetInitializerType() const
{
  return plGetStaticRTTI<plParticleInitializer_SkinnedMeshPosition>();
}

void plParticleInitializerFactory_SkinnedMeshPosition::CopyInitializerProperties(plParticleInitializer* pInitializer0, bool bFirstTime) const
{
  plParticleInitializer_SkinnedMeshPosition* pInitializer = static_cast<plParticleInitializer_SkinnedMeshPosition*>(pInitializer0);

  pInitializer->m_hMesh.Invalidate();

  if (!m_sMesh.IsEmpty())
  {
    pInitializer->m_hMesh = plResourceManager::LoadResource<plCpuMeshResource>(m_sMesh);
  }

  pInitializer->m_SpawnMode = m_SpawnMode;
  pInitializer->m_bSetVelocity = m_bSetVelocity;
  pInitializer->m_Speed = m_Speed;
}

void plParticleInitializerFactory_SkinnedMeshPosition::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_sMesh;
  inout_stream << m_SpawnMode;
  inout_stream << m_bSetVelocity;
  inout_stream << m_Speed.m_Value;
  inout_stream << m_Speed.m_fVariance;
}

void plParticleInitializerFactory_SkinnedMeshPosition::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  inout_stream >> m_sMesh;
  inout_stream >> m_SpawnMode;
  inout_stream >> m_bSetVelocity;
  inout_stream >> m_Speed.m_Value;
  inout_stream >> m_Speed.m_fVariance;
}

void plParticleInitializerFactory_SkinnedMeshPosition::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  if (m_bSetVelocity)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
  }
}

//////////////////////////////////////////////////////////////////////////

namespace
{
  struct SkinnedTriangleAccessor
  {
    const plMeshBufferResourceDescriptor* m_pDesc = nullptr;
    const plUInt16* m_pIndices16 = nullptr;
    const plUInt32* m_pIndices32 = nullptr;

    SkinnedTriangleAccessor(const plMeshBufferResourceDescriptor& desc)
      : m_pDesc(&desc)
    {
      if (desc.HasIndexBuffer())
      {
        if (desc.Uses32BitIndices())
          m_pIndices32 = reinterpret_cast<const plUInt32*>(desc.GetIndexBufferData().GetPtr());
        else
          m_pIndices16 = reinterpret_cast<const plUInt16*>(desc.GetIndexBufferData().GetPtr());
      }
    }

    PL_ALWAYS_INLINE plUInt32 GetVertexIndex(plUInt32 uiTriangle, plUInt32 uiCorner) const
    {
      const plUInt32 i = uiTriangle * 3 + uiCorner;

      if (m_pIndices32)
        return m_pIndices32[i];
      if (m_pIndices16)
        return m_pIndices16[i];

      return i;
    }
  };

  // skins a bind-pose position with up to 4 weighted bone matrices from the pose snapshot
  plVec3 SkinPosition(const plVec3& vBindPos, const plVec4U16& boneIndices, const plVec4& vWeights, plArrayPtr<const plMat4> boneMatrices)
  {
    plVec3 vResult = plVec3::MakeZero();
    float fTotalWeight = 0.0f;

    for (plUInt32 j = 0; j < 4; ++j)
    {
      const float fWeight = vWeights.GetData()[j];
      const plUInt32 uiBone = boneIndices.GetData()[j];

      if (fWeight <= 0.0f || uiBone >= boneMatrices.GetCount())
        continue;

      vResult += boneMatrices[uiBone].TransformPosition(vBindPos) * fWeight;
      fTotalWeight += fWeight;
    }

    if (fTotalWeight < 0.001f)
      return vBindPos;

    return vResult / fTotalWeight;
  }
} // namespace

void plParticleInitializer_SkinnedMeshPosition::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, true);

  // the surface normal for oriented spawning (quad Fixed: Start Axis orientation)
  CreateStream("Axis", plProcessingStream::DataType::Float3, &m_pStreamAxis, true);

  m_pStreamVelocity = nullptr;

  if (m_bSetVelocity)
  {
    CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, true);
  }
}

bool plParticleInitializer_SkinnedMeshPosition::UpdateMeshSamplingData(const plCpuMeshResource& mesh)
{
  const plMeshBufferResourceDescriptor& desc = mesh.GetDescriptor().MeshBufferDesc();

  const plUInt32 uiTriangles = desc.GetPrimitiveCount();
  if (uiTriangles == 0 || desc.GetVertexCount() == 0 || !desc.GetVertexStreamConfig().HasSkinningData())
    return false;

  if (m_SpawnMode != plParticleMeshSpawnMode::Surface)
    return true;

  if (m_uiCdfMeshIdHash == m_hMesh.GetResourceIDHash() && m_uiCdfChangeCounter == mesh.GetCurrentResourceChangeCounter())
    return m_fTotalArea > 0.0f;

  m_uiCdfMeshIdHash = m_hMesh.GetResourceIDHash();
  m_uiCdfChangeCounter = mesh.GetCurrentResourceChangeCounter();
  m_TriangleAreaCdf.SetCountUninitialized(uiTriangles);

  const SkinnedTriangleAccessor accessor(desc);

  float fTotal = 0.0f;
  for (plUInt32 t = 0; t < uiTriangles; ++t)
  {
    const plVec3 a = desc.GetPosition(accessor.GetVertexIndex(t, 0));
    const plVec3 b = desc.GetPosition(accessor.GetVertexIndex(t, 1));
    const plVec3 c = desc.GetPosition(accessor.GetVertexIndex(t, 2));

    fTotal += 0.5f * (b - a).CrossRH(c - a).GetLength();
    m_TriangleAreaCdf[t] = fTotal;
  }

  m_fTotalArea = fTotal;
  return m_fTotalArea > 0.0f;
}

void plParticleInitializer_SkinnedMeshPosition::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Skinned Mesh Position");

  const plVec3 startVel = GetOwnerSystem()->GetParticleStartVelocity();

  plVec4* pPosition = m_pStreamPosition->GetWritableData<plVec4>();
  plVec3* pAxis = m_pStreamAxis->GetWritableData<plVec3>();
  plVec3* pVelocity = m_bSetVelocity ? m_pStreamVelocity->GetWritableData<plVec3>() : nullptr;

  plRandom& rng = GetRNG();

  const auto& snapshot = GetOwnerEffect()->GetSkinningSnapshot();

  auto SpawnFallback = [&](plUInt64 i)
  {
    pPosition[i] = GetOwnerEffect()->GetTransform().m_vPosition.GetAsVec4(0);
    pAxis[i] = plVec3(0, 0, 1);

    if (pVelocity)
    {
      pVelocity[i] = startVel;
    }
  };

  if (!m_hMesh.IsValid() || !snapshot.m_bValid || snapshot.m_BoneMatrices.IsEmpty())
  {
    for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
      SpawnFallback(i);
    return;
  }

  plResourceLock<plCpuMeshResource> pMesh(m_hMesh, plResourceAcquireMode::BlockTillLoaded_NeverFail);
  if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
  {
    for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
      SpawnFallback(i);
    return;
  }

  const plMeshBufferResourceDescriptor& desc = pMesh->GetDescriptor().MeshBufferDesc();

  if (!UpdateMeshSamplingData(*pMesh.GetPointer()))
  {
    for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
      SpawnFallback(i);
    return;
  }

  const SkinnedTriangleAccessor accessor(desc);
  const plArrayPtr<const plMat4> boneMatrices = snapshot.m_BoneMatrices;

  // sampled positions are in the mesh's pose space; this maps them into the simulation space
  // (world space, or effect space for local-space simulations)
  plTransform toSim = snapshot.m_Transform;
  if (GetOwnerEffect()->IsSimulatedInLocalSpace())
  {
    toSim = GetOwnerEffect()->GetTransform().GetInverse() * toSim;
  }

  for (plUInt64 i = uiStartIndex; i < uiStartIndex + uiNumElements; ++i)
  {
    plVec3 pos, normal;

    if (m_SpawnMode == plParticleMeshSpawnMode::Vertex)
    {
      const plUInt32 uiVertex = rng.UIntInRange(desc.GetVertexCount());

      pos = SkinPosition(desc.GetPosition(uiVertex), desc.GetBoneIndices(uiVertex), desc.GetBoneWeights(uiVertex), boneMatrices);
      normal = desc.GetVertexStreamConfig().HasNormal() ? desc.GetNormal(uiVertex) : plVec3(0, 0, 1);
    }
    else
    {
      const float fPick = (float)rng.DoubleZeroToOneExclusive() * m_fTotalArea;

      plUInt32 uiLow = 0;
      plUInt32 uiHigh = m_TriangleAreaCdf.GetCount() - 1;
      while (uiLow < uiHigh)
      {
        const plUInt32 uiMid = uiLow + (uiHigh - uiLow) / 2;
        if (m_TriangleAreaCdf[uiMid] <= fPick)
          uiLow = uiMid + 1;
        else
          uiHigh = uiMid;
      }

      const plUInt32 i0 = accessor.GetVertexIndex(uiLow, 0);
      const plUInt32 i1 = accessor.GetVertexIndex(uiLow, 1);
      const plUInt32 i2 = accessor.GetVertexIndex(uiLow, 2);

      // skin the three corners, then interpolate - correct under animation, cheap enough
      const plVec3 a = SkinPosition(desc.GetPosition(i0), desc.GetBoneIndices(i0), desc.GetBoneWeights(i0), boneMatrices);
      const plVec3 b = SkinPosition(desc.GetPosition(i1), desc.GetBoneIndices(i1), desc.GetBoneWeights(i1), boneMatrices);
      const plVec3 c = SkinPosition(desc.GetPosition(i2), desc.GetBoneIndices(i2), desc.GetBoneWeights(i2), boneMatrices);

      float fU = (float)rng.DoubleZeroToOneInclusive();
      float fV = (float)rng.DoubleZeroToOneInclusive();
      if (fU + fV > 1.0f)
      {
        fU = 1.0f - fU;
        fV = 1.0f - fV;
      }

      pos = a + fU * (b - a) + fV * (c - a);

      // face normal of the posed triangle - follows the animation without skinning the vertex normals
      normal = (b - a).CrossRH(c - a);
    }

    normal = toSim.m_qRotation * normal;
    normal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();

    pAxis[i] = normal;

    if (pVelocity)
    {
      const float fSpeed = (float)rng.DoubleVariance(m_Speed.m_Value, m_Speed.m_fVariance);

      pVelocity[i] = startVel + normal * fSpeed;
    }

    pPosition[i] = (toSim * pos).GetAsVec4(0);
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Initializer_ParticleInitializer_SkinnedMeshPosition);
