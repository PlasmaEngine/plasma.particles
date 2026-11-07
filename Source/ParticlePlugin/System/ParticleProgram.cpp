#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/Curves/ColorGradientResource.h>
#include <Core/Curves/Curve1DResource.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <ParticlePlugin/System/ParticleFrameProgram.h>
#include <ParticlePlugin/System/ParticleProgram.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticlePerParticleAttribute, 1, plRTTIDefaultAllocator<plParticlePerParticleAttribute>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plUInt32 plParticleProgramAssets::GetNumGradientSlots() const
{
  return m_Gradients.GetCount() + m_InlineGradients.GetCount() / plParticleProgramTables::s_uiTableSize;
}

plUInt32 plParticleProgramAssets::GetNumCurveSlots() const
{
  return m_Curves.GetCount() + m_InlineCurves.GetCount() / plParticleProgramTables::s_uiTableSize;
}

void plParticleProgramAssets::Save(plStreamWriter& inout_stream) const
{
  inout_stream.WriteArray(m_Gradients).AssertSuccess();
  inout_stream.WriteArray(m_Curves).AssertSuccess();
  inout_stream.WriteArray(m_CurveNormalized).AssertSuccess();
  inout_stream.WriteArray(m_InlineGradients).AssertSuccess();
  inout_stream.WriteArray(m_InlineCurves).AssertSuccess();
}

void plParticleProgramAssets::Load(plStreamReader& inout_stream, bool bInlineTables)
{
  inout_stream.ReadArray(m_Gradients).AssertSuccess();
  inout_stream.ReadArray(m_Curves).AssertSuccess();

  m_CurveNormalized.Clear();
  m_InlineGradients.Clear();
  m_InlineCurves.Clear();

  if (bInlineTables)
  {
    inout_stream.ReadArray(m_CurveNormalized).AssertSuccess();
    inout_stream.ReadArray(m_InlineGradients).AssertSuccess();
    inout_stream.ReadArray(m_InlineCurves).AssertSuccess();
  }

  m_pTables.Clear();
  PreloadAssets();
}

void plParticleProgramAssets::PreloadAssets()
{
  // queued now, so the tables rarely wait for them when the first system that runs the program is created
  m_hPreloadedGradients.Clear();
  m_hPreloadedCurves.Clear();

  for (const plString& sGradient : m_Gradients)
  {
    if (sGradient.IsEmpty())
      continue;

    m_hPreloadedGradients.PushBack(plResourceManager::LoadResource<plColorGradientResource>(sGradient));
    plResourceManager::PreloadResource(m_hPreloadedGradients.PeekBack());
  }

  for (const plString& sCurve : m_Curves)
  {
    if (sCurve.IsEmpty())
      continue;

    m_hPreloadedCurves.PushBack(plResourceManager::LoadResource<plCurve1DResource>(sCurve));
    plResourceManager::PreloadResource(m_hPreloadedCurves.PeekBack());
  }
}

namespace
{
  /// Guards the lazy bake of shared program tables, which several update tasks may ask for at once.
  plMutex& GetTablesMutex()
  {
    static plMutex s_Mutex;
    return s_Mutex;
  }
} // namespace

plSharedPtr<plParticleProgramTables> plParticleProgramAssets::GetTables() const
{
  PL_LOCK(GetTablesMutex());

  if (m_pTables == nullptr || m_pTables->IsOutdated())
  {
    plSharedPtr<plParticleProgramTables> pTables = PL_DEFAULT_NEW(plParticleProgramTables);
    pTables->Bake(*this);
    m_pTables = pTables;
  }

  return m_pTables;
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

  const plUInt32 uiGradientAssets = assets.m_Gradients.GetCount();
  const plUInt32 uiCurveAssets = assets.m_Curves.GetCount();

  m_Gradients.SetCount(uiGradientAssets * n, plVec4(1.0f));
  m_Curves.SetCount(uiCurveAssets * n, 0.0f);
  m_hGradients.Clear();
  m_hCurves.Clear();
  m_ChangeCounters.Clear();

  for (plUInt32 uiSlot = 0; uiSlot < uiGradientAssets; ++uiSlot)
  {
    plColorGradientResourceHandle hGradient = plResourceManager::LoadResource<plColorGradientResource>(assets.m_Gradients[uiSlot]);
    plResourceLock<plColorGradientResource> pGradient(hGradient, plResourceAcquireMode::BlockTillLoaded);

    // the counter of the resource itself, not of a fallback that stands in for a missing one
    m_hGradients.PushBack(hGradient);
    m_ChangeCounters.PushBack(plResourceLock<plColorGradientResource>(hGradient, plResourceAcquireMode::PointerOnly)->GetCurrentResourceChangeCounter());

    if (pGradient.GetAcquireResult() == plResourceAcquireResult::MissingFallback)
    {
      plLog::Warning("Particle program: gradient '{}' is missing; it reads as white.", assets.m_Gradients[uiSlot]);
      continue;
    }

    BakeGradient(pGradient->GetDescriptor().m_Gradient, m_Gradients.GetArrayPtr().GetSubArray(uiSlot * n, n));
  }

  for (plUInt32 uiSlot = 0; uiSlot < uiCurveAssets; ++uiSlot)
  {
    plCurve1DResourceHandle hCurve = plResourceManager::LoadResource<plCurve1DResource>(assets.m_Curves[uiSlot]);
    plResourceLock<plCurve1DResource> pCurve(hCurve, plResourceAcquireMode::BlockTillLoaded);

    m_hCurves.PushBack(hCurve);
    m_ChangeCounters.PushBack(plResourceLock<plCurve1DResource>(hCurve, plResourceAcquireMode::PointerOnly)->GetCurrentResourceChangeCounter());

    if (pCurve.GetAcquireResult() == plResourceAcquireResult::MissingFallback || pCurve->GetDescriptor().m_Curves.IsEmpty())
    {
      plLog::Warning("Particle program: curve '{}' is missing; it reads as zero.", assets.m_Curves[uiSlot]);
      continue;
    }

    BakeCurve(pCurve->GetDescriptor().m_Curves[0], assets.IsCurveNormalized(uiSlot), m_Curves.GetArrayPtr().GetSubArray(uiSlot * n, n));
  }

  // the editor's inline tables take the slots after the assets'
  m_Gradients.PushBackRange(assets.m_InlineGradients);
  m_Curves.PushBackRange(assets.m_InlineCurves);

  plHashedString sKey;
  sKey.Assign(s_szTablesKey);

  m_GlobalData.Clear();
  m_GlobalData.Insert(sKey, plVariant(reinterpret_cast<plUInt64>(this)));
}

bool plParticleProgramTables::IsOutdated() const
{
  plUInt32 uiCounter = 0;

  for (const plColorGradientResourceHandle& hGradient : m_hGradients)
  {
    plResourceLock<plColorGradientResource> pGradient(hGradient, plResourceAcquireMode::PointerOnly);
    if (pGradient->GetCurrentResourceChangeCounter() != m_ChangeCounters[uiCounter++])
      return true;
  }

  for (const plCurve1DResourceHandle& hCurve : m_hCurves)
  {
    plResourceLock<plCurve1DResource> pCurve(hCurve, plResourceAcquireMode::PointerOnly);
    if (pCurve->GetCurrentResourceChangeCounter() != m_ChangeCounters[uiCounter++])
      return true;
  }

  return false;
}

float plParticleProgramTables::SampleCurveAsset(const plCurve1D& curve, float t, bool bNormalized)
{
  const double fValue = curve.Evaluate(curve.ConvertNormalizedPos(t));

  if (!bNormalized)
    return (float)fValue;

  double fMin = 0.0, fMax = 0.0;
  curve.QueryExtremeValues(fMin, fMax);

  // a flat curve keeps its scale whole instead of collapsing to nothing
  if (fMin >= fMax)
    return 1.0f;

  return (float)((fValue - fMin) / (fMax - fMin));
}

void plParticleProgramTables::BakeCurve(const plCurve1D& curve, bool bNormalized, plArrayPtr<float> out_table)
{
  const plUInt32 n = out_table.GetCount();

  for (plUInt32 i = 0; i < n; ++i)
    out_table[i] = SampleCurveAsset(curve, n > 1 ? (float)i / (n - 1) : 0.0f, bNormalized);
}

void plParticleProgramTables::BakeGradient(const plColorGradient& gradient, plArrayPtr<plVec4> out_table)
{
  const plUInt32 n = out_table.GetCount();

  double fMin = 0.0, fMax = 1.0;
  gradient.GetExtents(fMin, fMax);

  for (plUInt32 i = 0; i < n; ++i)
  {
    plColor color;
    gradient.Evaluate(plMath::Lerp(fMin, fMax, n > 1 ? (double)i / (n - 1) : 0.0), color);
    out_table[i].Set(color.r, color.g, color.b, color.a);
  }
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
  // version 3: the assets carry the inline tables and the curve normalization flags
  const plUInt8 uiVersion = 3;
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
  PL_ASSERT_DEV(uiVersion <= 3, "Invalid particle program version {0}", uiVersion);

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
    m_Assets.Load(inout_stream, uiVersion >= 3);
  }
}

//////////////////////////////////////////////////////////////////////////

namespace
{
  plUInt32 GetStreamTypeComponents(plProcessingStream::DataType type)
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
           sName == plParticleFrameInput::SpawnScale || sName == plParticleFrameInput::EffectRight || sName == plParticleFrameInput::EffectScale ||
           sName == plParticleFrameInput::CameraForward || sName == plParticleFrameInput::CameraRight || sName == plParticleFrameInput::CameraUp;
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
  m_pTables.Clear();
  m_Streams.Clear();
  m_Inputs.Clear();
  m_Outputs.Clear();
  m_OutputTargets.Clear();

  if (pData == nullptr || pData->IsEmpty())
    return;

  m_pData = pData;
  m_pTables = pData->m_Assets.GetTables();

  // the compiler splits every vector input into one input per component; each reads one particle stream
  const auto inputs = pData->m_ByteCode.GetInputs();

  for (const plExpression::StreamDesc& desc : inputs)
  {
    if (GetStreamTypeComponents(desc.m_DataType) != 1)
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

      const plUInt32 uiComponents = iComponent < 0 ? GetStreamTypeComponents(outputs[j].m_DataType) : (plUInt32)iComponent + 1;
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

  return m_VM.Execute(m_pData->m_ByteCode, m_InputViews, m_OutputViews, uiNum, m_pTables->GetGlobalData(), plExpressionVM::Flags::BestPerformance).Succeeded();
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

//////////////////////////////////////////////////////////////////////////

namespace plParticleProgramOps
{
  using NodeType = plExpressionAST::NodeType;
  using DataType = plExpressionAST::DataType;
  using Component = plExpressionAST::VectorComponent;

  namespace
  {
    Node* Binary(plExpressionAST& ref_ast, NodeType::Enum type, Node* a, Node* b)
    {
      return ref_ast.CreateBinaryOperator(type, a, b);
    }

    Node* Unary(plExpressionAST& ref_ast, NodeType::Enum type, Node* a)
    {
      return ref_ast.CreateUnaryOperator(type, a);
    }

    Node* Select(plExpressionAST& ref_ast, Node* pCondition, Node* pTrue, Node* pFalse)
    {
      return ref_ast.CreateTernaryOperator(NodeType::Select, pCondition, pTrue, pFalse);
    }

    Node* Swizzle(plExpressionAST& ref_ast, Node* pVector, plUInt32 uiComponent)
    {
      return ref_ast.CreateSwizzle(static_cast<Component::Enum>(uiComponent), pVector);
    }

    Node* Vector3(plExpressionAST& ref_ast, Node* x, Node* y, Node* z)
    {
      Node* components[] = {x, y, z};
      return ref_ast.CreateConstructorCall(DataType::Float3, components);
    }

    Node* IntConstant(plExpressionAST& ref_ast, plInt32 iValue)
    {
      return ref_ast.CreateConstant(plVariant(iValue), DataType::Int);
    }

    /// cos(phi) and sin(phi) around +Z at height z, as a unit vector.
    Node* DirectionAt(plExpressionAST& ref_ast, Node* pZ, Node* pPhi)
    {
      Node* pRadius = Unary(ref_ast, NodeType::Sqrt, Binary(ref_ast, NodeType::Max, Binary(ref_ast, NodeType::Subtract, Number(ref_ast, 1.0f), Binary(ref_ast, NodeType::Multiply, pZ, pZ)), Number(ref_ast, 0.0f)));
      return Vector3(ref_ast, Binary(ref_ast, NodeType::Multiply, pRadius, Unary(ref_ast, NodeType::Cos, pPhi)), Binary(ref_ast, NodeType::Multiply, pRadius, Unary(ref_ast, NodeType::Sin, pPhi)), pZ);
    }
  } // namespace

  Node* Number(plExpressionAST& ref_ast, float fValue)
  {
    return ref_ast.CreateConstant(plVariant(fValue), DataType::Float);
  }

  Node* Broadcast(plExpressionAST& ref_ast, Node* pValue, plUInt32 uiComponents)
  {
    if (uiComponents <= 1)
      return pValue;

    Node* components[4] = {pValue, pValue, pValue, pValue};
    return ref_ast.CreateConstructorCall(uiComponents == 4 ? DataType::Float4 : (uiComponents == 3 ? DataType::Float3 : DataType::Float2), plArrayPtr<Node*>(components, uiComponents));
  }

  Node* SafeDivide(plExpressionAST& ref_ast, Node* a, Node* b)
  {
    return Select(ref_ast, Binary(ref_ast, NodeType::Equal, b, Number(ref_ast, 0.0f)), Number(ref_ast, 0.0f), Binary(ref_ast, NodeType::Divide, a, b));
  }

  Node* Step(plExpressionAST& ref_ast, Node* pEdge, Node* pX)
  {
    return Select(ref_ast, Binary(ref_ast, NodeType::GreaterEqual, pX, pEdge), Number(ref_ast, 1.0f), Number(ref_ast, 0.0f));
  }

  Node* SmoothStep(plExpressionAST& ref_ast, Node* pEdge0, Node* pEdge1, Node* pX, bool bSmoother)
  {
    Node* pRange = Binary(ref_ast, NodeType::Subtract, pEdge1, pEdge0);
    Node* pRamp = Unary(ref_ast, NodeType::Saturate, Binary(ref_ast, NodeType::Divide, Binary(ref_ast, NodeType::Subtract, pX, pEdge0), pRange));

    // equal edges have no ramp to divide by; they step instead
    Node* t = Select(ref_ast, Binary(ref_ast, NodeType::Equal, pRange, Number(ref_ast, 0.0f)), Step(ref_ast, pEdge0, pX), pRamp);
    Node* tt = Binary(ref_ast, NodeType::Multiply, t, t);

    if (!bSmoother)
      return Binary(ref_ast, NodeType::Multiply, tt, Binary(ref_ast, NodeType::Subtract, Number(ref_ast, 3.0f), Binary(ref_ast, NodeType::Multiply, t, Number(ref_ast, 2.0f))));

    Node* pInner = Binary(ref_ast, NodeType::Add, Binary(ref_ast, NodeType::Multiply, t, Binary(ref_ast, NodeType::Subtract, Binary(ref_ast, NodeType::Multiply, t, Number(ref_ast, 6.0f)), Number(ref_ast, 15.0f))), Number(ref_ast, 10.0f));
    return Binary(ref_ast, NodeType::Multiply, Binary(ref_ast, NodeType::Multiply, tt, t), pInner);
  }

  Node* InverseLerp(plExpressionAST& ref_ast, Node* a, Node* b, Node* pValue, bool bClamp)
  {
    Node* t = SafeDivide(ref_ast, Binary(ref_ast, NodeType::Subtract, pValue, a), Binary(ref_ast, NodeType::Subtract, b, a));
    return bClamp ? Unary(ref_ast, NodeType::Saturate, t) : t;
  }

  Node* Remap(plExpressionAST& ref_ast, Node* pValue, Node* pInMin, Node* pInMax, Node* pOutMin, Node* pOutMax, bool bClamp)
  {
    Node* t = nullptr;

    if (pInMin->m_Type == NodeType::Constant && pInMax->m_Type == NodeType::Constant && pInMin->m_ReturnType == DataType::Float && pInMax->m_ReturnType == DataType::Float)
    {
      // a typed range folds into one multiply
      const float fInMin = static_cast<plExpressionAST::Constant*>(pInMin)->m_Value.ConvertTo<float>();
      const float fInMax = static_cast<plExpressionAST::Constant*>(pInMax)->m_Value.ConvertTo<float>();
      const float fInvRange = (fInMax != fInMin) ? 1.0f / (fInMax - fInMin) : 0.0f;
      t = Binary(ref_ast, NodeType::Multiply, Binary(ref_ast, NodeType::Subtract, pValue, pInMin), Number(ref_ast, fInvRange));
    }
    else
    {
      t = SafeDivide(ref_ast, Binary(ref_ast, NodeType::Subtract, pValue, pInMin), Binary(ref_ast, NodeType::Subtract, pInMax, pInMin));
    }

    if (bClamp)
      t = Unary(ref_ast, NodeType::Saturate, t);

    return Binary(ref_ast, NodeType::Add, pOutMin, Binary(ref_ast, NodeType::Multiply, t, Binary(ref_ast, NodeType::Subtract, pOutMax, pOutMin)));
  }

  Node* Sign(plExpressionAST& ref_ast, Node* pX)
  {
    Node* pNegative = Select(ref_ast, Binary(ref_ast, NodeType::Less, pX, Number(ref_ast, 0.0f)), Number(ref_ast, -1.0f), Number(ref_ast, 0.0f));
    return Select(ref_ast, Binary(ref_ast, NodeType::Greater, pX, Number(ref_ast, 0.0f)), Number(ref_ast, 1.0f), pNegative);
  }

  Node* ATan2(plExpressionAST& ref_ast, Node* pY, Node* pX)
  {
    const float fPi = plMath::Pi<float>();

    // atan(y / x) covers the right half; the left half is shifted by pi, and x = 0 points straight up or down
    Node* r = Unary(ref_ast, NodeType::ATan, SafeDivide(ref_ast, pY, pX));
    Node* pLeft = Select(ref_ast, Binary(ref_ast, NodeType::GreaterEqual, pY, Number(ref_ast, 0.0f)), Binary(ref_ast, NodeType::Add, r, Number(ref_ast, fPi)), Binary(ref_ast, NodeType::Subtract, r, Number(ref_ast, fPi)));
    Node* pDown = Select(ref_ast, Binary(ref_ast, NodeType::Less, pY, Number(ref_ast, 0.0f)), Number(ref_ast, -0.5f * fPi), Number(ref_ast, 0.0f));
    Node* pVertical = Select(ref_ast, Binary(ref_ast, NodeType::Greater, pY, Number(ref_ast, 0.0f)), Number(ref_ast, 0.5f * fPi), pDown);
    Node* pNotRight = Select(ref_ast, Binary(ref_ast, NodeType::Less, pX, Number(ref_ast, 0.0f)), pLeft, pVertical);

    return Select(ref_ast, Binary(ref_ast, NodeType::Greater, pX, Number(ref_ast, 0.0f)), r, pNotRight);
  }

  Node* Distance(plExpressionAST& ref_ast, Node* a, Node* b)
  {
    return Unary(ref_ast, NodeType::Length, Binary(ref_ast, NodeType::Subtract, a, b));
  }

  Node* ProjectOnto(plExpressionAST& ref_ast, Node* pVector, Node* pOnto)
  {
    Node* pScale = SafeDivide(ref_ast, Binary(ref_ast, NodeType::Dot, pVector, pOnto), Binary(ref_ast, NodeType::Dot, pOnto, pOnto));
    return Binary(ref_ast, NodeType::Multiply, pOnto, Broadcast(ref_ast, pScale, 3));
  }

  Node* RotateAboutAxis(plExpressionAST& ref_ast, Node* pVector, Node* pAxis, Node* pRadians)
  {
    // Rodrigues: v cos + (k x v) sin + k (k . v)(1 - cos), with k the unit axis
    Node* pLength = Unary(ref_ast, NodeType::Length, pAxis);
    Node* k = Binary(ref_ast, NodeType::Multiply, pAxis, Broadcast(ref_ast, SafeDivide(ref_ast, Number(ref_ast, 1.0f), pLength), 3));
    Node* c = Unary(ref_ast, NodeType::Cos, pRadians);
    Node* s = Unary(ref_ast, NodeType::Sin, pRadians);

    Node* pAlong = Binary(ref_ast, NodeType::Multiply, Binary(ref_ast, NodeType::Dot, k, pVector), Binary(ref_ast, NodeType::Subtract, Number(ref_ast, 1.0f), c));
    Node* pRotated = Binary(ref_ast, NodeType::Add, Binary(ref_ast, NodeType::Multiply, pVector, Broadcast(ref_ast, c, 3)), Binary(ref_ast, NodeType::Multiply, Binary(ref_ast, NodeType::Cross, k, pVector), Broadcast(ref_ast, s, 3)));
    pRotated = Binary(ref_ast, NodeType::Add, pRotated, Binary(ref_ast, NodeType::Multiply, k, Broadcast(ref_ast, pAlong, 3)));

    return Select(ref_ast, Binary(ref_ast, NodeType::Greater, pLength, Number(ref_ast, 0.0f)), pRotated, pVector);
  }

  Node* ParticleRandom(plExpressionAST& ref_ast, Node* pRandomSeed, plInt32 iSeed)
  {
    Node* arguments[] = {pRandomSeed, IntConstant(ref_ast, iSeed)};
    return ref_ast.CreateFunctionCall(plDefaultExpressionFunctions::s_RandomFunc.m_Desc, arguments);
  }

  plInt32 GetComponentSeed(plInt32 iSeed, plUInt32 uiComponent)
  {
    return iSeed + (plInt32)uiComponent * 7919;
  }

  Node* RandomRange(plExpressionAST& ref_ast, Node* pRandomSeed, plInt32 iSeed, Node* pMin, Node* pMax, plUInt32 uiComponents)
  {
    uiComponents = plMath::Clamp(uiComponents, 1u, 4u);

    Node* components[4] = {};
    for (plUInt32 c = 0; c < uiComponents; ++c)
      components[c] = ParticleRandom(ref_ast, pRandomSeed, GetComponentSeed(iSeed, c));

    Node* pRandom = components[0];
    if (uiComponents > 1)
      pRandom = ref_ast.CreateConstructorCall(uiComponents == 4 ? DataType::Float4 : DataType::Float3, plArrayPtr<Node*>(components, uiComponents));

    return Binary(ref_ast, NodeType::Add, pMin, Binary(ref_ast, NodeType::Multiply, Binary(ref_ast, NodeType::Subtract, pMax, pMin), pRandom));
  }

  Node* RandomInteger(plExpressionAST& ref_ast, Node* pRandomSeed, plInt32 iSeed, Node* pMin, Node* pMax)
  {
    Node* pLow = Unary(ref_ast, NodeType::Round, Binary(ref_ast, NodeType::Min, pMin, pMax));
    Node* pHigh = Unary(ref_ast, NodeType::Round, Binary(ref_ast, NodeType::Max, pMin, pMax));
    Node* pCount = Binary(ref_ast, NodeType::Add, Binary(ref_ast, NodeType::Subtract, pHigh, pLow), Number(ref_ast, 1.0f));
    Node* pValue = Unary(ref_ast, NodeType::Floor, Binary(ref_ast, NodeType::Add, pLow, Binary(ref_ast, NodeType::Multiply, ParticleRandom(ref_ast, pRandomSeed, iSeed), pCount)));

    // a random value of exactly 1 would land one past Max
    return Binary(ref_ast, NodeType::Min, pValue, pHigh);
  }

  Node* RandomVector(plExpressionAST& ref_ast, Node* pRandomSeed, plInt32 iSeed, RandomVectorShape shape, Node* pRadius, float fConeRadians)
  {
    Node* u = ParticleRandom(ref_ast, pRandomSeed, GetComponentSeed(iSeed, 0));
    Node* v = ParticleRandom(ref_ast, pRandomSeed, GetComponentSeed(iSeed, 1));
    Node* w = ParticleRandom(ref_ast, pRandomSeed, GetComponentSeed(iSeed, 2));

    auto signedUnit = [&](Node* r)
    { return Binary(ref_ast, NodeType::Subtract, Binary(ref_ast, NodeType::Multiply, r, Number(ref_ast, 2.0f)), Number(ref_ast, 1.0f)); };

    Node* pPhi = Binary(ref_ast, NodeType::Multiply, v, Number(ref_ast, 2.0f * plMath::Pi<float>()));
    Node* pUnit = nullptr;

    switch (shape)
    {
      case RandomVectorShape::InBox:
        pUnit = Vector3(ref_ast, signedUnit(u), signedUnit(v), signedUnit(w));
        break;

      case RandomVectorShape::InCone:
      {
        // z spreads evenly between cos(angle) and 1, which spreads the directions evenly over the cap
        const float fCosAngle = plMath::Cos(plAngle::MakeFromRadian(plMath::Clamp(fConeRadians, 0.0f, plMath::Pi<float>())));
        Node* pZ = Binary(ref_ast, NodeType::Subtract, Number(ref_ast, 1.0f), Binary(ref_ast, NodeType::Multiply, u, Number(ref_ast, 1.0f - fCosAngle)));
        pUnit = DirectionAt(ref_ast, pZ, pPhi);
        break;
      }

      case RandomVectorShape::InSphere:
      {
        // the cube root spreads the points evenly through the volume instead of crowding the centre
        Node* pCubeRoot = Binary(ref_ast, NodeType::Pow, Binary(ref_ast, NodeType::Max, w, Number(ref_ast, 1e-6f)), Number(ref_ast, 1.0f / 3.0f));
        pUnit = Binary(ref_ast, NodeType::Multiply, DirectionAt(ref_ast, signedUnit(u), pPhi), Broadcast(ref_ast, pCubeRoot, 3));
        break;
      }

      default:
        pUnit = DirectionAt(ref_ast, signedUnit(u), pPhi);
        break;
    }

    return Binary(ref_ast, NodeType::Multiply, pUnit, Broadcast(ref_ast, pRadius, 3));
  }

  Node* Noise(plExpressionAST& ref_ast, Node* pPosition, Node* pFrequency, Node* pOctaves)
  {
    Node* pScaled = Binary(ref_ast, NodeType::Multiply, pPosition, Broadcast(ref_ast, pFrequency, 3));

    Node* pOctaveCount = nullptr;
    if (pOctaves->m_Type == NodeType::Constant)
    {
      pOctaveCount = IntConstant(ref_ast, plMath::Clamp((plInt32)plMath::Round(static_cast<plExpressionAST::Constant*>(pOctaves)->m_Value.ConvertTo<float>()), 1, 8));
    }
    else
    {
      Node* pClamped = ref_ast.CreateTernaryOperator(NodeType::Clamp, Unary(ref_ast, NodeType::Round, pOctaves), Number(ref_ast, 1.0f), Number(ref_ast, 8.0f));
      pOctaveCount = ref_ast.CreateUnaryOperator(NodeType::TypeConversion, pClamped, DataType::Int);
    }

    Node* arguments[] = {Swizzle(ref_ast, pScaled, 0), Swizzle(ref_ast, pScaled, 1), Swizzle(ref_ast, pScaled, 2), pOctaveCount};
    return ref_ast.CreateFunctionCall(plDefaultExpressionFunctions::s_PerlinNoiseFunc.m_Desc, arguments);
  }

  Node* VectorNoise(plExpressionAST& ref_ast, Node* pPosition, Node* pFrequency, plInt32 iOctaves)
  {
    // the channels sample the same noise at offsets along the diagonal, as Turbulence does
    constexpr float fOffsets[] = {0.0f, 31.416f, 67.123f};

    Node* pScaled = Binary(ref_ast, NodeType::Multiply, pPosition, Broadcast(ref_ast, pFrequency, 3));
    Node* pOctaves = IntConstant(ref_ast, plMath::Clamp(iOctaves, 1, 8));

    Node* channels[3] = {};
    for (plUInt32 c = 0; c < 3; ++c)
    {
      Node* pShifted = Binary(ref_ast, NodeType::Add, pScaled, Broadcast(ref_ast, Number(ref_ast, fOffsets[c]), 3));
      Node* arguments[] = {Swizzle(ref_ast, pShifted, 0), Swizzle(ref_ast, pShifted, 1), Swizzle(ref_ast, pShifted, 2), pOctaves};
      Node* pNoise = ref_ast.CreateFunctionCall(plDefaultExpressionFunctions::s_PerlinNoiseFunc.m_Desc, arguments);
      channels[c] = Binary(ref_ast, NodeType::Subtract, Binary(ref_ast, NodeType::Multiply, pNoise, Number(ref_ast, 2.0f)), Number(ref_ast, 1.0f));
    }

    return Vector3(ref_ast, channels[0], channels[1], channels[2]);
  }

  Node* TransformPosition(plExpressionAST& ref_ast, const Frame& frame, Node* pVector, bool bToWorld)
  {
    if (bToWorld)
    {
      Node* pScaled = Binary(ref_ast, NodeType::Multiply, pVector, frame.m_pScale);
      return Binary(ref_ast, NodeType::Add, frame.m_pPosition, TransformDirection(ref_ast, frame, pScaled, true));
    }

    Node* pLocal = TransformDirection(ref_ast, frame, Binary(ref_ast, NodeType::Subtract, pVector, frame.m_pPosition), false);
    return SafeDivide(ref_ast, pLocal, frame.m_pScale);
  }

  Node* TransformDirection(plExpressionAST& ref_ast, const Frame& frame, Node* pVector, bool bToWorld)
  {
    if (bToWorld)
    {
      Node* pResult = Binary(ref_ast, NodeType::Multiply, frame.m_pForward, Broadcast(ref_ast, Swizzle(ref_ast, pVector, 0), 3));
      pResult = Binary(ref_ast, NodeType::Add, pResult, Binary(ref_ast, NodeType::Multiply, frame.m_pRight, Broadcast(ref_ast, Swizzle(ref_ast, pVector, 1), 3)));
      return Binary(ref_ast, NodeType::Add, pResult, Binary(ref_ast, NodeType::Multiply, frame.m_pUp, Broadcast(ref_ast, Swizzle(ref_ast, pVector, 2), 3)));
    }

    return Vector3(ref_ast, Binary(ref_ast, NodeType::Dot, pVector, frame.m_pForward), Binary(ref_ast, NodeType::Dot, pVector, frame.m_pRight), Binary(ref_ast, NodeType::Dot, pVector, frame.m_pUp));
  }

  Node* SampleCurve(plExpressionAST& ref_ast, Node* pT, Node* pSlot)
  {
    Node* arguments[] = {pT, pSlot};
    return ref_ast.CreateFunctionCall(plParticleProgramTables::s_CurveFunc.m_Desc, arguments);
  }

  Node* SampleGradient(plExpressionAST& ref_ast, Node* pT, Node* pSlot)
  {
    Node* components[4] = {};
    for (plInt32 c = 0; c < 4; ++c)
    {
      Node* arguments[] = {pT, pSlot, IntConstant(ref_ast, c)};
      components[c] = ref_ast.CreateFunctionCall(plParticleProgramTables::s_GradientFunc.m_Desc, arguments);
    }

    return ref_ast.CreateConstructorCall(DataType::Float4, components);
  }
} // namespace plParticleProgramOps

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_System_ParticleProgram);
