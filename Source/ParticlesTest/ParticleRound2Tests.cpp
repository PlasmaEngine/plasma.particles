#include <ParticlesTest/ParticlesTestPCH.h>

#include <Foundation/CodeUtils/Expression/ExpressionAST.h>
#include <Foundation/CodeUtils/Expression/ExpressionCompiler.h>
#include <Foundation/IO/MemoryStream.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_AttractToPosition.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Drag.h>
#include <ParticlePlugin/Components/ParticleComponent.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_Burst.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SpherePosition.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_VelocityCone.h>
#include <ParticlePlugin/Renderer/ParticleDistortionPass.h>
#include <ParticlePlugin/Renderer/ParticleLook.h>
#include <ParticlePlugin/Renderer/ParticleRenderer.h>
#include <ParticlePlugin/System/ParticleFrameProgram.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/GPU/ParticleTypeGPU.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>
#include <ParticlePlugin/Type/Ribbon/ParticleTypeRibbon.h>
#include <ParticlesTest/ParticleRuntimeTests.h>

// Artist requests, round 2: orientation and space fixes, Masked / Surface settings.

namespace
{
  template <typename T>
  T* AllocateRound2Module()
  {
    return plGetStaticRTTI<T>()->GetAllocator()->template Allocate<T>();
  }

  /// One system that bursts the given number of particles once, after the given delay.
  plParticleSystemDescriptor* AddBurst(plParticleEffectDescriptor& ref_effect, plUInt32 uiCount, plTime startDelay = plTime::MakeZero())
  {
    plParticleSystemDescriptor* pSystem = AllocateRound2Module<plParticleSystemDescriptor>();
    pSystem->m_LifeTime.m_Value = plTime::MakeFromSeconds(10.0);
    pSystem->m_LifeTime.m_fVariance = 0.0f;

    plParticleEmitterFactory_Burst* pBurst = AllocateRound2Module<plParticleEmitterFactory_Burst>();
    pBurst->m_uiSpawnCountMin = uiCount;
    pBurst->m_uiSpawnCountRange = 0;
    pBurst->m_StartDelay = startDelay;

    auto pEmitters = static_cast<const plAbstractArrayProperty*>(plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
    plParticleEmitterFactory* pEmitter = pBurst;
    pEmitters->Insert(pSystem, 0, &pEmitter);

    ref_effect.AddParticleSystem(pSystem);
    return pSystem;
  }

  plParticleInitializerFactory_VelocityCone* AddCone(plParticleSystemDescriptor* pSystem, float fAngleDegrees, float fSpeed)
  {
    plParticleInitializerFactory_VelocityCone* pCone = AllocateRound2Module<plParticleInitializerFactory_VelocityCone>();
    pCone->m_Angle = plAngle::MakeFromDegree(fAngleDegrees);
    pCone->m_Speed.m_Value = fSpeed;
    pCone->m_Speed.m_fVariance = 0.0f;
    pSystem->AddInitializerFactory(pCone);
    return pCone;
  }

  plParticleTypeQuadFactory* AddQuad(plParticleSystemDescriptor* pSystem, plQuadParticleOrientation::Enum orientation)
  {
    plParticleTypeQuadFactory* pQuad = AllocateRound2Module<plParticleTypeQuadFactory>();
    pQuad->m_Orientation = orientation;
    pSystem->AddTypeFactory(pQuad);
    return pQuad;
  }

  const plParticleSystemInstance* GetRound2System(const plParticleEffectInstance* pEffect, plUInt32 uiIndex)
  {
    return uiIndex < pEffect->GetParticleSystems().GetCount() ? pEffect->GetParticleSystems()[uiIndex] : nullptr;
  }

  const plVec3* GetAxes(const plParticleSystemInstance* pSystem)
  {
    const plProcessingStream* pAxis = pSystem != nullptr ? pSystem->QueryStream("Axis", plProcessingStream::DataType::Float3) : nullptr;
    return pAxis != nullptr ? pAxis->GetData<plVec3>() : nullptr;
  }

  bool IsUnitDirection(const plVec3& v)
  {
    return v.IsValid() && plMath::Abs(v.GetLength() - 1.0f) < 0.001f;
  }
} // namespace

void plGameEngineTestParticleRuntime::SetupRound2Test(plInt32 iIdentifier, plParticleEffectDescriptor& ref_desc)
{
  switch (iIdentifier)
  {
    case SubTests::ParticleDirAtRest:
    {
      // sparks flying off and braking to a halt: the facing must stay at the angle they stopped at
      plParticleSystemDescriptor* pSystem = AddBurst(ref_desc, 20);
      AddCone(pSystem, 30.0f, 2.0f);

      plParticleBehaviorFactory_Drag* pDrag = AllocateRound2Module<plParticleBehaviorFactory_Drag>();
      pDrag->m_fDrag = 6.0f;
      pSystem->AddBehaviorFactory(pDrag);

      plParticleTypeQuadFactory* pQuad = AddQuad(pSystem, plQuadParticleOrientation::FixedAxis_ParticleDir);
      pQuad->m_fMinSpeed = 0.1f;
      break;
    }

    case SubTests::VelocityConeAxisRotated:
    {
      // 0: the typed default axis, 1: the axis wired from Effect Forward per frame. The burst waits, so the
      // frame program has written the axis before anything spawns.
      plParticleSystemDescriptor* pTyped = AddBurst(ref_desc, 20, plTime::MakeFromSeconds(0.2));
      AddCone(pTyped, 1.0f, 1.0f);

      plParticleSystemDescriptor* pWired = AddBurst(ref_desc, 20, plTime::MakeFromSeconds(0.2));
      AddCone(pWired, 1.0f, 1.0f);

      plHashedString sEffectForward;
      sEffectForward.Assign(plParticleFrameInput::EffectForward);

      plExpressionAST ast;
      plExpressionAST::Node* pInput = ast.CreateInput({sEffectForward, plProcessingStream::DataType::Float3});
      ast.m_InputNodes.PushBack(static_cast<plExpressionAST::Input*>(pInput));
      ast.m_OutputNodes.PushBack(ast.CreateOutput({plMakeHashedString("Out0"), plProcessingStream::DataType::Float3}, pInput));

      plExpressionCompiler compiler;
      PL_TEST_BOOL(compiler.Compile(ast, pWired->m_FrameProgram.m_ByteCode).Succeeded());

      plParticleDrivenProperty& driven = pWired->m_FrameProgram.m_Driven.ExpandAndGetRef();
      driven.m_Module = plParticleDrivenProperty::Module::Initializer;
      driven.m_uiModuleIndex = 0;
      driven.m_uiOutput = 0;
      driven.m_sProperty = "Axis";
      break;
    }

    case SubTests::EmitterDirAtSpawn:
    {
      // 0: the emitter direction captured at spawn, 1: read live, as effects made before it was an option
      for (plUInt32 i = 0; i < 2; ++i)
      {
        plParticleSystemDescriptor* pSystem = AddBurst(ref_desc, 10);
        plParticleTypeQuadFactory* pQuad = AddQuad(pSystem, plQuadParticleOrientation::FixedAxis_EmitterDir);
        pQuad->m_EmitterDirSampling = i == 0 ? plQuadParticleEmitterDirSampling::AtSpawn : plQuadParticleEmitterDirSampling::Live;
      }
      break;
    }

    case SubTests::LocalSpaceAttractor:
    {
      // a local-space effect away from the origin: Effect Origin must pull to the effect's own origin
      ref_desc.m_bSimulateInLocalSpace = true;

      plParticleSystemDescriptor* pSystem = AddBurst(ref_desc, 40);

      plParticleInitializerFactory_SpherePosition* pSphere = AllocateRound2Module<plParticleInitializerFactory_SpherePosition>();
      pSphere->m_fRadius = 2.0f;
      pSphere->m_bSpawnOnSurface = true;
      pSphere->m_bSetVelocity = false;
      pSystem->AddInitializerFactory(pSphere);

      plParticleBehaviorFactory_AttractToPosition* pAttract = AllocateRound2Module<plParticleBehaviorFactory_AttractToPosition>();
      pAttract->m_Target = plParticleAttractorTarget::EffectOrigin;
      pAttract->m_Shape = plParticleAttractorShape::Point;
      pAttract->m_fForce = 20.0f;
      pAttract->m_fMaxDistance = 100.0f;
      pAttract->m_fMinDistance = 0.01f;
      pSystem->AddBehaviorFactory(pAttract);

      plParticleBehaviorFactory_Drag* pDrag = AllocateRound2Module<plParticleBehaviorFactory_Drag>();
      pDrag->m_fDrag = 4.0f;
      pSystem->AddBehaviorFactory(pDrag);

      AddQuad(pSystem, plQuadParticleOrientation::Billboard);
      break;
    }

    case SubTests::GPUSpawnAxis:
    {
      // a lowered quad with random fixed orientations fills the axis itself
      plParticleSystemDescriptor* pSystem = AddBurst(ref_desc, 30);
      pSystem->m_SimulationTarget = plParticleSimulationTarget::GPU;
      AddQuad(pSystem, plQuadParticleOrientation::Fixed_RandomDir);
      break;
    }

    default:
      break;
  }
}

void plGameEngineTestParticleRuntime::PlaceRound2Owner(plInt32 iIdentifier)
{
  plTransform transform = plTransform::MakeIdentity();

  if (iIdentifier == SubTests::VelocityConeAxisRotated)
  {
    // the way an impact prefab turns its effect towards a wall
    transform.m_qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(-90.0f));
  }
  else if (iIdentifier == SubTests::LocalSpaceAttractor)
  {
    transform.m_vPosition.Set(10.0f, 0.0f, 0.0f);
    transform.m_qRotation = plQuat::MakeFromAxisAndAngle(plVec3(0, 0, 1), plAngle::MakeFromDegree(90.0f));
  }

  SetEffectOwnerTransform(transform);
}

void plGameEngineTestParticleRuntime::SetEffectOwnerTransform(const plTransform& transform)
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());

  plParticleComponent* pComponent = nullptr;
  if (!PL_TEST_BOOL(pWorld->TryGetComponent(m_hEffectComponent, pComponent)))
    return;

  pComponent->GetOwner()->SetGlobalTransform(transform);
}

plTestAppRun plGameEngineTestParticleRuntime::RunRound2Test(plInt32 iIdentifier)
{
  if (iIdentifier == SubTests::Round2Serialization)
  {
    CheckRound2Serialization();
    return plTestAppRun::Quit;
  }

  // the steps below read the world; EmitterDirAtSpawn turns the owner between them
  if (iIdentifier == SubTests::EmitterDirAtSpawn && m_iFrame == 10)
  {
    SetEffectOwnerTransform(plTransform::Make(plVec3::MakeZero(), plQuat::MakeFromAxisAndAngle(plVec3(1, 0, 0), plAngle::MakeFromDegree(90.0f))));
    return plTestAppRun::Continue;
  }

  PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());

  const plParticleEffectInstance* pEffect = GetEffect();
  if (!PL_TEST_BOOL(pEffect != nullptr))
    return plTestAppRun::Quit;

  // the time-based checks give up when the effect clock does not get there
  if (!PL_TEST_BOOL(m_iFrame < 3000))
    return plTestAppRun::Quit;

  const double fEffectSeconds = pEffect->GetTotalEffectLifeTime().GetSeconds();

  switch (iIdentifier)
  {
    case SubTests::ParticleDirAtRest:
    {
      const plParticleSystemInstance* pSystem = GetRound2System(pEffect, 0);
      const plVec3* pAxis = GetAxes(pSystem);

      if (fEffectSeconds < 1.5)
        return plTestAppRun::Continue;

      if (!PL_TEST_BOOL(pAxis != nullptr && pSystem->GetNumActiveParticles() == 20))
        return plTestAppRun::Quit;

      // the particles have stopped by now: remember where they face
      if (m_Round2Axes.IsEmpty())
      {
        for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
        {
          PL_TEST_BOOL(IsUnitDirection(pAxis[i]));
          m_Round2Axes.PushBack(pAxis[i]);
        }

        return plTestAppRun::Continue;
      }

      if (fEffectSeconds < 2.5)
        return plTestAppRun::Continue;

      // a second later, at rest the whole time, every facing is where it was
      for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
      {
        PL_TEST_BOOL(IsUnitDirection(pAxis[i]));
        PL_TEST_BOOL(pAxis[i].Dot(m_Round2Axes[(plUInt32)i]) > 0.9998f);
      }

      return plTestAppRun::Quit;
    }

    case SubTests::VelocityConeAxisRotated:
    {
      if (fEffectSeconds < 0.5)
        return plTestAppRun::Continue;

      // owner turned -90 degrees about Y: the typed axis (+Z of the emitter) and the wired Effect Forward
      // (+X of the effect) each come out along the emitter's own direction, not turned twice
      const plQuat qOwner = plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(-90.0f));
      const plVec3 vExpected[2] = {qOwner * plVec3(0, 0, 1), qOwner * plVec3(1, 0, 0)};

      for (plUInt32 s = 0; s < 2; ++s)
      {
        const plParticleSystemInstance* pSystem = GetRound2System(pEffect, s);
        const plProcessingStream* pVelocity = pSystem != nullptr ? pSystem->QueryStream("Velocity", plProcessingStream::DataType::Float3) : nullptr;
        if (!PL_TEST_BOOL(pVelocity != nullptr && pSystem->GetNumActiveParticles() == 20))
          continue;

        plUInt32 uiOff = 0;
        for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
        {
          plVec3 vDir = pVelocity->GetData<plVec3>()[i];
          vDir.NormalizeIfNotZero(plVec3::MakeZero()).IgnoreResult();
          uiOff += vDir.Dot(vExpected[s]) > 0.99f ? 0 : 1;
        }

        PL_TEST_INT(uiOff, 0);
      }

      return plTestAppRun::Quit;
    }

    case SubTests::EmitterDirAtSpawn:
    {
      if (m_iFrame < 20)
        return plTestAppRun::Continue;

      // the owner turned after the spawn; the captured axis is still the one at spawn (+Z, the owner unturned)
      const plParticleSystemInstance* pAtSpawn = GetRound2System(pEffect, 0);
      const plVec3* pAxis = GetAxes(pAtSpawn);
      if (PL_TEST_BOOL(pAxis != nullptr && pAtSpawn->GetNumActiveParticles() == 10))
      {
        for (plUInt64 i = 0; i < pAtSpawn->GetNumActiveParticles(); ++i)
          PL_TEST_VEC3(pAxis[i], plVec3(0, 0, 1), 0.0001f);
      }

      // the system did turn, so the live one follows it
      PL_TEST_BOOL((pAtSpawn->GetTransform().m_qRotation * plVec3(0, 0, 1)).Dot(plVec3(0, 0, 1)) < 0.1f);

      // reading live needs no axis per particle
      PL_TEST_BOOL(GetAxes(GetRound2System(pEffect, 1)) == nullptr);
      return plTestAppRun::Quit;
    }

    case SubTests::LocalSpaceAttractor:
    {
      if (fEffectSeconds < 2.0)
        return plTestAppRun::Continue;

      const plParticleSystemInstance* pSystem = GetRound2System(pEffect, 0);
      const plProcessingStream* pPosition = pSystem != nullptr ? pSystem->QueryStream("Position", plProcessingStream::DataType::Float4) : nullptr;
      if (!PL_TEST_BOOL(pPosition != nullptr && pSystem->GetNumActiveParticles() == 40))
        return plTestAppRun::Quit;

      // positions are local; the attractor is the local origin, wherever the effect is in the world
      plVec3 vCenter = plVec3::MakeZero();
      for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
        vCenter += pPosition->GetData<plVec4>()[i].GetAsVec3();

      vCenter /= (float)pSystem->GetNumActiveParticles();
      PL_TEST_BOOL(vCenter.GetLength() < 0.75f);
      PL_TEST_BOOL(pSystem->GetTransform().IsEqual(plTransform::MakeIdentity(), 0.0001f));
      return plTestAppRun::Quit;
    }

    case SubTests::GPUSpawnAxis:
    {
      if (m_iFrame < 5)
        return plTestAppRun::Continue;

      PL_TEST_BOOL(GetLoweredType() != nullptr);

      const plParticleSystemInstance* pSystem = GetRound2System(pEffect, 0);
      const plVec3* pAxis = GetAxes(pSystem);
      if (PL_TEST_BOOL(pAxis != nullptr && pSystem->GetNumActiveParticles() > 0))
      {
        plUInt32 uiNotUp = 0;
        for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
        {
          PL_TEST_BOOL(IsUnitDirection(pAxis[i]));
          uiNotUp += pAxis[i].Dot(plVec3(0, 0, 1)) < 0.99f ? 1 : 0;
        }

        // random directions, not the +Z every particle had before the type filled the axis
        PL_TEST_BOOL(uiNotUp > 0);
      }

      return plTestAppRun::Quit;
    }

    default:
      break;
  }

  return plTestAppRun::Quit;
}

void plGameEngineTestParticleRuntime::CheckRound2Serialization()
{
  // the render mode decides where a type draws
  PL_TEST_BOOL(plParticleGetRenderDataCategory(plParticleTypeRenderMode::Masked) == plDefaultRenderDataCategories::LitForwardOpaqueDynamic);
  PL_TEST_BOOL(plParticleGetRenderDataCategory(plParticleTypeRenderMode::Opaque) == plDefaultRenderDataCategories::LitForwardOpaqueDynamic);
  PL_TEST_BOOL(plParticleGetRenderDataCategory(plParticleTypeRenderMode::Blended) == plDefaultRenderDataCategories::LitTransparent);
  PL_TEST_BOOL(plParticleGetRenderDataCategory(plParticleTypeRenderMode::Distortion) == plParticleRenderDataCategories::Distortion);

  // a look saved before alpha clip and shadows loads with the defaults
  {
    plDefaultMemoryStreamStorage storage;
    {
      plMemoryStreamWriter writer(&storage);
      writer << (plUInt8)1;
      writer << plString("");
      writer << plColor::White;
      writer << 1.0f;
      writer << plString("");
      writer << 0.1f;
      writer << 0.5f;
    }

    plParticleLookSettings look;
    look.m_fAlphaCutoff = 0.9f;
    look.m_bCastShadows = true;

    plMemoryStreamReader reader(&storage);
    look.Load(reader);

    PL_TEST_FLOAT(look.m_fAlphaCutoff, 0.5f, 0.0f);
    PL_TEST_BOOL(!look.m_bCastShadows);
  }

  // the quad's version 12 settings survive a round trip
  {
    plParticleTypeQuadFactory saved;
    saved.m_Orientation = plQuadParticleOrientation::FixedAxis_ParticleDir;
    saved.m_EmitterDirSampling = plQuadParticleEmitterDirSampling::Live;
    saved.m_fMinSpeed = 0.35f;
    saved.m_fTurnSmoothing = 0.5f;
    saved.m_RenderMode = plParticleTypeRenderMode::Masked;
    saved.m_LightingMode = plParticleLightingMode::Surface;
    saved.m_sOrmMap = "Orm";
    saved.m_fRoughness = 0.2f;
    saved.m_fMetallic = 0.3f;
    saved.m_fSpecular = 0.4f;
    saved.m_Look.m_fAlphaCutoff = 0.33f;
    saved.m_Look.m_bCastShadows = true;

    plDefaultMemoryStreamStorage storage;
    {
      plMemoryStreamWriter writer(&storage);
      saved.Save(writer);
    }

    plParticleTypeQuadFactory loaded;
    plMemoryStreamReader reader(&storage);
    loaded.Load(reader);

    PL_TEST_BOOL(loaded.m_EmitterDirSampling == plQuadParticleEmitterDirSampling::Live);
    PL_TEST_FLOAT(loaded.m_fMinSpeed, 0.35f, 0.0f);
    PL_TEST_FLOAT(loaded.m_fTurnSmoothing, 0.5f, 0.0f);
    PL_TEST_BOOL(loaded.m_RenderMode == plParticleTypeRenderMode::Masked);
    PL_TEST_BOOL(loaded.m_LightingMode == plParticleLightingMode::Surface);
    PL_TEST_STRING(loaded.m_sOrmMap, "Orm");
    PL_TEST_FLOAT(loaded.m_fRoughness, 0.2f, 0.0f);
    PL_TEST_FLOAT(loaded.m_fMetallic, 0.3f, 0.0f);
    PL_TEST_FLOAT(loaded.m_fSpecular, 0.4f, 0.0f);
    PL_TEST_FLOAT(loaded.m_Look.m_fAlphaCutoff, 0.33f, 0.0f);
    PL_TEST_BOOL(loaded.m_Look.m_bCastShadows);
  }

  // ribbons keep their distortion settings
  {
    plParticleTypeRibbonFactory saved;
    saved.m_RenderMode = plParticleTypeRenderMode::Distortion;
    saved.m_sDistortionTexture = "Haze";
    saved.m_fDistortionStrength = 42.0f;

    plDefaultMemoryStreamStorage storage;
    {
      plMemoryStreamWriter writer(&storage);
      saved.Save(writer);
    }

    plParticleTypeRibbonFactory loaded;
    plMemoryStreamReader reader(&storage);
    loaded.Load(reader);

    PL_TEST_STRING(loaded.m_sDistortionTexture, "Haze");
    PL_TEST_FLOAT(loaded.m_fDistortionStrength, 42.0f, 0.0f);
  }

  // only masked and opaque modes can be shadow casters
  PL_TEST_BOOL(plParticleIsOpaqueRenderMode(plParticleTypeRenderMode::Masked));
  PL_TEST_BOOL(plParticleIsOpaqueRenderMode(plParticleTypeRenderMode::Opaque));
  PL_TEST_BOOL(!plParticleIsOpaqueRenderMode(plParticleTypeRenderMode::Blended));
  PL_TEST_BOOL(!plParticleIsOpaqueRenderMode(plParticleTypeRenderMode::Additive));
}
