#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Math/Color16f.h>
#include <Foundation/Math/Float16.h>
#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Interfaces/WindWorldModule.h>
#include <Core/World/World.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Raycast.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>
#include <ParticlePlugin/Type/GPU/GPUParticleRenderer.h>
#include <ParticlePlugin/Type/GPU/ParticleTypeGPU.h>
#include <ParticlePlugin/Type/Point/PointRenderer.h>
#include <ParticlePlugin/Type/Quad/QuadParticleRenderer.h>
#include <ParticlePlugin/Type/Trail/TrailRenderer.h>
#include <RendererCore/Lights/GPUParticleDataProvider.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Textures/Texture2DResource.h>
#include <RendererFoundation/Device/Device.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTypeGPUFactory, 1, plRTTIDefaultAllocator<plParticleTypeGPUFactory>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("MaxParticles", m_uiMaxParticles)->AddAttributes(new plDefaultValueAttribute(10000), new plClampValueAttribute(64, 1048576)),
    PL_ENUM_MEMBER_PROPERTY("RenderMode", plParticleTypeRenderMode, m_RenderMode),
    PL_ENUM_MEMBER_PROPERTY("GPURenderType", plGPUParticleRenderType, m_GPURenderType),
    PL_MEMBER_PROPERTY("Texture", m_sTexture)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Texture_2D")),
    PL_MEMBER_PROPERTY("Gravity", m_fGravity)->AddAttributes(new plDefaultValueAttribute(9.81f)),
    PL_MEMBER_PROPERTY("Drag", m_fDragCoefficient)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("WindStrength", m_fWindStrength)->AddAttributes(new plDefaultValueAttribute(0.0f)),
    PL_MEMBER_PROPERTY("DepthCollision", m_bEnableDepthCollision),
    PL_MEMBER_PROPERTY("SDFCollision", m_bEnableSDFCollision),
    PL_ENUM_MEMBER_PROPERTY("CollisionReaction", plParticleRaycastHitReaction, m_CollisionReaction),
    PL_MEMBER_PROPERTY("BounceFactor", m_fCollisionBounceFactor)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("SlideFactor", m_fCollisionSlideFactor)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("CollisionThickness", m_fCollisionThickness)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.01f, 5.0f)),
    PL_MEMBER_PROPERTY("ColorStart", m_ColorStart)->AddAttributes(new plDefaultValueAttribute(plColor::White)),
    PL_MEMBER_PROPERTY("ColorEnd", m_ColorEnd)->AddAttributes(new plDefaultValueAttribute(plColor(1, 1, 1, 0))),
    PL_MEMBER_PROPERTY("MaxTrailPoints", m_uiMaxTrailPoints)->AddAttributes(new plDefaultValueAttribute(16), new plClampValueAttribute(4, 64)),
    PL_MEMBER_PROPERTY("VelocityStretch", m_fVelocityStretch)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 20.0f)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    // Retired in favour of the system's SimulationTarget: author with the normal CPU modules and
    // switch the target to GPU. Still reflected so existing assets keep loading.
    new plHiddenAttribute(),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleTypeGPU, 1, plRTTIDefaultAllocator<plParticleTypeGPU>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

const plRTTI* plParticleTypeGPUFactory::GetTypeType() const
{
  return plGetStaticRTTI<plParticleTypeGPU>();
}

void plParticleTypeGPUFactory::CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const
{
  plParticleTypeGPU* pType = static_cast<plParticleTypeGPU*>(pObject);

  pType->m_uiMaxGPUParticles = m_uiMaxParticles;
  pType->m_RenderMode = m_RenderMode;
  pType->m_hTexture = plResourceManager::LoadResource<plTexture2DResource>(m_sTexture);
  pType->m_fGravity = m_fGravity;
  pType->m_fDragCoefficient = m_fDragCoefficient;
  pType->m_fWindStrength = m_fWindStrength;
  pType->m_bEnableDepthCollision = m_bEnableDepthCollision;
  pType->m_bEnableSDFCollision = m_bEnableSDFCollision;
  pType->m_CollisionReaction = m_CollisionReaction;
  pType->m_fCollisionBounceFactor = m_fCollisionBounceFactor;
  pType->m_fCollisionSlideFactor = m_fCollisionSlideFactor;
  pType->m_fCollisionThickness = m_fCollisionThickness;
  pType->m_ColorStart = m_ColorStart;
  pType->m_ColorEnd = m_ColorEnd;
  pType->m_GPURenderType = m_GPURenderType;
  pType->m_uiMaxTrailPoints = m_uiMaxTrailPoints;
  pType->m_fVelocityStretch = m_fVelocityStretch;

  // the authored type animates colour and size from its own ColorStart/ColorEnd properties rather
  // than from gradient / curve modules, which the simulate shader gates behind these bits
  pType->m_bLowered = false;
  pType->m_uiFeatureFlags = GPU_PARTICLE_FEATURE_LEGACY_COLOR_LERP | GPU_PARTICLE_FEATURE_LEGACY_SIZE_SHRINK;
}

enum class TypeGPUVersion
{
  Version_1 = 1,
  Version_2_RenderType,

  Version_Count,
  Version_Current = Version_Count - 1
};

void plParticleTypeGPUFactory::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = (plUInt8)TypeGPUVersion::Version_Current;
  inout_stream << uiVersion;

  inout_stream << m_uiMaxParticles;
  inout_stream << m_RenderMode;
  inout_stream << m_sTexture;
  inout_stream << m_fGravity;
  inout_stream << m_fDragCoefficient;
  inout_stream << m_fWindStrength;
  inout_stream << m_bEnableDepthCollision;
  inout_stream << m_bEnableSDFCollision;
  inout_stream << m_CollisionReaction;
  inout_stream << m_fCollisionBounceFactor;
  inout_stream << m_fCollisionSlideFactor;
  inout_stream << m_fCollisionThickness;
  inout_stream << m_ColorStart;
  inout_stream << m_ColorEnd;

  // Version_2_RenderType
  inout_stream << m_GPURenderType;
  inout_stream << m_uiMaxTrailPoints;
  inout_stream << m_fVelocityStretch;
}

void plParticleTypeGPUFactory::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= (plUInt8)TypeGPUVersion::Version_Current, "Invalid version {0}", uiVersion);

  inout_stream >> m_uiMaxParticles;
  inout_stream >> m_RenderMode;
  inout_stream >> m_sTexture;
  inout_stream >> m_fGravity;
  inout_stream >> m_fDragCoefficient;
  inout_stream >> m_fWindStrength;
  inout_stream >> m_bEnableDepthCollision;
  inout_stream >> m_bEnableSDFCollision;
  inout_stream >> m_CollisionReaction;
  inout_stream >> m_fCollisionBounceFactor;
  inout_stream >> m_fCollisionSlideFactor;
  inout_stream >> m_fCollisionThickness;
  inout_stream >> m_ColorStart;
  inout_stream >> m_ColorEnd;

  if (uiVersion >= (plUInt8)TypeGPUVersion::Version_2_RenderType)
  {
    inout_stream >> m_GPURenderType;
    inout_stream >> m_uiMaxTrailPoints;
    inout_stream >> m_fVelocityStretch;
  }
}

//////////////////////////////////////////////////////////////////////////

plParticleTypeGPU::plParticleTypeGPU() = default;

plParticleTypeGPU::~plParticleTypeGPU()
{
  DestroyGPUBuffers();
}

void plParticleTypeGPU::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
  CreateStream("LifeTime", plProcessingStream::DataType::Half2, &m_pStreamLifeTime, false);
  CreateStream("Size", plProcessingStream::DataType::Half, &m_pStreamSize, false);
  CreateStream("Color", plProcessingStream::DataType::Half4, &m_pStreamColor, false);
  CreateStream("RotationSpeed", plProcessingStream::DataType::Half, &m_pStreamRotationSpeed, false);
  CreateStream("RotationOffset", plProcessingStream::DataType::Half, &m_pStreamRotationOffset, false);
}

void plParticleTypeGPU::StepParticleSystem(const plTime& tDiff, plUInt32 uiNumNewParticles)
{
  const plVec3 vPos = GetOwnerSystem()->GetTransform().m_vPosition;

  if (m_bPullAlongFirstStep)
  {
    m_bPullAlongFirstStep = false;
    m_vApplyPull.SetZero();
  }
  else
  {
    m_vApplyPull = (vPos - m_vLastEmitterPosition) * m_fPullAlongStrength;
  }

  m_vLastEmitterPosition = vPos;

  // flies: the timer runs on the CPU, the shader only sees "change heading this frame"
  m_bFliesChangeDirThisFrame = false;

  if (m_bFlies && m_fFliesSpeed > 0.0f)
  {
    const plTime tCur = GetOwnerEffect()->GetTotalEffectLifeTime();

    if (tCur >= m_FliesTimeToChangeDir)
    {
      m_FliesTimeToChangeDir = tCur + plTime::MakeFromSeconds(m_fFliesPathLength / m_fFliesSpeed);
      m_bFliesChangeDirThisFrame = true;
    }
  }
}

void plParticleTypeGPU::QueryOptionalStreams()
{
  m_pStreamAxis = GetOwnerSystem()->QueryStream("Axis", plProcessingStream::DataType::Float3);
  m_pStreamVariation = GetOwnerSystem()->QueryStream("Variation", plProcessingStream::DataType::Int);
}

void plParticleTypeGPU::InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements)
{
  m_uiNewParticleStartIndex = uiStartIndex;
  m_uiNumNewParticles = (plUInt32)uiNumElements;
}

void plParticleTypeGPU::EnsureGPUBuffers() const
{
  if (m_bGPUBuffersCreated)
    return;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  // Particle structured buffer (zero-initialized so all Flags=0 → dead)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plGPUParticle);
    desc.m_uiTotalSize = m_uiMaxGPUParticles * desc.m_uiStructSize;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
    desc.m_ResourceAccess.m_bImmutable = false;

    plDynamicArray<plUInt8> zeroData;
    zeroData.SetCountUninitialized(desc.m_uiTotalSize);
    plMemoryUtils::ZeroFill(zeroData.GetData(), zeroData.GetCount());
    m_hParticleBuffer = pDevice->CreateBuffer(desc, zeroData.GetArrayPtr());
  }

  // Counter buffer (ByteAddressBuffer): [0] aliveCount, [4] freeCount, [8+] free-slot stack.
  // Seeded with every slot free so emission pops recycled slots and never overwrites live particles.
  {
    plGALBufferCreationDescription desc;
    desc.m_uiTotalSize = (2 + m_uiMaxGPUParticles) * sizeof(plUInt32);
    desc.m_BufferFlags = plGALBufferUsageFlags::UnorderedAccess | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::ByteAddressBuffer;
    desc.m_ResourceAccess.m_bImmutable = false;

    plDynamicArray<plUInt32> initData;
    initData.SetCountUninitialized(2 + m_uiMaxGPUParticles);
    initData[0] = 0;                   // aliveCount
    initData[1] = m_uiMaxGPUParticles; // freeCount

    for (plUInt32 i = 0; i < m_uiMaxGPUParticles; ++i)
    {
      initData[2 + i] = i;
    }

    m_hCounterBuffer = pDevice->CreateBuffer(desc, initData.GetArrayPtr().ToByteArray());
  }

  // Sort keys, one per alive-list entry. Both buffers are padded to the power of two the
  // bitonic passes need, so the padding can hold +inf and sort to the end.
  const plUInt32 uiSortCount = plMath::PowerOfTwo_Ceil(m_uiMaxGPUParticles);

  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(float);
    desc.m_uiTotalSize = uiSortCount * sizeof(float);
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
    desc.m_ResourceAccess.m_bImmutable = false;

    plDynamicArray<plUInt8> zeroData;
    zeroData.SetCountUninitialized(desc.m_uiTotalSize);
    plMemoryUtils::ZeroFill(zeroData.GetData(), zeroData.GetCount());
    m_hSortKeyBuffer = pDevice->CreateBuffer(desc, zeroData.GetArrayPtr());
  }

  // Compacted alive-slot list, rebuilt by every simulate dispatch
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plUInt32);
    desc.m_uiTotalSize = uiSortCount * sizeof(plUInt32);
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
    desc.m_ResourceAccess.m_bImmutable = false;

    plDynamicArray<plUInt8> zeroData;
    zeroData.SetCountUninitialized(desc.m_uiTotalSize);
    plMemoryUtils::ZeroFill(zeroData.GetData(), zeroData.GetCount());
    m_hAliveListBuffer = pDevice->CreateBuffer(desc, zeroData.GetArrayPtr());
  }

  // Indirect draw args {vertexCount, instanceCount, firstVertex, firstInstance}; the simulate
  // dispatch grows vertexCount per surviving particle, so the draw covers exactly the alive set
  {
    plGALBufferCreationDescription desc;
    desc.m_uiTotalSize = 4 * sizeof(plUInt32);
    desc.m_BufferFlags = plGALBufferUsageFlags::UnorderedAccess | plGALBufferUsageFlags::ByteAddressBuffer | plGALBufferUsageFlags::DrawIndirect;
    desc.m_ResourceAccess.m_bImmutable = false;

    const plUInt32 args[4] = {0, 1, 0, 0};
    m_hDrawArgsBuffer = pDevice->CreateBuffer(desc, plMakeArrayPtr(args).ToByteArray());
  }

  // Per-particle structs for the quad renderer, written by the simulate dispatch. Both quad
  // layouts must be bound, but only the one the orientation selects is ever indexed, so the
  // other is allocated at a single element.
  if (m_bRenderAsQuad)
  {
    const bool bBillboard = m_QuadOrientation == plQuadParticleOrientation::Billboard;

    auto CreateStructured = [pDevice](plUInt32 uiStructSize, plUInt32 uiCount) -> plGALBufferHandle
    {
      plGALBufferCreationDescription desc;
      desc.m_uiStructSize = uiStructSize;
      desc.m_uiTotalSize = uiStructSize * uiCount;
      desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
      desc.m_ResourceAccess.m_bImmutable = false;

      plDynamicArray<plUInt8> zeroData;
      zeroData.SetCountUninitialized(desc.m_uiTotalSize);
      plMemoryUtils::ZeroFill(zeroData.GetData(), zeroData.GetCount());
      return pDevice->CreateBuffer(desc, zeroData.GetArrayPtr());
    };

    m_hQuadBaseBuffer = CreateStructured(sizeof(plBaseParticleShaderData), m_uiMaxGPUParticles);
    m_hQuadBillboardBuffer = CreateStructured(sizeof(plBillboardQuadParticleShaderData), bBillboard ? m_uiMaxGPUParticles : 1);
    m_hQuadTangentBuffer = CreateStructured(sizeof(plTangentQuadParticleShaderData), bBillboard ? 1 : m_uiMaxGPUParticles);

    if (m_CPURenderPath == CPURenderPath::Trail)
    {
      m_hTrailDataBuffer = CreateStructured(sizeof(plTrailParticleShaderData), m_uiMaxGPUParticles);
      m_hTrailPointsPackedBuffer = CreateStructured(sizeof(plVec4), m_uiMaxGPUParticles * m_uiMaxTrailPoints);
    }
  }

  // Trail position buffer (only for Trail render type)
  if (m_GPURenderType == plGPUParticleRenderType::Trail)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plVec4);
    desc.m_uiTotalSize = m_uiMaxGPUParticles * m_uiMaxTrailPoints * desc.m_uiStructSize;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess;
    desc.m_ResourceAccess.m_bImmutable = false;

    plDynamicArray<plUInt8> zeroData;
    zeroData.SetCountUninitialized(desc.m_uiTotalSize);
    plMemoryUtils::ZeroFill(zeroData.GetData(), zeroData.GetCount());
    m_hTrailPositionBuffer = pDevice->CreateBuffer(desc, zeroData.GetArrayPtr());
  }

  m_bGPUBuffersCreated = true;
  m_uiTrailWriteIndex = 0;
  m_uiActiveSlotCount = 0;
}

void plParticleTypeGPU::DestroyGPUBuffers()
{
  if (!m_bGPUBuffersCreated)
    return;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  if (!m_hParticleBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hParticleBuffer);
  if (!m_hCounterBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hCounterBuffer);
  if (!m_hAliveListBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hAliveListBuffer);
  if (!m_hDrawArgsBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hDrawArgsBuffer);
  if (!m_hTrailPositionBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hTrailPositionBuffer);
  if (!m_hQuadBaseBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hQuadBaseBuffer);
  if (!m_hQuadBillboardBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hQuadBillboardBuffer);
  if (!m_hQuadTangentBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hQuadTangentBuffer);
  if (!m_hTrailDataBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hTrailDataBuffer);
  if (!m_hTrailPointsPackedBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hTrailPointsPackedBuffer);
  if (!m_hSortKeyBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hSortKeyBuffer);

  m_hParticleBuffer.Invalidate();
  m_hCounterBuffer.Invalidate();
  m_hAliveListBuffer.Invalidate();
  m_hDrawArgsBuffer.Invalidate();
  m_hTrailPositionBuffer.Invalidate();
  m_hQuadBaseBuffer.Invalidate();
  m_hQuadBillboardBuffer.Invalidate();
  m_hQuadTangentBuffer.Invalidate();
  m_hTrailDataBuffer.Invalidate();
  m_hTrailPointsPackedBuffer.Invalidate();
  m_hSortKeyBuffer.Invalidate();
  m_bGPUBuffersCreated = false;
}

void plParticleTypeGPU::RequestRequiredWorldModulesForCache(plParticleWorldModule* pParticleModule)
{
  pParticleModule->CacheWorldModule<plPhysicsWorldModuleInterface>();
}

void plParticleTypeGPU::ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const
{
  EnsureGPUBuffers();

  const plUInt32 numParticles = m_uiNumNewParticles;
  m_uiNumNewParticles = 0;

  plArrayPtr<plGPUParticle> newParticles;
  if (numParticles > 0)
  {
    newParticles = PL_NEW_ARRAY(plFrameAllocator::GetCurrentAllocator(), plGPUParticle, numParticles);

    const plUInt64 srcStart = m_uiNewParticleStartIndex;
    const plVec4* pPosition = m_pStreamPosition->GetData<plVec4>();
    const plVec3* pVelocity = m_pStreamVelocity->GetData<plVec3>();
    const plFloat16Vec2* pLifeTime = m_pStreamLifeTime->GetData<plFloat16Vec2>();
    const plFloat16* pSize = m_pStreamSize->GetData<plFloat16>();
    const plColorLinear16f* pColor = m_pStreamColor->GetData<plColorLinear16f>();
    const plFloat16* pRotSpeed = m_pStreamRotationSpeed != nullptr ? m_pStreamRotationSpeed->GetData<plFloat16>() : nullptr;
    const plFloat16* pRotOffset = m_pStreamRotationOffset != nullptr ? m_pStreamRotationOffset->GetData<plFloat16>() : nullptr;
    const plVec3* pAxis = m_pStreamAxis != nullptr ? m_pStreamAxis->GetData<plVec3>() : nullptr;
    const plUInt32* pVariation = m_pStreamVariation != nullptr ? m_pStreamVariation->GetData<plUInt32>() : nullptr;

    // 3x10 bit signed, unpacked by UnpackParticleAxis in GPUParticleSimulate.plShader
    auto PackAxis = [](const plVec3& v) -> plUInt32
    {
      auto q = [](float f) -> plUInt32
      {
        const plInt32 i = (plInt32)plMath::Round(plMath::Clamp(f, -1.0f, 1.0f) * 511.0f);
        return (plUInt32)(i & 0x3FF);
      };

      return q(v.x) | (q(v.y) << 10) | (q(v.z) << 20);
    };

    for (plUInt32 i = 0; i < numParticles; ++i)
    {
      const plUInt64 srcIdx = srcStart + i;
      plGPUParticle& p = newParticles[i];

      p.Position = pPosition[srcIdx].GetAsVec3();
      p.Velocity = pVelocity[srcIdx];

      // LifeTime stream: x = remaining seconds, y = 1 / total seconds.
      // The simulate shader stores Life as the remaining-life fraction and ages it by dt * MaxLife.
      const float fRemainingSeconds = (float)pLifeTime[srcIdx].x;
      const float fInvTotalSeconds = plMath::Max((float)pLifeTime[srcIdx].y, 0.0001f);
      p.Life = plMath::Clamp(fRemainingSeconds * fInvTotalSeconds, 0.0f, 1.0f);
      p.MaxLife = fInvTotalSeconds;

      p.Size = pSize[srcIdx];
      p.InitialSize = p.Size;
      p.RotationOffset = pRotOffset != nullptr ? (float)pRotOffset[srcIdx] : 0.0f;
      p.RotationSpeed = pRotSpeed != nullptr ? (float)pRotSpeed[srcIdx] : 0.0f;

      // bit 0 alive, bits 8-15 the atlas variation
      p.Flags = 1;
      if (pVariation != nullptr)
      {
        p.Flags |= (pVariation[srcIdx] & 0xFF) << 8;
      }

      plColorLinear16f col = pColor[srcIdx];
      plUInt32* pColorU32 = reinterpret_cast<plUInt32*>(&p.Color);
      pColorU32[0] = (plUInt32)col.r.GetRawData() | ((plUInt32)col.g.GetRawData() << 16);
      pColorU32[1] = (plUInt32)col.b.GetRawData() | ((plUInt32)col.a.GetRawData() << 16);

      p.AxisPacked = pAxis != nullptr ? PackAxis(pAxis[srcIdx]) : PackAxis(plVec3(0, 0, 1));
    }
  }

  // Compute active slot count including the new particles about to be emitted
  const plUInt32 uiActiveSlotCount = plMath::Min(m_uiActiveSlotCount + numParticles, m_uiMaxGPUParticles);

  switch (m_CPURenderPath)
  {
    case CPURenderPath::Quad:
      AddQuadRenderData(ref_msg, instanceTransform);
      break;
    case CPURenderPath::Point:
      AddPointRenderData(ref_msg, instanceTransform);
      break;
    case CPURenderPath::Trail:
      AddTrailRenderData(ref_msg, instanceTransform);
      break;
    default:
      AddGPURenderData(ref_msg, instanceTransform, uiActiveSlotCount, newParticles);
      break;
  }

  QueueSimulation(instanceTransform, uiActiveSlotCount, newParticles);

  m_uiActiveSlotCount = uiActiveSlotCount;

  // Advance trail write index for trail types
  if (m_GPURenderType == plGPUParticleRenderType::Trail)
  {
    m_uiTrailWriteIndex++;
  }
}

void plParticleTypeGPU::AddQuadRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const
{
  auto pRenderData = ref_msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plParticleQuadRenderData>(nullptr);

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
  pRenderData->m_hDistortionTexture = m_hDistortionTexture;
  pRenderData->m_hSixWayMapA = m_hSixWayMapA;
  pRenderData->m_hSixWayMapB = m_hSixWayMapB;
  pRenderData->m_fSixWayAbsorption = m_fSixWayAbsorption;
  pRenderData->m_fDistortionStrength = m_fDistortionStrength;
  pRenderData->m_LightingMode = m_LightingMode;
  pRenderData->m_fNormalCurvature = m_fNormalCurvature;
  pRenderData->m_fLightDirectionality = m_fLightDirectionality;
  pRenderData->m_hCustomMaterial = m_hCustomMaterial;

  pRenderData->m_uiNumVariationsX = 1;
  pRenderData->m_uiNumVariationsY = 1;
  pRenderData->m_uiNumFlipbookAnimationsX = 1;
  pRenderData->m_uiNumFlipbookAnimationsY = 1;

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

  switch (m_QuadOrientation)
  {
    case plQuadParticleOrientation::Billboard:
      pRenderData->m_QuadModePermutation = "PARTICLE_QUAD_MODE_BILLBOARD";
      break;
    case plQuadParticleOrientation::FixedAxis_EmitterDir:
    case plQuadParticleOrientation::FixedAxis_ParticleDir:
      pRenderData->m_QuadModePermutation = "PARTICLE_QUAD_MODE_AXIS_ALIGNED";
      break;
    default:
      pRenderData->m_QuadModePermutation = "PARTICLE_QUAD_MODE_TANGENTS";
      break;
  }

  pRenderData->m_hGpuBaseDataBuffer = m_hQuadBaseBuffer;
  pRenderData->m_hGpuBillboardDataBuffer = m_hQuadBillboardBuffer;
  pRenderData->m_hGpuTangentDataBuffer = m_hQuadTangentBuffer;
  pRenderData->m_hGpuDrawArgsBuffer = m_hDrawArgsBuffer;

  ref_msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::LitTransparent, plRenderData::Caching::Never);

  // the transparent pass reads what the simulate dispatch wrote
  ref_msg.AddDependency(m_hDrawArgsBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::IndirectArgument);
  ref_msg.AddDependency(m_hQuadBaseBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
  ref_msg.AddDependency(m_hQuadBillboardBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
  ref_msg.AddDependency(m_hQuadTangentBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
}

void plParticleTypeGPU::AddPointRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const
{
  auto pRenderData = ref_msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plParticlePointRenderData>(nullptr);

  pRenderData->m_uiSortingKey = ComputeSortingKey(plParticleTypeRenderMode::Opaque, 0, 0);
  pRenderData->m_vGlobalPosition = instanceTransform.m_vPosition;
  pRenderData->m_GlobalTransform = GetOwnerEffect()->NeedsToApplyTransform() ? instanceTransform : plTransform::MakeIdentity();
  pRenderData->m_TotalEffectLifeTime = GetOwnerEffect()->GetTotalEffectLifeTime();

  pRenderData->m_hGpuBaseDataBuffer = m_hQuadBaseBuffer;
  pRenderData->m_hGpuBillboardDataBuffer = m_hQuadBillboardBuffer;
  pRenderData->m_hGpuDrawArgsBuffer = m_hDrawArgsBuffer;

  ref_msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::LitOpaque, plRenderData::Caching::Never);

  ref_msg.AddDependency(m_hDrawArgsBuffer, plDefaultRenderDataCategories::LitOpaque, plGALResourceState::IndirectArgument);
  ref_msg.AddDependency(m_hQuadBaseBuffer, plDefaultRenderDataCategories::LitOpaque, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
  ref_msg.AddDependency(m_hQuadBillboardBuffer, plDefaultRenderDataCategories::LitOpaque, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
}

void plParticleTypeGPU::AddTrailRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const
{
  auto pRenderData = ref_msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plParticleTrailRenderData>(nullptr);

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
  pRenderData->m_hCustomMaterial = m_hCustomMaterial;
  pRenderData->m_hDistortionTexture = m_hDistortionTexture;
  pRenderData->m_fDistortionStrength = m_fDistortionStrength;
  pRenderData->m_LightingMode = m_LightingMode;
  pRenderData->m_fNormalCurvature = m_fNormalCurvature;
  pRenderData->m_fLightDirectionality = m_fLightDirectionality;
  pRenderData->m_uiMaxTrailPoints = (plUInt16)m_uiMaxTrailPoints;

  // the GPU keeps every trail slot filled, so there is no partial snapshot to blend towards
  pRenderData->m_fSnapshotFraction = 1.0f;

  pRenderData->m_uiNumVariationsX = 1;
  pRenderData->m_uiNumVariationsY = 1;
  pRenderData->m_uiNumFlipbookAnimationsX = 1;
  pRenderData->m_uiNumFlipbookAnimationsY = 1;

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

  pRenderData->m_hGpuBaseDataBuffer = m_hQuadBaseBuffer;
  pRenderData->m_hGpuTrailDataBuffer = m_hTrailDataBuffer;
  pRenderData->m_hGpuTrailPointsBuffer = m_hTrailPointsPackedBuffer;
  pRenderData->m_hGpuDrawArgsBuffer = m_hDrawArgsBuffer;

  ref_msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::LitTransparent, plRenderData::Caching::Never);

  ref_msg.AddDependency(m_hDrawArgsBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::IndirectArgument);
  ref_msg.AddDependency(m_hQuadBaseBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
  ref_msg.AddDependency(m_hTrailDataBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
  ref_msg.AddDependency(m_hTrailPointsPackedBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
}

void plParticleTypeGPU::AddGPURenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform, plUInt32 uiActiveSlotCount, plArrayPtr<plGPUParticle> newParticles) const
{
  // Create render data
  auto pRenderData = ref_msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plGPUParticleRenderData>(nullptr);

  pRenderData->m_uiSortingKey = ComputeSortingKey(m_RenderMode, m_hTexture.GetResourceIDHash(), 0);
  pRenderData->m_vGlobalPosition = instanceTransform.m_vPosition;
  pRenderData->m_GlobalTransform = GetOwnerEffect()->NeedsToApplyTransform() ? instanceTransform : plTransform::MakeIdentity();
  pRenderData->m_TotalEffectLifeTime = GetOwnerEffect()->GetTotalEffectLifeTime();

  pRenderData->m_hTexture = m_hTexture;
  pRenderData->m_RenderMode = (plParticleTypeRenderMode::Enum)m_RenderMode;
  pRenderData->m_uiMaxParticles = m_uiMaxGPUParticles;
  pRenderData->m_uiActiveSlotCount = uiActiveSlotCount;

  pRenderData->m_hParticleBuffer = m_hParticleBuffer;
  pRenderData->m_hCounterBuffer = m_hCounterBuffer;
  pRenderData->m_hAliveListBuffer = m_hAliveListBuffer;
  pRenderData->m_hDrawArgsBuffer = m_hDrawArgsBuffer;

  pRenderData->m_NewParticles = newParticles;

  pRenderData->m_bEnableDepthCollision = m_bEnableDepthCollision;
  pRenderData->m_bEnableSDFCollision = m_bEnableSDFCollision;
  pRenderData->m_uiCollisionReaction = m_CollisionReaction;

  pRenderData->m_uiGPURenderType = m_GPURenderType;
  pRenderData->m_uiMaxTrailPoints = m_uiMaxTrailPoints;
  pRenderData->m_uiTrailWriteIndex = m_uiTrailWriteIndex;
  pRenderData->m_hTrailPositionBuffer = m_hTrailPositionBuffer;
  pRenderData->m_fVelocityStretch = m_fVelocityStretch;

  ref_msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::LitTransparent, plRenderData::Caching::Never);

  // The transparent pass consumes what the simulate dispatch wrote: the draw args must be in
  // indirect-argument state and the alive list readable in the vertex stage. Declaring them as
  // category dependencies makes the render graph emit those transitions before the draw.
  ref_msg.AddDependency(m_hDrawArgsBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::IndirectArgument);
  ref_msg.AddDependency(m_hAliveListBuffer, plDefaultRenderDataCategories::LitTransparent, plGALResourceState::ShaderRead, plGALShaderStageFlags::VertexShader);
}

// Copies the world's force-field snapshot into frame memory, transformed into simulation space.
// The CPU behavior does that transform per particle per field; once per system is equivalent.
plArrayPtr<plGPUParticleForceField> plParticleTypeGPU::GatherForceFields(const plTransform& instanceTransform) const
{
  const plArrayPtr<const plParticleForceFieldData> fields = GetOwnerSystem()->GetOwnerWorldModule()->GetForceFieldData();
  if (fields.IsEmpty())
    return {};

  const bool bLocalSpace = GetOwnerEffect()->IsSimulatedInLocalSpace();
  const plTransform invEffect = bLocalSpace ? instanceTransform.GetInverse() : plTransform::MakeIdentity();

  auto result = PL_NEW_ARRAY(plFrameAllocator::GetCurrentAllocator(), plGPUParticleForceField, fields.GetCount());

  for (plUInt32 i = 0; i < fields.GetCount(); ++i)
  {
    const plParticleForceFieldData& src = fields[i];
    plGPUParticleForceField& dst = result[i];

    dst.Center = bLocalSpace ? (invEffect * src.m_vCenter) : src.m_vCenter;
    dst.Axis = bLocalSpace ? (invEffect.m_qRotation * src.m_vAxis) : src.m_vAxis;
    dst.Radius = src.m_fRadius;
    dst.Strength = src.m_fStrength;
    dst.FalloffStart = src.m_fFalloffStart;
    dst.InvBoxHalfExtents = src.m_vInvBoxHalfExtents;
    dst.FieldType = src.m_uiType;
    dst.FieldShape = src.m_uiShape;
    dst.ForceFieldPadding0 = 0;
    dst.ForceFieldPadding1 = 0;

    // the box test rotates the particle-relative vector into field space; for a local-space
    // simulation that vector must first be brought back into world space
    const plQuat qInv = bLocalSpace ? (src.m_qInvRotation * instanceTransform.m_qRotation) : src.m_qInvRotation;
    dst.InvRotation.Set(qInv.x, qInv.y, qInv.z, qInv.w);
  }

  return result;
}

// Registers this system with the data provider so the compute pass simulates it this frame.
void plParticleTypeGPU::QueueSimulation(const plTransform& instanceTransform, plUInt32 uiActiveSlotCount, plArrayPtr<plGPUParticle> newParticles) const
{
  {
    plGPUParticleSystemInfo sysInfo;
    sysInfo.m_hParticleBuffer = m_hParticleBuffer;
    sysInfo.m_hCounterBuffer = m_hCounterBuffer;
    sysInfo.m_hAliveListBuffer = m_hAliveListBuffer;
    sysInfo.m_hDrawArgsBuffer = m_hDrawArgsBuffer;
    sysInfo.m_uiMaxParticles = m_uiMaxGPUParticles;
    sysInfo.m_uiActiveSlotCount = uiActiveSlotCount;
    sysInfo.m_NewParticles = newParticles;

    sysInfo.m_uiFeatureFlags = m_uiFeatureFlags;

    // Gravity: lowered systems use the physics module's gravity scaled by the module factor;
    // the authored GPU type's scalar means "downwards at this strength" (Z-up)
    if (m_bLowered)
    {
      plVec3 vGravity = plVec3(0, 0, -10.0f);
      if (auto pPhysics = static_cast<const plPhysicsWorldModuleInterface*>(GetOwnerSystem()->GetOwnerWorldModule()->GetCachedWorldModule(plGetStaticRTTI<plPhysicsWorldModuleInterface>())))
      {
        vGravity = pPhysics->GetGravity();
      }

      sysInfo.m_vGravity = vGravity * m_fGravityFactor;
    }
    else
    {
      sysInfo.m_vGravity = plVec3(0, 0, -m_fGravity);
    }

    sysInfo.m_fRiseSpeed = m_fRiseSpeed;
    sysInfo.m_fFriction = m_fFriction;
    sysInfo.m_fLinearDrag = m_fLinearDrag;
    sysInfo.m_fDragCoefficient = m_fDragCoefficient;
    sysInfo.m_fWindStrength = m_fWindStrength;

    // Sample wind at the effect's position
    if (const plWindWorldModuleInterface* pWind = GetOwnerEffect()->GetWorld()->GetModuleReadOnly<plWindWorldModuleInterface>())
    {
      sysInfo.m_vWindDirection = pWind->GetWindAt(instanceTransform.m_vPosition);
    }

    sysInfo.m_fFadeStartAlpha = m_fFadeStartAlpha;
    sysInfo.m_fFadeExponent = m_fFadeExponent;
    sysInfo.m_fColorMaxSpeed = m_fColorMaxSpeed;
    sysInfo.m_fSizeBase = m_fSizeBase;
    sysInfo.m_fSizeScale = m_fSizeScale;

    sysInfo.m_vTurbScroll = m_vTurbScrollSpeed * (float)GetOwnerEffect()->GetTotalEffectLifeTime().GetSeconds();
    sysInfo.m_fTurbStrength = m_fTurbStrength;
    sysInfo.m_fTurbFrequency = m_fTurbFrequency;
    sysInfo.m_uiTurbOctaves = m_uiTurbOctaves;

    // the vector field box is anchored at the effect
    sysInfo.m_vVectorFieldCenter = instanceTransform * m_vVectorFieldOffset;
    sysInfo.m_vVectorFieldInvSize = plVec3(1.0f / m_vVectorFieldSize.x, 1.0f / m_vVectorFieldSize.y, 1.0f / m_vVectorFieldSize.z);
    sysInfo.m_qVectorFieldRotation = instanceTransform.m_qRotation;
    sysInfo.m_fVectorFieldStrength = m_fVectorFieldStrength;

    sysInfo.m_vBoundsCenter = instanceTransform * m_vBoundsOffset;
    sysInfo.m_vBoundsExtents = m_vBoundsExtents;

    // an effect-origin attractor rides along with the effect; a custom position is already world-space
    sysInfo.m_vAttractorPos = m_bAttractorAtEffectOrigin ? instanceTransform.m_vPosition : m_vAttractorPos;
    sysInfo.m_vAttractorAxis = m_bAttractorAtEffectOrigin ? (instanceTransform.m_qRotation * m_vAttractorAxis) : m_vAttractorAxis;
    sysInfo.m_fAttractorForce = m_fAttractorForce;
    sysInfo.m_fAttractorMaxDist = m_fAttractorMaxDist;
    sysInfo.m_fAttractorMinDist = m_fAttractorMinDist;
    sysInfo.m_uiAttractorShape = m_uiAttractorShape;

    sysInfo.m_vPullAlong = m_vApplyPull;

    sysInfo.m_vSphereCenter = instanceTransform * m_vSphereOffset;
    sysInfo.m_fSphereRadius = m_fSphereRadius;

    sysInfo.m_hGradientLUT = m_hGradientLUT;
    sysInfo.m_hSizeCurveLUT = m_hSizeCurveLUT;
    sysInfo.m_hTurbulenceNoise = m_hTurbulenceNoise;
    sysInfo.m_hVectorFieldTexture = m_hVectorFieldTexture;

    sysInfo.m_bEnableDepthCollision = m_bEnableDepthCollision;
    sysInfo.m_bEnableSDFCollision = m_bEnableSDFCollision;
    sysInfo.m_uiCollisionReaction = m_CollisionReaction;
    sysInfo.m_fCollisionBounceFactor = m_fCollisionBounceFactor;
    sysInfo.m_fCollisionSlideFactor = m_fCollisionSlideFactor;
    sysInfo.m_fCollisionThickness = m_fCollisionThickness;
    sysInfo.m_ColorStart = m_ColorStart;
    sysInfo.m_ColorEnd = m_ColorEnd;

    sysInfo.m_uiGPURenderType = m_GPURenderType;
    sysInfo.m_uiMaxTrailPoints = m_uiMaxTrailPoints;
    sysInfo.m_uiTrailWriteIndex = m_uiTrailWriteIndex;
    sysInfo.m_hTrailPositionBuffer = m_hTrailPositionBuffer;
    sysInfo.m_fVelocityStretch = m_fVelocityStretch;

    // quad rendering: the simulate dispatch also writes the renderer's per-particle structs
    sysInfo.m_hQuadBaseBuffer = m_hQuadBaseBuffer;
    sysInfo.m_hQuadBillboardBuffer = m_hQuadBillboardBuffer;
    sysInfo.m_hQuadTangentBuffer = m_hQuadTangentBuffer;
    sysInfo.m_hTrailDataBuffer = m_hTrailDataBuffer;
    sysInfo.m_hTrailPointsPackedBuffer = m_hTrailPointsPackedBuffer;
    sysInfo.m_hSortKeyBuffer = m_hSortKeyBuffer;
    sysInfo.m_uiSortCount = plMath::PowerOfTwo_Ceil(m_uiMaxGPUParticles);
    // only blended modes need back-to-front ordering; additive and opaque are order independent
    sysInfo.m_bNeedsSorting = m_RenderMode == plParticleTypeRenderMode::Blended || m_RenderMode == plParticleTypeRenderMode::BlendedBackground || m_RenderMode == plParticleTypeRenderMode::BlendedForeground || m_RenderMode == plParticleTypeRenderMode::BlendAdd;
    sysInfo.m_uiQuadOrientation = m_QuadOrientation.GetValue();
    sysInfo.m_fQuadStretch = m_fStretch;
    sysInfo.m_vEmitterPosition = instanceTransform.m_vPosition;
    sysInfo.m_vEmitterDirection = instanceTransform.m_qRotation * plVec3(0, 0, 1);
    sysInfo.m_vEmitterDirOrtho = sysInfo.m_vEmitterDirection.GetOrthogonalVector();
    sysInfo.m_TintColor = GetOwnerEffect()->GetColorParameter(m_sTintColorParameter, plColor::White);

    sysInfo.m_fFliesSpeed = m_fFliesSpeed;
    sysInfo.m_fFliesMaxEmitterDistance = m_fFliesMaxEmitterDistance;
    sysInfo.m_fFliesSteeringAngle = m_FliesMaxSteeringAngle.GetRadian();
    sysInfo.m_bFliesChangeDirection = m_bFliesChangeDirThisFrame;
    sysInfo.m_uiRandomSeed = (plUInt32)plRenderWorld::GetFrameCounter();

    if (m_bSceneForces)
    {
      sysInfo.m_fForceFieldInfluence = m_fSceneForceInfluence * plMath::Max(GetOwnerEffect()->GetFloatParameter(m_sSceneForceParameter, 1.0f), 0.0f);
      sysInfo.m_ForceFields = GatherForceFields(instanceTransform);
    }

    plGPUParticleDataProvider::QueueSystem(sysInfo);
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_GPU_ParticleTypeGPU);