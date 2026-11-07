#include <ParticlesTest/ParticlesTestPCH.h>

#include <Core/Curves/ColorGradientResource.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/Threading/Mutex.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ColorGradient.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_FadeOut.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Gravity.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Raycast.h>
#include <ParticlePlugin/Components/ParticleComponent.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_Burst.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_Continuous.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_VelocityCone.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>
#include <ParticlesTest/ParticleRuntimeTests.h>

// The P7 measurements. Each one builds or loads effects, lets them run, and appends what the systems' updates
// cost to the file PL_PARTICLES_BENCH names, tagged with PL_PARTICLES_BENCH_LABEL ("before", "after"). They
// always pass; the numbers are the result. Run them in a Release build, one sub-test per process.

namespace
{
  template <typename T>
  T* Allocate()
  {
    return plGetStaticRTTI<T>()->GetAllocator()->template Allocate<T>();
  }

  plParticleSystemDescriptor* AddSystem(plParticleEffectDescriptor& ref_effect, float fLifeSeconds)
  {
    plParticleSystemDescriptor* pSystem = Allocate<plParticleSystemDescriptor>();
    pSystem->m_LifeTime.m_Value = plTime::MakeFromSeconds(fLifeSeconds);
    pSystem->m_LifeTime.m_fVariance = 0.0f;
    ref_effect.AddParticleSystem(pSystem);
    return pSystem;
  }

  void AddEmitter(plParticleSystemDescriptor* pSystem, plParticleEmitterFactory* pEmitter)
  {
    // the emitter list has no adder; it is filled through reflection like the asset transform does
    auto pEmitters = static_cast<const plAbstractArrayProperty*>(plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
    pEmitters->Insert(pSystem, pEmitters->GetCount(pSystem), &pEmitter);
  }

  void AddBurst(plParticleSystemDescriptor* pSystem, plUInt32 uiCount)
  {
    auto* pBurst = Allocate<plParticleEmitterFactory_Burst>();
    pBurst->m_uiSpawnCountMin = uiCount;
    pBurst->m_uiSpawnCountRange = 0;
    AddEmitter(pSystem, pBurst);
  }

  void AddContinuous(plParticleSystemDescriptor* pSystem, plUInt32 uiPerSecond)
  {
    auto* pContinuous = Allocate<plParticleEmitterFactory_Continuous>();
    pContinuous->m_uiSpawnCountPerSec = uiPerSecond;
    pContinuous->m_uiSpawnCountPerSecRange = 0;
    AddEmitter(pSystem, pContinuous);
  }

  void AddCone(plParticleSystemDescriptor* pSystem, float fSpeed)
  {
    auto* pCone = Allocate<plParticleInitializerFactory_VelocityCone>();
    pCone->m_Angle = plAngle::MakeFromDegree(30.0f);
    pCone->m_Speed.m_Value = fSpeed;
    pCone->m_Speed.m_fVariance = 0.25f;
    pSystem->AddInitializerFactory(pCone);
  }

  /// The system update time in ms, which covers its modules, programs, removals and spawns.
  double UpdateMs(const plParticleSystemInstance* pSystem)
  {
    return pSystem != nullptr ? pSystem->GetLastUpdateDuration().GetMilliseconds() : 0.0;
  }

  constexpr const char* s_szBenchGradient = "PfxBenchGradient";
} // namespace

void plGameEngineTestParticleRuntime::WriteBenchmarkResult(plStringView sName, double fAvgMs, double fMaxMs, plUInt32 uiSamples) const
{
  plStringBuilder sLine;
  sLine.SetFormat("{}\t{}\t{}\t{}\t{}\n", plEnvironmentVariableUtils::GetValueString("PL_PARTICLES_BENCH_LABEL", "unlabelled"), sName, plArgF(fAvgMs, 4), plArgF(fMaxMs, 4), uiSamples);
  plLog::Info("PfxBench: {}", sLine);

  const plString sFile = plEnvironmentVariableUtils::GetValueString("PL_PARTICLES_BENCH", "");
  if (sFile.IsEmpty())
    return;

  plOSFile file;
  if (file.Open(sFile, plFileOpenMode::Append).Succeeded())
  {
    file.Write(sLine.GetData(), sLine.GetElementCount()).IgnoreResult();
    file.Close();
  }
}

void plGameEngineTestParticleRuntime::SetupBenchmark(plInt32 iIdentifier, plParticleEffectDescriptor& ref_desc)
{
  m_BenchSeries.Clear();
  m_BenchComponents.Clear();

  switch (iIdentifier)
  {
    case SubTests::BenchMassDeaths:
    {
      // five systems of 2000 that each die all at once, at different times; a trickle keeps every system alive,
      // so the step its burst dies in can be read
      for (plUInt32 i = 0; i < 5; ++i)
      {
        plParticleSystemDescriptor* pSystem = AddSystem(ref_desc, 0.3f * (i + 1));
        AddBurst(pSystem, 2000);
        AddContinuous(pSystem, 1);
        m_BenchSeries.ExpandAndGetRef().m_sName = "MassDeaths: step where 2000 die";
      }

      StartEffect(ref_desc, {});
      break;
    }

    case SubTests::BenchRaycast:
    {
      // a steady 1500 particles thrown up and falling, every one cast against the scene each step; the second
      // system is the same without Raycast, so the difference is what the rays cost
      for (plUInt32 i = 0; i < 2; ++i)
      {
        plParticleSystemDescriptor* pSystem = AddSystem(ref_desc, 1.5f);
        AddContinuous(pSystem, 1000);
        AddCone(pSystem, 4.0f);
        pSystem->AddBehaviorFactory(Allocate<plParticleBehaviorFactory_Gravity>());

        if (i == 0)
          pSystem->AddBehaviorFactory(Allocate<plParticleBehaviorFactory_Raycast>());
      }

      m_BenchSeries.ExpandAndGetRef().m_sName = "Raycast: 1500 particles, step";
      m_BenchSeries.ExpandAndGetRef().m_sName = "Raycast: the same without Raycast, step";
      StartEffect(ref_desc, {});
      break;
    }

    case SubTests::BenchFarLod:
    {
      // PL_PARTICLES_BENCH_LOD=0 measures the fixed interval of 2 that every distance had before P7
      if (plCVarInt* pLod = static_cast<plCVarInt*>(plCVar::FindCVarByName("Particles.LodUpdateInterval")))
        *pLod = plEnvironmentVariableUtils::GetValueInt("PL_PARTICLES_BENCH_LOD", 1);

      // 2000 particles with Colour Gradient and Fade Out, once at the scene's effect and once 40 m further away
      plColorGradientResourceDescriptor gradient;
      gradient.m_Gradient.AddColorControlPoint(0.0, plColorGammaUB(255, 200, 80));
      gradient.m_Gradient.AddColorControlPoint(0.5, plColorGammaUB(255, 80, 20));
      gradient.m_Gradient.AddColorControlPoint(1.0, plColorGammaUB(40, 40, 40));
      gradient.m_Gradient.AddAlphaControlPoint(0.0, 255);
      gradient.m_Gradient.AddAlphaControlPoint(1.0, 0);
      gradient.m_Gradient.SortControlPoints();
      plResourceManager::GetOrCreateResource<plColorGradientResource>(s_szBenchGradient, std::move(gradient));

      plParticleSystemDescriptor* pSystem = AddSystem(ref_desc, 1.0f);
      AddContinuous(pSystem, 2000);
      AddCone(pSystem, 1.0f);

      auto* pGradient = Allocate<plParticleBehaviorFactory_ColorGradient>();
      pGradient->m_hGradient = plResourceManager::LoadResource<plColorGradientResource>(s_szBenchGradient);
      pSystem->AddBehaviorFactory(pGradient);
      pSystem->AddBehaviorFactory(Allocate<plParticleBehaviorFactory_FadeOut>());
      pSystem->AddTypeFactory(Allocate<plParticleTypeQuadFactory>());

      m_BenchSeries.ExpandAndGetRef().m_sName = "Gradient + Fade Out: 2000 particles, near";
      m_BenchSeries.ExpandAndGetRef().m_sName = "Gradient + Fade Out: 2000 particles, 40 m away";
      StartEffect(ref_desc, {});

      // the far copy plays the same resource on an object of its own, placed once the camera is known
      plWorld* pWorld = m_pOwnApplication->GetWorld();
      PL_LOCK(pWorld->GetWriteMarker());

      plParticleComponent* pNear = nullptr;
      if (!pWorld->TryGetComponent(m_hEffectComponent, pNear))
        break;

      plGameObjectDesc objDesc;
      objDesc.m_LocalPosition = pNear->GetOwner()->GetGlobalPosition();
      objDesc.m_bDynamic = true;
      plGameObject* pObject = nullptr;
      pWorld->CreateObject(objDesc, pObject);

      plParticleComponent* pFar = nullptr;
      m_BenchComponents.PushBack(pWorld->GetOrCreateComponentManager<plParticleComponentManager>()->CreateComponent(pObject, pFar));
      pFar->m_uiRandomSeed = 42;
      pFar->SetParticleEffect(pNear->GetParticleEffect());
      break;
    }

    case SubTests::BenchTestingChambers:
      StartTestingChambersEffects();
      break;

    default:
      break;
  }
}

void plGameEngineTestParticleRuntime::StartTestingChambersEffects()
{
  // every built effect of a TestingChambers copy, played at once near the scene's effect, restarting when done
  const plString sRoot = plEnvironmentVariableUtils::GetValueString("PL_PARTICLES_BENCH_TC", "E:/Temp/p7_tc/TestingChambers");
  if (!PL_TEST_BOOL_MSG(plFileSystem::AddDataDirectory(sRoot, "PfxBench", "tc").Succeeded(), "No TestingChambers copy at '%s'", sRoot.GetData()))
    return;

  plStringBuilder sCache(sRoot, "/AssetCache/Common");
  plDynamicArray<plString> files;

  plFileSystemIterator it;
  for (it.StartSearch(sCache, plFileSystemIteratorFlags::ReportFilesRecursive); it.IsValid(); it.Next())
  {
    if (!it.GetStats().m_sName.EndsWith_NoCase(".plBinParticleEffect"))
      continue;

    plStringBuilder sPath;
    it.GetStats().GetFullPath(sPath);
    sPath.MakeRelativeTo(sRoot).IgnoreResult();
    files.PushBack(plStringBuilder(":tc/", sPath));
  }

  files.Sort();

  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());

  plVec3 vCenter = plVec3::MakeZero();
  plGameObject* pAnchor = nullptr;
  if (pWorld->TryGetObjectWithGlobalKey("Effect", pAnchor))
    vCenter = pAnchor->GetGlobalPosition();

  for (plUInt32 i = 0; i < files.GetCount(); ++i)
  {
    // a 6 x 5 grid, 2 m apart, around the scene's effect
    plGameObjectDesc objDesc;
    objDesc.m_LocalPosition = vCenter + plVec3(((float)(i % 6) - 2.5f) * 2.0f, ((float)(i / 6) - 2.0f) * 2.0f, 0.0f);
    plGameObject* pObject = nullptr;
    pWorld->CreateObject(objDesc, pObject);

    plParticleComponent* pComponent = nullptr;
    m_BenchComponents.PushBack(pWorld->GetOrCreateComponentManager<plParticleComponentManager>()->CreateComponent(pObject, pComponent));
    pComponent->m_uiRandomSeed = 42 + i;
    pComponent->m_OnFinishedAction = plOnComponentFinishedAction2::Restart;
    pComponent->SetParticleEffectFile(files[i]);

    plStringBuilder sName(plPathUtils::GetFileName(files[i]));
    m_BenchSeries.ExpandAndGetRef().m_sName = plStringBuilder("TestingChambers: ", sName);
  }

  m_BenchSeries.ExpandAndGetRef().m_sName = "TestingChambers: all effects";
}

void plGameEngineTestParticleRuntime::MoveFarLodCopy()
{
  if (m_BenchComponents.IsEmpty())
    return;

  // the far copy goes 40 m further along the line from the camera through the near effect, so both stay in view
  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());

  plParticleComponent* pFar = nullptr;
  if (plParticleEffectInstance* pNear = GetEffect(); pNear != nullptr && pWorld->TryGetComponent(m_BenchComponents[0], pFar))
  {
    const plVec3 vCamera = pNear->GetOwnerWorldModule()->GetMainCameraPosition();
    const plVec3 vNear = pNear->GetTransform().m_vPosition;
    plVec3 vAway = vNear - vCamera;
    vAway.NormalizeIfNotZero(plVec3(1, 0, 0)).IgnoreResult();
    pFar->GetOwner()->SetGlobalPosition(vNear + vAway * 40.0f);
  }
}

plTestAppRun plGameEngineTestParticleRuntime::RunLodUpdateInterval()
{
  if (m_iFrame == 3)
    MoveFarLodCopy();

  if (m_iFrame < 20)
    return plTestAppRun::Continue;

  plCVarInt* pLod = static_cast<plCVarInt*>(plCVar::FindCVarByName("Particles.LodUpdateInterval"));
  if (!PL_TEST_BOOL(pLod != nullptr))
    return plTestAppRun::Quit;

  PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
  const plWorld* pWorld = m_pOwnApplication->GetWorld();

  const plParticleEffectInstance* pNear = GetEffect();
  const plParticleComponent* pFarComponent = nullptr;
  if (!PL_TEST_BOOL(pNear != nullptr && !m_BenchComponents.IsEmpty() && pWorld->TryGetComponent(m_BenchComponents[0], pFarComponent)))
    return plTestAppRun::Quit;

  const plParticleEffectInstance* pFar = pFarComponent->m_EffectController.GetInstance();
  if (!PL_TEST_BOOL(pFar != nullptr))
    return plTestAppRun::Quit;

  if (m_iFrame == 20)
  {
    // the near effect's interval follows its distance; the far one is 40 m beyond it
    const float fNear = pNear->GetLodDistance();
    PL_TEST_BOOL(fNear >= 0.0f);
    PL_TEST_INT(pNear->GetLodUpdateInterval(), fNear < 15.0f ? 1 : (fNear < 30.0f ? 2 : 4));

    PL_TEST_BOOL(pFar->GetLodDistance() > 30.0f);
    PL_TEST_INT(pFar->GetLodUpdateInterval(), 4);

    *pLod = 0;
    return plTestAppRun::Continue;
  }

  // off: every 2nd frame at any distance, as before the distance LOD
  PL_TEST_INT(pNear->GetLodUpdateInterval(), 2);
  PL_TEST_INT(pFar->GetLodUpdateInterval(), 2);

  *pLod = 1;
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestParticleRuntime::RunBenchmark(plInt32 iIdentifier)
{
  if (iIdentifier == SubTests::BenchMutex)
  {
    // what extraction pays for the world module's lock per effect when nothing else holds it
    plMutex mutex;
    constexpr plUInt32 uiCount = 1000000;

    const plTime tStart = plTime::Now();
    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      PL_LOCK(mutex);
    }
    const double fNsPerLock = (plTime::Now() - tStart).GetNanoseconds() / uiCount;

    WriteBenchmarkResult("Uncontended lock + unlock (ns, not ms)", fNsPerLock, fNsPerLock, uiCount);
    return plTestAppRun::Quit;
  }

  if (iIdentifier == SubTests::BenchFarLod && m_iFrame == 3)
    MoveFarLodCopy();

  PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
  const plWorld* pWorld = m_pOwnApplication->GetWorld();

  auto EffectOf = [&](const plComponentHandle& hComponent) -> plParticleEffectInstance*
  {
    const plParticleComponent* pComponent = nullptr;
    if (!pWorld->TryGetComponent(hComponent, pComponent) || !pComponent->m_EffectController.IsAlive())
      return nullptr;

    return const_cast<plParticleComponent*>(pComponent)->m_EffectController.GetInstance();
  };

  auto Sample = [](BenchSeries& ref_series, double fMs)
  {
    ref_series.m_fSum += fMs;
    ref_series.m_fMax = plMath::Max(ref_series.m_fMax, fMs);
    ++ref_series.m_uiSamples;
  };

  auto Finish = [&]()
  {
    for (const BenchSeries& series : m_BenchSeries)
      WriteBenchmarkResult(series.m_sName, series.m_uiSamples > 0 ? series.m_fSum / series.m_uiSamples : 0.0, series.m_fMax, series.m_uiSamples);
    return plTestAppRun::Quit;
  };

  // nothing comes back within 3000 frames: report what there is
  if (m_iFrame > 3000)
    return Finish();

  switch (iIdentifier)
  {
    case SubTests::BenchMassDeaths:
    {
      plParticleEffectInstance* pEffect = GetEffect();
      if (pEffect == nullptr)
        return plTestAppRun::Continue;

      for (plUInt32 i = 0; i < m_BenchSeries.GetCount() && i < pEffect->GetParticleSystems().GetCount(); ++i)
      {
        const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[i];
        if (pSystem == nullptr)
          continue;

        // the step that removed the burst
        const plUInt64 uiCount = pSystem->GetNumActiveParticles();
        if (m_BenchSeries[i].m_uiLastCount >= 1000 && uiCount < 100)
          Sample(m_BenchSeries[i], UpdateMs(pSystem));
        m_BenchSeries[i].m_uiLastCount = uiCount;
      }

      if (pEffect->GetTotalEffectLifeTime() < plTime::MakeFromSeconds(2.0))
        return plTestAppRun::Continue;

      // one line for the five
      BenchSeries all;
      all.m_sName = m_BenchSeries[0].m_sName;
      for (const BenchSeries& series : m_BenchSeries)
      {
        all.m_fSum += series.m_fSum;
        all.m_fMax = plMath::Max(all.m_fMax, series.m_fMax);
        all.m_uiSamples += series.m_uiSamples;
      }
      m_BenchSeries.Clear();
      m_BenchSeries.PushBack(all);
      return Finish();
    }

    case SubTests::BenchRaycast:
    {
      plParticleEffectInstance* pEffect = GetEffect();
      if (pEffect == nullptr || pEffect->GetParticleSystems().GetCount() < 2 || pEffect->GetTotalEffectLifeTime() < plTime::MakeFromSeconds(2.0))
        return plTestAppRun::Continue;

      Sample(m_BenchSeries[0], UpdateMs(pEffect->GetParticleSystems()[0]));
      Sample(m_BenchSeries[1], UpdateMs(pEffect->GetParticleSystems()[1]));
      return m_BenchSeries[0].m_uiSamples < 300 ? plTestAppRun::Continue : Finish();
    }

    case SubTests::BenchFarLod:
    {
      plParticleEffectInstance* pNear = GetEffect();
      plParticleEffectInstance* pFar = m_BenchComponents.IsEmpty() ? nullptr : EffectOf(m_BenchComponents[0]);
      if (pNear == nullptr || pFar == nullptr)
        return plTestAppRun::Continue;

      if (pNear->GetTotalEffectLifeTime() < plTime::MakeFromSeconds(1.5) || pNear->GetParticleSystems().IsEmpty() || pFar->GetParticleSystems().IsEmpty())
        return plTestAppRun::Continue;

      Sample(m_BenchSeries[0], UpdateMs(pNear->GetParticleSystems()[0]));
      Sample(m_BenchSeries[1], UpdateMs(pFar->GetParticleSystems()[0]));
      return m_BenchSeries[0].m_uiSamples < 300 ? plTestAppRun::Continue : Finish();
    }

    case SubTests::BenchTestingChambers:
    {
      if (m_iFrame < 120)
        return plTestAppRun::Continue;

      double fTotal = 0.0;
      for (plUInt32 i = 0; i < m_BenchComponents.GetCount(); ++i)
      {
        double fEffect = 0.0;
        if (const plParticleEffectInstance* pEffect = EffectOf(m_BenchComponents[i]))
        {
          for (const plParticleSystemInstance* pSystem : pEffect->GetParticleSystems())
            fEffect += UpdateMs(pSystem);
        }

        Sample(m_BenchSeries[i], fEffect);
        fTotal += fEffect;
      }

      Sample(m_BenchSeries.PeekBack(), fTotal);
      return m_BenchSeries.PeekBack().m_uiSamples < 300 ? plTestAppRun::Continue : Finish();
    }

    default:
      return plTestAppRun::Quit;
  }
}
