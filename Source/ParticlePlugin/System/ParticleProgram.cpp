#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Curves/ColorGradientResource.h>
#include <Core/Curves/Curve1DResource.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/System/ParticleFrameProgram.h>
#include <ParticlePlugin/System/ParticleProgram.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticlePerParticleAttribute, 1, plRTTIDefaultAllocator<plParticlePerParticleAttribute>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void plParticleProgramAssets::Save(plStreamWriter& inout_stream) const
{
  inout_stream.WriteArray(m_Gradients).AssertSuccess();
  inout_stream.WriteArray(m_Curves).AssertSuccess();
}

void plParticleProgramAssets::Load(plStreamReader& inout_stream)
{
  inout_stream.ReadArray(m_Gradients).AssertSuccess();
  inout_stream.ReadArray(m_Curves).AssertSuccess();
}

//////////////////////////////////////////////////////////////////////////

namespace
{
  static const char* s_szTablesKey = "PfxTables";

  const plParticleProgramTables* GetTables(const plExpression::GlobalData& globalData)
  {
    const plVariant* pValue = globalData.GetValue(plTempHashedString(s_szTablesKey));
    return pValue != nullptr ? reinterpret_cast<const plParticleProgramTables*>((plUInt64)pValue->Get<plUInt64>()) : nullptr;
  }

  plResult ValidateTables(const plExpression::GlobalData& globalData)
  {
    return GetTables(globalData) != nullptr ? PL_SUCCESS : PL_FAILURE;
  }

  void SampleGradientFunc(plExpression::Inputs inputs, plExpression::Output output, const plExpression::GlobalData& globalData)
  {
    const plParticleProgramTables* pTables = GetTables(globalData);

    const plExpression::Register* pT = inputs[0].GetPtr();
    const plExpression::Register* pTEnd = inputs[0].GetEndPtr();
    const plExpression::Register* pSlot = inputs[1].GetPtr();
    const plExpression::Register* pComponent = inputs[2].GetPtr();
    plExpression::Register* pOutput = output.GetPtr();

    // four instances per register; the tables are read one lane at a time
    float t[4], result[4];
    plInt32 slot[4], component[4];

    for (; pT < pTEnd; ++pT, ++pSlot, ++pComponent, ++pOutput)
    {
      pT->f.Store<4>(t);
      pSlot->i.Store<4>(slot);
      pComponent->i.Store<4>(component);

      for (plUInt32 i = 0; i < 4; ++i)
        result[i] = pTables->SampleGradient((plUInt32)slot[i], (plUInt32)component[i], t[i]);

      pOutput->f.Load<4>(result);
    }
  }

  void SampleCurveFunc(plExpression::Inputs inputs, plExpression::Output output, const plExpression::GlobalData& globalData)
  {
    const plParticleProgramTables* pTables = GetTables(globalData);

    const plExpression::Register* pT = inputs[0].GetPtr();
    const plExpression::Register* pTEnd = inputs[0].GetEndPtr();
    const plExpression::Register* pSlot = inputs[1].GetPtr();
    plExpression::Register* pOutput = output.GetPtr();

    float t[4], result[4];
    plInt32 slot[4];

    for (; pT < pTEnd; ++pT, ++pSlot, ++pOutput)
    {
      pT->f.Store<4>(t);
      pSlot->i.Store<4>(slot);

      for (plUInt32 i = 0; i < 4; ++i)
        result[i] = pTables->SampleCurve((plUInt32)slot[i], t[i]);

      pOutput->f.Load<4>(result);
    }
  }

  static const plEnum<plExpression::RegisterType> s_GradientInputTypes[] = {plExpression::RegisterType::Float, plExpression::RegisterType::Int, plExpression::RegisterType::Int};
  static const plEnum<plExpression::RegisterType> s_CurveInputTypes[] = {plExpression::RegisterType::Float, plExpression::RegisterType::Int};

  /// Linear interpolation into a table of s_uiTableSize entries covering t = 0 to 1.
  template <typename T>
  T SampleTable(const T* pTable, float t)
  {
    const float fPos = plMath::Saturate(t) * (plParticleProgramTables::s_uiTableSize - 1);
    const plUInt32 i0 = plMath::Min((plUInt32)fPos, plParticleProgramTables::s_uiTableSize - 2);
    return plMath::Lerp(pTable[i0], pTable[i0 + 1], fPos - (float)i0);
  }
} // namespace

plExpressionFunction plParticleProgramTables::s_GradientFunc = {
  {plMakeHashedString("PfxGradient"), plExpression::FunctionDesc::TypeList(s_GradientInputTypes), 3, plExpression::RegisterType::Float},
  &SampleGradientFunc,
  &ValidateTables,
};

plExpressionFunction plParticleProgramTables::s_CurveFunc = {
  {plMakeHashedString("PfxCurve"), plExpression::FunctionDesc::TypeList(s_CurveInputTypes), 2, plExpression::RegisterType::Float},
  &SampleCurveFunc,
  &ValidateTables,
};

void plParticleProgramTables::Bake(const plParticleProgramAssets& assets)
{
  constexpr plUInt32 n = s_uiTableSize;

  m_Gradients.SetCount(assets.m_Gradients.GetCount() * n, plVec4(1.0f));
  m_Curves.SetCount(assets.m_Curves.GetCount() * n, 0.0f);

  for (plUInt32 uiSlot = 0; uiSlot < assets.m_Gradients.GetCount(); ++uiSlot)
  {
    plColorGradientResourceHandle hGradient = plResourceManager::LoadResource<plColorGradientResource>(assets.m_Gradients[uiSlot]);
    plResourceLock<plColorGradientResource> pGradient(hGradient, plResourceAcquireMode::BlockTillLoaded);

    if (pGradient.GetAcquireResult() == plResourceAcquireResult::MissingFallback)
    {
      plLog::Warning("Particle program: gradient '{}' is missing; it reads as white.", assets.m_Gradients[uiSlot]);
      continue;
    }

    const plColorGradient& gradient = pGradient->GetDescriptor().m_Gradient;

    double fMin = 0.0, fMax = 1.0;
    gradient.GetExtents(fMin, fMax);

    for (plUInt32 i = 0; i < n; ++i)
    {
      plColor color;
      gradient.Evaluate(plMath::Lerp(fMin, fMax, (double)i / (n - 1)), color);
      m_Gradients[uiSlot * n + i].Set(color.r, color.g, color.b, color.a);
    }
  }

  for (plUInt32 uiSlot = 0; uiSlot < assets.m_Curves.GetCount(); ++uiSlot)
  {
    plCurve1DResourceHandle hCurve = plResourceManager::LoadResource<plCurve1DResource>(assets.m_Curves[uiSlot]);
    plResourceLock<plCurve1DResource> pCurve(hCurve, plResourceAcquireMode::BlockTillLoaded);

    if (pCurve.GetAcquireResult() == plResourceAcquireResult::MissingFallback || pCurve->GetDescriptor().m_Curves.IsEmpty())
    {
      plLog::Warning("Particle program: curve '{}' is missing; it reads as zero.", assets.m_Curves[uiSlot]);
      continue;
    }

    const plCurve1D& curve = pCurve->GetDescriptor().m_Curves[0];

    for (plUInt32 i = 0; i < n; ++i)
    {
      m_Curves[uiSlot * n + i] = (float)curve.Evaluate(curve.ConvertNormalizedPos((double)i / (n - 1)));
    }
  }

  plHashedString sKey;
  sKey.Assign(s_szTablesKey);

  m_GlobalData.Clear();
  m_GlobalData.Insert(sKey, plVariant(reinterpret_cast<plUInt64>(this)));
}

float plParticleProgramTables::SampleGradient(plUInt32 uiSlot, plUInt32 uiComponent, float t) const
{
  if ((uiSlot + 1) * s_uiTableSize > m_Gradients.GetCount() || uiComponent > 3)
    return 1.0f;

  return SampleTable(m_Gradients.GetData() + uiSlot * s_uiTableSize, t).GetData()[uiComponent];
}

float plParticleProgramTables::SampleCurve(plUInt32 uiSlot, float t) const
{
  if ((uiSlot + 1) * s_uiTableSize > m_Curves.GetCount())
    return 0.0f;

  return SampleTable(m_Curves.GetData() + uiSlot * s_uiTableSize, t);
}

//////////////////////////////////////////////////////////////////////////

void plParticleProgramData::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 2;
  inout_stream << uiVersion;

  inout_stream << m_Outputs.GetCount();
  for (const plString& sOutput : m_Outputs)
  {
    inout_stream << sOutput;
  }

  if (!m_Outputs.IsEmpty())
  {
    m_ByteCode.Save(inout_stream).AssertSuccess("Failed to save a particle program");
    m_Assets.Save(inout_stream);
  }
}

void plParticleProgramData::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  PL_ASSERT_DEV(uiVersion <= 2, "Invalid particle program version {0}", uiVersion);

  plUInt32 uiCount = 0;
  inout_stream >> uiCount;
  m_Outputs.SetCount(uiCount);

  for (plString& sOutput : m_Outputs)
  {
    inout_stream >> sOutput;
  }

  m_ByteCode.Clear();
  m_Assets = plParticleProgramAssets();

  if (uiCount > 0 && m_ByteCode.Load(inout_stream).Failed())
  {
    plLog::Error("Failed to load a particle program; the block uses its own values.");
    m_Outputs.Clear();
    return;
  }

  if (uiCount > 0 && uiVersion >= 2)
  {
    m_Assets.Load(inout_stream);
  }
}

//////////////////////////////////////////////////////////////////////////

namespace
{
  plUInt32 GetComponentCount(plProcessingStream::DataType type)
  {
    switch (type)
    {
      case plProcessingStream::DataType::Float2:
      case plProcessingStream::DataType::Half2:
      case plProcessingStream::DataType::Int2:
        return 2;
      case plProcessingStream::DataType::Float3:
      case plProcessingStream::DataType::Half3:
      case plProcessingStream::DataType::Int3:
        return 3;
      case plProcessingStream::DataType::Float4:
      case plProcessingStream::DataType::Half4:
      case plProcessingStream::DataType::Int4:
        return 4;
      default:
        return 1;
    }
  }

  bool IsFrameInputName(plStringView sName)
  {
    return sName.StartsWith(plParticleFrameInput::FloatParameterPrefix) || sName.StartsWith(plParticleFrameInput::ColorParameterPrefix) ||
           sName.StartsWith(plParticleFrameInput::BlackboardPrefix) || sName.StartsWith(plParticleFrameInput::BlackboardFoundPrefix) ||
           sName == plParticleFrameInput::Time || sName == plParticleFrameInput::DeltaTime || sName == plParticleFrameInput::EffectPosition ||
           sName == plParticleFrameInput::EffectForward || sName == plParticleFrameInput::EffectUp || sName == plParticleFrameInput::EffectVelocity ||
           sName == plParticleFrameInput::CameraPosition || sName == plParticleFrameInput::CameraDistance || sName == plParticleFrameInput::ParticleCount ||
           sName == plParticleFrameInput::SpawnScale;
  }

  /// The type of the whole particle stream behind a (possibly split) input of the given element type.
  plProcessingStream::DataType GetParticleStreamType(plStringView sStream, plProcessingStream::DataType elementType, bool bSplit)
  {
    if (sStream == "Position")
      return plProcessingStream::DataType::Float4;
    if (sStream == "Velocity" || sStream == "Axis" || sStream == "LastPosition" || sStream == "EventPosition" || sStream == "EventVelocity" || sStream == "EventNormal")
      return plProcessingStream::DataType::Float3;
    if (sStream == "Color")
      return plProcessingStream::DataType::Half4;
    if (sStream == "LifeTime")
      return plProcessingStream::DataType::Float2;

    if (!bSplit)
      return elementType;

    // custom attributes: a split Half input is a colour (Half4), a split Float input a vector (Float3)
    return elementType == plProcessingStream::DataType::Half ? plProcessingStream::DataType::Half4 : plProcessingStream::DataType::Float3;
  }
} // namespace

bool plParticleIsFrameInput(plStringView sName)
{
  return IsFrameInputName(sName);
}

plStringView plParticleSplitInputComponent(plStringView sName, plInt32& out_iComponent)
{
  out_iComponent = -1;

  const plUInt32 uiBytes = sName.GetElementCount();
  if (uiBytes < 3 || sName.GetStartPointer()[uiBytes - 2] != '.')
    return sName;

  const char c = sName.GetStartPointer()[uiBytes - 1];
  const plInt32 iComponent = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1;
  if (iComponent < 0)
    return sName;

  const plStringView sBase(sName.GetStartPointer(), uiBytes - 2);

  // "Param.x" is a float parameter named x, not a component of "Param"
  plStringBuilder sPrefix;
  sPrefix.Set(sBase, ".");
  if (sPrefix == plParticleFrameInput::FloatParameterPrefix || sPrefix == plParticleFrameInput::ColorParameterPrefix || sPrefix == "Custom.")
    return sName;

  out_iComponent = iComponent;
  return sBase;
}

plParticleProgramRunner::plParticleProgramRunner()
{
  m_VM.RegisterFunction(plParticleProgramTables::s_GradientFunc);
  m_VM.RegisterFunction(plParticleProgramTables::s_CurveFunc);
}

plParticleProgramRunner::~plParticleProgramRunner() = default;

void plParticleProgramRunner::Configure(const plParticleProgramData* pData)
{
  m_pData = nullptr;
  m_Streams.Clear();
  m_Inputs.Clear();
  m_Outputs.Clear();
  m_OutputTargets.Clear();

  if (pData == nullptr || pData->IsEmpty())
    return;

  m_pData = pData;
  m_Tables.Bake(pData->m_Assets);

  // the compiler splits every vector input into one input per component; each reads one particle stream
  const auto inputs = pData->m_ByteCode.GetInputs();

  for (const plExpression::StreamDesc& desc : inputs)
  {
    if (GetComponentCount(desc.m_DataType) != 1)
    {
      plLog::Error("Particle program input '{}' is not scalar; the block uses its own values.", desc.m_sName);
      Configure(nullptr);
      return;
    }

    Input& input = m_Inputs.ExpandAndGetRef();
    input.m_sName = desc.m_sName;
    input.m_DataType = desc.m_DataType;

    const plStringView sSource = plParticleSplitInputComponent(desc.m_sName.GetView(), input.m_iComponent);
    input.m_sSource = sSource;
    input.m_bFrameInput = IsFrameInputName(sSource);

    if (input.m_bFrameInput)
      continue;

    input.m_uiStream = m_Streams.GetCount();
    for (plUInt32 s = 0; s < m_Streams.GetCount(); ++s)
    {
      if (m_Streams[s].m_sName == sSource)
      {
        input.m_uiStream = s;
        break;
      }
    }

    if (input.m_uiStream == m_Streams.GetCount())
    {
      Stream& stream = m_Streams.ExpandAndGetRef();
      stream.m_sName = sSource;
      stream.m_DataType = GetParticleStreamType(sSource, desc.m_DataType, input.m_iComponent >= 0);
    }
  }

  // outputs are split the same way: "Out0.x" is component x of output 0, named by the index the editor gave it
  const auto outputs = pData->m_ByteCode.GetOutputs();
  m_OutputTargets.SetCount(outputs.GetCount());

  for (const plExpression::StreamDesc& desc : outputs)
  {
    if (desc.m_DataType != plProcessingStream::DataType::Float)
    {
      plLog::Error("Particle program output '{}' is not a float; the block uses its own values.", desc.m_sName);
      Configure(nullptr);
      return;
    }
  }

  for (plUInt32 i = 0; i < pData->m_Outputs.GetCount(); ++i)
  {
    plStringBuilder sName;
    sName.SetFormat("Out{}", i);

    Output output;
    output.m_sProperty = pData->m_Outputs[i];
    output.m_uiComponents = 0;

    for (plUInt32 j = 0; j < outputs.GetCount(); ++j)
    {
      plInt32 iComponent = -1;
      if (plParticleSplitInputComponent(outputs[j].m_sName.GetView(), iComponent) != sName)
        continue;

      m_OutputTargets[j].m_uiOutput = m_Outputs.GetCount();
      m_OutputTargets[j].m_uiComponent = (plUInt32)plMath::Max(iComponent, 0);

      const plUInt32 uiComponents = iComponent < 0 ? GetComponentCount(outputs[j].m_DataType) : (plUInt32)iComponent + 1;
      output.m_uiComponents = plMath::Max(output.m_uiComponents, uiComponents);
    }

    if (output.m_uiComponents > 0)
    {
      m_Outputs.PushBack(output);
    }
  }
}

bool plParticleProgramRunner::Run(const plParticleSystemInstance* pSystem, plUInt64 uiStart, plUInt64 uiCount, const plTime& diff)
{
  if (m_pData == nullptr || uiCount == 0)
    return false;

  PL_PROFILE_SCOPE("PFX: Particle Program");

  const plUInt32 uiNum = (plUInt32)uiCount;

  // per-frame inputs: their one value for every particle
  plUInt32 uiFrameInputs = 0;
  for (const Input& input : m_Inputs)
  {
    if (input.m_bFrameInput)
      ++uiFrameInputs;
  }

  m_FrameData.SetCountUninitialized(uiFrameInputs * uiNum);
  m_InputViews.Clear();

  // the views are in the byte code's order and already scalar, so the VM neither maps nor splits them
  plUInt32 uiFrameOffset = 0;
  for (const Input& input : m_Inputs)
  {
    if (input.m_bFrameInput)
    {
      plVec4 vValue(0.0f);
      plParticleReadFrameInput(input.m_sSource, pSystem, diff, vValue);
      const float fValue = vValue.GetData()[plMath::Max(input.m_iComponent, 0)];

      float* pData = m_FrameData.GetData() + uiFrameOffset;
      for (plUInt32 i = 0; i < uiNum; ++i)
      {
        pData[i] = fValue;
      }

      plArrayPtr<plUInt8> data = plArrayPtr<float>(pData, uiNum).ToByteArray();
      m_InputViews.PushBack(plProcessingStream(input.m_sName, data, input.m_DataType, (plUInt16)sizeof(float)));
      uiFrameOffset += uiNum;
    }
    else
    {
      const plProcessingStream* pStream = m_Streams[input.m_uiStream].m_pStream;
      if (pStream == nullptr)
        return false;

      // one component of the particles to process, at the stream's own layout
      const plUInt64 uiStride = pStream->GetElementStride();
      const plUInt64 uiElementSize = plProcessingStream::GetDataTypeSize(input.m_DataType);
      plUInt8* pBase = static_cast<plUInt8*>(const_cast<void*>(pStream->GetData())) + uiStart * uiStride + plMath::Max(input.m_iComponent, 0) * uiElementSize;
      plArrayPtr<plUInt8> data(pBase, (plUInt32)((uiCount - 1) * uiStride + uiElementSize));
      m_InputViews.PushBack(plProcessingStream(input.m_sName, data, input.m_DataType, (plUInt16)uiStride));
    }
  }

  // outputs: each property's components side by side per particle, as GetOutput hands them out
  plUInt32 uiOutputFloats = 0;
  for (Output& output : m_Outputs)
  {
    output.m_uiOffset = uiOutputFloats;
    uiOutputFloats += uiNum * output.m_uiComponents;
  }

  const plUInt32 uiScratchOffset = uiOutputFloats;
  uiOutputFloats += uiNum;

  m_OutputData.SetCountUninitialized(uiOutputFloats);
  m_OutputViews.Clear();

  const auto outputDescs = m_pData->m_ByteCode.GetOutputs();
  for (plUInt32 j = 0; j < outputDescs.GetCount(); ++j)
  {
    const OutputTarget& target = m_OutputTargets[j];

    plUInt32 uiOffset = uiScratchOffset;
    plUInt32 uiStride = 1;

    if (target.m_uiOutput != plInvalidIndex)
    {
      const Output& output = m_Outputs[target.m_uiOutput];
      uiOffset = output.m_uiOffset + target.m_uiComponent;
      uiStride = output.m_uiComponents;
    }

    plArrayPtr<plUInt8> data = plArrayPtr<float>(m_OutputData.GetData() + uiOffset, (uiNum - 1) * uiStride + 1).ToByteArray();
    m_OutputViews.PushBack(plProcessingStream(outputDescs[j].m_sName, data, outputDescs[j].m_DataType, (plUInt16)(uiStride * sizeof(float))));
  }

  return m_VM.Execute(m_pData->m_ByteCode, m_InputViews, m_OutputViews, uiNum, m_Tables.GetGlobalData(), plExpressionVM::Flags::BestPerformance).Succeeded();
}

const float* plParticleProgramRunner::GetOutput(plStringView sProperty, plUInt32& out_uiComponents) const
{
  for (const Output& output : m_Outputs)
  {
    if (output.m_sProperty == sProperty)
    {
      out_uiComponents = output.m_uiComponents;
      return m_OutputData.GetData() + output.m_uiOffset;
    }
  }

  return nullptr;
}

const float* plParticleProgramRunner::RunProperty(const plParticleSystemInstance* pSystem, plUInt64 uiCount, const plTime& diff, plStringView sProperty, plUInt32& out_uiComponents)
{
  out_uiComponents = 0;

  if (!IsActive() || uiCount == 0 || !Run(pSystem, 0, uiCount, diff))
    return nullptr;

  return GetOutput(sProperty, out_uiComponents);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_System_ParticleProgram);
