#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Utils/Blackboard.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Types/VarianceTypes.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Emitter/ParticleEmitter.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>
#include <ParticlePlugin/System/ParticleFrameProgram.h>
#include <ParticlePlugin/System/ParticleProgram.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/GPU/ParticleGPULowering.h>
#include <ParticlePlugin/Type/GPU/ParticleTypeGPU.h>
#include <ParticlePlugin/Type/ParticleType.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>

// defined with the emitters, which apply it to every spawn count
extern plCVarFloat cvar_ParticlesSpawnCountScale;

void plParticleDrivenProperty::Save(plStreamWriter& inout_stream) const
{
  inout_stream << (plUInt8)m_Module;
  inout_stream << m_uiModuleIndex;
  inout_stream << m_uiOutput;
  inout_stream << m_sProperty;
}

void plParticleDrivenProperty::Load(plStreamReader& inout_stream)
{
  plUInt8 uiModule = 0;
  inout_stream >> uiModule;
  m_Module = (Module)uiModule;
  inout_stream >> m_uiModuleIndex;
  inout_stream >> m_uiOutput;
  inout_stream >> m_sProperty;
}

void plParticleFrameProgram::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 2;
  inout_stream << uiVersion;

  inout_stream << m_Driven.GetCount();
  for (const plParticleDrivenProperty& driven : m_Driven)
  {
    driven.Save(inout_stream);
  }

  if (!m_Driven.IsEmpty())
  {
    m_ByteCode.Save(inout_stream).AssertSuccess("Failed to save a particle frame program");
    m_Assets.Save(inout_stream);
  }
}

void plParticleFrameProgram::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  PL_ASSERT_DEV(uiVersion <= 2, "Invalid particle frame program version {0}", uiVersion);

  plUInt32 uiCount = 0;
  inout_stream >> uiCount;
  m_Driven.SetCount(uiCount);

  for (plParticleDrivenProperty& driven : m_Driven)
  {
    driven.Load(inout_stream);
  }

  m_ByteCode.Clear();
  m_Assets = plParticleProgramAssets();

  if (uiCount > 0 && m_ByteCode.Load(inout_stream).Failed())
  {
    plLog::Error("Failed to load a particle frame program; its driven properties keep their authored values.");
    m_Driven.Clear();
    return;
  }

  if (uiCount > 0 && uiVersion >= 2)
  {
    m_Assets.Load(inout_stream);
  }
}

//////////////////////////////////////////////////////////////////////////

plVariant plParticleMakeDrivenValue(const plVariant& current, const plVec4& vValue)
{
  const float x = vValue.x;

  switch (current.GetType())
  {
    case plVariantType::Bool:
      return plVariant(x != 0.0f);

    case plVariantType::Int8:
    case plVariantType::UInt8:
    case plVariantType::Int16:
    case plVariantType::UInt16:
    case plVariantType::Int32:
    case plVariantType::UInt32:
    case plVariantType::Int64:
    case plVariantType::UInt64:
    {
      // counts and indices: round, and never wrap a negative into a huge unsigned value
      double fRounded = plMath::Round((double)x);
      if (current.GetType() == plVariantType::UInt8 || current.GetType() == plVariantType::UInt16 || current.GetType() == plVariantType::UInt32 || current.GetType() == plVariantType::UInt64)
        fRounded = plMath::Max(fRounded, 0.0);

      return plVariant(fRounded).ConvertTo(current.GetType());
    }

    case plVariantType::Float:
      return plVariant(x);

    case plVariantType::Double:
      return plVariant((double)x);

    case plVariantType::Time:
      return plVariant(plTime::MakeFromSeconds(x));

    case plVariantType::Angle:
      return plVariant(plAngle::MakeFromDegree(x));

    case plVariantType::Vector2:
      return plVariant(plVec2(vValue.x, vValue.y));

    case plVariantType::Vector3:
      return plVariant(plVec3(vValue.x, vValue.y, vValue.z));

    case plVariantType::Color:
      return plVariant(plColor(vValue.x, vValue.y, vValue.z, vValue.w));

    case plVariantType::ColorGamma:
      return plVariant(plColorGammaUB(plColor(vValue.x, vValue.y, vValue.z, vValue.w)));

    default:
      break;
  }

  // a variance pair takes the value and keeps its authored variance
  if (current.IsA<plVarianceTypeFloat>())
  {
    plVarianceTypeFloat value = current.Get<plVarianceTypeFloat>();
    value.m_Value = x;
    return plVariant(value);
  }

  if (current.IsA<plVarianceTypeTime>())
  {
    plVarianceTypeTime value = current.Get<plVarianceTypeTime>();
    value.m_Value = plTime::MakeFromSeconds(x);
    return plVariant(value);
  }

  if (current.IsA<plVarianceTypeAngle>())
  {
    plVarianceTypeAngle value = current.Get<plVarianceTypeAngle>();
    value.m_Value = plAngle::MakeFromDegree(x);
    return plVariant(value);
  }

  return plVariant();
}

//////////////////////////////////////////////////////////////////////////

namespace
{
  plUInt32 GetComponentCount(plProcessingStream::DataType type)
  {
    switch (type)
    {
      case plProcessingStream::DataType::Float2:
        return 2;
      case plProcessingStream::DataType::Float3:
        return 3;
      case plProcessingStream::DataType::Float4:
        return 4;
      default:
        return 1;
    }
  }

  /// A deep copy through the factory's own serialization, which every module factory implements.
  template <typename FactoryBase>
  plReflectedClass* CloneFactory(const FactoryBase* pSource)
  {
    FactoryBase* pClone = pSource->GetDynamicRTTI()->GetAllocator()->template Allocate<FactoryBase>();

    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    pSource->Save(writer);

    plMemoryStreamReader reader(&storage);
    pClone->Load(reader);

    return pClone;
  }
} // namespace

plParticleFrameProgramRunner::plParticleFrameProgramRunner()
{
  m_VM.RegisterFunction(plParticleProgramTables::s_GradientFunc);
  m_VM.RegisterFunction(plParticleProgramTables::s_CurveFunc);
}

plParticleFrameProgramRunner::~plParticleFrameProgramRunner()
{
  Clear();
}

void plParticleFrameProgramRunner::Clear()
{
  for (Clone& clone : m_Clones)
  {
    clone.m_pFactory->GetDynamicRTTI()->GetAllocator()->Deallocate(clone.m_pFactory);
  }

  m_Clones.Clear();
  m_Driven.Clear();
  m_Inputs.Clear();
  m_InputStreams.Clear();
  m_OutputStreams.Clear();
  m_InputData.Clear();
  m_OutputData.Clear();

  m_pProgram = nullptr;
  m_pTemplate = nullptr;
  m_pSystem = nullptr;
}

void plParticleFrameProgramRunner::Configure(const plParticleSystemDescriptor* pTemplate, plParticleSystemInstance* pSystem, bool bLoweredToGPU)
{
  Clear();

  const plParticleFrameProgram& program = pTemplate->m_FrameProgram;
  if (program.IsEmpty())
    return;

  m_pProgram = &program;
  m_pTemplate = pTemplate;
  m_pSystem = pSystem;
  m_Tables.Bake(program.m_Assets);

  // ---- inputs: classify each by name, then lay out the data before any stream points into it ----
  const auto inputDescs = program.m_ByteCode.GetInputs();
  plUInt32 uiInputFloats = 0;

  for (const plExpression::StreamDesc& desc : inputDescs)
  {
    Input& input = m_Inputs.ExpandAndGetRef();
    input.m_sSource = plParticleSplitInputComponent(desc.m_sName.GetView(), input.m_iComponent);
    input.m_uiComponents = GetComponentCount(desc.m_DataType);
    input.m_uiOffset = uiInputFloats;
    uiInputFloats += 4 * input.m_uiComponents;

    plVec4 vUnused;
    if (!plParticleReadFrameInput(input.m_sSource, pSystem, plTime::MakeZero(), vUnused))
    {
      plLog::Warning("Particle frame program reads an unknown input '{}'; it stays zero.", desc.m_sName);
    }
  }

  m_InputData.SetCount(uiInputFloats);

  for (plUInt32 i = 0; i < inputDescs.GetCount(); ++i)
  {
    const plExpression::StreamDesc& desc = inputDescs[i];
    const plUInt32 uiComponents = GetComponentCount(desc.m_DataType);
    plArrayPtr<plUInt8> data = m_InputData.GetArrayPtr().GetSubArray(m_Inputs[i].m_uiOffset, 4 * uiComponents).ToByteArray();
    m_InputStreams.PushBack(plProcessingStream(desc.m_sName, data, desc.m_DataType, (plUInt16)(uiComponents * sizeof(float))));
  }

  // ---- outputs: vector outputs come split into one output per component ("Out0.x") ----
  const auto outputDescs = program.m_ByteCode.GetOutputs();
  plHybridArray<plUInt32, 8> outputOffsets;
  plUInt32 uiOutputFloats = 0;

  for (const plExpression::StreamDesc& desc : outputDescs)
  {
    outputOffsets.PushBack(uiOutputFloats);
    uiOutputFloats += 4 * GetComponentCount(desc.m_DataType);
  }

  m_OutputData.SetCount(uiOutputFloats);

  for (plUInt32 i = 0; i < outputDescs.GetCount(); ++i)
  {
    const plExpression::StreamDesc& desc = outputDescs[i];
    const plUInt32 uiComponents = GetComponentCount(desc.m_DataType);
    plArrayPtr<plUInt8> data = m_OutputData.GetArrayPtr().GetSubArray(outputOffsets[i], 4 * uiComponents).ToByteArray();
    m_OutputStreams.PushBack(plProcessingStream(desc.m_sName, data, desc.m_DataType, (plUInt16)(uiComponents * sizeof(float))));
  }

  // ---- driven properties: one private factory copy per driven block ----
  for (const plParticleDrivenProperty& src : program.m_Driven)
  {
    // outputs are named by the index the editor gave them; the compiler may store them in another order
    plStringBuilder sOutputName;
    sOutputName.SetFormat("Out{}", src.m_uiOutput);

    plUInt32 componentOffsets[4] = {};
    plUInt32 uiComponents = 0;

    for (plUInt32 i = 0; i < outputDescs.GetCount(); ++i)
    {
      plInt32 iComponent = -1;
      if (plParticleSplitInputComponent(outputDescs[i].m_sName.GetView(), iComponent) != sOutputName)
        continue;

      if (iComponent < 0)
      {
        uiComponents = GetComponentCount(outputDescs[i].m_DataType);
        for (plUInt32 c = 0; c < uiComponents; ++c)
          componentOffsets[c] = outputOffsets[i] + c;
        break;
      }

      componentOffsets[iComponent] = outputOffsets[i];
      uiComponents = plMath::Max(uiComponents, (plUInt32)iComponent + 1);
    }

    if (uiComponents == 0)
      continue;

    plUInt32 uiClone = plInvalidIndex;
    for (plUInt32 i = 0; i < m_Clones.GetCount(); ++i)
    {
      if (m_Clones[i].m_Module == src.m_Module && m_Clones[i].m_uiModuleIndex == src.m_uiModuleIndex)
      {
        uiClone = i;
        break;
      }
    }

    if (uiClone == plInvalidIndex)
    {
      Clone clone;
      clone.m_Module = src.m_Module;
      clone.m_uiModuleIndex = src.m_uiModuleIndex;

      const plUInt32 i = src.m_uiModuleIndex;

      switch (src.m_Module)
      {
        case plParticleDrivenProperty::Module::Emitter:
          if (i < pTemplate->GetEmitterFactories().GetCount() && i < pSystem->m_Emitters.GetCount())
          {
            clone.m_pFactory = CloneFactory(pTemplate->GetEmitterFactories()[i]);
            clone.m_pModule = pSystem->m_Emitters[i];
          }
          break;

        case plParticleDrivenProperty::Module::Initializer:
          if (i < pTemplate->GetInitializerFactories().GetCount() && i < pSystem->m_Initializers.GetCount())
          {
            clone.m_pFactory = CloneFactory(pTemplate->GetInitializerFactories()[i]);
            clone.m_pModule = pSystem->m_Initializers[i];
          }
          break;

        case plParticleDrivenProperty::Module::Behavior:
          if (i < pTemplate->GetBehaviorFactories().GetCount())
          {
            clone.m_pFactory = CloneFactory(pTemplate->GetBehaviorFactories()[i]);
            clone.m_pModule = (!bLoweredToGPU && i < pSystem->m_Behaviors.GetCount()) ? pSystem->m_Behaviors[i] : nullptr;
          }
          break;

        case plParticleDrivenProperty::Module::Type:
          if (i < pTemplate->GetTypeFactories().GetCount())
          {
            clone.m_pFactory = CloneFactory(pTemplate->GetTypeFactories()[i]);
            clone.m_pModule = (!bLoweredToGPU && i < pSystem->m_Types.GetCount()) ? pSystem->m_Types[i] : nullptr;
          }
          break;
      }

      if (clone.m_pFactory == nullptr)
      {
        plLog::Warning("Particle frame program drives a block that does not exist (module {} #{}).", (plUInt32)src.m_Module, src.m_uiModuleIndex);
        continue;
      }

      uiClone = m_Clones.GetCount();
      m_Clones.PushBack(clone);
    }

    const plAbstractProperty* pProp = m_Clones[uiClone].m_pFactory->GetDynamicRTTI()->FindPropertyByName(src.m_sProperty);
    if (pProp == nullptr || pProp->GetCategory() != plPropertyCategory::Member)
    {
      plLog::Warning("Particle frame program drives an unknown property '{}'.", src.m_sProperty);
      continue;
    }

    Driven& driven = m_Driven.ExpandAndGetRef();
    driven.m_pProperty = static_cast<const plAbstractMemberProperty*>(pProp);
    driven.m_uiClone = uiClone;
    driven.m_uiComponents = uiComponents;
    for (plUInt32 c = 0; c < 4; ++c)
      driven.m_uiOutputOffsets[c] = componentOffsets[c];
  }

  if (m_Driven.IsEmpty())
  {
    Clear();
  }
}

void plParticleMakeBlackboardInput(plStringView sBlackboard, plStringView sEntry, bool bFound, plStringBuilder& out_sName)
{
  out_sName.Set(bFound ? plParticleFrameInput::BlackboardFoundPrefix : plParticleFrameInput::BlackboardPrefix, sBlackboard.IsEmpty() ? plStringView("Global") : sBlackboard, "|", sEntry);
}

plMutex& plParticleGetBlackboardMutex()
{
  static plMutex s_Mutex;
  return s_Mutex;
}

namespace
{
  /// Reads "<blackboard>|<entry>" into out_vValue: a number into x, a vector into xyz, a colour into rgba,
  /// a bool as 0 or 1. False when the blackboard or the entry does not exist, or holds something else.
  bool ReadBlackboardEntry(plStringView sPath, plVec4& out_vValue)
  {
    const char* szSeparator = sPath.FindSubString("|");
    if (szSeparator == nullptr)
      return false;

    const plTempHashedString sBlackboard(plStringView(sPath.GetStartPointer(), szSeparator));
    const plTempHashedString sEntry(plStringView(szSeparator + 1, sPath.GetEndPointer()));

    PL_LOCK(plParticleGetBlackboardMutex());

    plSharedPtr<plBlackboard> pBlackboard = plBlackboard::FindGlobal(sBlackboard);
    if (pBlackboard == nullptr)
      return false;

    const plBlackboard::Entry* pEntry = pBlackboard->GetEntry(sEntry);
    if (pEntry == nullptr)
      return false;

    const plVariant& value = pEntry->m_Value;

    if (value.IsA<plVec3>())
    {
      out_vValue = value.Get<plVec3>().GetAsVec4(0);
    }
    else if (value.IsA<plColor>())
    {
      const plColor color = value.Get<plColor>();
      out_vValue.Set(color.r, color.g, color.b, color.a);
    }
    else if (value.IsA<bool>())
    {
      out_vValue.x = value.Get<bool>() ? 1.0f : 0.0f;
    }
    else if (value.IsNumber())
    {
      out_vValue.x = value.ConvertTo<float>();
    }
    else
    {
      return false;
    }

    return true;
  }
} // namespace

bool plParticleHasBlackboardEntry(plStringView sPath)
{
  plVec4 vIgnored = plVec4::MakeZero();
  return ReadBlackboardEntry(sPath, vIgnored);
}

bool plParticleReadFrameInput(plStringView sName, const plParticleSystemInstance* pSystem, const plTime& diff, plVec4& out_vValue)
{
  const plParticleEffectInstance* pEffect = pSystem->GetOwnerEffect();
  const plTransform& transform = pSystem->GetTransform();

  out_vValue.SetZero();

  if (sName.StartsWith(plParticleFrameInput::FloatParameterPrefix))
  {
    const plStringView sParameter = sName.GetSubString(plStringUtils::GetStringElementCount(plParticleFrameInput::FloatParameterPrefix), plInvalidIndex);
    out_vValue.x = pEffect->GetFloatParameter(plTempHashedString(sParameter), 0.0f);
  }
  else if (sName.StartsWith(plParticleFrameInput::ColorParameterPrefix))
  {
    const plStringView sParameter = sName.GetSubString(plStringUtils::GetStringElementCount(plParticleFrameInput::ColorParameterPrefix), plInvalidIndex);
    const plColor color = pEffect->GetColorParameter(plTempHashedString(sParameter), plColor::White);
    out_vValue.Set(color.r, color.g, color.b, color.a);
  }
  else if (sName.StartsWith(plParticleFrameInput::BlackboardFoundPrefix))
  {
    plVec4 vIgnored = plVec4::MakeZero();
    out_vValue.x = ReadBlackboardEntry(sName.GetSubString(plStringUtils::GetStringElementCount(plParticleFrameInput::BlackboardFoundPrefix), plInvalidIndex), vIgnored) ? 1.0f : 0.0f;
  }
  else if (sName.StartsWith(plParticleFrameInput::BlackboardPrefix))
  {
    if (!ReadBlackboardEntry(sName.GetSubString(plStringUtils::GetStringElementCount(plParticleFrameInput::BlackboardPrefix), plInvalidIndex), out_vValue))
      out_vValue.SetZero();
  }
  else if (sName == plParticleFrameInput::Time)
    out_vValue.x = (float)pEffect->GetTotalEffectLifeTime().GetSeconds();
  else if (sName == plParticleFrameInput::DeltaTime)
    out_vValue.x = (float)diff.GetSeconds();
  else if (sName == plParticleFrameInput::EffectPosition)
    out_vValue = transform.m_vPosition.GetAsVec4(0);
  else if (sName == plParticleFrameInput::EffectForward)
    out_vValue = (transform.m_qRotation * plVec3(1, 0, 0)).GetAsVec4(0);
  else if (sName == plParticleFrameInput::EffectUp)
    out_vValue = (transform.m_qRotation * plVec3(0, 0, 1)).GetAsVec4(0);
  else if (sName == plParticleFrameInput::EffectVelocity)
    out_vValue = pEffect->GetVelocity().GetAsVec4(0);
  else if (sName == plParticleFrameInput::CameraPosition)
    out_vValue = pSystem->GetOwnerWorldModule()->GetMainCameraPosition().GetAsVec4(0);
  else if (sName == plParticleFrameInput::CameraDistance)
    out_vValue.x = (pEffect->GetTransform().m_vPosition - pSystem->GetOwnerWorldModule()->GetMainCameraPosition()).GetLength();
  else if (sName == plParticleFrameInput::ParticleCount)
    out_vValue.x = (float)pSystem->GetNumActiveParticles();
  else if (sName == plParticleFrameInput::SpawnScale)
    out_vValue.x = plMath::Max((float)cvar_ParticlesSpawnCountScale, 0.0f) * pEffect->GetDynamicSpawnScale();
  else
    return false;

  return true;
}

void plParticleFrameProgramRunner::FillInputs(const plTime& diff)
{
  for (const Input& input : m_Inputs)
  {
    plVec4 vValue;
    plParticleReadFrameInput(input.m_sSource, m_pSystem, diff, vValue);

    if (input.m_iComponent >= 0)
    {
      m_InputData[input.m_uiOffset] = vValue.GetData()[input.m_iComponent];
      continue;
    }

    // the one instance that is evaluated: its components side by side, as the stream stride says
    for (plUInt32 c = 0; c < input.m_uiComponents; ++c)
    {
      m_InputData[input.m_uiOffset + c] = vValue.GetData()[c];
    }
  }
}

void plParticleFrameProgramRunner::Evaluate(const plTime& diff)
{
  if (m_pProgram == nullptr)
    return;

  PL_PROFILE_SCOPE("PFX: Frame Program");

  FillInputs(diff);

  if (m_VM.Execute(m_pProgram->m_ByteCode, m_InputStreams, m_OutputStreams, 1, m_Tables.GetGlobalData()).Failed())
    return;

  bool bAnyLowered = false;

  for (Driven& driven : m_Driven)
  {
    plVec4 vValue(0.0f);
    for (plUInt32 c = 0; c < driven.m_uiComponents; ++c)
    {
      vValue.GetData()[c] = m_OutputData[driven.m_uiOutputOffsets[c]];
    }

    // NaN never equals itself, so the first evaluation always applies
    if (vValue == driven.m_vLastValue)
      continue;

    driven.m_vLastValue = vValue;

    Clone& clone = m_Clones[driven.m_uiClone];
    const plVariant current = plReflectionUtils::GetMemberPropertyValue(driven.m_pProperty, clone.m_pFactory);
    const plVariant value = plParticleMakeDrivenValue(current, vValue);

    if (!value.IsValid())
      continue;

    plReflectionUtils::SetMemberPropertyValue(driven.m_pProperty, clone.m_pFactory, value);
    clone.m_bDirty = true;
  }

  for (Clone& clone : m_Clones)
  {
    if (!clone.m_bDirty)
      continue;

    clone.m_bDirty = false;

    if (clone.m_pModule == nullptr)
    {
      bAnyLowered = true;
      continue;
    }

    switch (clone.m_Module)
    {
      case plParticleDrivenProperty::Module::Emitter:
        static_cast<const plParticleEmitterFactory*>(clone.m_pFactory)->CopyEmitterProperties(static_cast<plParticleEmitter*>(clone.m_pModule), false);
        break;
      case plParticleDrivenProperty::Module::Initializer:
        static_cast<const plParticleInitializerFactory*>(clone.m_pFactory)->CopyInitializerProperties(static_cast<plParticleInitializer*>(clone.m_pModule), false);
        break;
      case plParticleDrivenProperty::Module::Behavior:
        static_cast<const plParticleBehaviorFactory*>(clone.m_pFactory)->CopyBehaviorProperties(static_cast<plParticleBehavior*>(clone.m_pModule), false);
        break;
      case plParticleDrivenProperty::Module::Type:
        static_cast<const plParticleTypeFactory*>(clone.m_pFactory)->CopyTypeProperties(static_cast<plParticleType*>(clone.m_pModule), false);
        break;
    }
  }

  if (bAnyLowered)
  {
    ApplyLowered();
  }
}

void plParticleFrameProgramRunner::ApplyLowered()
{
  if (m_pSystem->m_Types.IsEmpty())
    return;

  plParticleTypeGPU* pType = plDynamicCast<plParticleTypeGPU*>(m_pSystem->m_Types[0]);
  if (pType == nullptr)
    return;

  // the template's lists with every driven block replaced by its driven copy
  plHybridArray<const plParticleBehaviorFactory*, 8> behaviors;
  for (const plParticleBehaviorFactory* pFactory : m_pTemplate->GetBehaviorFactories())
    behaviors.PushBack(pFactory);

  plHybridArray<const plParticleTypeFactory*, 4> types;
  for (const plParticleTypeFactory* pFactory : m_pTemplate->GetTypeFactories())
    types.PushBack(pFactory);

  for (const Clone& clone : m_Clones)
  {
    if (clone.m_Module == plParticleDrivenProperty::Module::Behavior && clone.m_uiModuleIndex < behaviors.GetCount())
      behaviors[clone.m_uiModuleIndex] = static_cast<const plParticleBehaviorFactory*>(clone.m_pFactory);
    else if (clone.m_Module == plParticleDrivenProperty::Module::Type && clone.m_uiModuleIndex < types.GetCount())
      types[clone.m_uiModuleIndex] = static_cast<const plParticleTypeFactory*>(clone.m_pFactory);
  }

  plParticleGPULowering::ConfigureLoweredType(pType, behaviors.GetArrayPtr(), types.GetArrayPtr(), m_pSystem);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_System_ParticleFrameProgram);
