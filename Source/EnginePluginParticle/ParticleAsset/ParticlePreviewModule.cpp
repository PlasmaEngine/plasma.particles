#include <EnginePluginParticle/EnginePluginParticlePCH.h>

#include <EnginePluginParticle/ParticleAsset/ParticlePreviewMessages.h>
#include <EnginePluginParticle/ParticleAsset/ParticlePreviewModule.h>
#include <Foundation/IO/MemoryStream.h>
#include <ParticlePlugin/Components/ParticleComponent.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/GPU/ParticleTypeGPU.h>

// clang-format off
PL_IMPLEMENT_WORLD_MODULE(plParticlePreviewModule);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticlePreviewModule, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticlePreviewModule::plParticlePreviewModule(plWorld* pWorld)
  : plWorldModule(pWorld)
{
}

plParticlePreviewModule::~plParticlePreviewModule() = default;

void plParticlePreviewModule::Initialize()
{
  // the colours go back before the particle module starts simulating (priority 1000) ...
  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plParticlePreviewModule::BeforeSimulation, this);
    desc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PreAsync;
    desc.m_bOnlyUpdateWhenSimulating = false;
    desc.m_fPriority = 2000.0f;
    RegisterUpdateFunction(desc);
  }

  // ... and are tinted after it has finished (priority -1000), before the frame is extracted
  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plParticlePreviewModule::AfterSimulation, this);
    desc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PostTransform;
    desc.m_bOnlyUpdateWhenSimulating = false;
    desc.m_fPriority = -2000.0f;
    RegisterUpdateFunction(desc);
  }
}

void plParticlePreviewModule::SetSpy(const plParticleSpyMsgToEngine& msg)
{
  RestoreColors();

  // a GPU system stops computing the old spy's values
  if (plParticleTypeGPU* pType = GetSpiedGPUType(); pType != nullptr && pType == m_pSpyType)
    pType->SetSpyProgram(nullptr);
  m_pSpyType = nullptr;

  m_Result = SpyResult();
  m_SpyProgram = plParticleProgramData();
  m_SpyRunner.Configure(nullptr);
  m_SpyNode = plUuid();

  if (msg.m_Program.IsEmpty())
    return;

  plRawMemoryStreamReader reader(msg.m_Program.GetData(), msg.m_Program.GetCount());
  m_SpyProgram.Load(reader);

  m_SpyNode = msg.m_Node;
  m_uiSpySystem = msg.m_uiSystem;
  m_bSpyPerFrame = msg.m_bPerFrame;
  m_Result.m_Node = m_SpyNode;
}

plParticleSystemInstance* plParticlePreviewModule::GetSpiedSystem() const
{
  if (m_pComponent == nullptr || !m_pComponent->m_EffectController.IsAlive())
    return nullptr;

  const auto& systems = m_pComponent->m_EffectController.GetInstance()->GetParticleSystems();
  return m_uiSpySystem < systems.GetCount() ? systems[m_uiSpySystem] : nullptr;
}

plParticleTypeGPU* plParticlePreviewModule::GetSpiedGPUType() const
{
  plParticleSystemInstance* pSystem = GetSpiedSystem();
  if (pSystem == nullptr || !pSystem->IsLoweredToGPU() || pSystem->GetTypes().IsEmpty())
    return nullptr;

  return plDynamicCast<plParticleTypeGPU*>(pSystem->GetTypes()[0]);
}

void plParticlePreviewModule::RestoreColors()
{
  if (m_pTintedSystem == nullptr)
    return;

  PL_SCOPE_EXIT(m_pTintedSystem = nullptr; m_SavedColors.Clear());

  // the system may have been destroyed or have lost particles since; then there is nothing to put back
  const plParticleSystemInstance* pSystem = GetSpiedSystem();
  if (pSystem != m_pTintedSystem || pSystem->GetNumActiveParticles() != m_SavedColors.GetCount())
    return;

  if (plProcessingStream* pColor = pSystem->QueryStream("Color", plProcessingStream::DataType::Half4))
  {
    plFloat16Vec4* pColors = pColor->GetWritableData<plFloat16Vec4>();
    for (plUInt32 i = 0; i < m_SavedColors.GetCount(); ++i)
      pColors[i] = m_SavedColors[i];
  }
}

void plParticlePreviewModule::BeforeSimulation(const plWorldModule::UpdateContext& context)
{
  RestoreColors();
}

void plParticlePreviewModule::AfterSimulation(const plWorldModule::UpdateContext& context)
{
  if (!m_SpyNode.IsValid())
    return;

  plParticleSystemInstance* pSystem = GetSpiedSystem();

  // a system on the GPU keeps its particles there: the kernel computes the values and they come back late
  if (pSystem != nullptr && pSystem->IsLoweredToGPU() && !m_bSpyPerFrame)
  {
    SpyOnGPU();
    return;
  }

  m_Result.m_uiCount = 0;
  m_Result.m_bOnGPU = false;

  if (pSystem == nullptr)
    return;

  const plUInt32 uiCount = m_bSpyPerFrame ? 1u : (plUInt32)pSystem->GetNumActiveParticles();
  if (uiCount == 0)
    return;

  // the streams can be recreated when the effect changes, so the program is bound to them every time
  bool bMissingStream = false;
  m_SpyRunner.Configure(&m_SpyProgram);
  m_SpyRunner.CreateRequiredStreams([&](const char* szName, plProcessingStream::DataType type, plProcessingStream** ppStream)
    {
      *ppStream = pSystem->QueryStream(szName, type);
      bMissingStream = bMissingStream || *ppStream == nullptr;
    });

  plUInt32 uiComponents = 0;
  const float* pValues = bMissingStream ? nullptr : m_SpyRunner.RunProperty(pSystem, uiCount, plTime::MakeFromSeconds(1.0 / 60.0), "Spy", uiComponents);
  if (pValues == nullptr || uiComponents == 0)
    return;

  ComputeResult(pValues, uiCount, uiComponents);

  if (!m_bSpyPerFrame)
    Tint(pSystem, pValues, uiComponents);
}

void plParticlePreviewModule::SpyOnGPU()
{
  plParticleTypeGPU* pType = GetSpiedGPUType();
  if (pType == nullptr)
  {
    m_Result.m_uiCount = 0;
    return;
  }

  // a new effect makes a new type; it gets the program once
  if (pType != m_pSpyType)
  {
    pType->SetSpyProgram(&m_SpyProgram);
    m_pSpyType = pType;
    m_Result.m_uiCount = 0;
  }

  // the last result stays until the next readback arrives
  if (pType->FetchSpyValues(m_GPUValues))
  {
    m_Result.m_uiCount = 0;
    if (!m_GPUValues.IsEmpty())
      ComputeResult(m_GPUValues.GetData(), m_GPUValues.GetCount(), 1);
  }

  m_Result.m_bOnGPU = true;
  pType->SetSpyRange(m_Result.m_fMin, m_Result.m_fMax);
}

void plParticlePreviewModule::ComputeResult(const float* pValues, plUInt32 uiCount, plUInt32 uiStride)
{
  float fMin = pValues[0], fMax = pValues[0];
  double fSum = 0.0;
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const float f = pValues[i * uiStride];
    fMin = plMath::Min(fMin, f);
    fMax = plMath::Max(fMax, f);
    fSum += f;
  }

  m_Result.m_uiCount = uiCount;
  m_Result.m_fMin = fMin;
  m_Result.m_fMax = fMax;
  m_Result.m_fAvg = (float)(fSum / uiCount);

  m_Result.m_Bins.SetCount(s_uiSpyBins);
  for (plUInt32& uiBin : m_Result.m_Bins)
    uiBin = 0;

  const float fRange = fMax - fMin;
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const float t = fRange > 0.0f ? (pValues[i * uiStride] - fMin) / fRange : 0.5f;
    ++m_Result.m_Bins[plMath::Clamp((plInt32)(t * s_uiSpyBins), 0, (plInt32)s_uiSpyBins - 1)];
  }
}

void plParticlePreviewModule::Tint(plParticleSystemInstance* pSystem, const float* pValues, plUInt32 uiComponents)
{
  plProcessingStream* pColor = pSystem->QueryStream("Color", plProcessingStream::DataType::Half4);
  if (pColor == nullptr)
    return;

  const plUInt32 uiCount = (plUInt32)pSystem->GetNumActiveParticles();
  plFloat16Vec4* pColors = pColor->GetWritableData<plFloat16Vec4>();

  m_pTintedSystem = pSystem;
  m_SavedColors.SetCountUninitialized(uiCount);

  // dark to bright in the per-particle tier's green, low values to high; the particle keeps its alpha
  const plColor low(0.10f, 0.20f, 0.05f);
  const plColor high(0.75f, 0.95f, 0.45f);
  const float fRange = m_Result.m_fMax - m_Result.m_fMin;

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    m_SavedColors[i] = pColors[i];

    const float t = fRange > 0.0f ? (pValues[i * uiComponents] - m_Result.m_fMin) / fRange : 0.5f;
    const plColor tint = plMath::Lerp(low, high, t);
    pColors[i] = plFloat16Vec4(plVec4(tint.r, tint.g, tint.b, (float)pColors[i].w));
  }
}
