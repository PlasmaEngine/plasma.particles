#include <ParticlesTest/ParticlesTestPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Math/Float16.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SizeCurve.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_Burst.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_RandomSize.h>
#include <ParticlePlugin/Renderer/ParticleDistortionPass.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/GPU/ParticleGPULowering.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>
#include <ParticlePlugin/Type/Ribbon/ParticleTypeRibbon.h>
#include <ParticlePlugin/Type/Trail/ParticleTypeTrail.h>
#include <ParticlesTest/ParticleRuntimeTests.h>
#include <RendererCore/Pipeline/Passes/CopyTexturePass.h>
#include <RendererCore/Pipeline/Passes/TransparentForwardRenderPass.h>
#include <RendererCore/Pipeline/RenderPipeline.h>

namespace
{
  template <typename T>
  T* AllocateModule()
  {
    return plGetStaticRTTI<T>()->GetAllocator()->template Allocate<T>();
  }

  plParticleSystemDescriptor* AddSystem(plParticleEffectDescriptor& ref_effect, plUInt32 uiCount)
  {
    plParticleSystemDescriptor* pSystem = AllocateModule<plParticleSystemDescriptor>();
    pSystem->m_LifeTime.m_Value = plTime::MakeFromSeconds(10.0);
    pSystem->m_LifeTime.m_fVariance = 0.0f;

    plParticleEmitterFactory_Burst* pBurst = AllocateModule<plParticleEmitterFactory_Burst>();
    pBurst->m_uiSpawnCountMin = uiCount;
    pBurst->m_uiSpawnCountRange = 0;

    auto pEmitters = static_cast<const plAbstractArrayProperty*>(plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
    plParticleEmitterFactory* pEmitter = pBurst;
    pEmitters->Insert(pSystem, 0, &pEmitter);

    ref_effect.AddParticleSystem(pSystem);
    return pSystem;
  }

  plParticleInitializerFactory_RandomSize* AddRandomSize(plParticleSystemDescriptor* pSystem, plParticleSizeMode::Enum mode, float fSize, float fVariance)
  {
    plParticleInitializerFactory_RandomSize* pSize = AllocateModule<plParticleInitializerFactory_RandomSize>();
    pSize->m_Mode = mode;
    pSize->m_Size.m_Value = fSize;
    pSize->m_Size.m_fVariance = fVariance;
    pSize->m_Aspect.m_Value = 1.0f;
    pSize->m_Aspect.m_fVariance = 0.0f;
    pSystem->AddInitializerFactory(pSize);
    return pSize;
  }

  plParticleBehaviorFactory_Attribute* AddSetAttribute(plParticleSystemDescriptor* pSystem, plParticleSinkTarget::Enum target)
  {
    plParticleBehaviorFactory_Attribute* pSet = AllocateModule<plParticleBehaviorFactory_Attribute>();
    pSet->m_Settings.m_Target = target;
    pSet->m_Settings.m_Mode = plParticleSinkMode::Replace;
    pSystem->AddBehaviorFactory(pSet);
    return pSet;
  }

  const plParticleSystemInstance* GetSystemAt(const plParticleEffectInstance* pEffect, plUInt32 uiIndex)
  {
    if (pEffect == nullptr || uiIndex >= pEffect->GetParticleSystems().GetCount())
      return nullptr;

    return pEffect->GetParticleSystems()[uiIndex];
  }

  template <typename T>
  void SaveLoad(const T& source, T& out_target)
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    source.Save(writer);

    plMemoryStreamReader reader(&storage);
    out_target.Load(reader);
  }

  void FillLook(plParticleLookSettings& ref_look)
  {
    ref_look.m_sEmissiveMap = "{ 00000000-0000-0000-0000-000000000001 }";
    ref_look.m_EmissiveColor = plColor(1.0f, 0.5f, 0.25f, 1.0f);
    ref_look.m_fEmissiveIntensity = 4.0f;
    ref_look.m_sEmissiveColorParameter = "Glow";
    ref_look.m_fSoftEdgeDistance = 0.3f;
    ref_look.m_fCameraFadeDistance = 1.5f;
  }

  bool SameLook(const plParticleLookSettings& a, const plParticleLookSettings& b)
  {
    return a.m_sEmissiveMap == b.m_sEmissiveMap && a.m_EmissiveColor == b.m_EmissiveColor && a.m_fEmissiveIntensity == b.m_fEmissiveIntensity &&
           a.m_sEmissiveColorParameter == b.m_sEmissiveColorParameter && a.m_fSoftEdgeDistance == b.m_fSoftEdgeDistance &&
           a.m_fCameraFadeDistance == b.m_fCameraFadeDistance;
  }
} // namespace

void plGameEngineTestParticleRuntime::SetupArtistRequestTest(plInt32 iIdentifier, plParticleEffectDescriptor& ref_desc)
{
  if (iIdentifier == SubTests::SizePerAxis)
  {
    // 0: per-axis sizes sharing one random value, 1: the uniform aspect ratio, 2: per-axis sizes each with their own
    {
      plParticleSystemDescriptor* pSystem = AddSystem(ref_desc, 10);
      plParticleInitializerFactory_RandomSize* pSize = AddRandomSize(pSystem, plParticleSizeMode::PerAxis, 0.5f, 0.0f);
      pSize->m_vSizeAxes.Set(2.0f, 0.5f, 0.25f);
      pSize->m_bSameRandomForAllAxes = true;
      pSystem->AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());
    }

    {
      plParticleSystemDescriptor* pSystem = AddSystem(ref_desc, 10);
      plParticleInitializerFactory_RandomSize* pSize = AddRandomSize(pSystem, plParticleSizeMode::Uniform, 1.0f, 0.0f);
      pSize->m_Aspect.m_Value = 3.0f;
      pSystem->AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());
    }

    {
      plParticleSystemDescriptor* pSystem = AddSystem(ref_desc, 50);
      plParticleInitializerFactory_RandomSize* pSize = AddRandomSize(pSystem, plParticleSizeMode::PerAxis, 0.5f, 0.5f);
      pSize->m_vSizeAxes.Set(2.0f, 1.0f, 1.0f);
      pSize->m_bSameRandomForAllAxes = false;
      pSystem->AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());
    }

    return;
  }

  if (iIdentifier == SubTests::NewSinkTargets)
  {
    plParticleSystemDescriptor* pSystem = AddSystem(ref_desc, 10);

    AddSetAttribute(pSystem, plParticleSinkTarget::Rotation)->m_Settings.m_fNumber = 90.0f;
    AddSetAttribute(pSystem, plParticleSinkTarget::Facing)->m_Settings.m_vVector.Set(0.0f, 0.0f, 2.0f);
    AddSetAttribute(pSystem, plParticleSinkTarget::Emissive)->m_Settings.m_fNumber = 0.25f;

    plParticleBehaviorFactory_Attribute* pScaleY = AddSetAttribute(pSystem, plParticleSinkTarget::SizeScale);
    pScaleY->m_Settings.m_Component = plParticleSinkComponent::Y;
    pScaleY->m_Settings.m_fNumber = 3.0f;

    plParticleTypeQuadFactory* pQuad = AllocateModule<plParticleTypeQuadFactory>();
    pQuad->m_Orientation = plQuadParticleOrientation::Custom_Facing;
    pSystem->AddTypeFactory(pQuad);
  }
}

plTestAppRun plGameEngineTestParticleRuntime::RunArtistRequestTest(plInt32 iIdentifier)
{
  switch (iIdentifier)
  {
    case SubTests::LookSettings:
      CheckLookSettings();
      return plTestAppRun::Quit;

    case SubTests::GPULoweringNewFeatures:
      CheckGPULoweringNewFeatures();
      return plTestAppRun::Quit;

    case SubTests::DistortionPipeline:
      CheckDistortionPipeline();
      return plTestAppRun::Quit;

    default:
      break;
  }

  if (m_iFrame < 5)
    return plTestAppRun::Continue;

  const plParticleEffectInstance* pEffect = GetEffect();
  if (!PL_TEST_BOOL(pEffect != nullptr))
    return plTestAppRun::Quit;

  if (iIdentifier == SubTests::SizePerAxis)
  {
    const plVec3 vExpected[2] = {plVec3(2.0f, 0.5f, 0.25f), plVec3(3.0f, 1.0f, 1.0f)};
    const float fExpectedSize[2] = {0.5f, 1.0f};

    for (plUInt32 s = 0; s < 2; ++s)
    {
      const plParticleSystemInstance* pSystem = GetSystemAt(pEffect, s);
      const plProcessingStream* pSize = pSystem != nullptr ? pSystem->QueryStream("Size", plProcessingStream::DataType::Half) : nullptr;
      const plProcessingStream* pScale = pSystem != nullptr ? pSystem->QueryStream("SizeScale", plProcessingStream::DataType::Half3) : nullptr;
      if (!PL_TEST_BOOL(pSize != nullptr && pScale != nullptr && pSystem->GetNumActiveParticles() == 10))
        continue;

      for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
      {
        PL_TEST_FLOAT((float)pSize->GetData<plFloat16>()[i], fExpectedSize[s], 0.001f);
        PL_TEST_VEC3((plVec3)pScale->GetData<plFloat16Vec3>()[i], vExpected[s], 0.001f);
      }
    }

    // every axis varies on its own, around its multiplier, while Size stays the authored value
    const plParticleSystemInstance* pSystem = GetSystemAt(pEffect, 2);
    const plProcessingStream* pSize = pSystem != nullptr ? pSystem->QueryStream("Size", plProcessingStream::DataType::Half) : nullptr;
    const plProcessingStream* pScale = pSystem != nullptr ? pSystem->QueryStream("SizeScale", plProcessingStream::DataType::Half3) : nullptr;
    if (PL_TEST_BOOL(pSize != nullptr && pScale != nullptr && pSystem->GetNumActiveParticles() == 50))
    {
      plUInt32 uiDifferentRatio = 0;
      for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
      {
        const plVec3 vScale = pScale->GetData<plFloat16Vec3>()[i];
        PL_TEST_FLOAT((float)pSize->GetData<plFloat16>()[i], 0.5f, 0.001f);
        PL_TEST_BOOL(vScale.x > 0.0f && vScale.x < 4.0f && vScale.y > 0.0f && vScale.y < 2.0f);

        if (plMath::Abs(vScale.x / 2.0f - vScale.y) > 0.01f)
          ++uiDifferentRatio;
      }

      PL_TEST_BOOL(uiDifferentRatio > 0);
    }

    return plTestAppRun::Quit;
  }

  if (iIdentifier == SubTests::NewSinkTargets)
  {
    const plParticleSystemInstance* pSystem = GetSystemAt(pEffect, 0);
    if (!PL_TEST_BOOL(pSystem != nullptr && pSystem->GetNumActiveParticles() == 10))
      return plTestAppRun::Quit;

    const plProcessingStream* pRotation = pSystem->QueryStream("RotationOffset", plProcessingStream::DataType::Half);
    const plProcessingStream* pAxis = pSystem->QueryStream("Axis", plProcessingStream::DataType::Float3);
    const plProcessingStream* pEmissive = pSystem->QueryStream("Emissive", plProcessingStream::DataType::Half);
    const plProcessingStream* pScale = pSystem->QueryStream("SizeScale", plProcessingStream::DataType::Half3);

    if (!PL_TEST_BOOL(pRotation != nullptr && pAxis != nullptr && pEmissive != nullptr && pScale != nullptr))
      return plTestAppRun::Quit;

    for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
    {
      // degrees in, radians in the stream
      PL_TEST_FLOAT((float)pRotation->GetData<plFloat16>()[i], plMath::Pi<float>() * 0.5f, 0.002f);
      PL_TEST_VEC3(pAxis->GetData<plVec3>()[i], plVec3(0.0f, 0.0f, 2.0f), 0.0001f);
      PL_TEST_FLOAT((float)pEmissive->GetData<plFloat16>()[i], 0.25f, 0.001f);

      // only the Y component is written, the others keep the stream's default of 1
      PL_TEST_VEC3((plVec3)pScale->GetData<plFloat16Vec3>()[i], plVec3(1.0f, 3.0f, 1.0f), 0.001f);
    }

    return plTestAppRun::Quit;
  }

  return plTestAppRun::Quit;
}

void plGameEngineTestParticleRuntime::CheckLookSettings()
{
  // defaults keep the fades every effect had before the settings existed
  {
    const plParticleLookSettings look;
    PL_TEST_FLOAT(look.m_fSoftEdgeDistance, 0.1f, 0.0f);
    PL_TEST_FLOAT(look.m_fCameraFadeDistance, 0.5f, 0.0f);
    PL_TEST_BOOL(look.m_sEmissiveMap.IsEmpty());

    plParticleLook runtime;
    runtime.Configure(look);
    const plParticleLookData data = runtime.GetRenderData(nullptr, true);
    PL_TEST_BOOL(!data.m_hEmissiveMap.IsValid());
    PL_TEST_BOOL(data.m_Emissive == plColor::Black);
    PL_TEST_BOOL(!data.m_bPerParticleEmissive);
  }

  // each render type writes and reads its settings
  {
    plParticleTypeQuadFactory quad, quadLoaded;
    FillLook(quad.m_Look);
    quad.m_LightingMode = plParticleLightingMode::SixWay;
    SaveLoad(quad, quadLoaded);
    PL_TEST_BOOL(SameLook(quad.m_Look, quadLoaded.m_Look));
  }

  {
    plParticleTypeTrailFactory trail, trailLoaded;
    FillLook(trail.m_Look);
    SaveLoad(trail, trailLoaded);
    PL_TEST_BOOL(SameLook(trail.m_Look, trailLoaded.m_Look));
  }

  {
    plParticleTypeRibbonFactory ribbon, ribbonLoaded;
    FillLook(ribbon.m_Look);
    SaveLoad(ribbon, ribbonLoaded);
    PL_TEST_BOOL(SameLook(ribbon.m_Look, ribbonLoaded.m_Look));
  }

  // per-axis size settings survive the binary format too
  {
    plParticleInitializerFactory_RandomSize size, sizeLoaded;
    size.m_Mode = plParticleSizeMode::PerAxis;
    size.m_vSizeAxes.Set(1.0f, 2.0f, 3.0f);
    size.m_bSameRandomForAllAxes = true;
    SaveLoad(size, sizeLoaded);
    PL_TEST_BOOL(sizeLoaded.m_Mode == plParticleSizeMode::PerAxis);
    PL_TEST_VEC3(sizeLoaded.m_vSizeAxes, plVec3(1.0f, 2.0f, 3.0f), 0.0f);
    PL_TEST_BOOL(sizeLoaded.m_bSameRandomForAllAxes);
  }

  {
    plParticleBehaviorFactory_SizeCurve curve, curveLoaded;
    curve.m_Mode = plParticleSizeMode::PerAxis;
    curve.m_fBaseSize = 0.25f;
    curve.m_fCurveScale = 2.0f;
    SaveLoad(curve, curveLoaded);
    PL_TEST_BOOL(curveLoaded.m_Mode == plParticleSizeMode::PerAxis);
    PL_TEST_FLOAT(curveLoaded.m_fBaseSize, 0.25f, 0.0f);
  }
}

void plGameEngineTestParticleRuntime::CheckGPULoweringNewFeatures()
{
  using Kind = plParticleGPULowering::BlockerKind;

  auto Check = [](plParticleSystemDescriptor& ref_system, Kind expectedKind, const char* szCase)
  {
    Kind kind = Kind::None;
    plUInt32 uiIndex = 99;
    const plString sReason = plParticleGPULowering::FindLoweringBlocker(&ref_system, &kind, &uiIndex);

    PL_TEST_BOOL_MSG(kind == expectedKind, "%s: blocker kind %d, reason '%s'", szCase, (int)kind, sReason.GetData());
    PL_TEST_BOOL_MSG((expectedKind == Kind::None) == sReason.IsEmpty(), "%s: reason '%s'", szCase, sReason.GetData());
  };

  {
    plParticleSystemDescriptor system;
    AddRandomSize(&system, plParticleSizeMode::Uniform, 1.0f, 0.0f);
    system.AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());
    Check(system, Kind::None, "uniform size");
  }

  {
    plParticleSystemDescriptor system;
    AddRandomSize(&system, plParticleSizeMode::PerAxis, 1.0f, 0.0f);
    system.AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());
    Check(system, Kind::Initializer, "per-axis Random Size");
  }

  {
    plParticleSystemDescriptor system;
    plParticleBehaviorFactory_SizeCurve* pCurve = AllocateModule<plParticleBehaviorFactory_SizeCurve>();
    pCurve->m_Mode = plParticleSizeMode::PerAxis;
    system.AddBehaviorFactory(pCurve);
    system.AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());
    Check(system, Kind::Behavior, "per-axis Size Curve");
  }

  for (plParticleSinkTarget::Enum target : {plParticleSinkTarget::Rotation, plParticleSinkTarget::Facing, plParticleSinkTarget::SizeScale, plParticleSinkTarget::Emissive})
  {
    plParticleSystemDescriptor system;
    AddSetAttribute(&system, target);
    system.AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());
    Check(system, Kind::Behavior, "Set Attribute target");
  }

  {
    plParticleSystemDescriptor system;
    plParticleTypeQuadFactory* pQuad = AllocateModule<plParticleTypeQuadFactory>();
    pQuad->m_Orientation = plQuadParticleOrientation::Custom_Facing;
    system.AddTypeFactory(pQuad);
    Check(system, Kind::Type, "Custom Facing");
  }
}

void plGameEngineTestParticleRuntime::CheckDistortionPipeline()
{
  PL_TEST_BOOL(plParticleRenderDataCategories::Distortion != plInvalidRenderDataCategory);
  PL_TEST_BOOL(plRenderData::FindCategory("ParticleDistortion") == plParticleRenderDataCategories::Distortion);

  // depth source -> transparent pass -> consumer, as in the forward pipelines
  plRenderPipeline* pPipeline = PL_DEFAULT_NEW(plRenderPipeline);

  plUniquePtr<plCopyTexturePass> pDepthSource = PL_DEFAULT_NEW(plCopyTexturePass);
  plUniquePtr<plTransparentForwardRenderPass> pTransparent = PL_DEFAULT_NEW(plTransparentForwardRenderPass);
  plUniquePtr<plCopyTexturePass> pConsumer = PL_DEFAULT_NEW(plCopyTexturePass);

  plRenderPipelinePass* pDepth = pDepthSource.Borrow();
  plRenderPipelinePass* pTransparentPass = pTransparent.Borrow();
  plRenderPipelinePass* pConsumerPass = pConsumer.Borrow();

  pPipeline->AddPass(std::move(pDepthSource));
  pPipeline->AddPass(std::move(pTransparent));
  pPipeline->AddPass(std::move(pConsumer));

  PL_TEST_BOOL(pPipeline->Connect(pDepth, "Output", pTransparentPass, "ResolvedDepth"));
  PL_TEST_BOOL(pPipeline->Connect(pTransparentPass, "Color", pConsumerPass, "Input"));

  plParticleDistortionPipelineExtension extension;
  PL_TEST_BOOL(extension.Apply(*pPipeline).Succeeded());

  auto FindDistortion = [&]() -> plRenderPipelinePass*
  {
    plHybridArray<plRenderPipelinePass*, 8> passes;
    pPipeline->GetPasses(passes);

    plRenderPipelinePass* pFound = nullptr;
    plUInt32 uiCount = 0;
    for (plRenderPipelinePass* pPass : passes)
    {
      if (plDynamicCast<plParticleDistortionPass*>(pPass) != nullptr)
      {
        pFound = pPass;
        ++uiCount;
      }
    }

    PL_TEST_INT(uiCount, 1);
    return pFound;
  };

  plRenderPipelinePass* pDistortion = FindDistortion();
  if (PL_TEST_BOOL(pDistortion != nullptr))
  {
    const plRenderPipelinePassConnection* pConsumerInput = pPipeline->GetInputConnection(pConsumerPass, plMakeHashedString("Input"));
    const plRenderPipelinePassConnection* pDistortionColor = pPipeline->GetInputConnection(pDistortion, plMakeHashedString("Color"));
    const plRenderPipelinePassConnection* pDistortionDepth = pPipeline->GetInputConnection(pDistortion, plMakeHashedString("ResolvedDepth"));

    PL_TEST_BOOL(pConsumerInput != nullptr && pConsumerInput->m_pOutput->m_pParent == pDistortion);
    PL_TEST_BOOL(pDistortionColor != nullptr && pDistortionColor->m_pOutput->m_pParent == pTransparentPass);
    PL_TEST_BOOL(pDistortionDepth != nullptr && pDistortionDepth->m_pOutput->m_pParent == pDepth);
  }

  // applying again leaves the pipeline as it is
  PL_TEST_BOOL(extension.Apply(*pPipeline).Succeeded());
  FindDistortion();

  PL_DEFAULT_DELETE(pPipeline);
}
