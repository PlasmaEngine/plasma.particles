#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Math/Color16f.h>
#include <Foundation/Math/Float16.h>
#include <Core/Interfaces/WindWorldModule.h>
#include <Core/World/World.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Raycast.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/GPU/GPUParticleRenderer.h>
#include <ParticlePlugin/Type/GPU/ParticleTypeGPU.h>
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
  CreateStream("Velocity", plProcessingStream::DataType::Half4, &m_pStreamVelocity, false);
  CreateStream("LifeTime", plProcessingStream::DataType::Half2, &m_pStreamLifeTime, false);
  CreateStream("Size", plProcessingStream::DataType::Half, &m_pStreamSize, false);
  CreateStream("Color", plProcessingStream::DataType::Half4, &m_pStreamColor, false);
  CreateStream("RotationSpeed", plProcessingStream::DataType::Half, &m_pStreamRotationSpeed, false);
  CreateStream("RotationOffset", plProcessingStream::DataType::Half, &m_pStreamRotationOffset, false);
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

  // Counter + freelist buffer (ByteAddressBuffer, zero-initialized: aliveCount=0, deadCount=0)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiTotalSize = (2 + m_uiMaxGPUParticles) * sizeof(plUInt32);
    desc.m_BufferFlags = plGALBufferUsageFlags::UnorderedAccess | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::ByteAddressBuffer;
    desc.m_ResourceAccess.m_bImmutable = false;

    plDynamicArray<plUInt8> zeroData;
    zeroData.SetCountUninitialized(desc.m_uiTotalSize);
    plMemoryUtils::ZeroFill(zeroData.GetData(), zeroData.GetCount());
    m_hCounterBuffer = pDevice->CreateBuffer(desc, zeroData.GetArrayPtr());
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
  m_uiGPUEmitIndex = 0;
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
  if (!m_hTrailPositionBuffer.IsInvalidated())
    pDevice->DestroyBuffer(m_hTrailPositionBuffer);

  m_hParticleBuffer.Invalidate();
  m_hCounterBuffer.Invalidate();
  m_hTrailPositionBuffer.Invalidate();
  m_bGPUBuffersCreated = false;
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
    const plFloat16Vec4* pVelocity = m_pStreamVelocity->GetData<plFloat16Vec4>();
    const plFloat16Vec2* pLifeTime = m_pStreamLifeTime->GetData<plFloat16Vec2>();
    const plFloat16* pSize = m_pStreamSize->GetData<plFloat16>();
    const plColorLinear16f* pColor = m_pStreamColor->GetData<plColorLinear16f>();
    const plFloat16* pRotSpeed = m_pStreamRotationSpeed != nullptr ? m_pStreamRotationSpeed->GetData<plFloat16>() : nullptr;
    const plFloat16* pRotOffset = m_pStreamRotationOffset != nullptr ? m_pStreamRotationOffset->GetData<plFloat16>() : nullptr;

    for (plUInt32 i = 0; i < numParticles; ++i)
    {
      const plUInt64 srcIdx = srcStart + i;
      plGPUParticle& p = newParticles[i];

      p.Position = pPosition[srcIdx].GetAsVec3();
      const float vx = (float)pVelocity[srcIdx].x;
      const float vy = (float)pVelocity[srcIdx].y;
      const float vz = (float)pVelocity[srcIdx].z;
      const float speed = (float)pVelocity[srcIdx].w;
      p.Velocity.Set(vx * speed, vy * speed, vz * speed);

      const float life = (float)pLifeTime[srcIdx].x;
      const float maxLife = (float)pLifeTime[srcIdx].y;
      p.Life = life / plMath::Max(maxLife, 0.001f);
      p.MaxLife = 1.0f / plMath::Max(maxLife, 0.001f);

      p.Size = pSize[srcIdx];
      p.InitialSize = p.Size;
      p.RotationOffset = pRotOffset != nullptr ? (float)pRotOffset[srcIdx] : 0.0f;
      p.RotationSpeed = pRotSpeed != nullptr ? (float)pRotSpeed[srcIdx] : 0.0f;
      p.Flags = 1; // alive

      plColorLinear16f col = pColor[srcIdx];
      plUInt32* pColorU32 = reinterpret_cast<plUInt32*>(&p.Color);
      pColorU32[0] = (plUInt32)col.r.GetRawData() | ((plUInt32)col.g.GetRawData() << 16);
      pColorU32[1] = (plUInt32)col.b.GetRawData() | ((plUInt32)col.a.GetRawData() << 16);

      p.GPUPartPadding0 = 0;
    }
  }

  // Compute active slot count including the new particles about to be emitted
  const plUInt32 uiActiveSlotCount = plMath::Min(m_uiActiveSlotCount + numParticles, m_uiMaxGPUParticles);

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

  pRenderData->m_NewParticles = newParticles;
  pRenderData->m_uiEmitStartIndex = m_uiGPUEmitIndex;

  pRenderData->m_fGravity = m_fGravity;
  pRenderData->m_fDragCoefficient = m_fDragCoefficient;
  pRenderData->m_fWindStrength = m_fWindStrength;
  pRenderData->m_bEnableDepthCollision = m_bEnableDepthCollision;
  pRenderData->m_bEnableSDFCollision = m_bEnableSDFCollision;
  pRenderData->m_uiCollisionReaction = m_CollisionReaction;
  pRenderData->m_fCollisionBounceFactor = m_fCollisionBounceFactor;
  pRenderData->m_fCollisionSlideFactor = m_fCollisionSlideFactor;
  pRenderData->m_fCollisionThickness = m_fCollisionThickness;
  pRenderData->m_ColorStart = m_ColorStart;
  pRenderData->m_ColorEnd = m_ColorEnd;

  pRenderData->m_uiGPURenderType = m_GPURenderType;
  pRenderData->m_uiMaxTrailPoints = m_uiMaxTrailPoints;
  pRenderData->m_hTrailPositionBuffer = m_hTrailPositionBuffer;
  pRenderData->m_fVelocityStretch = m_fVelocityStretch;

  ref_msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::LitTransparent, plRenderData::Caching::Never);

  // Register with the data provider so the compute pass can simulate this system
  {
    plGPUParticleSystemInfo sysInfo;
    sysInfo.m_hParticleBuffer = m_hParticleBuffer;
    sysInfo.m_hCounterBuffer = m_hCounterBuffer;
    sysInfo.m_uiMaxParticles = m_uiMaxGPUParticles;
    sysInfo.m_uiActiveSlotCount = uiActiveSlotCount;
    sysInfo.m_NewParticles = newParticles;
    sysInfo.m_uiEmitStartIndex = m_uiGPUEmitIndex;
    sysInfo.m_fGravity = m_fGravity;
    sysInfo.m_fDragCoefficient = m_fDragCoefficient;
    sysInfo.m_fWindStrength = m_fWindStrength;

    // Sample wind at the effect's position
    if (const plWindWorldModuleInterface* pWind = GetOwnerEffect()->GetWorld()->GetModuleReadOnly<plWindWorldModuleInterface>())
    {
      sysInfo.m_vWindDirection = pWind->GetWindAt(instanceTransform.m_vPosition);
    }

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

    plGPUParticleDataProvider::QueueSystem(sysInfo);
  }

  // Advance emit index (wrap around)
  m_uiGPUEmitIndex = (m_uiGPUEmitIndex + numParticles) % m_uiMaxGPUParticles;

  m_uiActiveSlotCount = uiActiveSlotCount;

  // Advance trail write index for trail types
  if (m_GPURenderType == plGPUParticleRenderType::Trail)
  {
    m_uiTrailWriteIndex++;
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_GPU_ParticleTypeGPU);