#include <ParticlesTest/ParticlesTestPCH.h>

#include <Core/Curves/Curve1DResource.h>
#include <Foundation/CodeUtils/Expression/ExpressionAST.h>
#include <Foundation/CodeUtils/Expression/ExpressionCompiler.h>
#include <Foundation/CodeUtils/Expression/ExpressionVM.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Math/Color16f.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Math/Random.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SizeCurve.h>
#include <ParticlePlugin/Components/ParticleComponent.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_Burst.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SetAttribute.h>
#include <ParticlePlugin/Resources/ParticleEffectResource.h>
#include <ParticlePlugin/System/ParticleProgram.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/GPU/ParticleGPULowering.h>
#include <ParticlePlugin/Type/GPU/ParticleGPUProgram.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>
#include <ParticlesTest/ParticleTests.h>

// The graph nodes of the particle editor, checked where they end up: the expressions plParticleProgramOps builds for
// them (the editor's compiler uses the same functions), evaluated on the CPU VM, on the C++ form of the GPU kernel, and
// in running effects. Plus the inline curve tables, the Opacity target and the Size Curve changes.

class plGameEngineTestParticleGraphNodes : public plGameEngineTest
{
  using SUPER = plGameEngineTest;

public:
  virtual const char* GetTestName() const override { return "Particle Graph Nodes"; }
  virtual plGameEngineTestApplication* CreateApplication() override;

private:
  enum SubTests
  {
    NodeOperations,
    SplitMakeRoundTrip,
    AdaptiveMaths,
    GPUParity,
    ProgramDataVersions,
    InitializeCustomOnGPU,
    OpacityTarget,
    CurveOverLifeInline,
    SizeCurveFlat,
    SizeCurveEveryParticle,
  };

  virtual void SetupSubTests() override;
  virtual plResult InitializeSubTest(plInt32 iIdentifier) override;
  virtual plTestAppRun RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount) override;

  void CheckNodeOperations();
  void CheckSplitMakeRoundTrip();
  void CheckAdaptiveMaths();
  void CheckGPUParity();
  void CheckProgramDataVersions();
  void CheckInitializeCustomOnGPU();
  plTestAppRun RunEffectCheck(plInt32 iIdentifier);

  void StartEffect(const plParticleEffectDescriptor& desc);
  plParticleEffectInstance* GetEffect() const;

  plInt32 m_iFrame = 0;
  plUInt32 m_uiEffectCounter = 0;
  plComponentHandle m_hEffectComponent;
  plGameEngineTestApplication_Particles* m_pOwnApplication = nullptr;
};

static plGameEngineTestParticleGraphNodes s_GameEngineTestParticleGraphNodes;

namespace plParticleGraphNodeTestUtils
{
  using AST = plExpressionAST;
  using Node = plExpressionAST::Node;
  namespace Ops = plParticleProgramOps;

  template <typename T>
  T* NewModule()
  {
    return plGetStaticRTTI<T>()->GetAllocator()->template Allocate<T>();
  }

  plParticleSystemDescriptor* AddTestSystem(plParticleEffectDescriptor& ref_effect, plUInt32 uiCount, float fLifeSeconds)
  {
    plParticleSystemDescriptor* pSystem = NewModule<plParticleSystemDescriptor>();
    pSystem->m_LifeTime.m_Value = plTime::MakeFromSeconds(fLifeSeconds);
    pSystem->m_LifeTime.m_fVariance = 0.0f;

    plParticleEmitterFactory_Burst* pBurst = NewModule<plParticleEmitterFactory_Burst>();
    pBurst->m_uiSpawnCountMin = uiCount;
    pBurst->m_uiSpawnCountRange = 0;

    auto pEmitters = static_cast<const plAbstractArrayProperty*>(plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
    plParticleEmitterFactory* pEmitter = pBurst;
    pEmitters->Insert(pSystem, 0, &pEmitter);

    ref_effect.AddParticleSystem(pSystem);
    return pSystem;
  }

  Node* Input(AST& ref_ast, const char* szName, plProcessingStream::DataType type)
  {
    plHashedString sName;
    sName.Assign(szName);
    AST::Input* pInput = ref_ast.CreateInput({sName, type});
    ref_ast.m_InputNodes.PushBack(pInput);
    return pInput;
  }

  Node* Channel(AST& ref_ast, Node* pVector, plUInt32 uiComponent)
  {
    return ref_ast.CreateSwizzle(static_cast<AST::VectorComponent::Enum>(uiComponent), pVector);
  }

  Node* Vector(AST& ref_ast, const plVec3& v)
  {
    Node* components[] = {Ops::Number(ref_ast, v.x), Ops::Number(ref_ast, v.y), Ops::Number(ref_ast, v.z)};
    return ref_ast.CreateConstructorCall(AST::DataType::Float3, components);
  }

  bool Near(float a, float b, float fTolerance = 1e-4f)
  {
    return plMath::Abs(a - b) <= fTolerance * plMath::Max(1.0f, plMath::Abs(a), plMath::Abs(b));
  }

  /// What every check runs over: two numbers X and Y, two vectors V and W and the random seed, per particle.
  /// The first particles hold the edge cases: zeros, equal values, opposite signs.
  struct Particles
  {
    plDynamicArray<float> m_X;
    plDynamicArray<float> m_Y;
    plDynamicArray<plVec3> m_V;
    plDynamicArray<plVec3> m_W;
    plDynamicArray<plUInt32> m_Seeds;

    plUInt32 GetCount() const { return m_X.GetCount(); }
  };

  Particles MakeParticles(plUInt32 uiCount)
  {
    Particles p;
    plRandom rng;
    rng.Initialize(23);

    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      p.m_X.PushBack((float)rng.DoubleMinMax(-3.0, 3.0));
      p.m_Y.PushBack((float)rng.DoubleMinMax(-3.0, 3.0));
      p.m_V.PushBack(plVec3((float)rng.DoubleMinMax(-2, 2), (float)rng.DoubleMinMax(-2, 2), (float)rng.DoubleMinMax(-2, 2)));
      p.m_W.PushBack(plVec3((float)rng.DoubleMinMax(-2, 2), (float)rng.DoubleMinMax(-2, 2), (float)rng.DoubleMinMax(-2, 2)));
      p.m_Seeds.PushBack(rng.UInt());
    }

    p.m_Y[0] = 0.0f;               // divide by zero, an angle straight up or down
    p.m_X[1] = 0.0f;               // x = 0
    p.m_X[2] = p.m_Y[2];           // equal edges
    p.m_W[3] = plVec3::MakeZero(); // a zero axis and a zero projection target
    p.m_X[4] = -1.5f;
    p.m_Y[4] = 1.25f;
    return p;
  }

  /// One view per (split) stream the byte code names, into the whole streams given, as the runner builds them.
  bool SplitStreams(plArrayPtr<const plExpression::StreamDesc> descs, plArrayPtr<plProcessingStream> streams, plDynamicArray<plProcessingStream>& out_views)
  {
    for (const plExpression::StreamDesc& desc : descs)
    {
      plInt32 iComponent = -1;
      const plStringView sSource = plParticleSplitInputComponent(desc.m_sName.GetView(), iComponent);

      const plProcessingStream* pStream = nullptr;
      for (const plProcessingStream& stream : streams)
      {
        if (stream.GetName().GetView() == sSource)
          pStream = &stream;
      }

      if (pStream == nullptr)
        return false;

      const plUInt32 uiElementSize = plProcessingStream::GetDataTypeSize(desc.m_DataType);
      const plUInt32 uiOffset = plMath::Max(iComponent, 0) * uiElementSize;
      plArrayPtr<plUInt8> data(static_cast<plUInt8*>(pStream->GetWritableData()) + uiOffset, (plUInt32)pStream->GetDataSize() - uiOffset);
      out_views.PushBack(plProcessingStream(desc.m_sName, data, desc.m_DataType, pStream->GetElementStride()));
    }

    return true;
  }

  /// Compiles pValue as the one output "Out0" and runs it over the particles; out_values gets uiComponents floats per particle.
  bool Evaluate(AST& ref_ast, Node* pValue, plUInt32 uiComponents, Particles& ref_particles, plDynamicArray<float>& out_values, const plExpression::GlobalData& globalData = plExpression::GlobalData())
  {
    const plProcessingStream::DataType outputType = uiComponents == 4 ? plProcessingStream::DataType::Float4 : (uiComponents == 3 ? plProcessingStream::DataType::Float3 : plProcessingStream::DataType::Float);
    ref_ast.m_OutputNodes.PushBack(ref_ast.CreateOutput({plMakeHashedString("Out0"), outputType}, pValue));

    plExpressionByteCode byteCode;
    plExpressionCompiler compiler;
    if (compiler.Compile(ref_ast, byteCode).Failed())
      return false;

    const plUInt32 uiCount = ref_particles.GetCount();
    out_values.SetCount(uiCount * uiComponents);

    plProcessingStream inputs[] = {
      plProcessingStream(plMakeHashedString("X"), ref_particles.m_X.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float, sizeof(float)),
      plProcessingStream(plMakeHashedString("Y"), ref_particles.m_Y.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float, sizeof(float)),
      plProcessingStream(plMakeHashedString("V"), ref_particles.m_V.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float3, sizeof(plVec3)),
      plProcessingStream(plMakeHashedString("W"), ref_particles.m_W.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float3, sizeof(plVec3)),
      plProcessingStream(plMakeHashedString("RandomSeed"), ref_particles.m_Seeds.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Int, sizeof(plUInt32)),
    };
    plProcessingStream outputs[] = {
      plProcessingStream(plMakeHashedString("Out0"), out_values.GetArrayPtr().ToByteArray(), outputType, (plUInt16)(uiComponents * sizeof(float))),
    };

    plDynamicArray<plProcessingStream> scalarInputs, scalarOutputs;
    if (!SplitStreams(byteCode.GetInputs(), inputs, scalarInputs) || !SplitStreams(byteCode.GetOutputs(), outputs, scalarOutputs))
      return false;

    plExpressionVM vm;
    vm.RegisterFunction(plParticleProgramTables::s_CurveFunc);
    vm.RegisterFunction(plParticleProgramTables::s_GradientFunc);
    return vm.Execute(byteCode, scalarInputs, scalarOutputs, uiCount, globalData, plExpressionVM::Flags::BestPerformance).Succeeded();
  }

  /// Compiles one output computing sProperty into a block's program, the way the editor does.
  bool CompileInto(AST& ref_ast, Node* pValue, plProcessingStream::DataType outputType, const char* szProperty, plParticleProgramData& out_program)
  {
    ref_ast.m_OutputNodes.PushBack(ref_ast.CreateOutput({plMakeHashedString("Out0"), outputType}, pValue));
    out_program.m_Outputs.PushBack(szProperty);

    plExpressionCompiler compiler;
    return compiler.Compile(ref_ast, out_program.m_ByteCode).Succeeded();
  }

  float SmoothStep(float e0, float e1, float x, bool bSmoother)
  {
    const float t = (e1 == e0) ? (x >= e0 ? 1.0f : 0.0f) : plMath::Saturate((x - e0) / (e1 - e0));
    return bSmoother ? t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f) : t * t * (3.0f - 2.0f * t);
  }

  plVec3 RotateAboutAxis(const plVec3& v, const plVec3& axis, float fRadians)
  {
    const float fLength = axis.GetLength();
    if (fLength <= 0.0f)
      return v;

    const plVec3 k = axis / fLength;
    const float c = plMath::Cos(plAngle::MakeFromRadian(fRadians));
    const float s = plMath::Sin(plAngle::MakeFromRadian(fRadians));
    return v * c + k.CrossRH(v) * s + k * (k.Dot(v) * (1.0f - c));
  }

  /// A ramp from 0 to 1, as an empty inline curve bakes.
  void FillRamp(plDynamicArray<float>& out_table)
  {
    constexpr plUInt32 n = plParticleProgramTables::s_uiTableSize;
    out_table.SetCount(n);
    for (plUInt32 i = 0; i < n; ++i)
      out_table[i] = (float)i / (n - 1);
  }

  /// A curve resource from points (x, y) with linear tangents.
  plCurve1DResourceHandle CreateCurveResource(plStringView sName, plArrayPtr<const plVec2> points)
  {
    plCurve1DResourceDescriptor desc;
    plCurve1D& curve = desc.m_Curves.ExpandAndGetRef();

    for (const plVec2& point : points)
    {
      auto& cp = curve.AddControlPoint(point.x);
      cp.m_Position.y = point.y;
      cp.m_TangentModeLeft = plCurveTangentMode::Linear;
      cp.m_TangentModeRight = plCurveTangentMode::Linear;
    }

    curve.SortControlPoints();
    curve.CreateLinearApproximation();

    return plResourceManager::GetOrCreateResource<plCurve1DResource>(sName, std::move(desc), "particle graph node test curve");
  }
} // namespace plParticleGraphNodeTestUtils

using namespace plParticleGraphNodeTestUtils;

plGameEngineTestApplication* plGameEngineTestParticleGraphNodes::CreateApplication()
{
  m_pOwnApplication = PL_DEFAULT_NEW(plGameEngineTestApplication_Particles);
  return m_pOwnApplication;
}

void plGameEngineTestParticleGraphNodes::SetupSubTests()
{
  AddSubTest("NodeOperations", SubTests::NodeOperations);
  AddSubTest("SplitMakeRoundTrip", SubTests::SplitMakeRoundTrip);
  AddSubTest("AdaptiveMaths", SubTests::AdaptiveMaths);
  AddSubTest("GPUParity", SubTests::GPUParity);
  AddSubTest("ProgramDataVersions", SubTests::ProgramDataVersions);
  AddSubTest("InitializeCustomOnGPU", SubTests::InitializeCustomOnGPU);
  AddSubTest("OpacityTarget", SubTests::OpacityTarget);
  AddSubTest("CurveOverLifeInline", SubTests::CurveOverLifeInline);
  AddSubTest("SizeCurveFlat", SubTests::SizeCurveFlat);
  AddSubTest("SizeCurveEveryParticle", SubTests::SizeCurveEveryParticle);
}

plResult plGameEngineTestParticleGraphNodes::InitializeSubTest(plInt32 iIdentifier)
{
  PL_SUCCEED_OR_RETURN(SUPER::InitializeSubTest(iIdentifier));

  m_iFrame = -1;
  m_pOwnApplication->SetupSceneSubTest("Particles/AssetCache/Common/Particles1.plBinScene");

  plParticleEffectDescriptor desc;

  switch (iIdentifier)
  {
    case SubTests::OpacityTarget:
    {
      // Initialize: Color (0.2, 0.4, 0.6, 1). Update: Opacity 0.25, which may only touch alpha.
      plParticleSystemDescriptor* pSystem = AddTestSystem(desc, 8, 10.0f);

      plParticleInitializerFactory_SetAttribute* pColor = NewModule<plParticleInitializerFactory_SetAttribute>();
      pColor->m_Settings.m_Target = plParticleSinkTarget::Color;
      pColor->m_Settings.m_Color = plColor(0.2f, 0.4f, 0.6f, 1.0f);
      pSystem->AddInitializerFactory(pColor);

      plParticleBehaviorFactory_Attribute* pOpacity = NewModule<plParticleBehaviorFactory_Attribute>();
      pOpacity->m_Settings.m_Target = plParticleSinkTarget::Opacity;
      pOpacity->m_Settings.m_fNumber = 0.25f;
      pSystem->AddBehaviorFactory(pOpacity);

      pSystem->AddTypeFactory(NewModule<plParticleTypeQuadFactory>());
      StartEffect(desc);
      break;
    }

    case SubTests::CurveOverLifeInline:
    {
      // Curve Over Life into Size, Output 0.5..2 over Normalized Age, with the ramp an empty inline curve bakes
      plParticleSystemDescriptor* pSystem = AddTestSystem(desc, 16, 2.0f);

      plParticleBehaviorFactory_Attribute* pSize = NewModule<plParticleBehaviorFactory_Attribute>();
      pSize->m_Settings.m_Target = plParticleSinkTarget::Size;

      AST ast;
      Node* pLifeTime = Input(ast, "LifeTime", plProcessingStream::DataType::Float2);
      Node* pAge = ast.CreateBinaryOperator(AST::NodeType::Subtract, Ops::Number(ast, 1.0f), ast.CreateBinaryOperator(AST::NodeType::Multiply, Channel(ast, pLifeTime, 0), Channel(ast, pLifeTime, 1)));
      Node* pCurve = Ops::SampleCurve(ast, pAge, ast.CreateConstant(plVariant(0), AST::DataType::Int));
      Node* pValue = ast.CreateTernaryOperator(AST::NodeType::Lerp, Ops::Number(ast, 0.5f), Ops::Number(ast, 2.0f), pCurve);
      PL_TEST_BOOL(CompileInto(ast, pValue, plProcessingStream::DataType::Float, "Number", pSize->m_Settings.m_Program));
      FillRamp(pSize->m_Settings.m_Program.m_Assets.m_InlineCurves);

      pSystem->AddBehaviorFactory(pSize);
      pSystem->AddTypeFactory(NewModule<plParticleTypeQuadFactory>());
      StartEffect(desc);
      break;
    }

    case SubTests::SizeCurveFlat:
    case SubTests::SizeCurveEveryParticle:
    {
      // flat: a curve that holds 3 must give Base Size + Curve Scale, as if it read 1.
      // every particle: a short-lived ramp; one burst of equal particles must all have the same size after a step
      const bool bFlat = iIdentifier == SubTests::SizeCurveFlat;
      const plVec2 flat[] = {plVec2(0.0f, 3.0f), plVec2(1.0f, 3.0f)};
      const plVec2 ramp[] = {plVec2(0.0f, 0.0f), plVec2(1.0f, 10.0f)};

      plParticleSystemDescriptor* pSystem = AddTestSystem(desc, bFlat ? 10 : 200, bFlat ? 10.0f : 0.5f);

      plParticleBehaviorFactory_SizeCurve* pSizeCurve = NewModule<plParticleBehaviorFactory_SizeCurve>();
      pSizeCurve->m_Mode = plParticleSizeMode::Uniform;
      pSizeCurve->m_fBaseSize = bFlat ? 0.0f : 1.0f;
      pSizeCurve->m_fCurveScale = bFlat ? 0.5f : 1.0f;
      pSizeCurve->m_hCurve = bFlat ? CreateCurveResource("ParticleGraphNodeTest_FlatCurve", flat) : CreateCurveResource("ParticleGraphNodeTest_RampCurve", ramp);
      pSystem->AddBehaviorFactory(pSizeCurve);

      pSystem->AddTypeFactory(NewModule<plParticleTypeQuadFactory>());
      StartEffect(desc);
      break;
    }

    default:
      break;
  }

  return PL_SUCCESS;
}

plTestAppRun plGameEngineTestParticleGraphNodes::RunSubTest(plInt32 iIdentifier, plUInt32 /*uiInvocationCount*/)
{
  ++m_iFrame;

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  switch (iIdentifier)
  {
    case SubTests::NodeOperations:
      CheckNodeOperations();
      return plTestAppRun::Quit;

    case SubTests::SplitMakeRoundTrip:
      CheckSplitMakeRoundTrip();
      return plTestAppRun::Quit;

    case SubTests::AdaptiveMaths:
      CheckAdaptiveMaths();
      return plTestAppRun::Quit;

    case SubTests::GPUParity:
      CheckGPUParity();
      return plTestAppRun::Quit;

    case SubTests::ProgramDataVersions:
      CheckProgramDataVersions();
      return plTestAppRun::Quit;

    case SubTests::InitializeCustomOnGPU:
      CheckInitializeCustomOnGPU();
      return plTestAppRun::Quit;

    default:
      break;
  }

  PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
  return RunEffectCheck(iIdentifier);
}

void plGameEngineTestParticleGraphNodes::StartEffect(const plParticleEffectDescriptor& desc)
{
  // every sub-test gets its own resource, so no instance or shared state carries over
  plStringBuilder sName;
  sName.SetFormat("ParticleGraphNodeTest{}", m_uiEffectCounter++);

  plParticleEffectResourceDescriptor resDesc;

  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    desc.Save(writer);

    plMemoryStreamReader reader(&storage);
    resDesc.m_Effect.Load(reader);
  }

  plParticleEffectResourceHandle hEffect = plResourceManager::CreateResource<plParticleEffectResource>(sName, std::move(resDesc));

  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());

  plGameObject* pObject = nullptr;
  if (!pWorld->TryGetObjectWithGlobalKey("Effect", pObject))
  {
    plGameObjectDesc objDesc;
    pWorld->CreateObject(objDesc, pObject);
  }

  plParticleComponent* pComponent = nullptr;
  m_hEffectComponent = pWorld->GetOrCreateComponentManager<plParticleComponentManager>()->CreateComponent(pObject, pComponent);
  pComponent->m_uiRandomSeed = 42;
  pComponent->SetParticleEffect(hEffect);
}

plParticleEffectInstance* plGameEngineTestParticleGraphNodes::GetEffect() const
{
  const plWorld* pWorld = m_pOwnApplication->GetWorld();
  const plParticleComponent* pComponent = nullptr;
  if (!pWorld->TryGetComponent(m_hEffectComponent, pComponent))
    return nullptr;

  if (!pComponent->m_EffectController.IsAlive())
    return nullptr;

  return pComponent->m_EffectController.GetInstance();
}

//////////////////////////////////////////////////////////////////////////
// Expressions on the CPU VM

void plGameEngineTestParticleGraphNodes::CheckNodeOperations()
{
  Particles p = MakeParticles(64);
  const plUInt32 uiCount = p.GetCount();

  plDynamicArray<float> values;

  // runs one expression and compares every particle with the C++ reference
  auto check = [&](const char* szName, auto fnBuild, plUInt32 uiComponents, auto fnReference, float fTolerance)
  {
    AST ast;
    Node* x = Input(ast, "X", plProcessingStream::DataType::Float);
    Node* y = Input(ast, "Y", plProcessingStream::DataType::Float);
    Node* v = Input(ast, "V", plProcessingStream::DataType::Float3);
    Node* w = Input(ast, "W", plProcessingStream::DataType::Float3);
    Node* pSeed = Input(ast, "RandomSeed", plProcessingStream::DataType::Int);

    if (!PL_TEST_BOOL_MSG(Evaluate(ast, fnBuild(ast, x, y, v, w, pSeed), uiComponents, p, values), "%s does not compile or run", szName))
      return;

    plUInt32 uiMismatches = 0;
    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      float reference[4] = {};
      fnReference(i, reference);

      for (plUInt32 c = 0; c < uiComponents; ++c)
        uiMismatches += Near(values[i * uiComponents + c], reference[c], fTolerance) ? 0 : 1;
    }

    PL_TEST_BOOL_MSG(uiMismatches == 0, "%s: %u of %u values differ from the reference", szName, uiMismatches, uiCount * uiComponents);
  };

  check("Divide", [](AST& ast, Node* x, Node* y, Node*, Node*, Node*)
    { return Ops::SafeDivide(ast, x, y); }, 1, [&](plUInt32 i, float* r)
    { r[0] = p.m_Y[i] == 0.0f ? 0.0f : p.m_X[i] / p.m_Y[i]; }, 1e-4f);

  check("Modulo", [](AST& ast, Node* x, Node* y, Node*, Node*, Node*)
    { return ast.CreateTernaryOperator(AST::NodeType::Select, ast.CreateBinaryOperator(AST::NodeType::Equal, y, Ops::Number(ast, 0.0f)), Ops::Number(ast, 0.0f), ast.CreateBinaryOperator(AST::NodeType::Modulo, x, y)); }, 1, [&](plUInt32 i, float* r)
    { r[0] = p.m_Y[i] == 0.0f ? 0.0f : p.m_X[i] - p.m_Y[i] * plMath::Trunc(p.m_X[i] / p.m_Y[i]); }, 1e-4f);

  check("Step", [](AST& ast, Node* x, Node* y, Node*, Node*, Node*)
    { return Ops::Step(ast, x, y); }, 1, [&](plUInt32 i, float* r)
    { r[0] = p.m_Y[i] >= p.m_X[i] ? 1.0f : 0.0f; }, 1e-6f);

  check("Smooth Step", [](AST& ast, Node* x, Node* y, Node*, Node*, Node*)
    { return Ops::SmoothStep(ast, x, y, Ops::Number(ast, 0.3f), false); }, 1, [&](plUInt32 i, float* r)
    { r[0] = SmoothStep(p.m_X[i], p.m_Y[i], 0.3f, false); }, 1e-4f);

  check("Smoother Step", [](AST& ast, Node* x, Node* y, Node*, Node*, Node*)
    { return Ops::SmoothStep(ast, x, y, Ops::Number(ast, 0.3f), true); }, 1, [&](plUInt32 i, float* r)
    { r[0] = SmoothStep(p.m_X[i], p.m_Y[i], 0.3f, true); }, 1e-4f);

  check("Inverse Lerp", [](AST& ast, Node* x, Node* y, Node*, Node*, Node*)
    { return Ops::InverseLerp(ast, x, y, Ops::Number(ast, 0.25f), true); }, 1, [&](plUInt32 i, float* r)
    {
      const float fRange = p.m_Y[i] - p.m_X[i];
      r[0] = plMath::Saturate(fRange == 0.0f ? 0.0f : (0.25f - p.m_X[i]) / fRange); }, 1e-4f);

  // a typed range folds into one multiply, a wired one divides
  check("Remap (typed range)", [](AST& ast, Node* x, Node*, Node*, Node*, Node*)
    { return Ops::Remap(ast, x, Ops::Number(ast, -1.0f), Ops::Number(ast, 1.0f), Ops::Number(ast, 0.0f), Ops::Number(ast, 10.0f), true); }, 1, [&](plUInt32 i, float* r)
    { r[0] = 10.0f * plMath::Saturate((p.m_X[i] + 1.0f) * 0.5f); }, 1e-4f);

  check("Remap (wired range)", [](AST& ast, Node* x, Node* y, Node*, Node*, Node*)
    { return Ops::Remap(ast, x, y, ast.CreateBinaryOperator(AST::NodeType::Add, y, Ops::Number(ast, 2.0f)), Ops::Number(ast, 1.0f), Ops::Number(ast, 3.0f), false); }, 1, [&](plUInt32 i, float* r)
    { r[0] = 1.0f + 2.0f * ((p.m_X[i] - p.m_Y[i]) / 2.0f); }, 1e-4f);

  check("Sign", [](AST& ast, Node* x, Node*, Node*, Node*, Node*)
    { return Ops::Sign(ast, x); }, 1, [&](plUInt32 i, float* r)
    { r[0] = p.m_X[i] > 0.0f ? 1.0f : (p.m_X[i] < 0.0f ? -1.0f : 0.0f); }, 1e-6f);

  check("ATan2", [](AST& ast, Node* x, Node* y, Node*, Node*, Node*)
    { return Ops::ATan2(ast, y, x); }, 1, [&](plUInt32 i, float* r)
    { r[0] = (p.m_X[i] == 0.0f && p.m_Y[i] == 0.0f) ? 0.0f : plMath::ATan2(p.m_Y[i], p.m_X[i]).GetRadian(); }, 1e-3f);

  check("Distance", [](AST& ast, Node*, Node*, Node* v, Node* w, Node*)
    { return Ops::Distance(ast, v, w); }, 1, [&](plUInt32 i, float* r)
    { r[0] = (p.m_V[i] - p.m_W[i]).GetLength(); }, 1e-4f);

  check("Project On", [](AST& ast, Node*, Node*, Node* v, Node* w, Node*)
    { return Ops::ProjectOnto(ast, v, w); }, 3, [&](plUInt32 i, float* r)
    {
      const float fLengthSquared = p.m_W[i].Dot(p.m_W[i]);
      const plVec3 vResult = fLengthSquared == 0.0f ? plVec3::MakeZero() : p.m_W[i] * (p.m_V[i].Dot(p.m_W[i]) / fLengthSquared);
      r[0] = vResult.x;
      r[1] = vResult.y;
      r[2] = vResult.z; }, 1e-4f);

  check("Rotate About Axis", [](AST& ast, Node* x, Node*, Node* v, Node* w, Node*)
    { return Ops::RotateAboutAxis(ast, v, w, x); }, 3, [&](plUInt32 i, float* r)
    {
      const plVec3 vResult = RotateAboutAxis(p.m_V[i], p.m_W[i], p.m_X[i]);
      r[0] = vResult.x;
      r[1] = vResult.y;
      r[2] = vResult.z; }, 1e-3f);

  // the local frame and back gives the vector again
  check("Transform Position round trip", [](AST& ast, Node*, Node*, Node* v, Node*, Node*)
    {
      const plQuat qRotation = plQuat::MakeFromAxisAndAngle(plVec3(1, 2, 3).GetNormalized(), plAngle::MakeFromDegree(37.0f));
      Ops::Frame frame;
      frame.m_pPosition = Vector(ast, plVec3(1, 2, 3));
      frame.m_pForward = Vector(ast, qRotation * plVec3(1, 0, 0));
      frame.m_pRight = Vector(ast, qRotation * plVec3(0, 1, 0));
      frame.m_pUp = Vector(ast, qRotation * plVec3(0, 0, 1));
      frame.m_pScale = Vector(ast, plVec3(2.0f, 1.0f, 0.5f));
      return Ops::TransformPosition(ast, frame, Ops::TransformPosition(ast, frame, v, true), false); }, 3, [&](plUInt32 i, float* r)
    {
      r[0] = p.m_V[i].x;
      r[1] = p.m_V[i].y;
      r[2] = p.m_V[i].z; }, 1e-4f);

  // random values: within their range, and not all the same
  {
    AST ast;
    Node* pSeed = Input(ast, "RandomSeed", plProcessingStream::DataType::Int);
    if (PL_TEST_BOOL(Evaluate(ast, Ops::RandomInteger(ast, pSeed, 5, Ops::Number(ast, 3.0f), Ops::Number(ast, 7.0f)), 1, p, values)))
    {
      plUInt32 uiOutside = 0;
      plUInt32 uiSeen = 0;
      for (plUInt32 i = 0; i < uiCount; ++i)
      {
        const float f = values[i];
        uiOutside += (f >= 3.0f && f <= 7.0f && f == plMath::Floor(f)) ? 0 : 1;
        uiSeen |= 1u << plMath::Clamp((plInt32)f, 0, 31);
      }

      PL_TEST_INT(uiOutside, 0);
      PL_TEST_BOOL_MSG(plMath::CountBits(uiSeen) >= 4, "Random Integer gives too few distinct values");
    }
  }

  {
    AST ast;
    Node* pSeed = Input(ast, "RandomSeed", plProcessingStream::DataType::Int);
    if (PL_TEST_BOOL(Evaluate(ast, Ops::RandomRange(ast, pSeed, 9, Ops::Number(ast, 2.0f), Ops::Number(ast, 5.0f), 3), 3, p, values)))
    {
      plUInt32 uiOutside = 0;
      plUInt32 uiSameComponents = 0;
      for (plUInt32 i = 0; i < uiCount; ++i)
      {
        for (plUInt32 c = 0; c < 3; ++c)
          uiOutside += (values[i * 3 + c] >= 2.0f && values[i * 3 + c] <= 5.0f) ? 0 : 1;

        uiSameComponents += values[i * 3] == values[i * 3 + 1] ? 1 : 0;
      }

      PL_TEST_INT(uiOutside, 0);
      PL_TEST_BOOL_MSG(uiSameComponents < uiCount / 4, "Random Range gives the components the same random value");
    }
  }

  for (plUInt32 uiShape = 0; uiShape < 4; ++uiShape)
  {
    const auto shape = static_cast<Ops::RandomVectorShape>(uiShape);
    const float fRadius = 2.0f;
    const float fCone = plAngle::MakeFromDegree(30.0f).GetRadian();

    AST ast;
    Node* pSeed = Input(ast, "RandomSeed", plProcessingStream::DataType::Int);
    if (!PL_TEST_BOOL(Evaluate(ast, Ops::RandomVector(ast, pSeed, 3, shape, Ops::Number(ast, fRadius), fCone), 3, p, values)))
      continue;

    plUInt32 uiOutside = 0;
    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      const plVec3 vValue(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]);
      const float fLength = vValue.GetLength();

      switch (shape)
      {
        case Ops::RandomVectorShape::InSphere:
          uiOutside += fLength <= fRadius + 1e-3f ? 0 : 1;
          break;
        case Ops::RandomVectorShape::OnSphere:
          uiOutside += Near(fLength, fRadius, 1e-3f) ? 0 : 1;
          break;
        case Ops::RandomVectorShape::InBox:
          uiOutside += (plMath::Abs(vValue.x) <= fRadius && plMath::Abs(vValue.y) <= fRadius && plMath::Abs(vValue.z) <= fRadius) ? 0 : 1;
          break;
        case Ops::RandomVectorShape::InCone:
          uiOutside += (Near(fLength, fRadius, 1e-3f) && vValue.z / fRadius >= plMath::Cos(plAngle::MakeFromRadian(fCone)) - 1e-3f) ? 0 : 1;
          break;
      }
    }

    PL_TEST_BOOL_MSG(uiOutside == 0, "Random Vector shape %u: %u of %u vectors outside the shape", uiShape, uiOutside, uiCount);
  }

  // noise: 0..1 for Noise, -1..1 per channel for Vector Noise
  {
    AST ast;
    Node* v = Input(ast, "V", plProcessingStream::DataType::Float3);
    if (PL_TEST_BOOL(Evaluate(ast, Ops::Noise(ast, v, Ops::Number(ast, 1.5f), Ops::Number(ast, 2.0f)), 1, p, values)))
    {
      plUInt32 uiOutside = 0;
      for (float f : values)
        uiOutside += (f >= 0.0f && f <= 1.0f) ? 0 : 1;
      PL_TEST_INT(uiOutside, 0);
    }
  }

  {
    AST ast;
    Node* v = Input(ast, "V", plProcessingStream::DataType::Float3);
    if (PL_TEST_BOOL(Evaluate(ast, Ops::VectorNoise(ast, v, Ops::Number(ast, 1.5f), 1), 3, p, values)))
    {
      plUInt32 uiOutside = 0;
      plUInt32 uiEqualChannels = 0;
      for (plUInt32 i = 0; i < uiCount; ++i)
      {
        for (plUInt32 c = 0; c < 3; ++c)
          uiOutside += (values[i * 3 + c] >= -1.0f && values[i * 3 + c] <= 1.0f) ? 0 : 1;

        uiEqualChannels += values[i * 3] == values[i * 3 + 1] ? 1 : 0;
      }

      PL_TEST_INT(uiOutside, 0);
      PL_TEST_BOOL_MSG(uiEqualChannels < uiCount / 4, "Vector Noise channels are not independent");
    }
  }
}

void plGameEngineTestParticleGraphNodes::CheckSplitMakeRoundTrip()
{
  // Split Vector gives one swizzle per output; Make Vector of the three gives the vector back
  Particles p = MakeParticles(32);
  plDynamicArray<float> values;

  {
    AST ast;
    Node* v = Input(ast, "V", plProcessingStream::DataType::Float3);
    Node* components[] = {Channel(ast, v, 0), Channel(ast, v, 1), Channel(ast, v, 2)};
    if (PL_TEST_BOOL(Evaluate(ast, ast.CreateConstructorCall(AST::DataType::Float3, components), 3, p, values)))
    {
      plUInt32 uiMismatches = 0;
      for (plUInt32 i = 0; i < p.GetCount(); ++i)
        uiMismatches += (values[i * 3] == p.m_V[i].x && values[i * 3 + 1] == p.m_V[i].y && values[i * 3 + 2] == p.m_V[i].z) ? 0 : 1;
      PL_TEST_INT(uiMismatches, 0);
    }
  }

  // Make Colour of four numbers, split again: the channels come back in order
  {
    AST ast;
    Node* x = Input(ast, "X", plProcessingStream::DataType::Float);
    Node* y = Input(ast, "Y", plProcessingStream::DataType::Float);
    Node* channels[] = {x, y, Ops::Number(ast, 0.5f), Ops::Number(ast, 1.0f)};
    Node* pColor = ast.CreateConstructorCall(AST::DataType::Float4, channels);
    Node* split[] = {Channel(ast, pColor, 3), Channel(ast, pColor, 2), Channel(ast, pColor, 1), Channel(ast, pColor, 0)};
    if (PL_TEST_BOOL(Evaluate(ast, ast.CreateConstructorCall(AST::DataType::Float4, split), 4, p, values)))
    {
      plUInt32 uiMismatches = 0;
      for (plUInt32 i = 0; i < p.GetCount(); ++i)
        uiMismatches += (values[i * 4] == 1.0f && values[i * 4 + 1] == 0.5f && values[i * 4 + 2] == p.m_Y[i] && values[i * 4 + 3] == p.m_X[i]) ? 0 : 1;
      PL_TEST_INT(uiMismatches, 0);
    }
  }

  // Swizzle "zyx"
  {
    AST ast;
    Node* v = Input(ast, "V", plProcessingStream::DataType::Float3);
    if (PL_TEST_BOOL(Evaluate(ast, ast.CreateSwizzle("zyx", v), 3, p, values)))
    {
      plUInt32 uiMismatches = 0;
      for (plUInt32 i = 0; i < p.GetCount(); ++i)
        uiMismatches += (values[i * 3] == p.m_V[i].z && values[i * 3 + 1] == p.m_V[i].y && values[i * 3 + 2] == p.m_V[i].x) ? 0 : 1;
      PL_TEST_INT(uiMismatches, 0);
    }
  }
}

void plGameEngineTestParticleGraphNodes::CheckAdaptiveMaths()
{
  // adaptive Add of a vector and a number: the number is added to every component
  Particles p = MakeParticles(32);
  plDynamicArray<float> values;

  {
    AST ast;
    Node* x = Input(ast, "X", plProcessingStream::DataType::Float);
    Node* v = Input(ast, "V", plProcessingStream::DataType::Float3);
    if (PL_TEST_BOOL(Evaluate(ast, ast.CreateBinaryOperator(AST::NodeType::Add, v, Ops::Broadcast(ast, x, 3)), 3, p, values)))
    {
      plUInt32 uiMismatches = 0;
      for (plUInt32 i = 0; i < p.GetCount(); ++i)
      {
        for (plUInt32 c = 0; c < 3; ++c)
          uiMismatches += Near(values[i * 3 + c], p.m_V[i].GetData()[c] + p.m_X[i]) ? 0 : 1;
      }
      PL_TEST_INT(uiMismatches, 0);
    }
  }

  // adaptive Lerp of two colours by a number, every channel alike
  {
    AST ast;
    Node* x = Input(ast, "X", plProcessingStream::DataType::Float);
    Node* a[] = {Ops::Number(ast, 0.0f), Ops::Number(ast, 0.2f), Ops::Number(ast, 0.4f), Ops::Number(ast, 1.0f)};
    Node* b[] = {Ops::Number(ast, 1.0f), Ops::Number(ast, 0.6f), Ops::Number(ast, 0.4f), Ops::Number(ast, 0.0f)};
    Node* t = Ops::Broadcast(ast, ast.CreateUnaryOperator(AST::NodeType::Saturate, x), 4);
    Node* pLerp = ast.CreateTernaryOperator(AST::NodeType::Lerp, ast.CreateConstructorCall(AST::DataType::Float4, a), ast.CreateConstructorCall(AST::DataType::Float4, b), t);
    if (PL_TEST_BOOL(Evaluate(ast, pLerp, 4, p, values)))
    {
      const float fA[] = {0.0f, 0.2f, 0.4f, 1.0f};
      const float fB[] = {1.0f, 0.6f, 0.4f, 0.0f};

      plUInt32 uiMismatches = 0;
      for (plUInt32 i = 0; i < p.GetCount(); ++i)
      {
        const float fT = plMath::Saturate(p.m_X[i]);
        for (plUInt32 c = 0; c < 4; ++c)
          uiMismatches += Near(values[i * 4 + c], plMath::Lerp(fA[c], fB[c], fT)) ? 0 : 1;
      }
      PL_TEST_INT(uiMismatches, 0);
    }
  }

  // a vector wired as a colour is opaque; a colour wired as a vector drops its alpha (D3)
  {
    AST ast;
    Node* v = Input(ast, "V", plProcessingStream::DataType::Float3);
    Node* asColor[] = {Channel(ast, v, 0), Channel(ast, v, 1), Channel(ast, v, 2), Ops::Number(ast, 1.0f)};
    if (PL_TEST_BOOL(Evaluate(ast, ast.CreateConstructorCall(AST::DataType::Float4, asColor), 4, p, values)))
    {
      plUInt32 uiMismatches = 0;
      for (plUInt32 i = 0; i < p.GetCount(); ++i)
        uiMismatches += (values[i * 4] == p.m_V[i].x && values[i * 4 + 3] == 1.0f) ? 0 : 1;
      PL_TEST_INT(uiMismatches, 0);
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// CPU and GPU agree

void plGameEngineTestParticleGraphNodes::CheckGPUParity()
{
  // four Set Attribute records built from the new operations, run by the C++ form of the simulate kernel and by the
  // CPU VM over the same particles: they must agree particle by particle
  plParticleBehaviorFactory_Attribute setVelocity, setColor, setSize, setOpacity;
  setVelocity.m_Settings.m_Target = plParticleSinkTarget::Velocity;
  setColor.m_Settings.m_Target = plParticleSinkTarget::Color;
  setSize.m_Settings.m_Target = plParticleSinkTarget::Size;
  setOpacity.m_Settings.m_Target = plParticleSinkTarget::Opacity;

  {
    // Velocity = Rotate About Axis(Velocity, Position, Size x 90 degrees)
    AST ast;
    Node* pPosition = Input(ast, "Position", plProcessingStream::DataType::Float3);
    Node* pVelocity = Input(ast, "Velocity", plProcessingStream::DataType::Float3);
    Node* pSize = Input(ast, "Size", plProcessingStream::DataType::Half);
    Node* pAngle = ast.CreateBinaryOperator(AST::NodeType::Multiply, pSize, Ops::Number(ast, plAngle::MakeFromDegree(90.0f).GetRadian()));
    PL_TEST_BOOL(CompileInto(ast, Ops::RotateAboutAxis(ast, pVelocity, pPosition, pAngle), plProcessingStream::DataType::Float3, "Vector", setVelocity.m_Settings.m_Program));
  }

  {
    // Color = Make Colour(Sign(Position.x), Step(0.5, LifeTime.x), Smooth Step(0, 1, Size), Random(seed 5))
    AST ast;
    Node* pPosition = Input(ast, "Position", plProcessingStream::DataType::Float3);
    Node* pLifeTime = Input(ast, "LifeTime", plProcessingStream::DataType::Float2);
    Node* pSize = Input(ast, "Size", plProcessingStream::DataType::Half);
    Node* pSeed = Input(ast, "RandomSeed", plProcessingStream::DataType::Int);
    Node* channels[] = {Ops::Sign(ast, Channel(ast, pPosition, 0)), Ops::Step(ast, Ops::Number(ast, 0.5f), Channel(ast, pLifeTime, 0)),
      Ops::SmoothStep(ast, Ops::Number(ast, 0.0f), Ops::Number(ast, 1.0f), pSize, false), Ops::ParticleRandom(ast, pSeed, 5)};
    PL_TEST_BOOL(CompileInto(ast, ast.CreateConstructorCall(AST::DataType::Float4, channels), plProcessingStream::DataType::Float4, "Color", setColor.m_Settings.m_Program));
  }

  {
    // Size = Curve Over Life: lerp(0.1, 2, inline curve(Normalized Age)), the curve a ramp in inline slot 0
    AST ast;
    Node* pLifeTime = Input(ast, "LifeTime", plProcessingStream::DataType::Float2);
    Node* pAge = ast.CreateBinaryOperator(AST::NodeType::Subtract, Ops::Number(ast, 1.0f), ast.CreateBinaryOperator(AST::NodeType::Multiply, Channel(ast, pLifeTime, 0), Channel(ast, pLifeTime, 1)));
    Node* pCurve = Ops::SampleCurve(ast, pAge, ast.CreateConstant(plVariant(0), AST::DataType::Int));
    PL_TEST_BOOL(CompileInto(ast, ast.CreateTernaryOperator(AST::NodeType::Lerp, Ops::Number(ast, 0.1f), Ops::Number(ast, 2.0f), pCurve), plProcessingStream::DataType::Float, "Number", setSize.m_Settings.m_Program));
    FillRamp(setSize.m_Settings.m_Program.m_Assets.m_InlineCurves);
  }

  {
    // Opacity = ATan2(Position.y, Position.x) / 2pi + 0.5, which may only write the colour's alpha
    AST ast;
    Node* pPosition = Input(ast, "Position", plProcessingStream::DataType::Float3);
    Node* pAngle = Ops::ATan2(ast, Channel(ast, pPosition, 1), Channel(ast, pPosition, 0));
    Node* pValue = ast.CreateBinaryOperator(AST::NodeType::Add, ast.CreateBinaryOperator(AST::NodeType::Multiply, pAngle, Ops::Number(ast, 0.5f / plMath::Pi<float>())), Ops::Number(ast, 0.5f));
    PL_TEST_BOOL(CompileInto(ast, pValue, plProcessingStream::DataType::Float, "Number", setOpacity.m_Settings.m_Program));
  }

  const plParticleBehaviorFactory* factories[] = {&setVelocity, &setColor, &setSize, &setOpacity};
  plParticleGPUProgramSet programs;
  const plString sReason = programs.Build(factories);
  if (!PL_TEST_BOOL_MSG(sReason.IsEmpty(), "Did not build: %s", sReason.GetData()))
    return;

  PL_TEST_INT(programs.GetProgramCount(), 4);

  plDynamicArray<float> data;
  data.SetCount(programs.GetDataCount());
  programs.FillData(nullptr, plTime::MakeZero(), data);

  // particles as the GPU holds them, and the same values as the CPU streams hold them
  constexpr plUInt32 uiCount = 256;
  plRandom rng;
  rng.Initialize(11);

  plDynamicArray<plGPUParticle> particles;
  plDynamicArray<plVec4> position;
  plDynamicArray<plVec3> velocity;
  plDynamicArray<plFloat16> size;
  plDynamicArray<plColorLinear16f> color;
  plDynamicArray<plVec2> lifeTime;
  plDynamicArray<plUInt32> seeds;

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    plGPUParticle& gpu = particles.ExpandAndGetRef();
    gpu.Position.Set((float)rng.DoubleMinMax(-2, 2), (float)rng.DoubleMinMax(-2, 2), (float)rng.DoubleMinMax(-2, 2));
    gpu.Velocity.Set((float)rng.DoubleMinMax(-3, 3), (float)rng.DoubleMinMax(-3, 3), (float)rng.DoubleMinMax(-3, 3));
    gpu.Size = plFloat16((float)rng.DoubleMinMax(0.1, 1.0)); // the CPU stream is half
    gpu.Color = plColorLinear16f(plColor(0.5f, 0.5f, 0.5f, 1.0f));
    gpu.Life = (float)rng.DoubleMinMax(0.05, 1.0);
    gpu.MaxLife = 1.0f / (float)rng.DoubleMinMax(1.0, 4.0);
    gpu.RotationOffset = 0.0f;
    gpu.RotationSpeed = 0.0f;
    gpu.Flags = 1;
    gpu.InitialSize = gpu.Size;
    gpu.AxisPacked = 0;

    position.PushBack(gpu.Position.GetAsVec4(0.0f));
    velocity.PushBack(gpu.Velocity);
    size.PushBack(plFloat16(gpu.Size));
    color.PushBack(gpu.Color);
    lifeTime.PushBack(plVec2(gpu.Life / gpu.MaxLife, gpu.MaxLife));
    seeds.PushBack(rng.UInt());
  }

  plProcessingStream inputs[] = {
    plProcessingStream(plMakeHashedString("Position"), position.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float4, sizeof(plVec4)),
    plProcessingStream(plMakeHashedString("Velocity"), velocity.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float3, sizeof(plVec3)),
    plProcessingStream(plMakeHashedString("Size"), size.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Half, sizeof(plFloat16)),
    plProcessingStream(plMakeHashedString("Color"), color.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Half4, sizeof(plColorLinear16f)),
    plProcessingStream(plMakeHashedString("LifeTime"), lifeTime.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float2, sizeof(plVec2)),
    plProcessingStream(plMakeHashedString("RandomSeed"), seeds.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Int, sizeof(plUInt32)),
  };

  plExpressionVM vm;
  vm.RegisterFunction(plParticleProgramTables::s_CurveFunc);
  vm.RegisterFunction(plParticleProgramTables::s_GradientFunc);

  auto runVM = [&](const plParticleProgramData& program, plProcessingStream& ref_output) -> bool
  {
    plDynamicArray<plProcessingStream> scalarInputs, scalarOutputs;
    if (!SplitStreams(program.m_ByteCode.GetInputs(), inputs, scalarInputs) || !SplitStreams(program.m_ByteCode.GetOutputs(), plMakeArrayPtr(&ref_output, 1), scalarOutputs))
      return false;

    const plSharedPtr<plParticleProgramTables> pTables = program.m_Assets.GetTables();
    return vm.Execute(program.m_ByteCode, scalarInputs, scalarOutputs, uiCount, pTables->GetGlobalData(), plExpressionVM::Flags::BestPerformance).Succeeded();
  };

  plDynamicArray<plVec3> velocityVM;
  plDynamicArray<plVec4> colorVM;
  plDynamicArray<float> sizeVM, opacityVM;
  velocityVM.SetCount(uiCount);
  colorVM.SetCount(uiCount);
  sizeVM.SetCount(uiCount);
  opacityVM.SetCount(uiCount);

  plProcessingStream velocityOut(plMakeHashedString("Out0"), velocityVM.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float3, sizeof(plVec3));
  plProcessingStream colorOut(plMakeHashedString("Out0"), colorVM.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float4, sizeof(plVec4));
  plProcessingStream sizeOut(plMakeHashedString("Out0"), sizeVM.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float, sizeof(float));
  plProcessingStream opacityOut(plMakeHashedString("Out0"), opacityVM.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float, sizeof(float));

  if (!PL_TEST_BOOL(runVM(setVelocity.m_Settings.m_Program, velocityOut) && runVM(setColor.m_Settings.m_Program, colorOut) && runVM(setSize.m_Settings.m_Program, sizeOut) &&
                    runVM(setOpacity.m_Settings.m_Program, opacityOut)))
    return;

  plUInt32 uiVelocityMismatches = 0, uiColorMismatches = 0, uiSizeMismatches = 0, uiOpacityMismatches = 0;
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    plGPUParticle gpu = particles[i];
    plParticleGPUProgramSet::RunSinkPrograms(programs.GetWords(), data, gpu, seeds[i], 1.0f / 60.0f);

    // the trigonometry of the two VMs differs by a few ulps; the colour is stored as halves
    uiVelocityMismatches += Near(gpu.Velocity.x, velocityVM[i].x, 1e-3f) && Near(gpu.Velocity.y, velocityVM[i].y, 1e-3f) && Near(gpu.Velocity.z, velocityVM[i].z, 1e-3f) ? 0 : 1;
    uiColorMismatches += Near(gpu.Color.r, colorVM[i].x, 2e-3f) && Near(gpu.Color.g, colorVM[i].y, 2e-3f) && Near(gpu.Color.b, colorVM[i].z, 2e-3f) ? 0 : 1;
    uiSizeMismatches += Near(gpu.Size, sizeVM[i]) ? 0 : 1;
    uiOpacityMismatches += Near(gpu.Color.a, opacityVM[i], 2e-3f) ? 0 : 1;
  }

  PL_TEST_INT(uiVelocityMismatches, 0);
  PL_TEST_INT(uiColorMismatches, 0);
  PL_TEST_INT(uiSizeMismatches, 0);
  PL_TEST_INT(uiOpacityMismatches, 0);

  // the inline curve read at a known age: Normalized Age 0.25 on the ramp gives 0.1 + 1.9 x 0.25
  {
    const plSharedPtr<plParticleProgramTables> pTables = setSize.m_Settings.m_Program.m_Assets.GetTables();
    PL_TEST_FLOAT(pTables->SampleCurve(0, 0.25f), 0.25f, 1e-4f);
  }
}

//////////////////////////////////////////////////////////////////////////
// Formats and lowering

void plGameEngineTestParticleGraphNodes::CheckProgramDataVersions()
{
  // format 3 keeps the inline tables and the normalization flags
  plParticleProgramData program;
  {
    AST ast;
    Node* pLifeTime = Input(ast, "LifeTime", plProcessingStream::DataType::Float2);
    PL_TEST_BOOL(CompileInto(ast, Ops::SampleCurve(ast, Channel(ast, pLifeTime, 0), ast.CreateConstant(plVariant(1), AST::DataType::Int)), plProcessingStream::DataType::Float, "Number", program));
  }

  // an empty asset slot: it keeps its place and reads zero, and nothing is loaded for it
  program.m_Assets.m_Curves.PushBack("");
  program.m_Assets.m_CurveNormalized.PushBack(1);
  FillRamp(program.m_Assets.m_InlineCurves);
  program.m_Assets.m_InlineGradients.SetCount(plParticleProgramTables::s_uiTableSize, plVec4(0.25f, 0.5f, 0.75f, 1.0f));

  plParticleProgramData loaded;
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    program.Save(writer);

    plMemoryStreamReader reader(&storage);
    loaded.Load(reader);
  }

  PL_TEST_INT(loaded.m_Outputs.GetCount(), 1);
  PL_TEST_INT(loaded.m_Assets.m_Curves.GetCount(), 1);
  PL_TEST_BOOL(loaded.m_Assets.IsCurveNormalized(0));
  PL_TEST_INT(loaded.m_Assets.m_InlineCurves.GetCount(), plParticleProgramTables::s_uiTableSize);
  PL_TEST_INT(loaded.m_Assets.m_InlineGradients.GetCount(), plParticleProgramTables::s_uiTableSize);
  PL_TEST_INT(loaded.m_Assets.GetNumCurveSlots(), 2);
  PL_TEST_INT(loaded.m_Assets.GetNumGradientSlots(), 1);
  if (loaded.m_Assets.m_InlineCurves.GetCount() == plParticleProgramTables::s_uiTableSize)
    PL_TEST_FLOAT(loaded.m_Assets.m_InlineCurves[plParticleProgramTables::s_uiTableSize - 1], 1.0f, 0.0f);

  // format 2, as 1.9 wrote it: the asset lists without inline tables
  plParticleProgramData old;
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);

    const plUInt8 uiVersion = 2;
    writer << uiVersion;
    writer << (plUInt32)1;
    writer << plString("Number");
    program.m_ByteCode.Save(writer).AssertSuccess();

    plHybridArray<plString, 2> gradients;
    plHybridArray<plString, 2> curves;
    curves.PushBack("");
    writer.WriteArray(gradients).AssertSuccess();
    writer.WriteArray(curves).AssertSuccess();

    plMemoryStreamReader reader(&storage);
    old.Load(reader);
  }

  PL_TEST_INT(old.m_Outputs.GetCount(), 1);
  PL_TEST_INT(old.m_Assets.m_Curves.GetCount(), 1);
  PL_TEST_BOOL(!old.m_Assets.IsCurveNormalized(0));
  PL_TEST_BOOL(old.m_Assets.m_InlineCurves.IsEmpty() && old.m_Assets.m_InlineGradients.IsEmpty());

  // a flat curve reads 1 when normalized, the way Size Curve reads it, and its value when raw
  {
    plCurve1D flat;
    flat.AddControlPoint(0.0).m_Position.y = 3.0;
    flat.AddControlPoint(1.0).m_Position.y = 3.0;
    flat.SortControlPoints();
    flat.CreateLinearApproximation();

    PL_TEST_FLOAT(plParticleProgramTables::SampleCurveAsset(flat, 0.3f, true), 1.0f, 0.0f);
    PL_TEST_FLOAT(plParticleProgramTables::SampleCurveAsset(flat, 0.3f, false), 3.0f, 1e-5f);
  }
}

void plGameEngineTestParticleGraphNodes::CheckInitializeCustomOnGPU()
{
  // an Initialize Set Attribute into a custom attribute keeps a GPU system on the CPU, and says so
  plParticleEffectDescriptor desc;
  plParticleSystemDescriptor* pSystem = AddTestSystem(desc, 4, 1.0f);
  pSystem->m_SimulationTarget = plParticleSimulationTarget::GPU;

  plParticleInitializerFactory_SetAttribute* pSet = NewModule<plParticleInitializerFactory_SetAttribute>();
  pSet->m_Settings.m_Target = plParticleSinkTarget::Custom;
  pSet->m_Settings.m_sCustomAttribute = "Heat";
  pSystem->AddInitializerFactory(pSet);
  pSystem->AddTypeFactory(NewModule<plParticleTypeQuadFactory>());

  plParticleGPULowering::BlockerKind kind = plParticleGPULowering::BlockerKind::None;
  plUInt32 uiIndex = plInvalidIndex;
  const plString sReason = plParticleGPULowering::FindLoweringBlocker(pSystem, &kind, &uiIndex);

  PL_TEST_BOOL_MSG(sReason.FindSubString("custom attribute") != nullptr, "%s", sReason.GetData());
  PL_TEST_BOOL(kind == plParticleGPULowering::BlockerKind::Initializer);
  PL_TEST_INT(uiIndex, 0);

  // Opacity is a Color record that writes A, so it lowers
  plParticleBehaviorFactory_Attribute setOpacity;
  setOpacity.m_Settings.m_Target = plParticleSinkTarget::Opacity;
  setOpacity.m_Settings.m_fNumber = 0.25f;

  const plParticleBehaviorFactory* factories[] = {&setOpacity};
  plParticleGPUProgramSet programs;
  const plString sOpacityReason = programs.Build(factories);
  if (!PL_TEST_BOOL_MSG(sOpacityReason.IsEmpty(), "%s", sOpacityReason.GetData()))
    return;

  plDynamicArray<float> data;
  data.SetCount(programs.GetDataCount());
  programs.FillData(nullptr, plTime::MakeZero(), data);

  plGPUParticle gpu = {};
  gpu.Color = plColorLinear16f(plColor(0.2f, 0.4f, 0.6f, 1.0f));
  gpu.Flags = 1;
  plParticleGPUProgramSet::RunSinkPrograms(programs.GetWords(), data, gpu, 0, 1.0f / 60.0f);

  PL_TEST_FLOAT(gpu.Color.r, 0.2f, 1e-3f);
  PL_TEST_FLOAT(gpu.Color.g, 0.4f, 1e-3f);
  PL_TEST_FLOAT(gpu.Color.b, 0.6f, 1e-3f);
  PL_TEST_FLOAT(gpu.Color.a, 0.25f, 1e-3f);
}

//////////////////////////////////////////////////////////////////////////
// Running effects

plTestAppRun plGameEngineTestParticleGraphNodes::RunEffectCheck(plInt32 iIdentifier)
{
  plParticleEffectInstance* pEffect = GetEffect();

  // seen every frame, so the update runs at the visible rate
  if (pEffect != nullptr)
    pEffect->SetIsVisible();

  if (m_iFrame < 5)
    return plTestAppRun::Continue;

  if (!PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
    return plTestAppRun::Quit;

  const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
  const plUInt64 uiParticles = pSystem->GetNumActiveParticles();

  if (iIdentifier == SubTests::OpacityTarget)
  {
    const plProcessingStream* pColor = pSystem->QueryStream("Color", plProcessingStream::DataType::Half4);
    if (!PL_TEST_BOOL(pColor != nullptr && uiParticles == 8))
      return plTestAppRun::Quit;

    plUInt32 uiMismatches = 0;
    for (plUInt64 i = 0; i < uiParticles; ++i)
    {
      const plColor c = pColor->GetData<plColorLinear16f>()[i].ToLinearFloat();
      uiMismatches += (Near(c.r, 0.2f, 2e-3f) && Near(c.g, 0.4f, 2e-3f) && Near(c.b, 0.6f, 2e-3f) && Near(c.a, 0.25f, 2e-3f)) ? 0 : 1;
    }

    PL_TEST_INT(uiMismatches, 0);
    return plTestAppRun::Quit;
  }

  const plProcessingStream* pSize = pSystem->QueryStream("Size", plProcessingStream::DataType::Half);
  const plProcessingStream* pLifeTime = pSystem->QueryStream("LifeTime", plProcessingStream::DataType::Float2);
  if (!PL_TEST_BOOL(pSize != nullptr && pLifeTime != nullptr && uiParticles > 0))
    return plTestAppRun::Quit;

  const plFloat16* pSizes = pSize->GetData<plFloat16>();
  const plVec2* pLifeTimes = pLifeTime->GetData<plVec2>();

  if (iIdentifier == SubTests::CurveOverLifeInline)
  {
    // 0.5 + 1.5 x Normalized Age; the size is from the start of the last step, one step younger than the stream says
    plUInt32 uiMismatches = 0;
    for (plUInt64 i = 0; i < uiParticles; ++i)
    {
      const float fAge = 1.0f - pLifeTimes[i].x * pLifeTimes[i].y;
      uiMismatches += Near((float)pSizes[i], 0.5f + 1.5f * fAge, 0.05f) ? 0 : 1;
    }

    PL_TEST_INT(uiMismatches, 0);
    PL_TEST_BOOL_MSG((float)pSizes[0] > 0.5f, "The curve was not applied");
    return plTestAppRun::Quit;
  }

  if (iIdentifier == SubTests::SizeCurveFlat)
  {
    // Base Size 0 + Curve Scale 0.5 x a flat curve read as 1
    plUInt32 uiMismatches = 0;
    for (plUInt64 i = 0; i < uiParticles; ++i)
      uiMismatches += Near((float)pSizes[i], 0.5f, 2e-3f) ? 0 : 1;

    PL_TEST_INT(uiMismatches, 0);
    return plTestAppRun::Quit;
  }

  if (iIdentifier == SubTests::SizeCurveEveryParticle)
  {
    // one burst of equal particles: after a visible step every one of them has the same size, and it follows the curve
    float fMin = plMath::MaxValue<float>();
    float fMax = -plMath::MaxValue<float>();
    for (plUInt64 i = 0; i < uiParticles; ++i)
    {
      fMin = plMath::Min(fMin, (float)pSizes[i]);
      fMax = plMath::Max(fMax, (float)pSizes[i]);
    }

    PL_TEST_INT(uiParticles, 200);
    PL_TEST_BOOL_MSG(fMax - fMin < 1e-3f, "Sizes range from %.4f to %.4f: not every particle was updated in the last step", fMin, fMax);
    PL_TEST_BOOL_MSG(fMin > 1.0f, "The curve was not applied (size %.4f)", fMin);
    return plTestAppRun::Quit;
  }

  return plTestAppRun::Quit;
}
