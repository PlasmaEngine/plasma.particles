#include <ParticlesTest/ParticlesTestPCH.h>

#include <Core/Curves/ColorGradientResource.h>
#include <Core/Utils/Blackboard.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Math/Color16f.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_OnEvent.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_VelocityCone.h>
#include <ParticlePlugin/System/ParticleFrameProgram.h>
#include <Core/Curves/Curve1DResource.h>
#include <Foundation/CodeUtils/Expression/ExpressionAST.h>
#include <Foundation/CodeUtils/Expression/ExpressionCompiler.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ConditionalKill.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Expression.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Gravity.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_If.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_KillIf.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Remap.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Switch.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SetAttribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SetAttribute.h>
#include <ParticlePlugin/Components/ParticleComponent.h>
#include <ParticlePlugin/Effect/ParticleEffectDescriptor.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_Burst.h>
#include <ParticlePlugin/Resources/ParticleEffectResource.h>
#include <ParticlePlugin/System/ParticleProgram.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Drag.h>
#include <ParticlePlugin/Type/GPU/ParticleGPULowering.h>
#include <ParticlePlugin/Type/GPU/ParticleGPUProgram.h>
#include <ParticlePlugin/Type/GPU/ParticleTypeGPU.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>
#include <ParticlesTest/ParticleRuntimeTests.h>
#include <Foundation/SimdMath/SimdRandom.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/ReadbackHelper.h>

static plGameEngineTestParticleRuntime s_GameEngineTestParticleRuntime;

namespace
{
  template <typename T>
  T* AllocateModule()
  {
    return plGetStaticRTTI<T>()->GetAllocator()->template Allocate<T>();
  }

  /// One system that bursts the given number of particles once, each living the given time.
  plParticleSystemDescriptor* AddBurstSystem(plParticleEffectDescriptor& ref_effect, plUInt32 uiCount, float fLifeSeconds)
  {
    plParticleSystemDescriptor* pSystem = AllocateModule<plParticleSystemDescriptor>();
    pSystem->m_LifeTime.m_Value = plTime::MakeFromSeconds(fLifeSeconds);
    pSystem->m_LifeTime.m_fVariance = 0.0f;

    plParticleEmitterFactory_Burst* pBurst = AllocateModule<plParticleEmitterFactory_Burst>();
    pBurst->m_uiSpawnCountMin = uiCount;
    pBurst->m_uiSpawnCountRange = 0;

    // the emitter list has no adder; it is filled through reflection like the asset transform does
    auto pEmitters = static_cast<const plAbstractArrayProperty*>(plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
    plParticleEmitterFactory* pEmitter = pBurst;
    pEmitters->Insert(pSystem, 0, &pEmitter);

    ref_effect.AddParticleSystem(pSystem);
    return pSystem;
  }

  using AST = plExpressionAST;

  AST::Node* ProgramInput(AST& ref_ast, const char* szName, plProcessingStream::DataType type)
  {
    plHashedString sName;
    sName.Assign(szName);
    AST::Input* pInput = ref_ast.CreateInput({sName, type});
    ref_ast.m_InputNodes.PushBack(pInput);
    return pInput;
  }

  AST::Node* Number(AST& ref_ast, float f)
  {
    return ref_ast.CreateConstant(plVariant(f), AST::DataType::Float);
  }

  /// Random(RandomSeed, seed), as the editor builds it for a Random node
  AST::Node* ParticleRandom(AST& ref_ast, plInt32 iSeed)
  {
    AST::Node* arguments[] = {ProgramInput(ref_ast, "RandomSeed", plProcessingStream::DataType::Int), ref_ast.CreateConstant(plVariant(iSeed), AST::DataType::Int)};
    return ref_ast.CreateFunctionCall(plDefaultExpressionFunctions::s_RandomFunc.m_Desc, arguments);
  }

  /// Color.x of a Color input, Position.z of a Position input, ...
  AST::Node* Component(AST& ref_ast, AST::Node* pVector, AST::VectorComponent::Enum component)
  {
    return ref_ast.CreateSwizzle(component, pVector);
  }

  /// Select(bCondition, 1, 0) as a float, how the editor feeds a condition into a number
  AST::Node* ConditionAsNumber(AST& ref_ast, AST::Node* pCondition)
  {
    return ref_ast.CreateTernaryOperator(AST::NodeType::Select, pCondition, Number(ref_ast, 1.0f), Number(ref_ast, 0.0f));
  }

  /// Compiles one output computing sProperty into a block's program, the way the editor does.
  bool CompileProgram(AST& ref_ast, AST::Node* pValue, plProcessingStream::DataType outputType, const char* szProperty, plParticleProgramData& out_program)
  {
    ref_ast.m_OutputNodes.PushBack(ref_ast.CreateOutput({plMakeHashedString("Out0"), outputType}, pValue));
    out_program.m_Outputs.PushBack(szProperty);

    plExpressionCompiler compiler;
    return compiler.Compile(ref_ast, out_program.m_ByteCode).Succeeded();
  }

  /// One view per (split) stream the byte code names, into the whole streams given, as the runner builds them.
  bool ScalarViews(plArrayPtr<const plExpression::StreamDesc> descs, plArrayPtr<plProcessingStream> streams, plDynamicArray<plProcessingStream>& out_views)
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
} // namespace

const char* plGameEngineTestParticleRuntime::GetTestName() const
{
  return "Particle Runtime";
}

plGameEngineTestApplication* plGameEngineTestParticleRuntime::CreateApplication()
{
  m_pOwnApplication = PL_DEFAULT_NEW(plGameEngineTestApplication_Particles);
  return m_pOwnApplication;
}

void plGameEngineTestParticleRuntime::SetupSubTests()
{
  AddSubTest("SharedEffectDefaults", SubTests::SharedEffectDefaults);
  AddSubTest("BehaviorOrder", SubTests::BehaviorOrder);
  AddSubTest("FixedTick", SubTests::FixedTick);
  AddSubTest("GPUEffectFinishes", SubTests::GPUEffectFinishes);
  AddSubTest("FrameProgramParameter", SubTests::FrameProgramParameter);
  AddSubTest("ParticleProgramKill", SubTests::ParticleProgramKill);
  AddSubTest("ParticleProgramCustomAttribute", SubTests::ParticleProgramCustomAttribute);
  AddSubTest("ParticleProgramCost", SubTests::ParticleProgramCost);
  AddSubTest("ParticleProgramSampleAssets", SubTests::ParticleProgramSampleAssets);
  AddSubTest("ConvertedLogicBlocks", SubTests::ConvertedLogicBlocks);
  AddSubTest("PerParticleProperty", SubTests::PerParticleProperty);
  AddSubTest("PreviewResimulation", SubTests::PreviewResimulation);
  AddSubTest("GPUProgramReference", SubTests::GPUProgramReference);
  AddSubTest("GPUProgramLowering", SubTests::GPUProgramLowering);
  AddSubTest("GPUProgramOnGPU", SubTests::GPUProgramOnGPU);
  AddSubTest("GPUSortAliveCount", SubTests::GPUSortAliveCount);
  AddSubTest("GPUSpyReadback", SubTests::GPUSpyReadback);
  AddSubTest("EventSpawnPerEvent", SubTests::EventSpawnPerEvent);
  AddSubTest("EventInherit", SubTests::EventInherit);
  AddSubTest("EventQueueDrops", SubTests::EventQueueDrops);
  AddSubTest("EventOncePerFrame", SubTests::EventOncePerFrame);
  AddSubTest("GPUEvents", SubTests::GPUEvents);
  AddSubTest("BlackboardEntry", SubTests::BlackboardEntry);
  AddSubTest("VelocityConeAxis", SubTests::VelocityConeAxis);
  AddSubTest("LodUpdateInterval", SubTests::LodUpdateInterval);
  AddSubTest("SizePerAxis", SubTests::SizePerAxis);
  AddSubTest("NewSinkTargets", SubTests::NewSinkTargets);
  AddSubTest("LookSettings", SubTests::LookSettings);
  AddSubTest("GPULoweringNewFeatures", SubTests::GPULoweringNewFeatures);
  AddSubTest("DistortionPipeline", SubTests::DistortionPipeline);
  AddSubTest("BenchMassDeaths", SubTests::BenchMassDeaths);
  AddSubTest("BenchRaycast", SubTests::BenchRaycast);
  AddSubTest("BenchFarLod", SubTests::BenchFarLod);
  AddSubTest("BenchMutex", SubTests::BenchMutex);
  AddSubTest("BenchTestingChambers", SubTests::BenchTestingChambers);
}

plResult plGameEngineTestParticleRuntime::InitializeSubTest(plInt32 iIdentifier)
{
  PL_SUCCEED_OR_RETURN(SUPER::InitializeSubTest(iIdentifier));

  m_iFrame = -1;
  m_pOwnApplication->SetupSceneSubTest("Particles/AssetCache/Common/Particles1.plBinScene");

  plParticleEffectDescriptor desc;

  switch (iIdentifier)
  {
    case SubTests::SharedEffectDefaults:
    {
      // a shared effect takes no per-instance parameters, but must still see the effect's defaults
      AddBurstSystem(desc, 4, 10.0f);
      desc.m_FloatParameters["TestFloat"] = 0.25f;
      desc.m_ColorParameters["TestColor"] = plColor::Red;
      StartEffect(desc, "ParticleRuntimeShared");
      break;
    }

    case SubTests::BehaviorOrder:
    {
      // more behaviors than the engine sort handles by insertion; all share one priority, so only
      // a stable sort keeps the authored order and leaves the last block's value in the stream
      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 4, 10.0f);

      for (plUInt32 i = 0; i < 24; ++i)
      {
        plParticleBehaviorFactory_SetAttribute* pSet = AllocateModule<plParticleBehaviorFactory_SetAttribute>();
        pSet->m_Attribute = plParticleAttribute::Size;
        pSet->m_Mode = plParticleSetAttributeMode::Replace;
        pSet->m_fValue = (float)(i + 1);
        pSystem->AddBehaviorFactory(pSet);
      }

      StartEffect(desc, {});
      break;
    }

    case SubTests::FixedTick:
    {
      desc.m_fFixedTickHz = 10.0f;
      AddBurstSystem(desc, 4, 10.0f);
      StartEffect(desc, {});
      break;
    }

    case SubTests::GPUEffectFinishes:
    {
      // a lowered system must report itself finished once its particles have run out of life
      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 16, 0.5f);
      pSystem->m_SimulationTarget = plParticleSimulationTarget::GPU;
      pSystem->AddBehaviorFactory(AllocateModule<plParticleBehaviorFactory_Gravity>());
      pSystem->AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());
      StartEffect(desc, {});
      break;
    }

    case SubTests::FrameProgramParameter:
    {
      // Set Attribute Size = Param.Size x 2, evaluated per frame: the effect parameter set by the
      // component after the start must reach the particles
      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 4, 10.0f);
      desc.m_FloatParameters["Size"] = 1.5f;

      plParticleBehaviorFactory_SetAttribute* pSet = AllocateModule<plParticleBehaviorFactory_SetAttribute>();
      pSet->m_Attribute = plParticleAttribute::Size;
      pSet->m_Mode = plParticleSetAttributeMode::Replace;
      pSet->m_fValue = 1.0f;
      pSystem->AddBehaviorFactory(pSet);

      plExpressionAST ast;
      plExpressionAST::Node* pInput = ast.CreateInput({plMakeHashedString("Param.Size"), plProcessingStream::DataType::Float});
      ast.m_InputNodes.PushBack(static_cast<plExpressionAST::Input*>(pInput));
      plExpressionAST::Node* pDoubled = ast.CreateBinaryOperator(plExpressionAST::NodeType::Multiply, pInput, ast.CreateConstant(2.0f));
      ast.m_OutputNodes.PushBack(ast.CreateOutput({plMakeHashedString("Out0"), plProcessingStream::DataType::Float}, pDoubled));

      plExpressionCompiler compiler;
      PL_TEST_BOOL(compiler.Compile(ast, pSystem->m_FrameProgram.m_ByteCode).Succeeded());

      plParticleDrivenProperty& driven = pSystem->m_FrameProgram.m_Driven.ExpandAndGetRef();
      driven.m_Module = plParticleDrivenProperty::Module::Behavior;
      driven.m_uiModuleIndex = 0;
      driven.m_uiOutput = 0;
      driven.m_sProperty = "Value";

      StartEffect(desc, {});
      break;
    }

    case SubTests::ParticleProgramKill:
    {
      // Initialize: Position = (0, 0, Random - 0.5), so about half start below the floor.
      // Update: Kill If Position.z < 0 removes exactly those.
      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 200, 10.0f);

      {
        plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
        pSet->m_Settings.m_Target = plParticleSinkTarget::Position;

        AST ast;
        AST::Node* pZ = ast.CreateBinaryOperator(AST::NodeType::Subtract, ParticleRandom(ast, 3), Number(ast, 0.5f));
        AST::Node* components[] = {Number(ast, 0.0f), Number(ast, 0.0f), pZ};
        PL_TEST_BOOL(CompileProgram(ast, ast.CreateConstructorCall(AST::DataType::Float3, components), plProcessingStream::DataType::Float3, "Vector", pSet->m_Settings.m_Program));
        pSystem->AddInitializerFactory(pSet);
      }

      {
        plParticleBehaviorFactory_KillIf* pKill = AllocateModule<plParticleBehaviorFactory_KillIf>();

        AST ast;
        AST::Node* pPosition = ProgramInput(ast, "Position", plProcessingStream::DataType::Float4);
        AST::Node* pBelow = ast.CreateBinaryOperator(AST::NodeType::Less, ast.CreateSwizzle(AST::VectorComponent::Z, pPosition), Number(ast, 0.0f));
        AST::Node* pCondition = ast.CreateTernaryOperator(AST::NodeType::Select, pBelow, Number(ast, 1.0f), Number(ast, 0.0f));
        PL_TEST_BOOL(CompileProgram(ast, pCondition, plProcessingStream::DataType::Float, "Condition", pKill->m_Program));
        pSystem->AddBehaviorFactory(pKill);
      }

      StartEffect(desc, {});
      break;
    }

    case SubTests::ParticleProgramCustomAttribute:
    {
      // Initialize: custom IsHot = Random > 0.5 ? 1 : 0. Update: Size = IsHot x 2 + 1, so every size is 1 or 3.
      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 200, 10.0f);

      {
        plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
        pSet->m_Settings.m_Target = plParticleSinkTarget::Custom;
        pSet->m_Settings.m_sCustomAttribute = "IsHot";
        pSet->m_Settings.m_CustomType = plParticleCustomAttributeType::Float;

        AST ast;
        AST::Node* pHot = ast.CreateBinaryOperator(AST::NodeType::Greater, ParticleRandom(ast, 7), Number(ast, 0.5f));
        AST::Node* pValue = ast.CreateTernaryOperator(AST::NodeType::Select, pHot, Number(ast, 1.0f), Number(ast, 0.0f));
        PL_TEST_BOOL(CompileProgram(ast, pValue, plProcessingStream::DataType::Float, "Number", pSet->m_Settings.m_Program));
        pSystem->AddInitializerFactory(pSet);
      }

      {
        plParticleBehaviorFactory_Attribute* pSet = AllocateModule<plParticleBehaviorFactory_Attribute>();
        pSet->m_Settings.m_Target = plParticleSinkTarget::Size;

        AST ast;
        AST::Node* pHot = ProgramInput(ast, "Custom.IsHot", plProcessingStream::DataType::Float);
        AST::Node* pValue = ast.CreateBinaryOperator(AST::NodeType::Add, ast.CreateBinaryOperator(AST::NodeType::Multiply, pHot, Number(ast, 2.0f)), Number(ast, 1.0f));
        PL_TEST_BOOL(CompileProgram(ast, pValue, plProcessingStream::DataType::Float, "Number", pSet->m_Settings.m_Program));
        pSystem->AddBehaviorFactory(pSet);
      }

      StartEffect(desc, {});
      break;
    }

    case SubTests::ParticleProgramCost:
    case SubTests::GPUProgramReference:
    case SubTests::GPUProgramLowering:
      // checked directly in RunSubTest, no effect needed
      break;

    case SubTests::GPUProgramOnGPU:
      SetupGPUProgramOnGPU(desc);
      StartEffect(desc, {});
      break;

    case SubTests::EventSpawnPerEvent:
    case SubTests::EventInherit:
    case SubTests::EventQueueDrops:
    case SubTests::EventOncePerFrame:
    case SubTests::GPUEvents:
    case SubTests::BlackboardEntry:
    case SubTests::VelocityConeAxis:
      SetupEventTest(iIdentifier, desc);
      StartEffect(desc, {});
      break;

    case SubTests::LodUpdateInterval:
      // the near and far effects of the Colour Gradient / Fade Out measurement
      SetupBenchmark(SubTests::BenchFarLod, desc);
      break;

    case SubTests::SizePerAxis:
    case SubTests::NewSinkTargets:
      SetupArtistRequestTest(iIdentifier, desc);
      StartEffect(desc, {});
      break;

    case SubTests::LookSettings:
    case SubTests::GPULoweringNewFeatures:
    case SubTests::DistortionPipeline:
      // checked directly in RunSubTest, no effect needed
      break;

    case SubTests::BenchMassDeaths:
    case SubTests::BenchRaycast:
    case SubTests::BenchFarLod:
    case SubTests::BenchMutex:
    case SubTests::BenchTestingChambers:
      SetupBenchmark(iIdentifier, desc);
      break;

    case SubTests::GPUSpyReadback:
    {
      // 50 GPU particles of random size; the spy reads Size x 2 back from the kernel
      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 50, 10.0f);
      pSystem->m_SimulationTarget = plParticleSimulationTarget::GPU;

      plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
      pSet->m_Settings.m_Target = plParticleSinkTarget::Size;
      {
        AST ast;
        PL_TEST_BOOL(CompileProgram(ast, ast.CreateBinaryOperator(AST::NodeType::Add, ParticleRandom(ast, 9), Number(ast, 0.25f)), plProcessingStream::DataType::Float, "Number", pSet->m_Settings.m_Program));
      }
      pSystem->AddInitializerFactory(pSet);
      pSystem->AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());

      StartEffect(desc, {});
      break;
    }

    case SubTests::GPUSortAliveCount:
    {
      // 1000 blended GPU particles spread along X; a GPU Kill If (X > 2) leaves about 100 of them in the
      // 1024 sort slots, so the sort covers far fewer than the pool
      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 1000, 10.0f);
      pSystem->m_SimulationTarget = plParticleSimulationTarget::GPU;

      plParticleBehaviorFactory_KillIf* pKill = AllocateModule<plParticleBehaviorFactory_KillIf>();
      {
        AST ast;
        AST::Node* pPosition = ProgramInput(ast, "Position", plProcessingStream::DataType::Float3);
        PL_TEST_BOOL(CompileProgram(ast, ConditionAsNumber(ast, ast.CreateBinaryOperator(AST::NodeType::Greater, Component(ast, pPosition, AST::VectorComponent::X), Number(ast, 2.0f))), plProcessingStream::DataType::Float, "Condition", pKill->m_Program));
      }
      pSystem->AddBehaviorFactory(pKill);

      plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
      pSet->m_Settings.m_Target = plParticleSinkTarget::Position;
      {
        AST ast;
        AST::Node* components[] = {ast.CreateBinaryOperator(AST::NodeType::Multiply, ParticleRandom(ast, 1), Number(ast, 20.0f)), ParticleRandom(ast, 2), ParticleRandom(ast, 3)};
        PL_TEST_BOOL(CompileProgram(ast, ast.CreateConstructorCall(AST::DataType::Float3, components), plProcessingStream::DataType::Float3, "Vector", pSet->m_Settings.m_Program));
      }
      pSystem->AddInitializerFactory(pSet);

      plParticleTypeQuadFactory* pQuad = AllocateModule<plParticleTypeQuadFactory>();
      pQuad->m_RenderMode = plParticleTypeRenderMode::Blended;
      pSystem->AddTypeFactory(pQuad);

      StartEffect(desc, {});
      break;
    }

    case SubTests::ParticleProgramSampleAssets:
    {
      // Initialize: Color = Sample Gradient (black to white) and Size = Sample Curve (0 to 2), both at
      // the same Random, so every particle is grey and its size grows with its brightness
      {
        plColorGradientResourceDescriptor gradient;
        gradient.m_Gradient.AddColorControlPoint(0.0, plColorGammaUB(0, 0, 0));
        gradient.m_Gradient.AddColorControlPoint(1.0, plColorGammaUB(255, 255, 255));
        gradient.m_Gradient.SortControlPoints();
        plResourceManager::GetOrCreateResource<plColorGradientResource>("PfxTestGradient", std::move(gradient));

        plCurve1DResourceDescriptor curves;
        plCurve1D& curve = curves.m_Curves.ExpandAndGetRef();
        curve.AddControlPoint(0.0).m_Position.Set(0.0, 0.0);
        curve.AddControlPoint(1.0).m_Position.Set(1.0, 2.0);
        curve.SortControlPoints();
        curve.CreateLinearApproximation();
        plResourceManager::GetOrCreateResource<plCurve1DResource>("PfxTestCurve", std::move(curves));
      }

      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 200, 10.0f);

      {
        plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
        pSet->m_Settings.m_Target = plParticleSinkTarget::Color;

        AST ast;
        AST::Node* pT = ParticleRandom(ast, 5);
        AST::Node* components[4];
        for (plInt32 c = 0; c < 4; ++c)
        {
          AST::Node* arguments[] = {pT, ast.CreateConstant(plVariant(0), AST::DataType::Int), ast.CreateConstant(plVariant(c), AST::DataType::Int)};
          components[c] = ast.CreateFunctionCall(plParticleProgramTables::s_GradientFunc.m_Desc, arguments);
        }

        PL_TEST_BOOL(CompileProgram(ast, ast.CreateConstructorCall(AST::DataType::Float4, components), plProcessingStream::DataType::Float4, "Color", pSet->m_Settings.m_Program));
        pSet->m_Settings.m_Program.m_Assets.m_Gradients.PushBack("PfxTestGradient");
        pSystem->AddInitializerFactory(pSet);
      }

      {
        plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
        pSet->m_Settings.m_Target = plParticleSinkTarget::Size;

        AST ast;
        AST::Node* arguments[] = {ParticleRandom(ast, 5), ast.CreateConstant(plVariant(0), AST::DataType::Int)};
        PL_TEST_BOOL(CompileProgram(ast, ast.CreateFunctionCall(plParticleProgramTables::s_CurveFunc.m_Desc, arguments), plProcessingStream::DataType::Float, "Number", pSet->m_Settings.m_Program));
        pSet->m_Settings.m_Program.m_Assets.m_Curves.PushBack("PfxTestCurve");
        pSystem->AddInitializerFactory(pSet);
      }

      StartEffect(desc, {});
      break;
    }

    case SubTests::ConvertedLogicBlocks:
      SetupConvertedLogicBlocks(desc);
      StartEffect(desc, {});
      break;

    case SubTests::PreviewResimulation:
    {
      // random velocity and size at spawn, drag per particle from Random, a continuous emitter: what the
      // editor's timeline restarts and re-simulates with the preview's seed
      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 100, 10.0f);

      {
        plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
        pSet->m_Settings.m_Target = plParticleSinkTarget::Velocity;

        AST ast;
        AST::Node* components[] = {ast.CreateBinaryOperator(AST::NodeType::Subtract, ParticleRandom(ast, 1), Number(ast, 0.5f)),
          ast.CreateBinaryOperator(AST::NodeType::Subtract, ParticleRandom(ast, 2), Number(ast, 0.5f)), ast.CreateBinaryOperator(AST::NodeType::Multiply, ParticleRandom(ast, 3), Number(ast, 6.0f))};
        PL_TEST_BOOL(CompileProgram(ast, ast.CreateConstructorCall(AST::DataType::Float3, components), plProcessingStream::DataType::Float3, "Vector", pSet->m_Settings.m_Program));
        pSystem->AddInitializerFactory(pSet);
      }

      {
        plParticleBehaviorFactory_Gravity* pGravity = AllocateModule<plParticleBehaviorFactory_Gravity>();

        AST ast;
        PL_TEST_BOOL(CompileProgram(ast, ParticleRandom(ast, 4), plProcessingStream::DataType::Float, "GravityFactor", pGravity->m_Program));
        pSystem->AddBehaviorFactory(pGravity);
      }

      StartEffect(desc, {});
      break;
    }

    case SubTests::PerParticleProperty:
    {
      // Initialize: Size = Random. Update: Gravity with Factor wired per particle to Size, so after
      // any number of steps every particle's falling speed divided by its size is the same.
      plParticleSystemDescriptor* pSystem = AddBurstSystem(desc, 200, 10.0f);

      {
        plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
        pSet->m_Settings.m_Target = plParticleSinkTarget::Size;

        AST ast;
        AST::Node* pValue = ast.CreateBinaryOperator(AST::NodeType::Add, ast.CreateBinaryOperator(AST::NodeType::Multiply, ParticleRandom(ast, 1), Number(ast, 2.0f)), Number(ast, 0.5f));
        PL_TEST_BOOL(CompileProgram(ast, pValue, plProcessingStream::DataType::Float, "Number", pSet->m_Settings.m_Program));
        pSystem->AddInitializerFactory(pSet);
      }

      {
        plParticleBehaviorFactory_Gravity* pGravity = AllocateModule<plParticleBehaviorFactory_Gravity>();

        AST ast;
        PL_TEST_BOOL(CompileProgram(ast, ProgramInput(ast, "Size", plProcessingStream::DataType::Half), plProcessingStream::DataType::Float, "GravityFactor", pGravity->m_Program));
        pSystem->AddBehaviorFactory(pGravity);
      }

      StartEffect(desc, {});
      break;
    }
  }

  return PL_SUCCESS;
}

void plGameEngineTestParticleRuntime::SetupConvertedLogicBlocks(plParticleEffectDescriptor& ref_desc)
{
  // Each 1.3 logic block runs next to the program its conversion compiles to, the converted one
  // first and writing a custom attribute, so both see the same particles. The old block writes the
  // real attribute; after a few steps the two must hold the same values for every particle.
  plParticleSystemDescriptor* pSystem = AddBurstSystem(ref_desc, 200, 10.0f);

  using DT = plProcessingStream::DataType;

  // Initialize: random size, velocity, colour and position; then Position.z = 0.25 by component
  auto addInitialize = [&](plParticleSinkTarget::Enum target, plParticleSinkComponent::Enum component, AST& ref_ast, AST::Node* pValue, DT type, const char* szProperty)
  {
    plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
    pSet->m_Settings.m_Target = target;
    pSet->m_Settings.m_Component = component;
    PL_TEST_BOOL(CompileProgram(ref_ast, pValue, type, szProperty, pSet->m_Settings.m_Program));
    pSystem->AddInitializerFactory(pSet);
  };

  auto spread = [](AST& ref_ast, plInt32 iSeed, float fScale, float fOffset)
  { return ref_ast.CreateBinaryOperator(AST::NodeType::Add, ref_ast.CreateBinaryOperator(AST::NodeType::Multiply, ParticleRandom(ref_ast, iSeed), Number(ref_ast, fScale)), Number(ref_ast, fOffset)); };

  {
    AST ast;
    addInitialize(plParticleSinkTarget::Size, plParticleSinkComponent::All, ast, ParticleRandom(ast, 1), DT::Float, "Number");
  }
  {
    AST ast;
    AST::Node* components[] = {spread(ast, 2, 4.0f, -2.0f), spread(ast, 3, 4.0f, -2.0f), spread(ast, 4, 4.0f, -2.0f)};
    addInitialize(plParticleSinkTarget::Velocity, plParticleSinkComponent::All, ast, ast.CreateConstructorCall(AST::DataType::Float3, components), DT::Float3, "Vector");
  }
  {
    AST ast;
    AST::Node* components[] = {ParticleRandom(ast, 5), ParticleRandom(ast, 6), ParticleRandom(ast, 7), ParticleRandom(ast, 8)};
    addInitialize(plParticleSinkTarget::Color, plParticleSinkComponent::All, ast, ast.CreateConstructorCall(AST::DataType::Float4, components), DT::Float4, "Color");
  }
  {
    AST ast;
    AST::Node* components[] = {spread(ast, 9, 2.0f, -1.0f), spread(ast, 10, 2.0f, -1.0f), spread(ast, 11, 2.0f, -1.0f)};
    addInitialize(plParticleSinkTarget::Position, plParticleSinkComponent::All, ast, ast.CreateConstructorCall(AST::DataType::Float3, components), DT::Float3, "Vector");
  }
  {
    // one component only: x and y keep their random values
    plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
    pSet->m_Settings.m_Target = plParticleSinkTarget::Position;
    pSet->m_Settings.m_Component = plParticleSinkComponent::Z;
    pSet->m_Settings.m_fNumber = 0.25f;
    pSystem->AddInitializerFactory(pSet);
  }

  // Update: converted program into a custom attribute, then the old block
  auto addConverted = [&](const char* szName, plParticleCustomAttributeType::Enum type, AST& ref_ast, AST::Node* pValue)
  {
    plParticleBehaviorFactory_Attribute* pSet = AllocateModule<plParticleBehaviorFactory_Attribute>();
    pSet->m_Settings.m_Target = plParticleSinkTarget::Custom;
    pSet->m_Settings.m_sCustomAttribute = szName;
    pSet->m_Settings.m_CustomType = type;

    const bool bVector = type == plParticleCustomAttributeType::Vector;
    PL_TEST_BOOL(CompileProgram(ref_ast, pValue, bVector ? DT::Float3 : DT::Float, bVector ? "Vector" : "Number", pSet->m_Settings.m_Program));
    pSystem->AddBehaviorFactory(pSet);
  };

  auto select = [](AST& ref_ast, AST::Node* pCondition, AST::Node* pTrue, AST::Node* pFalse)
  { return ref_ast.CreateTernaryOperator(AST::NodeType::Select, pCondition, pTrue, pFalse); };

  auto compare = [](AST& ref_ast, AST::NodeType::Enum op, AST::Node* a, float b)
  { return ref_ast.CreateBinaryOperator(op, a, Number(ref_ast, b)); };

  // If: Size > 0.5 ? 1 : 0.2 into Color A
  {
    AST ast;
    AST::Node* pSize = ProgramInput(ast, "Size", DT::Half);
    addConverted("If", plParticleCustomAttributeType::Float, ast, select(ast, compare(ast, AST::NodeType::Greater, pSize, 0.5f), Number(ast, 1.0f), Number(ast, 0.2f)));

    plParticleBehaviorFactory_If* pIf = AllocateModule<plParticleBehaviorFactory_If>();
    pIf->m_ConditionInput = plParticleAttribute::Size;
    pIf->m_Comparison = plParticleConditionOp::Greater;
    pIf->m_fThreshold = 0.5f;
    pIf->m_OutputAttribute = plParticleAttribute::ColorA;
    pIf->m_fTrueValue = 1.0f;
    pIf->m_fFalseValue = 0.2f;
    pSystem->AddBehaviorFactory(pIf);
  }

  // Switch on Size into Color G: each Branch picks its bucket or everything after it
  {
    AST ast;
    AST::Node* pSize = ProgramInput(ast, "Size", DT::Half);
    AST::Node* pBucket3 = select(ast, compare(ast, AST::NodeType::Less, pSize, 0.75f), Number(ast, 0.6f), Number(ast, 0.9f));
    AST::Node* pBucket2 = select(ast, compare(ast, AST::NodeType::Less, pSize, 0.5f), Number(ast, 0.3f), pBucket3);
    addConverted("Switch", plParticleCustomAttributeType::Float, ast, select(ast, compare(ast, AST::NodeType::Less, pSize, 0.25f), Number(ast, 0.1f), pBucket2));

    plParticleBehaviorFactory_Switch* pSwitch = AllocateModule<plParticleBehaviorFactory_Switch>();
    pSwitch->m_InputAttribute = plParticleAttribute::Size;
    pSwitch->m_OutputAttribute = plParticleAttribute::ColorG;
    pSwitch->m_fValue0 = 0.1f;
    pSwitch->m_fValue1 = 0.3f;
    pSwitch->m_fValue2 = 0.6f;
    pSwitch->m_fValue3 = 0.9f;
    pSystem->AddBehaviorFactory(pSwitch);
  }

  // Remap Speed 0..3 to Color B 1..0, clamped, as the Remap node compiles
  {
    AST ast;
    AST::Node* pSpeed = ast.CreateUnaryOperator(AST::NodeType::Length, ProgramInput(ast, "Velocity", DT::Float3));
    AST::Node* t = ast.CreateUnaryOperator(AST::NodeType::Saturate, ast.CreateBinaryOperator(AST::NodeType::Multiply, ast.CreateBinaryOperator(AST::NodeType::Subtract, pSpeed, Number(ast, 0.0f)), Number(ast, 1.0f / 3.0f)));
    addConverted("Remap", plParticleCustomAttributeType::Float, ast, ast.CreateBinaryOperator(AST::NodeType::Add, Number(ast, 1.0f), ast.CreateBinaryOperator(AST::NodeType::Multiply, t, Number(ast, -1.0f))));

    plParticleBehaviorFactory_Remap* pRemap = AllocateModule<plParticleBehaviorFactory_Remap>();
    pRemap->m_InputAttribute = plParticleAttribute::Speed;
    pRemap->m_OutputAttribute = plParticleAttribute::ColorB;
    pRemap->m_fInputMax = 3.0f;
    pRemap->m_fOutputMin = 1.0f;
    pRemap->m_fOutputMax = 0.0f;
    pSystem->AddBehaviorFactory(pRemap);
  }

  // old Set Attribute Speed = 2: Velocity x (Speed > 0.0001 ? 2 / Speed : 1)
  {
    AST ast;
    AST::Node* pVelocity = ProgramInput(ast, "Velocity", DT::Float3);
    AST::Node* pSpeed = ast.CreateUnaryOperator(AST::NodeType::Length, pVelocity);
    AST::Node* pRatio = select(ast, compare(ast, AST::NodeType::Equal, pSpeed, 0.0f), Number(ast, 0.0f), ast.CreateBinaryOperator(AST::NodeType::Divide, Number(ast, 2.0f), pSpeed));
    AST::Node* pScale = select(ast, compare(ast, AST::NodeType::Greater, pSpeed, 0.0001f), pRatio, Number(ast, 1.0f));
    AST::Node* broadcast[] = {pScale, pScale, pScale};
    addConverted("Velocity", plParticleCustomAttributeType::Vector, ast, ast.CreateBinaryOperator(AST::NodeType::Multiply, pVelocity, ast.CreateConstructorCall(AST::DataType::Float3, broadcast)));

    plParticleBehaviorFactory_SetAttribute* pSet = AllocateModule<plParticleBehaviorFactory_SetAttribute>();
    pSet->m_Attribute = plParticleAttribute::Speed;
    pSet->m_Mode = plParticleSetAttributeMode::Replace;
    pSet->m_fValue = 2.0f;
    pSystem->AddBehaviorFactory(pSet);
  }

  // Expression a * (1 + 0.5 * sin(b * 12.57)) with a = Size, b = Position X, into Size
  {
    AST ast;
    AST::Node* a = ProgramInput(ast, "Size", DT::Half);
    AST::Node* b = ast.CreateSwizzle(AST::VectorComponent::X, ProgramInput(ast, "Position", DT::Float4));
    AST::Node* pWave = ast.CreateUnaryOperator(AST::NodeType::Sin, ast.CreateBinaryOperator(AST::NodeType::Multiply, b, Number(ast, 12.57f)));
    addConverted("Expression", plParticleCustomAttributeType::Float, ast,
      ast.CreateBinaryOperator(AST::NodeType::Multiply, a, ast.CreateBinaryOperator(AST::NodeType::Add, Number(ast, 1.0f), ast.CreateBinaryOperator(AST::NodeType::Multiply, Number(ast, 0.5f), pWave))));

    plParticleBehaviorFactory_Expression* pExpression = AllocateModule<plParticleBehaviorFactory_Expression>();
    pExpression->m_sExpression = "a * (1 + 0.5 * sin(b * 12.57))";
    pExpression->m_InputA = plParticleExpressionBinding::Size;
    pExpression->m_InputB = plParticleExpressionBinding::PositionX;
    pExpression->m_Output = plParticleExpressionBinding::Size;
    pSystem->AddBehaviorFactory(pExpression);
  }

  // Conditional Kill with the merged attribute list: Color R < 0.3
  {
    plParticleBehaviorFactory_ConditionalKill* pKill = AllocateModule<plParticleBehaviorFactory_ConditionalKill>();
    pKill->m_Attribute = plParticleAttribute::ColorR;
    pKill->m_Comparison = plParticleConditionOp::Less;
    pKill->m_fThreshold = 0.3f;
    pSystem->AddBehaviorFactory(pKill);
  }
}

void plGameEngineTestParticleRuntime::CheckConvertedLogicBlocks()
{
  plParticleEffectInstance* pEffect = GetEffect();
  if (!PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
    return;

  const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
  const plUInt64 uiAlive = pSystem->GetNumActiveParticles();

  // the kill removed the particles with little red, and only those
  PL_TEST_BOOL_MSG(uiAlive > 90 && uiAlive < 190, "%u of 200 particles survived Conditional Kill Color R < 0.3", (plUInt32)uiAlive);

  const plProcessingStream* pColor = pSystem->QueryStream("Color", plProcessingStream::DataType::Half4);
  const plProcessingStream* pSize = pSystem->QueryStream("Size", plProcessingStream::DataType::Half);
  const plProcessingStream* pVelocity = pSystem->QueryStream("Velocity", plProcessingStream::DataType::Float3);
  const plProcessingStream* pPosition = pSystem->QueryStream("Position", plProcessingStream::DataType::Float4);
  const plProcessingStream* pIf = pSystem->QueryStream("Custom.If", plProcessingStream::DataType::Float);
  const plProcessingStream* pSwitch = pSystem->QueryStream("Custom.Switch", plProcessingStream::DataType::Float);
  const plProcessingStream* pRemap = pSystem->QueryStream("Custom.Remap", plProcessingStream::DataType::Float);
  const plProcessingStream* pConvertedVelocity = pSystem->QueryStream("Custom.Velocity", plProcessingStream::DataType::Float3);
  const plProcessingStream* pExpression = pSystem->QueryStream("Custom.Expression", plProcessingStream::DataType::Float);

  if (!PL_TEST_BOOL(pColor && pSize && pVelocity && pPosition && pIf && pSwitch && pRemap && pConvertedVelocity && pExpression))
    return;

  plUInt32 uiMismatches[6] = {};
  float fPositionXMin = 1.0f, fPositionXMax = -1.0f;

  for (plUInt64 i = 0; i < uiAlive; ++i)
  {
    const plFloat16* pRgba = pColor->GetData<plFloat16>() + i * 4;
    const plVec3 vVelocity = pVelocity->GetData<plVec3>()[i];
    const plVec3 vConverted = pConvertedVelocity->GetData<plVec3>()[i];
    const plVec4 vPosition = pPosition->GetData<plVec4>()[i];

    uiMismatches[0] += plMath::IsEqual((float)pRgba[3], pIf->GetData<float>()[i], 0.002f) ? 0 : 1;
    uiMismatches[1] += plMath::IsEqual((float)pRgba[1], pSwitch->GetData<float>()[i], 0.002f) ? 0 : 1;
    uiMismatches[2] += plMath::IsEqual((float)pRgba[2], pRemap->GetData<float>()[i], 0.003f) ? 0 : 1;
    uiMismatches[3] += vVelocity.IsEqual(vConverted, 0.0005f) && plMath::IsEqual(vVelocity.GetLength(), 2.0f, 0.001f) ? 0 : 1;

    // Size is half precision; the converted value is the full float of the same formula
    const float fSize = pSize->GetData<plFloat16>()[i];
    const float fExpected = pExpression->GetData<float>()[i];
    uiMismatches[4] += plMath::IsEqual(fSize, fExpected, plMath::Max(0.002f, plMath::Abs(fExpected) * 0.002f)) ? 0 : 1;

    // the killed particles are gone, and the component write left x alone
    uiMismatches[5] += ((float)pRgba[0] >= 0.3f && vPosition.z > 0.0f) ? 0 : 1;
    fPositionXMin = plMath::Min(fPositionXMin, vPosition.x);
    fPositionXMax = plMath::Max(fPositionXMax, vPosition.x);
  }

  PL_TEST_INT(uiMismatches[0], 0); // If
  PL_TEST_INT(uiMismatches[1], 0); // Switch
  PL_TEST_INT(uiMismatches[2], 0); // Remap
  PL_TEST_INT(uiMismatches[3], 0); // old Set Attribute Speed
  PL_TEST_INT(uiMismatches[4], 0); // Expression
  PL_TEST_INT(uiMismatches[5], 0); // Conditional Kill
  PL_TEST_BOOL_MSG(fPositionXMax - fPositionXMin > 1.0f, "Set Attribute Position.z wrote x as well");
}

void plGameEngineTestParticleRuntime::CheckConditionalKillVersion1()
{
  // a 1.3 binary: version 1, attribute 5 (its own ColorAlpha), comparison 2 (Greater), threshold 0.5
  plDefaultMemoryStreamStorage storage;
  {
    plMemoryStreamWriter writer(&storage);
    writer << (plUInt8)1 << (plUInt8)5 << (plUInt8)2 << 0.5f;
  }

  plParticleBehaviorFactory_ConditionalKill kill;
  {
    plMemoryStreamReader reader(&storage);
    kill.Load(reader);
  }

  PL_TEST_BOOL(kill.m_Attribute == plParticleAttribute::ColorA);
  PL_TEST_BOOL(kill.m_Comparison == plParticleConditionOp::Greater);
  PL_TEST_FLOAT(kill.m_fThreshold, 0.5f, 0.0f);
}

plTestAppRun plGameEngineTestParticleRuntime::RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount)
{
  ++m_iFrame;

  m_pOwnApplication->Run();
  if (m_pOwnApplication->WasQuitRequested())
    return plTestAppRun::Quit;

  // the parameter test changes a component between checks, so it takes its own locks
  if (iIdentifier == SubTests::FrameProgramParameter)
    return RunFrameProgramParameter();

  if (iIdentifier == SubTests::PreviewResimulation)
    return RunPreviewResimulation();

  // read GPU buffers back, which waits for the GPU outside the world lock
  if (iIdentifier == SubTests::GPUProgramOnGPU)
    return RunGPUProgramOnGPU();

  if (iIdentifier == SubTests::GPUSortAliveCount)
    return RunGPUSortAliveCount();

  if (iIdentifier == SubTests::GPUSpyReadback)
    return RunGPUSpyReadback();

  // changes the global blackboard between checks
  if (iIdentifier == SubTests::BlackboardEntry)
    return RunBlackboardEntry();

  if (iIdentifier == SubTests::LodUpdateInterval)
    return RunLodUpdateInterval();

  if (iIdentifier >= SubTests::BenchMassDeaths && iIdentifier <= SubTests::BenchTestingChambers)
    return RunBenchmark(iIdentifier);

  if (iIdentifier >= SubTests::SizePerAxis && iIdentifier <= SubTests::DistortionPipeline)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    return RunArtistRequestTest(iIdentifier);
  }

  if (iIdentifier >= SubTests::EventSpawnPerEvent && iIdentifier <= SubTests::VelocityConeAxis)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    return RunEventTest(iIdentifier);
  }

  PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());

  switch (iIdentifier)
  {
    case SubTests::SharedEffectDefaults:
    {
      if (m_iFrame < 3)
        return plTestAppRun::Continue;

      plParticleEffectInstance* pEffect = GetEffect();
      if (PL_TEST_BOOL(pEffect != nullptr))
      {
        PL_TEST_BOOL(pEffect->IsSharedEffect());
        PL_TEST_FLOAT(pEffect->GetFloatParameter(plTempHashedString("TestFloat"), 1.0f), 0.25f, 0.0001f);
        PL_TEST_BOOL(pEffect->GetColorParameter(plTempHashedString("TestColor"), plColor::White) == plColor::Red);
      }

      return plTestAppRun::Quit;
    }

    case SubTests::BehaviorOrder:
    {
      if (m_iFrame < 3)
        return plTestAppRun::Continue;

      plParticleEffectInstance* pEffect = GetEffect();
      if (PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
      {
        const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
        const plProcessingStream* pSize = pSystem->QueryStream("Size", plProcessingStream::DataType::Half);

        if (PL_TEST_BOOL(pSize != nullptr && pSystem->GetNumActiveParticles() > 0))
        {
          const plFloat16* pSizes = pSize->GetData<plFloat16>();

          for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
          {
            PL_TEST_FLOAT((float)pSizes[i], 24.0f, 0.001f);
          }
        }
      }

      return plTestAppRun::Quit;
    }

    case SubTests::FixedTick:
    {
      if (m_iFrame < 20)
        return plTestAppRun::Continue;

      // the effect only ever advances in whole 0.1 s steps
      plParticleEffectInstance* pEffect = GetEffect();
      if (PL_TEST_BOOL(pEffect != nullptr))
      {
        const double fSteps = pEffect->GetTotalEffectLifeTime().GetSeconds() * 10.0;
        PL_TEST_BOOL(fSteps >= 1.0);
        PL_TEST_DOUBLE(fSteps, plMath::Round(fSteps), 0.001);
      }

      return plTestAppRun::Quit;
    }

    case SubTests::GPUEffectFinishes:
    {
      // the world steps 1/30 s per frame, so the 0.5 s particles are gone after 15 frames
      if (m_iFrame == 3)
      {
        PL_TEST_BOOL(GetEffect() != nullptr);
      }

      if (m_iFrame < 45)
        return plTestAppRun::Continue;

      PL_TEST_BOOL_MSG(GetEffect() == nullptr, "The GPU effect is still alive 1.5 s after its last particle died");
      return plTestAppRun::Quit;
    }

    case SubTests::ParticleProgramKill:
    {
      if (m_iFrame < 3)
        return plTestAppRun::Continue;

      plParticleEffectInstance* pEffect = GetEffect();
      if (PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
      {
        const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
        const plUInt64 uiAlive = pSystem->GetNumActiveParticles();

        // a fair coin over 200 particles lands well inside this range
        PL_TEST_BOOL_MSG(uiAlive > 60 && uiAlive < 140, "%u of 200 particles survived", (plUInt32)uiAlive);

        const plProcessingStream* pPosition = pSystem->QueryStream("Position", plProcessingStream::DataType::Float4);
        if (PL_TEST_BOOL(pPosition != nullptr))
        {
          for (plUInt64 i = 0; i < uiAlive; ++i)
          {
            const plVec4 p = pPosition->GetData<plVec4>()[i];
            PL_TEST_BOOL(p.z >= 0.0f && p.z <= 0.5f && p.x == 0.0f);
          }
        }
      }

      return plTestAppRun::Quit;
    }

    case SubTests::ParticleProgramCustomAttribute:
    {
      if (m_iFrame < 3)
        return plTestAppRun::Continue;

      plParticleEffectInstance* pEffect = GetEffect();
      if (PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
      {
        const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
        const plProcessingStream* pSize = pSystem->QueryStream("Size", plProcessingStream::DataType::Half);

        plUInt32 uiSmall = 0, uiLarge = 0;
        if (PL_TEST_BOOL(pSize != nullptr && pSystem->GetNumActiveParticles() == 200))
        {
          for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
          {
            const float fSize = pSize->GetData<plFloat16>()[i];
            if (plMath::IsEqual(fSize, 1.0f, 0.001f))
              ++uiSmall;
            else if (plMath::IsEqual(fSize, 3.0f, 0.001f))
              ++uiLarge;
          }
        }

        // every particle got one of the two sizes, and both occur
        PL_TEST_INT(uiSmall + uiLarge, 200);
        PL_TEST_BOOL(uiSmall > 40 && uiLarge > 40);
      }

      return plTestAppRun::Quit;
    }

    case SubTests::ParticleProgramSampleAssets:
    {
      if (m_iFrame < 3)
        return plTestAppRun::Continue;

      plParticleEffectInstance* pEffect = GetEffect();
      if (PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
      {
        const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
        const plProcessingStream* pSize = pSystem->QueryStream("Size", plProcessingStream::DataType::Half);
        const plProcessingStream* pColor = pSystem->QueryStream("Color", plProcessingStream::DataType::Half4);

        if (PL_TEST_BOOL(pSize != nullptr && pColor != nullptr && pSystem->GetNumActiveParticles() == 200))
        {
          struct Sample
          {
            PL_DECLARE_POD_TYPE();

            float m_fSize;
            float m_fGrey;
            bool operator<(const Sample& other) const { return m_fSize < other.m_fSize; }
          };

          plDynamicArray<Sample> samples;
          bool bAllGrey = true;

          for (plUInt64 i = 0; i < 200; ++i)
          {
            const plFloat16* pRgba = pColor->GetData<plFloat16>() + i * 4;
            const float r = pRgba[0], g = pRgba[1], b = pRgba[2];
            bAllGrey = bAllGrey && plMath::IsEqual(r, g, 0.01f) && plMath::IsEqual(r, b, 0.01f);
            samples.PushBack({(float)pSize->GetData<plFloat16>()[i], r});
          }

          samples.Sort();

          // the curve spans 0 to 2 and the gradient black to white; both read the same Random
          PL_TEST_BOOL_MSG(bAllGrey, "Sample Gradient gave a colour off the black-to-white gradient");
          PL_TEST_BOOL_MSG(samples[0].m_fSize >= 0.0f && samples.PeekBack().m_fSize <= 2.01f, "Sample Curve left the curve's range");
          PL_TEST_BOOL_MSG(samples[0].m_fSize < 0.5f && samples.PeekBack().m_fSize > 1.5f, "Sample Curve barely varies: %.3f to %.3f", samples[0].m_fSize, samples.PeekBack().m_fSize);
          PL_TEST_BOOL_MSG(samples[0].m_fGrey < 0.1f && samples.PeekBack().m_fGrey > 0.5f, "Sample Gradient does not follow the curve: %.3f to %.3f", samples[0].m_fGrey, samples.PeekBack().m_fGrey);
        }
      }

      return plTestAppRun::Quit;
    }

    case SubTests::ParticleProgramCost:
      MeasureProgramCost();
      return plTestAppRun::Quit;

    case SubTests::GPUProgramReference:
      CheckGPUProgramReference();
      return plTestAppRun::Quit;

    case SubTests::GPUProgramLowering:
      CheckGPUProgramLowering();
      return plTestAppRun::Quit;

    case SubTests::ConvertedLogicBlocks:
    {
      if (m_iFrame < 3)
        return plTestAppRun::Continue;

      CheckConvertedLogicBlocks();
      CheckConditionalKillVersion1();
      return plTestAppRun::Quit;
    }

    case SubTests::PerParticleProperty:
    {
      if (m_iFrame < 3)
        return plTestAppRun::Continue;

      plParticleEffectInstance* pEffect = GetEffect();
      if (PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
      {
        const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
        const plProcessingStream* pSize = pSystem->QueryStream("Size", plProcessingStream::DataType::Half);
        const plProcessingStream* pVelocity = pSystem->QueryStream("Velocity", plProcessingStream::DataType::Float3);

        if (PL_TEST_BOOL(pSize != nullptr && pVelocity != nullptr && pSystem->GetNumActiveParticles() == 200))
        {
          // the fall is gravity x factor x time: per unit of factor it is the same for all
          const float fReference = pVelocity->GetData<plVec3>()[0].z / (float)pSize->GetData<plFloat16>()[0];
          float fSlowest = 0.0f, fFastest = 0.0f;
          plUInt32 uiMismatches = 0;

          for (plUInt64 i = 0; i < 200; ++i)
          {
            const float fFall = pVelocity->GetData<plVec3>()[i].z;
            const float fPerFactor = fFall / (float)pSize->GetData<plFloat16>()[i];
            uiMismatches += plMath::IsEqual(fPerFactor, fReference, plMath::Abs(fReference) * 0.002f) ? 0 : 1;
            fSlowest = plMath::Max(fSlowest, fFall);
            fFastest = plMath::Min(fFastest, fFall);
          }

          PL_TEST_BOOL_MSG(fReference < 0.0f, "The particles do not fall");
          PL_TEST_INT(uiMismatches, 0);
          PL_TEST_BOOL_MSG(fFastest < fSlowest * 2.0f, "Every particle fell at the same speed: the factor was not read per particle");
        }
      }

      return plTestAppRun::Quit;
    }

    default:
      break;
  }

  return plTestAppRun::Quit;
}

void plGameEngineTestParticleRuntime::MeasureProgramCost()
{
  // D2: a 10K-particle Update program of about 30 instructions must cost under 0.3 ms on one core.
  // This is the program colour-by-speed-and-age with a per-particle random tint, run on the VM the
  // particle blocks use, over streams laid out like a system's.
  constexpr plUInt32 uiCount = 10000;

  AST ast;
  AST::Node* pVelocity = ProgramInput(ast, "Velocity", plProcessingStream::DataType::Float3);
  AST::Node* pLifeTime = ProgramInput(ast, "LifeTime", plProcessingStream::DataType::Float2);

  AST::Node* pSpeed = ast.CreateUnaryOperator(AST::NodeType::Length, pVelocity);
  AST::Node* pHeat = ast.CreateUnaryOperator(AST::NodeType::Saturate, ast.CreateBinaryOperator(AST::NodeType::Multiply, pSpeed, Number(ast, 0.125f)));
  AST::Node* pAge = ast.CreateBinaryOperator(AST::NodeType::Subtract, Number(ast, 1.0f),
    ast.CreateBinaryOperator(AST::NodeType::Multiply, ast.CreateSwizzle(AST::VectorComponent::X, pLifeTime), ast.CreateSwizzle(AST::VectorComponent::Y, pLifeTime)));
  AST::Node* pFlicker = ast.CreateUnaryOperator(AST::NodeType::Sin, ast.CreateBinaryOperator(AST::NodeType::Multiply, ast.CreateBinaryOperator(AST::NodeType::Multiply, pAge, pAge), Number(ast, 6.28f)));
  AST::Node* pTint = ast.CreateBinaryOperator(AST::NodeType::Add, ast.CreateBinaryOperator(AST::NodeType::Multiply, ParticleRandom(ast, 1), Number(ast, 0.3f)), Number(ast, 0.7f));
  AST::Node* pLevel = ast.CreateTernaryOperator(AST::NodeType::Clamp,
    ast.CreateBinaryOperator(AST::NodeType::Add, ast.CreateBinaryOperator(AST::NodeType::Multiply, pHeat, pTint), ast.CreateBinaryOperator(AST::NodeType::Multiply, pFlicker, Number(ast, 0.1f))),
    Number(ast, 0.0f), Number(ast, 1.0f));
  AST::Node* components[] = {pLevel, ast.CreateBinaryOperator(AST::NodeType::Multiply, pLevel, pLevel), ast.CreateBinaryOperator(AST::NodeType::Subtract, Number(ast, 1.0f), pLevel), Number(ast, 1.0f)};
  ast.m_OutputNodes.PushBack(ast.CreateOutput({plMakeHashedString("Out0"), plProcessingStream::DataType::Float4}, ast.CreateConstructorCall(AST::DataType::Float4, components)));

  plExpressionByteCode byteCode;
  plExpressionCompiler compiler;
  if (!PL_TEST_BOOL(compiler.Compile(ast, byteCode).Succeeded()))
    return;

  plDynamicArray<plVec3> velocity;
  plDynamicArray<plVec2> lifeTime;
  plDynamicArray<plUInt32> seeds;
  plDynamicArray<plVec4> color;
  velocity.SetCount(uiCount);
  lifeTime.SetCount(uiCount);
  seeds.SetCount(uiCount);
  color.SetCount(uiCount);

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    velocity[i].Set((float)(i % 7), 1.0f, (float)(i % 3));
    lifeTime[i].Set(1.0f + (i % 5) * 0.1f, 0.5f);
    seeds[i] = i * 2654435761u;
  }

  plProcessingStream inputs[] = {
    plProcessingStream(plMakeHashedString("Velocity"), velocity.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float3, sizeof(plVec3)),
    plProcessingStream(plMakeHashedString("LifeTime"), lifeTime.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float2, sizeof(plVec2)),
    plProcessingStream(plMakeHashedString("RandomSeed"), seeds.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Int, sizeof(plUInt32)),
  };
  plProcessingStream outputs[] = {
    plProcessingStream(plMakeHashedString("Out0"), color.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float4, sizeof(plVec4)),
  };

  // the blocks hand the VM one scalar view per input and output, in the byte code's order
  plDynamicArray<plProcessingStream> scalarInputs, scalarOutputs;
  if (!PL_TEST_BOOL(ScalarViews(byteCode.GetInputs(), inputs, scalarInputs) && ScalarViews(byteCode.GetOutputs(), outputs, scalarOutputs)))
    return;

  plExpressionVM vm;
  const auto flags = plExpressionVM::Flags::BestPerformance;

  // warm-up, then the median of several runs
  PL_TEST_BOOL(vm.Execute(byteCode, scalarInputs, scalarOutputs, uiCount, plExpression::GlobalData(), flags).Succeeded());

  plHybridArray<double, 32> timings;
  for (plUInt32 iRun = 0; iRun < 31; ++iRun)
  {
    const plTime tStart = plTime::Now();
    vm.Execute(byteCode, scalarInputs, scalarOutputs, uiCount, plExpression::GlobalData(), flags).IgnoreResult();
    timings.PushBack((plTime::Now() - tStart).GetMilliseconds());
  }

  timings.Sort();
  const double fMedian = timings[timings.GetCount() / 2];

  plLog::Info("Particle program cost: {} instructions over {} particles, median {} ms", byteCode.GetNumInstructions(), uiCount, plArgF(fMedian, 3));

#if defined(PLASMABUILD_DEVELOPMENT) || defined(PLASMABUILD_DEBUG)
  // these configurations build the VM without optimizations, so the time says nothing about D2
  plLog::Warning("Particle program cost is only checked against D2 (0.3 ms) in optimized builds.");
#else
  PL_TEST_BOOL_MSG(fMedian < 0.3, "A 10K-particle program of %u instructions took %.3f ms; D2 allows 0.3 ms", byteCode.GetNumInstructions(), fMedian);
#endif

  // spot check one result: the program ran and produced colours in range
  PL_TEST_BOOL(color[17].x >= 0.0f && color[17].x <= 1.0f && color[17].w == 1.0f);
}

plTestAppRun plGameEngineTestParticleRuntime::RunFrameProgramParameter()
{
  plWorld* pWorld = m_pOwnApplication->GetWorld();

  auto checkSize = [&](float fExpected)
  {
    PL_LOCK(pWorld->GetReadMarker());

    plParticleEffectInstance* pEffect = GetEffect();
    if (!PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
      return;

    const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
    const plProcessingStream* pSize = pSystem->QueryStream("Size", plProcessingStream::DataType::Half);

    if (PL_TEST_BOOL(pSize != nullptr && pSystem->GetNumActiveParticles() > 0))
    {
      PL_TEST_FLOAT((float)pSize->GetData<plFloat16>()[0], fExpected, 0.01f);
    }
  };

  if (m_iFrame == 3)
  {
    // the parameter's default, doubled
    checkSize(3.0f);

    PL_LOCK(pWorld->GetWriteMarker());

    plParticleComponent* pComponent = nullptr;
    if (pWorld->TryGetComponent(m_hEffectComponent, pComponent))
      pComponent->SetParameter("Size", plVariant(2.5f));
  }

  if (m_iFrame < 8)
    return plTestAppRun::Continue;

  // the value gameplay set, doubled, without restarting the effect
  checkSize(5.0f);
  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestParticleRuntime::RunPreviewResimulation()
{
  if (m_iFrame < 2)
    return plTestAppRun::Continue;

  plWorld* pWorld = m_pOwnApplication->GetWorld();
  PL_LOCK(pWorld->GetWriteMarker());

  plParticleComponent* pComponent = nullptr;
  if (!PL_TEST_BOOL(pWorld->TryGetComponent(m_hEffectComponent, pComponent)))
    return plTestAppRun::Quit;

  // restart and step 1/60 s at a time, as the timeline does, and keep where every particle ended up
  auto simulate = [&](plDynamicArray<plVec4>& out_positions)
  {
    pComponent->InterruptEffect();
    pComponent->StartEffect();

    for (plUInt32 i = 0; i < 60 && pComponent->m_EffectController.IsAlive(); ++i)
    {
      pComponent->m_EffectController.ForceVisible();
      pComponent->m_EffectController.Tick(plTime::MakeFromSeconds(1.0 / 60.0));
    }

    out_positions.Clear();
    if (!pComponent->m_EffectController.IsAlive())
      return;

    const plParticleSystemInstance* pSystem = pComponent->m_EffectController.GetInstance()->GetParticleSystems()[0];
    const plProcessingStream* pPosition = pSystem->QueryStream("Position", plProcessingStream::DataType::Float4);
    for (plUInt64 p = 0; pPosition != nullptr && p < pSystem->GetNumActiveParticles(); ++p)
      out_positions.PushBack(pPosition->GetData<plVec4>()[p]);
  };

  plDynamicArray<plVec4> first, second;
  simulate(first);
  simulate(second);

  // the same seed and steps give the same particles, which is what a scrub relies on
  if (PL_TEST_BOOL(!first.IsEmpty() && first.GetCount() == second.GetCount()))
  {
    plUInt32 uiDifferent = 0;
    float fSpread = 0.0f;
    for (plUInt32 i = 0; i < first.GetCount(); ++i)
    {
      uiDifferent += first[i] == second[i] ? 0 : 1;
      fSpread = plMath::Max(fSpread, plMath::Abs(first[i].x - first[0].x));
    }

    PL_TEST_INT(uiDifferent, 0);
    PL_TEST_BOOL_MSG(fSpread > 0.01f, "The particles all ended up in one place; the test effect is not random enough to tell");
  }

  return plTestAppRun::Quit;
}

void plGameEngineTestParticleRuntime::StartEffect(const plParticleEffectDescriptor& desc, plStringView sSharedInstanceName)
{
  // every sub-test gets its own resource, so no instance or shared state carries over
  plStringBuilder sName;
  sName.SetFormat("ParticleRuntimeTest{}", m_uiEffectCounter++);

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
  pComponent->m_sSharedInstanceName = sSharedInstanceName;
  pComponent->SetParticleEffect(hEffect);
}

plParticleEffectInstance* plGameEngineTestParticleRuntime::GetEffect() const
{
  // the const lookup only needs the read lock the callers hold
  const plWorld* pWorld = m_pOwnApplication->GetWorld();
  const plParticleComponent* pComponent = nullptr;
  if (!pWorld->TryGetComponent(m_hEffectComponent, pComponent))
    return nullptr;

  if (!pComponent->m_EffectController.IsAlive())
    return nullptr;

  return pComponent->m_EffectController.GetInstance();
}

//////////////////////////////////////////////////////////////////////////
// P5: graph programs on the GPU

namespace
{
  bool IsClose(float a, float b)
  {
    return plMath::Abs(a - b) <= 1e-4f * plMath::Max(1.0f, plMath::Abs(a));
  }
} // namespace

void plGameEngineTestParticleRuntime::CheckGPUProgramReference()
{
  // D25: the kernel's form of the programs must give what the CPU VM gives, particle by particle. The C++
  // reference interpreter runs the exact words and data the kernel reads; here it is compared with the VM.
  plParticleBehaviorFactory_Drag drag;
  {
    // Drag = |sin(Position.z)| + floor(Size * 3)
    AST ast;
    AST::Node* pPosition = ProgramInput(ast, "Position", plProcessingStream::DataType::Float3);
    AST::Node* pSize = ProgramInput(ast, "Size", plProcessingStream::DataType::Half);
    AST::Node* pValue = ast.CreateBinaryOperator(AST::NodeType::Add, ast.CreateUnaryOperator(AST::NodeType::Absolute, ast.CreateUnaryOperator(AST::NodeType::Sin, Component(ast, pPosition, AST::VectorComponent::Z))),
      ast.CreateUnaryOperator(AST::NodeType::Floor, ast.CreateBinaryOperator(AST::NodeType::Multiply, pSize, Number(ast, 3.0f))));
    PL_TEST_BOOL(CompileProgram(ast, pValue, plProcessingStream::DataType::Float, "Drag", drag.m_Program));
  }

  plParticleBehaviorFactory_Attribute set;
  set.m_Settings.m_Target = plParticleSinkTarget::Velocity;
  {
    // Velocity = (Random(seed, 7), Size * 2 + Position.x, LifeTime.x > 0.5 ? Color.r : Color.g)
    AST ast;
    AST::Node* pPosition = ProgramInput(ast, "Position", plProcessingStream::DataType::Float3);
    AST::Node* pSize = ProgramInput(ast, "Size", plProcessingStream::DataType::Half);
    AST::Node* pLifeTime = ProgramInput(ast, "LifeTime", plProcessingStream::DataType::Float2);
    AST::Node* pColor = ProgramInput(ast, "Color", plProcessingStream::DataType::Half4);

    AST::Node* pLate = ast.CreateBinaryOperator(AST::NodeType::Greater, Component(ast, pLifeTime, AST::VectorComponent::X), Number(ast, 0.5f));
    AST::Node* components[] = {ParticleRandom(ast, 7),
      ast.CreateBinaryOperator(AST::NodeType::Add, ast.CreateBinaryOperator(AST::NodeType::Multiply, pSize, Number(ast, 2.0f)), Component(ast, pPosition, AST::VectorComponent::X)),
      ast.CreateTernaryOperator(AST::NodeType::Select, pLate, Component(ast, pColor, AST::VectorComponent::X), Component(ast, pColor, AST::VectorComponent::Y))};
    PL_TEST_BOOL(CompileProgram(ast, ast.CreateConstructorCall(AST::DataType::Float3, components), plProcessingStream::DataType::Float3, "Vector", set.m_Settings.m_Program));
  }

  plParticleBehaviorFactory_KillIf kill;
  {
    // Condition = PerlinNoise(Position.x * 0.7, Position.y, Position.z) > 0.5
    AST ast;
    AST::Node* pPosition = ProgramInput(ast, "Position", plProcessingStream::DataType::Float3);
    AST::Node* arguments[] = {ast.CreateBinaryOperator(AST::NodeType::Multiply, Component(ast, pPosition, AST::VectorComponent::X), Number(ast, 0.7f)),
      Component(ast, pPosition, AST::VectorComponent::Y), Component(ast, pPosition, AST::VectorComponent::Z)};
    AST::Node* pNoise = ast.CreateFunctionCall(plDefaultExpressionFunctions::s_PerlinNoiseFunc.m_Desc, arguments);
    PL_TEST_BOOL(CompileProgram(ast, ConditionAsNumber(ast, ast.CreateBinaryOperator(AST::NodeType::Greater, pNoise, Number(ast, 0.5f))), plProcessingStream::DataType::Float, "Condition", kill.m_Program));
  }

  const plParticleBehaviorFactory* factories[] = {&drag, &set, &kill};
  plParticleGPUProgramSet programs;
  const plString sReason = programs.Build(factories);
  if (!PL_TEST_BOOL_MSG(sReason.IsEmpty(), "Did not build: %s", sReason.GetData()))
    return;

  PL_TEST_INT(programs.GetProgramCount(), 3);
  PL_TEST_BOOL(programs.ReadsRandomSeed());

  plDynamicArray<float> data;
  data.SetCount(programs.GetDataCount());
  programs.FillData(nullptr, plTime::MakeZero(), data);

  // particles as the GPU holds them, and the same values as the CPU streams hold them
  constexpr plUInt32 uiCount = 256;
  plRandom rng;
  rng.Initialize(7);

  plDynamicArray<plGPUParticle> particles;
  plDynamicArray<plVec4> position;
  plDynamicArray<plVec3> velocity;
  plDynamicArray<plFloat16> size;
  plDynamicArray<plColorLinear16f> color;
  plDynamicArray<plVec2> lifeTime;
  plDynamicArray<plUInt32> seeds;

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    plGPUParticle& p = particles.ExpandAndGetRef();
    p.Position.Set((float)rng.DoubleMinMax(-2, 2), (float)rng.DoubleMinMax(-2, 2), (float)rng.DoubleMinMax(-2, 2));
    p.Velocity.Set((float)rng.DoubleMinMax(-3, 3), (float)rng.DoubleMinMax(-3, 3), (float)rng.DoubleMinMax(-3, 3));
    p.Size = plFloat16((float)rng.DoubleMinMax(0.1, 1.0)); // the CPU stream is half
    p.Color = plColorLinear16f(plColor((float)rng.DoubleZeroToOneExclusive(), (float)rng.DoubleZeroToOneExclusive(), (float)rng.DoubleZeroToOneExclusive(), 1.0f));
    p.Life = (float)rng.DoubleMinMax(0.05, 1.0);
    p.MaxLife = 1.0f / (float)rng.DoubleMinMax(1.0, 4.0);
    p.RotationOffset = 0.0f;
    p.RotationSpeed = 0.0f;
    p.Flags = 1;
    p.InitialSize = p.Size;
    p.AxisPacked = 0;

    position.PushBack(p.Position.GetAsVec4(0.0f));
    velocity.PushBack(p.Velocity);
    size.PushBack(plFloat16(p.Size));
    color.PushBack(p.Color);
    lifeTime.PushBack(plVec2(p.Life / p.MaxLife, p.MaxLife));
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
  auto RunVM = [&](const plParticleProgramData& program, plProcessingStream& ref_output) -> bool
  {
    plDynamicArray<plProcessingStream> scalarInputs, scalarOutputs;
    if (!ScalarViews(program.m_ByteCode.GetInputs(), inputs, scalarInputs) || !ScalarViews(program.m_ByteCode.GetOutputs(), plMakeArrayPtr(&ref_output, 1), scalarOutputs))
      return false;

    return vm.Execute(program.m_ByteCode, scalarInputs, scalarOutputs, uiCount, plExpression::GlobalData(), plExpressionVM::Flags::BestPerformance).Succeeded();
  };

  plDynamicArray<float> dragVM, conditionVM;
  plDynamicArray<plVec3> vectorVM;
  dragVM.SetCount(uiCount);
  conditionVM.SetCount(uiCount);
  vectorVM.SetCount(uiCount);

  plProcessingStream dragOut(plMakeHashedString("Out0"), dragVM.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float, sizeof(float));
  plProcessingStream vectorOut(plMakeHashedString("Out0"), vectorVM.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float3, sizeof(plVec3));
  plProcessingStream conditionOut(plMakeHashedString("Out0"), conditionVM.GetArrayPtr().ToByteArray(), plProcessingStream::DataType::Float, sizeof(float));

  if (!PL_TEST_BOOL(RunVM(drag.m_Program, dragOut) && RunVM(set.m_Settings.m_Program, vectorOut) && RunVM(kill.m_Program, conditionOut)))
    return;

  plUInt32 uiDragMismatches = 0, uiVelocityMismatches = 0, uiKillMismatches = 0, uiKilled = 0;
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const plParticleGPUProgramSet::Properties props = plParticleGPUProgramSet::RunPropertyPrograms(programs.GetWords(), data, particles[i], seeds[i]);
    uiDragMismatches += (props.m_uiMask & (1u << GPU_PROGRAM_PROPERTY_DRAG)) != 0 && IsClose(props.m_fValues[GPU_PROGRAM_PROPERTY_DRAG], plMath::Max(dragVM[i], 0.0f)) ? 0 : 1;

    plGPUParticle p = particles[i];
    const bool bAlive = plParticleGPUProgramSet::RunSinkPrograms(programs.GetWords(), data, p, seeds[i], 1.0f / 60.0f);

    uiVelocityMismatches += IsClose(p.Velocity.x, vectorVM[i].x) && IsClose(p.Velocity.y, vectorVM[i].y) && IsClose(p.Velocity.z, vectorVM[i].z) ? 0 : 1;
    uiKillMismatches += bAlive == (conditionVM[i] == 0.0f) ? 0 : 1;
    uiKilled += bAlive ? 0 : 1;
  }

  PL_TEST_INT(uiDragMismatches, 0);
  PL_TEST_INT(uiVelocityMismatches, 0);
  PL_TEST_INT(uiKillMismatches, 0);

  // the noise threshold kills some and keeps some, so the condition was really evaluated
  PL_TEST_BOOL_MSG(uiKilled > 0 && uiKilled < uiCount, "%u of %u particles killed", uiKilled, uiCount);
}

void plGameEngineTestParticleRuntime::CheckGPUProgramLowering()
{
  // D23/D24: what keeps graph programs off the GPU, and the reasons the editor shows
  auto MakeColorSet = [](plParticleBehaviorFactory_Attribute& ref_set)
  {
    ref_set.m_Settings.m_Target = plParticleSinkTarget::Color;

    AST ast;
    AST::Node* components[] = {ParticleRandom(ast, 1), Number(ast, 0.5f), Number(ast, 0.5f), Number(ast, 1.0f)};
    CompileProgram(ast, ast.CreateConstructorCall(AST::DataType::Float4, components), plProcessingStream::DataType::Float4, "Color", ref_set.m_Settings.m_Program);
  };

  plParticleBehaviorFactory_Gravity gravity;
  plParticleBehaviorFactory_Attribute setColor;
  MakeColorSet(setColor);

  // after the built-in blocks: lowers
  {
    const plParticleBehaviorFactory* factories[] = {&gravity, &setColor};
    plUInt32 uiBlock = 0;
    const plString sReason = plParticleGPUProgramSet::FindBlocker(factories, &uiBlock);
    PL_TEST_BOOL_MSG(sReason.IsEmpty(), "%s", sReason.GetData());
  }

  // above a built-in block: the GPU would run it later than the CPU does
  {
    const plParticleBehaviorFactory* factories[] = {&setColor, &gravity};
    plUInt32 uiBlock = plInvalidIndex;
    const plString sReason = plParticleGPUProgramSet::FindBlocker(factories, &uiBlock);
    PL_TEST_BOOL_MSG(sReason.FindSubString("is above Gravity") != nullptr, "%s", sReason.GetData());
    PL_TEST_INT(uiBlock, 0);
  }

  // a custom attribute target
  {
    plParticleBehaviorFactory_Attribute setCustom;
    setCustom.m_Settings.m_Target = plParticleSinkTarget::Custom;
    setCustom.m_Settings.m_sCustomAttribute = "Heat";

    const plParticleBehaviorFactory* factories[] = {&setCustom};
    const plString sReason = plParticleGPUProgramSet::FindBlocker(factories);
    PL_TEST_BOOL_MSG(sReason.FindSubString("custom attribute") != nullptr, "%s", sReason.GetData());
  }

  // a program over the instruction limit
  {
    plParticleBehaviorFactory_KillIf kill;

    AST ast;
    AST::Node* pPosition = ProgramInput(ast, "Position", plProcessingStream::DataType::Float3);
    AST::Node* pValue = Component(ast, pPosition, AST::VectorComponent::X);
    for (plUInt32 i = 0; i < 300; ++i)
      pValue = ast.CreateUnaryOperator(AST::NodeType::Sin, ast.CreateBinaryOperator(AST::NodeType::Add, pValue, Component(ast, pPosition, (AST::VectorComponent::Enum)(i % 3))));
    PL_TEST_BOOL(CompileProgram(ast, ConditionAsNumber(ast, ast.CreateBinaryOperator(AST::NodeType::Greater, pValue, Number(ast, 0.0f))), plProcessingStream::DataType::Float, "Condition", kill.m_Program));
    PL_TEST_BOOL(kill.m_Program.m_ByteCode.GetNumInstructions() > GPU_PROGRAM_MAX_INSTRUCTIONS);

    const plParticleBehaviorFactory* factories[] = {&kill};
    const plString sReason = plParticleGPUProgramSet::FindBlocker(factories);
    PL_TEST_BOOL_MSG(sReason.FindSubString("instructions; the GPU takes at most 256") != nullptr, "%s", sReason.GetData());
  }

  // more programs than a context may have
  {
    plParticleBehaviorFactory_Attribute sets[5];
    const plParticleBehaviorFactory* factories[] = {&sets[0], &sets[1], &sets[2], &sets[3], &sets[4]};
    const plString sReason = plParticleGPUProgramSet::FindBlocker(factories);
    PL_TEST_BOOL_MSG(sReason.FindSubString("at most 4") != nullptr, "%s", sReason.GetData());
  }

  // a Kill If that never kills is nothing to run
  {
    plParticleBehaviorFactory_KillIf never;
    const plParticleBehaviorFactory* factories[] = {&gravity, &never};
    plParticleGPUProgramSet programs;
    PL_TEST_BOOL(programs.Build(factories).IsEmpty());
    PL_TEST_BOOL(programs.IsEmpty());
  }

  // the whole system: a GPU target with programs now lowers, several Set Attribute and Kill If blocks too
  {
    plParticleSystemDescriptor desc;
    desc.m_SimulationTarget = plParticleSimulationTarget::GPU;
    plParticleBehaviorFactory_Attribute* pSet = AllocateModule<plParticleBehaviorFactory_Attribute>();
    MakeColorSet(*pSet);
    plParticleBehaviorFactory_Attribute* pSetSize = AllocateModule<plParticleBehaviorFactory_Attribute>();
    pSetSize->m_Settings.m_Target = plParticleSinkTarget::Size;
    plParticleBehaviorFactory_KillIf* pKill = AllocateModule<plParticleBehaviorFactory_KillIf>();
    pKill->m_bCondition = false;
    desc.AddBehaviorFactory(AllocateModule<plParticleBehaviorFactory_Gravity>());
    desc.AddBehaviorFactory(pSet);
    desc.AddBehaviorFactory(pSetSize);
    desc.AddBehaviorFactory(pKill);
    desc.AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());

    const plString sReason = plParticleGPULowering::FindLoweringBlocker(&desc);
    PL_TEST_BOOL_MSG(sReason.IsEmpty(), "%s", sReason.GetData());
  }
}

void plGameEngineTestParticleRuntime::SetupGPUProgramOnGPU(plParticleEffectDescriptor& ref_desc)
{
  // a GPU system whose Update sets Color = (Random(seed, 3), Random(seed, 5), 0.25, 1)
  plParticleSystemDescriptor* pSystem = AddBurstSystem(ref_desc, 64, 10.0f);
  pSystem->m_SimulationTarget = plParticleSimulationTarget::GPU;

  plParticleBehaviorFactory_Attribute* pSet = AllocateModule<plParticleBehaviorFactory_Attribute>();
  pSet->m_Settings.m_Target = plParticleSinkTarget::Color;

  AST ast;
  AST::Node* components[] = {ParticleRandom(ast, 3), ParticleRandom(ast, 5), Number(ast, 0.25f), Number(ast, 1.0f)};
  PL_TEST_BOOL(CompileProgram(ast, ast.CreateConstructorCall(AST::DataType::Float4, components), plProcessingStream::DataType::Float4, "Color", pSet->m_Settings.m_Program));

  pSystem->AddBehaviorFactory(pSet);
  pSystem->AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());
}

plTestAppRun plGameEngineTestParticleRuntime::RunGPUProgramOnGPU()
{
  if (m_iFrame < 6)
    return plTestAppRun::Continue;

  // what the CPU expects for every spawned particle, from the seeds the CPU kept
  plGALBufferHandle hParticles;
  plDynamicArray<float> expectedRed, expectedGreen;
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());

    plParticleEffectInstance* pEffect = GetEffect();
    if (!PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
      return plTestAppRun::Quit;

    const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
    if (!PL_TEST_BOOL_MSG(pSystem->IsLoweredToGPU(), "The system with a Set Attribute program did not lower to the GPU"))
      return plTestAppRun::Quit;

    const plParticleTypeGPU* pType = pSystem->GetTypes().IsEmpty() ? nullptr : plDynamicCast<const plParticleTypeGPU*>(pSystem->GetTypes()[0]);
    const plProcessingStream* pSeeds = pSystem->QueryStream("RandomSeed", plProcessingStream::DataType::Int);
    if (!PL_TEST_BOOL(pType != nullptr && pSeeds != nullptr))
      return plTestAppRun::Quit;

    hParticles = pType->GetParticleBuffer();

    for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
    {
      // Random(RandomSeed, 3): the particle's seed is the position, the node's seed the seed
      const plInt32 iSeed = pSeeds->GetData<plInt32>()[i];
      expectedRed.PushBack(plFloat16(plSimdRandom::FloatZeroToOne(plSimdVec4i(iSeed), plSimdVec4u(3)).x()));
      expectedGreen.PushBack(plFloat16(plSimdRandom::FloatZeroToOne(plSimdVec4i(iSeed), plSimdVec4u(5)).x()));
    }
  }

  if (!PL_TEST_BOOL(!hParticles.IsInvalidated() && expectedRed.GetCount() == 64))
    return plTestAppRun::Quit;

  // what the GPU computed
  plDynamicArray<float> red, green;
  {
    plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
    plGALReadbackBufferHelper readback;

    plGALCommandEncoder* pEncoder = pDevice->BeginCommands("Particle program readback");
    readback.ReadbackBuffer(*pEncoder, hParticles);
    pDevice->EndCommands(pEncoder);

    if (!PL_TEST_BOOL(readback.GetReadbackResult(plTime::MakeFromSeconds(10)) == plGALAsyncResult::Ready))
      return plTestAppRun::Quit;

    plArrayPtr<const plUInt8> memory;
    plReadbackBufferLock lock = readback.LockBuffer(memory);
    if (!PL_TEST_BOOL(lock))
      return plTestAppRun::Quit;

    const plArrayPtr<const plGPUParticle> pool(reinterpret_cast<const plGPUParticle*>(memory.GetPtr()), memory.GetCount() / sizeof(plGPUParticle));
    for (const plGPUParticle& p : pool)
    {
      if ((p.Flags & 1u) == 0)
        continue;

      red.PushBack(p.Color.r);
      green.PushBack(p.Color.g);
      PL_TEST_FLOAT((float)p.Color.b, 0.25f, 0.001f);
    }
  }

  // the slots the GPU gives the spawns are not the CPU order, so compare the sorted values
  PL_TEST_INT(red.GetCount(), expectedRed.GetCount());
  if (red.GetCount() == expectedRed.GetCount())
  {
    red.Sort();
    green.Sort();
    expectedRed.Sort();
    expectedGreen.Sort();

    // the colour is stored as half; Vulkan's conversion rounds some values the other way than the CPU's,
    // so one half step apart is the same result
    auto IsSameHalf = [](float a, float b)
    { return plMath::Abs(a - b) <= plMath::Max(1e-4f, plMath::Abs(b) * (1.0f / 1024.0f)); };

    plUInt32 uiMismatches = 0;
    for (plUInt32 i = 0; i < red.GetCount(); ++i)
      uiMismatches += (IsSameHalf(red[i], expectedRed[i]) && IsSameHalf(green[i], expectedGreen[i])) ? 0 : 1;

    if (!PL_TEST_INT(uiMismatches, 0))
    {
      for (plUInt32 i = 0; i < red.GetCount(); i += 16)
        plLog::Info("GPU ({}, {}) expected ({}, {})", red[i], green[i], expectedRed[i], expectedGreen[i]);
    }
  }

  return plTestAppRun::Quit;
}

namespace
{
  /// Copies a GPU buffer to memory, waiting for the GPU.
  bool ReadBack(plGALBufferHandle hBuffer, plDynamicArray<plUInt8>& out_data)
  {
    plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
    plGALReadbackBufferHelper readback;

    plGALCommandEncoder* pEncoder = pDevice->BeginCommands("Particle test readback");
    readback.ReadbackBuffer(*pEncoder, hBuffer);
    pDevice->EndCommands(pEncoder);

    if (readback.GetReadbackResult(plTime::MakeFromSeconds(10)) != plGALAsyncResult::Ready)
      return false;

    plArrayPtr<const plUInt8> memory;
    plReadbackBufferLock lock = readback.LockBuffer(memory);
    if (!lock)
      return false;

    out_data = memory;
    return true;
  }
} // namespace

const plParticleTypeGPU* plGameEngineTestParticleRuntime::GetLoweredType() const
{
  PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());

  plParticleEffectInstance* pEffect = GetEffect();
  if (!PL_TEST_BOOL(pEffect != nullptr && !pEffect->GetParticleSystems().IsEmpty() && pEffect->GetParticleSystems()[0] != nullptr))
    return nullptr;

  const plParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[0];
  if (!PL_TEST_BOOL_MSG(pSystem->IsLoweredToGPU(), "The system did not lower to the GPU"))
    return nullptr;

  const plParticleTypeGPU* pType = pSystem->GetTypes().IsEmpty() ? nullptr : plDynamicCast<const plParticleTypeGPU*>(pSystem->GetTypes()[0]);
  PL_TEST_BOOL(pType != nullptr);
  return pType;
}

plTestAppRun plGameEngineTestParticleRuntime::RunGPUSortAliveCount()
{
  if (m_iFrame < 6)
    return plTestAppRun::Continue;

  // D26: the sort covers the alive particles padded to a power of two, not the pool
  const plParticleTypeGPU* pType = GetLoweredType();
  if (pType == nullptr)
    return plTestAppRun::Quit;

  plDynamicArray<plUInt8> drawArgs, sortArgs, keyData;
  if (!PL_TEST_BOOL(ReadBack(pType->GetDrawArgsBuffer(), drawArgs) && ReadBack(pType->GetSortArgsBuffer(), sortArgs) && ReadBack(pType->GetSortKeyBuffer(), keyData)))
    return plTestAppRun::Quit;

  const plUInt32 uiAlive = reinterpret_cast<const plUInt32*>(drawArgs.GetData())[0] / 6; // quads: 6 vertices per particle
  const plUInt32* pSortArgs = reinterpret_cast<const plUInt32*>(sortArgs.GetData());
  const plUInt32 uiSorted = pSortArgs[3];
  const plArrayPtr<const float> keys(reinterpret_cast<const float*>(keyData.GetData()), keyData.GetCount() / sizeof(float));

  plLog::Info("GPU sort: {} alive, {} sorted, {} slots, {} groups", uiAlive, uiSorted, keys.GetCount(), pSortArgs[0]);

  // the Kill If left about a tenth of the 1000
  PL_TEST_BOOL_MSG(uiAlive > 40 && uiAlive < 200, "%u particles alive", uiAlive);
  PL_TEST_INT(uiSorted, plMath::PowerOfTwo_Ceil(uiAlive));
  PL_TEST_BOOL(uiSorted < keys.GetCount());
  PL_TEST_INT(pSortArgs[0], (uiSorted + 255) / 256);

  // the alive keys ascend (far particles first), and the padding up to the sorted count stays at the end
  plUInt32 uiOutOfOrder = 0;
  for (plUInt32 i = 1; i < uiAlive && i < keys.GetCount(); ++i)
    uiOutOfOrder += keys[i - 1] <= keys[i] ? 0 : 1;
  PL_TEST_INT(uiOutOfOrder, 0);

  for (plUInt32 i = uiAlive; i < uiSorted && i < keys.GetCount(); ++i)
    PL_TEST_BOOL(keys[i] > 1e30f);

  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestParticleRuntime::RunGPUSpyReadback()
{
  // D27: the editor's value spy on a GPU system: the kernel computes Size x 2 per particle, read back late
  if (m_iFrame < 3)
    return plTestAppRun::Continue;

  plParticleTypeGPU* pType = const_cast<plParticleTypeGPU*>(GetLoweredType());
  if (pType == nullptr)
    return plTestAppRun::Quit;

  if (m_iFrame == 3)
  {
    AST ast;
    plParticleProgramData spy;
    PL_TEST_BOOL(CompileProgram(ast, ast.CreateBinaryOperator(AST::NodeType::Multiply, ProgramInput(ast, "Size", plProcessingStream::DataType::Half), Number(ast, 2.0f)), plProcessingStream::DataType::Float, "Spy", spy));
    pType->SetSpyProgram(&spy);
    PL_TEST_BOOL(pType->IsSpying() && pType->m_Programs.HasSpy());
    return plTestAppRun::Continue;
  }

  // the readback arrives a few frames later
  plDynamicArray<float> values;
  if (!pType->FetchSpyValues(values))
  {
    if (!PL_TEST_BOOL_MSG(m_iFrame < 60, "No spy readback arrived within 57 frames"))
      return plTestAppRun::Quit;
    return plTestAppRun::Continue;
  }

  // the CPU's sizes, doubled
  plDynamicArray<float> expected;
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());
    const plParticleSystemInstance* pSystem = GetEffect()->GetParticleSystems()[0];
    const plProcessingStream* pSize = pSystem->QueryStream("Size", plProcessingStream::DataType::Half);
    if (!PL_TEST_BOOL(pSize != nullptr))
      return plTestAppRun::Quit;

    for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
      expected.PushBack((float)pSize->GetData<plFloat16>()[i] * 2.0f);
  }

  // an empty readback is the first frame after the program arrived, before the kernel ran it
  if (values.IsEmpty())
    return plTestAppRun::Continue;

  PL_TEST_INT(values.GetCount(), expected.GetCount());
  if (values.GetCount() == expected.GetCount())
  {
    values.Sort();
    expected.Sort();

    plUInt32 uiMismatches = 0;
    for (plUInt32 i = 0; i < values.GetCount(); ++i)
      uiMismatches += plMath::IsEqual(values[i], expected[i], 1e-5f) ? 0 : 1;
    PL_TEST_INT(uiMismatches, 0);
  }

  pType->SetSpyProgram(nullptr);
  PL_TEST_BOOL(!pType->m_Programs.HasSpy());
  return plTestAppRun::Quit;
}

//////////////////////////////////////////////////////////////////////////
// Events and chaining (P6)

namespace
{
  /// A system that spawns uiCount particles per event (or per frame) when the effect queues szEvent.
  plParticleSystemDescriptor* AddOnEventSystem(plParticleEffectDescriptor& ref_effect, const char* szEvent, plUInt32 uiCount, plParticleOnEventSpawn::Enum mode)
  {
    plParticleSystemDescriptor* pSystem = AllocateModule<plParticleSystemDescriptor>();
    pSystem->m_LifeTime.m_Value = plTime::MakeFromSeconds(10.0f);
    pSystem->m_LifeTime.m_fVariance = 0.0f;

    plParticleEmitterFactory_OnEvent* pOnEvent = AllocateModule<plParticleEmitterFactory_OnEvent>();
    pOnEvent->m_sEventName = szEvent;
    pOnEvent->m_uiSpawnCountMin = uiCount;
    pOnEvent->m_uiSpawnCountRange = 0;
    pOnEvent->m_SpawnMode = mode;

    auto pEmitters = static_cast<const plAbstractArrayProperty*>(plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
    plParticleEmitterFactory* pEmitter = pOnEvent;
    pEmitters->Insert(pSystem, 0, &pEmitter);

    ref_effect.AddParticleSystem(pSystem);
    return pSystem;
  }

  plParticleEmitterFactory_OnEvent* GetOnEvent(plParticleSystemDescriptor* pSystem)
  {
    return static_cast<plParticleEmitterFactory_OnEvent*>(pSystem->GetEmitterFactories()[0]);
  }

  /// An Initialize Set Attribute whose program computes the target's value.
  void AddInitializeProgram(plParticleSystemDescriptor* pSystem, plParticleSinkTarget::Enum target, AST& ref_ast, AST::Node* pValue)
  {
    plParticleInitializerFactory_SetAttribute* pSet = AllocateModule<plParticleInitializerFactory_SetAttribute>();
    pSet->m_Settings.m_Target = target;

    const bool bColor = target == plParticleSinkTarget::Color;
    const bool bVector = target == plParticleSinkTarget::Position || target == plParticleSinkTarget::Velocity;
    const plProcessingStream::DataType type = bColor ? plProcessingStream::DataType::Float4 : (bVector ? plProcessingStream::DataType::Float3 : plProcessingStream::DataType::Float);
    PL_TEST_BOOL(CompileProgram(ref_ast, pValue, type, bColor ? "Color" : (bVector ? "Vector" : "Number"), pSet->m_Settings.m_Program));

    pSystem->AddInitializerFactory(pSet);
  }

  AST::Node* Vector3(AST& ref_ast, float x, float y, float z)
  {
    AST::Node* components[] = {Number(ref_ast, x), Number(ref_ast, y), Number(ref_ast, z)};
    return ref_ast.CreateConstructorCall(AST::DataType::Float3, components);
  }

  /// Initialize: Position = (Random x 10, 0, 5), so every particle of the system has its own x.
  void AddSpreadPositions(plParticleSystemDescriptor* pSystem)
  {
    AST ast;
    AST::Node* components[] = {ast.CreateBinaryOperator(AST::NodeType::Multiply, ParticleRandom(ast, 3), Number(ast, 10.0f)), Number(ast, 0.0f), Number(ast, 5.0f)};
    AddInitializeProgram(pSystem, plParticleSinkTarget::Position, ast, ast.CreateConstructorCall(AST::DataType::Float3, components));
  }

  const plParticleSystemInstance* GetSystem(const plParticleEffectInstance* pEffect, plUInt32 uiIndex)
  {
    if (pEffect == nullptr || uiIndex >= pEffect->GetParticleSystems().GetCount())
      return nullptr;

    return pEffect->GetParticleSystems()[uiIndex];
  }

  constexpr const char* s_szBlackboardEntry = "ParticleTestWind";
} // namespace

void plGameEngineTestParticleRuntime::SetupEventTest(plInt32 iIdentifier, plParticleEffectDescriptor& ref_desc)
{
  m_uiMaxEventsRaised = 0;
  m_uiMaxEventsDropped = 0;
  m_uiEventLatency = 0;
  m_bSparksLowered = false;

  switch (iIdentifier)
  {
    case SubTests::EventSpawnPerEvent:
    {
      // 10 particles die where they spawned and raise Pop; each Pop spawns 3 particles there. The
      // new ones read the event position as an input: Color = (x / 10, z / 10, 0, 1).
      // two Raise on Death blocks: every death raises both names
      plParticleSystemDescriptor* pSparks = AddBurstSystem(ref_desc, 10, 0.05f);
      pSparks->m_sOnDeathEvent = "Pop;Glow";
      AddSpreadPositions(pSparks);

      plParticleSystemDescriptor* pSplash = AddOnEventSystem(ref_desc, "Pop", 3, plParticleOnEventSpawn::PerEvent);

      AST ast;
      AST::Node* pEventPosition = ProgramInput(ast, "EventPosition", plProcessingStream::DataType::Float3);
      AST::Node* components[] = {
        ast.CreateBinaryOperator(AST::NodeType::Multiply, Component(ast, pEventPosition, AST::VectorComponent::X), Number(ast, 0.1f)),
        ast.CreateBinaryOperator(AST::NodeType::Multiply, Component(ast, pEventPosition, AST::VectorComponent::Z), Number(ast, 0.1f)),
        Number(ast, 0.0f), Number(ast, 1.0f)};
      AddInitializeProgram(pSplash, plParticleSinkTarget::Color, ast, ast.CreateConstructorCall(AST::DataType::Float4, components));
      break;
    }

    case SubTests::EventInherit:
    {
      // the raising particles: colour (1, 0.5, 0.25, 1), size 2, velocity (0, 0, 2); the new ones keep half the velocity
      plParticleSystemDescriptor* pSparks = AddBurstSystem(ref_desc, 4, 0.05f);
      pSparks->m_sOnDeathEvent = "Pop";

      {
        AST ast;
        AST::Node* components[] = {Number(ast, 1.0f), Number(ast, 0.5f), Number(ast, 0.25f), Number(ast, 1.0f)};
        AddInitializeProgram(pSparks, plParticleSinkTarget::Color, ast, ast.CreateConstructorCall(AST::DataType::Float4, components));
      }
      {
        AST ast;
        AddInitializeProgram(pSparks, plParticleSinkTarget::Size, ast, Number(ast, 2.0f));
      }
      {
        AST ast;
        AddInitializeProgram(pSparks, plParticleSinkTarget::Velocity, ast, Vector3(ast, 0.0f, 0.0f, 2.0f));
      }

      plParticleSystemDescriptor* pSplash = AddOnEventSystem(ref_desc, "Pop", 2, plParticleOnEventSpawn::PerEvent);
      GetOnEvent(pSplash)->m_fInheritVelocity = 0.5f;
      GetOnEvent(pSplash)->m_bInheritColor = true;
      GetOnEvent(pSplash)->m_bInheritSize = true;
      break;
    }

    case SubTests::EventQueueDrops:
    {
      // 300 deaths in one step, but the queue takes 256 a frame
      plParticleSystemDescriptor* pSparks = AddBurstSystem(ref_desc, 300, 0.05f);
      pSparks->m_sOnDeathEvent = "Pop";
      AddOnEventSystem(ref_desc, "Pop", 1, plParticleOnEventSpawn::PerEvent);
      break;
    }

    case SubTests::EventOncePerFrame:
    {
      // as before 1.6: 10 deaths in one step spawn one batch of 3
      CheckOnEventVersion2();

      plParticleSystemDescriptor* pSparks = AddBurstSystem(ref_desc, 10, 0.05f);
      pSparks->m_sOnDeathEvent = "Pop";
      AddOnEventSystem(ref_desc, "Pop", 3, plParticleOnEventSpawn::OncePerFrame);
      break;
    }

    case SubTests::GPUEvents:
    {
      // 20 GPU particles die and raise Pop from the kernel; a CPU system spawns 2 per event where they died
      plParticleSystemDescriptor* pSparks = AddBurstSystem(ref_desc, 20, 0.1f);
      pSparks->m_SimulationTarget = plParticleSimulationTarget::GPU;
      pSparks->m_sOnDeathEvent = "Pop";
      AddSpreadPositions(pSparks);
      pSparks->AddTypeFactory(AllocateModule<plParticleTypeQuadFactory>());

      // a per-frame wire (Drag = Time x 0.01) configures the lowered type again every frame, which must keep its events
      {
        pSparks->AddBehaviorFactory(AllocateModule<plParticleBehaviorFactory_Drag>());

        AST ast;
        AST::Node* pTime = ProgramInput(ast, "Time", plProcessingStream::DataType::Float);
        ast.m_OutputNodes.PushBack(ast.CreateOutput({plMakeHashedString("Out0"), plProcessingStream::DataType::Float}, ast.CreateBinaryOperator(AST::NodeType::Multiply, pTime, Number(ast, 0.01f))));

        plExpressionCompiler compiler;
        PL_TEST_BOOL(compiler.Compile(ast, pSparks->m_FrameProgram.m_ByteCode).Succeeded());

        plParticleDrivenProperty& driven = pSparks->m_FrameProgram.m_Driven.ExpandAndGetRef();
        driven.m_Module = plParticleDrivenProperty::Module::Behavior;
        driven.m_uiModuleIndex = 0;
        driven.m_uiOutput = 0;
        driven.m_sProperty = "Drag";
      }

      AddOnEventSystem(ref_desc, "Pop", 2, plParticleOnEventSpawn::PerEvent);
      break;
    }

    case SubTests::BlackboardEntry:
    {
      // Set Attribute Size = 2 x (the entry, or 0.5 without it), per frame, as the editor builds a Blackboard Entry node
      plBlackboard::GetOrCreateGlobal(plMakeHashedString("Global"))->SetEntryValue(plMakeHashedString("ParticleTestWind"), 3.0f);

      plParticleSystemDescriptor* pSystem = AddBurstSystem(ref_desc, 4, 10.0f);

      plParticleBehaviorFactory_SetAttribute* pSet = AllocateModule<plParticleBehaviorFactory_SetAttribute>();
      pSet->m_Attribute = plParticleAttribute::Size;
      pSet->m_Mode = plParticleSetAttributeMode::Replace;
      pSet->m_fValue = 1.0f;
      pSystem->AddBehaviorFactory(pSet);

      plStringBuilder sValue, sFound;
      plParticleMakeBlackboardInput("Global", s_szBlackboardEntry, false, sValue);
      plParticleMakeBlackboardInput("Global", s_szBlackboardEntry, true, sFound);

      AST ast;
      AST::Node* pValue = ProgramInput(ast, sValue, plProcessingStream::DataType::Float);
      AST::Node* pFound = ProgramInput(ast, sFound, plProcessingStream::DataType::Float);
      AST::Node* pDefault = Number(ast, 0.5f);
      AST::Node* pDelta = ast.CreateBinaryOperator(AST::NodeType::Subtract, pValue, pDefault);
      AST::Node* pPicked = ast.CreateBinaryOperator(AST::NodeType::Add, pDefault, ast.CreateBinaryOperator(AST::NodeType::Multiply, pDelta, pFound));
      AST::Node* pSize = ast.CreateBinaryOperator(AST::NodeType::Multiply, pPicked, Number(ast, 2.0f));
      ast.m_OutputNodes.PushBack(ast.CreateOutput({plMakeHashedString("Out0"), plProcessingStream::DataType::Float}, pSize));

      plExpressionCompiler compiler;
      PL_TEST_BOOL(compiler.Compile(ast, pSystem->m_FrameProgram.m_ByteCode).Succeeded());

      plParticleDrivenProperty& driven = pSystem->m_FrameProgram.m_Driven.ExpandAndGetRef();
      driven.m_Module = plParticleDrivenProperty::Module::Behavior;
      driven.m_uiModuleIndex = 0;
      driven.m_uiOutput = 0;
      driven.m_sProperty = "Value";
      break;
    }

    case SubTests::VelocityConeAxis:
    {
      // a narrow cone turned by a constant axis (effect space), and one turned by a per-particle axis (simulation space)
      for (plUInt32 i = 0; i < 2; ++i)
      {
        plParticleSystemDescriptor* pSystem = AddBurstSystem(ref_desc, 20, 10.0f);

        plParticleInitializerFactory_VelocityCone* pCone = AllocateModule<plParticleInitializerFactory_VelocityCone>();
        pCone->m_Angle = plAngle::MakeFromDegree(1.0f);
        pCone->m_Speed.m_Value = 1.0f;
        pCone->m_Speed.m_fVariance = 0.0f;

        if (i == 0)
        {
          pCone->m_vAxis.Set(0, 1, 0);
        }
        else
        {
          AST ast;
          PL_TEST_BOOL(CompileProgram(ast, Vector3(ast, 1.0f, 0.0f, 0.0f), plProcessingStream::DataType::Float3, "Axis", pCone->m_Program));
        }

        pSystem->AddInitializerFactory(pCone);
      }
      break;
    }
  }
}

void plGameEngineTestParticleRuntime::CheckOnEventVersion2()
{
  // an On Event saved before 1.6 (version 2) loads as Once Per Frame
  plDefaultMemoryStreamStorage storage;
  {
    plMemoryStreamWriter writer(&storage);
    writer << (plUInt8)2;
    writer << plString("Pop");
    writer << (plUInt32)3;
    writer << (plUInt32)0;
    writer << plString();
  }

  plParticleEmitterFactory_OnEvent factory;
  plMemoryStreamReader reader(&storage);
  factory.Load(reader);

  PL_TEST_BOOL(factory.m_SpawnMode == plParticleOnEventSpawn::OncePerFrame);
  PL_TEST_INT(factory.m_uiSpawnCountMin, 3);
  PL_TEST_BOOL(factory.m_sEventName == "Pop");

  // a new block spawns per event
  PL_TEST_BOOL(plParticleEmitterFactory_OnEvent().m_SpawnMode == plParticleOnEventSpawn::PerEvent);
}

plTestAppRun plGameEngineTestParticleRuntime::RunEventTest(plInt32 iIdentifier)
{
  const plParticleEffectInstance* pEffect = GetEffect();
  const plParticleSystemInstance* pSparks = GetSystem(pEffect, 0);
  const plParticleSystemInstance* pSplash = GetSystem(pEffect, 1);

  // the raising system goes away once its particles are gone, so what it reports is kept while it lives
  if (pSparks != nullptr)
  {
    m_uiMaxEventsRaised = plMath::Max(m_uiMaxEventsRaised, pSparks->GetEventsRaised());
    m_uiMaxEventsDropped = plMath::Max(m_uiMaxEventsDropped, pSparks->GetEventsDropped());

    if (const plParticleTypeGPU* pType = pSparks->GetTypes().IsEmpty() ? nullptr : plDynamicCast<const plParticleTypeGPU*>(pSparks->GetTypes()[0]))
    {
      m_bSparksLowered = true;
      m_uiEventLatency = plMath::Max(m_uiEventLatency, pType->GetEventLatencyFrames());
    }
  }

  // GPU events come back a few frames late
  const plInt32 iCheckFrame = iIdentifier == SubTests::GPUEvents ? 90 : 40;
  if (m_iFrame < iCheckFrame)
    return plTestAppRun::Continue;

  if (iIdentifier == SubTests::VelocityConeAxis)
  {
    // the constant axis is in the effect space, the per-particle one in simulation space
    const plVec3 vExpected[2] = {pEffect->GetTransform().m_qRotation * plVec3(0, 1, 0), plVec3(1, 0, 0)};

    for (plUInt32 s = 0; s < 2; ++s)
    {
      const plParticleSystemInstance* pSystem = GetSystem(pEffect, s);
      const plProcessingStream* pVelocity = pSystem != nullptr ? pSystem->QueryStream("Velocity", plProcessingStream::DataType::Float3) : nullptr;
      if (!PL_TEST_BOOL(pVelocity != nullptr && pSystem->GetNumActiveParticles() == 20))
        continue;

      plUInt32 uiOff = 0;
      for (plUInt64 i = 0; i < pSystem->GetNumActiveParticles(); ++i)
        uiOff += pVelocity->GetData<plVec3>()[i].Dot(vExpected[s]) > 0.99f ? 0 : 1;

      PL_TEST_INT(uiOff, 0);
    }

    return plTestAppRun::Quit;
  }

  if (!PL_TEST_BOOL(pSplash != nullptr && (pSparks == nullptr || pSparks->GetNumActiveParticles() == 0)))
    return plTestAppRun::Quit;

  const plUInt64 uiSplash = pSplash->GetNumActiveParticles();
  const plProcessingStream* pPosition = pSplash->QueryStream("Position", plProcessingStream::DataType::Float4);

  switch (iIdentifier)
  {
    case SubTests::EventSpawnPerEvent:
    case SubTests::GPUEvents:
    {
      const plUInt32 uiEvents = iIdentifier == SubTests::GPUEvents ? 20 : 10;
      const plUInt32 uiPerEvent = iIdentifier == SubTests::GPUEvents ? 2 : 3;
      PL_TEST_INT(uiSplash, uiEvents * uiPerEvent);

      if (iIdentifier == SubTests::GPUEvents)
      {
        PL_TEST_BOOL(m_bSparksLowered);
        PL_TEST_BOOL_MSG(m_uiEventLatency >= 2, "events came back %u frames late", m_uiEventLatency);
      }

      // every new particle sits where a Pop happened: z = 5, an x of its own per event, uiPerEvent particles each
      if (!PL_TEST_BOOL(pPosition != nullptr && uiSplash == uiEvents * uiPerEvent))
        break;

      plDynamicArray<float> xs;
      for (plUInt64 i = 0; i < uiSplash; ++i)
      {
        const plVec4 p = pPosition->GetData<plVec4>()[i];
        PL_TEST_FLOAT(p.z, 5.0f, 0.001f);
        PL_TEST_BOOL(p.x >= 0.0f && p.x <= 10.0f);
        xs.PushBack(p.x);
      }

      xs.Sort();
      plUInt32 uiGroups = 0;
      for (plUInt32 i = 0; i < xs.GetCount(); i += uiPerEvent)
      {
        ++uiGroups;
        PL_TEST_FLOAT(xs[i + uiPerEvent - 1], xs[i], 0.0001f);
      }
      PL_TEST_INT(uiGroups, uiEvents);

      // the Event Position input reached the Initialize program
      if (iIdentifier == SubTests::EventSpawnPerEvent)
      {
        const plProcessingStream* pColor = pSplash->QueryStream("Color", plProcessingStream::DataType::Half4);
        if (PL_TEST_BOOL(pColor != nullptr))
        {
          for (plUInt64 i = 0; i < uiSplash; ++i)
          {
            const plColor c = pColor->GetData<plColorLinear16f>()[i].ToLinearFloat();
            PL_TEST_FLOAT(c.r * 10.0f, pPosition->GetData<plVec4>()[i].x, 0.02f);
            PL_TEST_FLOAT(c.g, 0.5f, 0.001f);
          }
        }

        PL_TEST_INT(pEffect->GetTotalEventsRaised(), 20);
        PL_TEST_INT(pEffect->GetTotalEventsDropped(), 0);
      }
      break;
    }

    case SubTests::EventInherit:
    {
      PL_TEST_INT(uiSplash, 8);

      const plProcessingStream* pVelocity = pSplash->QueryStream("Velocity", plProcessingStream::DataType::Float3);
      const plProcessingStream* pColor = pSplash->QueryStream("Color", plProcessingStream::DataType::Half4);
      const plProcessingStream* pSize = pSplash->QueryStream("Size", plProcessingStream::DataType::Half);
      if (!PL_TEST_BOOL(pVelocity != nullptr && pColor != nullptr && pSize != nullptr))
        break;

      for (plUInt64 i = 0; i < uiSplash; ++i)
      {
        PL_TEST_VEC3(pVelocity->GetData<plVec3>()[i], plVec3(0, 0, 1), 0.001f);

        const plColor c = pColor->GetData<plColorLinear16f>()[i].ToLinearFloat();
        PL_TEST_FLOAT(c.r, 1.0f, 0.001f);
        PL_TEST_FLOAT(c.g, 0.5f, 0.001f);
        PL_TEST_FLOAT(c.b, 0.25f, 0.001f);
        PL_TEST_FLOAT((float)pSize->GetData<plFloat16>()[i], 2.0f, 0.001f);
      }
      break;
    }

    case SubTests::EventQueueDrops:
      // the full queue drops the rest, and says how many
      PL_TEST_INT(uiSplash, plParticleEffectInstance::s_uiMaxEventsPerFrame);
      PL_TEST_INT(pEffect->GetTotalEventsRaised(), 300);
      PL_TEST_INT(pEffect->GetTotalEventsDropped(), 300 - plParticleEffectInstance::s_uiMaxEventsPerFrame);
      break;

    case SubTests::EventOncePerFrame:
      PL_TEST_INT(uiSplash, 3);
      break;
  }

  return plTestAppRun::Quit;
}

plTestAppRun plGameEngineTestParticleRuntime::RunBlackboardEntry()
{
  auto CheckSizes = [&](float fExpected)
  {
    PL_LOCK(m_pOwnApplication->GetWorld()->GetReadMarker());

    const plParticleSystemInstance* pSystem = GetSystem(GetEffect(), 0);
    const plProcessingStream* pSize = pSystem != nullptr ? pSystem->QueryStream("Size", plProcessingStream::DataType::Half) : nullptr;
    if (!PL_TEST_BOOL(pSize != nullptr && pSystem->GetNumActiveParticles() == 4))
      return;

    for (plUInt64 i = 0; i < 4; ++i)
      PL_TEST_FLOAT((float)pSize->GetData<plFloat16>()[i], fExpected, 0.001f);
  };

  plSharedPtr<plBlackboard> pBlackboard = plBlackboard::GetOrCreateGlobal(plMakeHashedString("Global"));

  if (m_iFrame == 5)
  {
    // the entry: 2 x 3
    CheckSizes(6.0f);
    pBlackboard->RemoveEntry(plMakeHashedString("ParticleTestWind"));
    return plTestAppRun::Continue;
  }

  if (m_iFrame == 10)
  {
    // no entry: 2 x the default
    CheckSizes(1.0f);
    return plTestAppRun::Quit;
  }

  return plTestAppRun::Continue;
}
