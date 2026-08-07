#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/CodeUtils/Expression/ExpressionCompiler.h>
#include <Foundation/CodeUtils/Expression/ExpressionParser.h>
#include <Foundation/DataProcessing/Stream/ProcessingStreamIterator.h>
#include <Foundation/Profiling/Profiling.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Expression.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Finalizer/ParticleFinalizer_ApplyVelocity.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleExpressionBinding, 1)
  PL_ENUM_CONSTANT(plParticleExpressionBinding::PositionX),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::PositionY),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::PositionZ),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::VelocityX),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::VelocityY),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::VelocityZ),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::Speed),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::Size),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::LifeFraction),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::ColorR),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::ColorG),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::ColorB),
  PL_ENUM_CONSTANT(plParticleExpressionBinding::ColorA),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehaviorFactory_Expression, 1, plRTTIDefaultAllocator<plParticleBehaviorFactory_Expression>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Expression", m_sExpression),
    PL_ENUM_MEMBER_PROPERTY("InputA", plParticleExpressionBinding, m_InputA),
    PL_ENUM_MEMBER_PROPERTY("InputB", plParticleExpressionBinding, m_InputB),
    PL_ENUM_MEMBER_PROPERTY("InputC", plParticleExpressionBinding, m_InputC),
    PL_ENUM_MEMBER_PROPERTY("InputD", plParticleExpressionBinding, m_InputD),
    PL_ENUM_MEMBER_PROPERTY("Output", plParticleExpressionBinding, m_Output),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleBehavior_Expression, 1, plRTTIDefaultAllocator<plParticleBehavior_Expression>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleBehaviorFactory_Expression::plParticleBehaviorFactory_Expression() = default;

const plRTTI* plParticleBehaviorFactory_Expression::GetBehaviorType() const
{
  return plGetStaticRTTI<plParticleBehavior_Expression>();
}

void plParticleBehaviorFactory_Expression::CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const
{
  plParticleBehavior_Expression* pBehavior = static_cast<plParticleBehavior_Expression*>(pObject);

  pBehavior->m_InputA = m_InputA;
  pBehavior->m_InputB = m_InputB;
  pBehavior->m_InputC = m_InputC;
  pBehavior->m_InputD = m_InputD;
  pBehavior->m_Output = m_Output;

  if (pBehavior->m_sExpression != m_sExpression)
  {
    pBehavior->m_sExpression = m_sExpression;
    pBehavior->CompileExpression();
  }
  else if (bFirstTime)
  {
    pBehavior->CompileExpression();
  }
}

void plParticleBehaviorFactory_Expression::QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const
{
  // If the output modifies velocity, we need the ApplyVelocity finalizer
  if (m_Output == plParticleExpressionBinding::VelocityX ||
      m_Output == plParticleExpressionBinding::VelocityY ||
      m_Output == plParticleExpressionBinding::VelocityZ ||
      m_Output == plParticleExpressionBinding::Speed)
  {
    inout_finalizerDeps.Insert(plGetStaticRTTI<plParticleFinalizerFactory_ApplyVelocity>());
  }
}

void plParticleBehaviorFactory_Expression::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_sExpression;
  inout_stream << m_InputA;
  inout_stream << m_InputB;
  inout_stream << m_InputC;
  inout_stream << m_InputD;
  inout_stream << m_Output;
}

void plParticleBehaviorFactory_Expression::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  PL_ASSERT_DEV(uiVersion <= 1, "Invalid version {0}", uiVersion);

  inout_stream >> m_sExpression;
  inout_stream >> m_InputA;
  inout_stream >> m_InputB;
  inout_stream >> m_InputC;
  inout_stream >> m_InputD;
  inout_stream >> m_Output;
}

//////////////////////////////////////////////////////////////////////////

void plParticleBehavior_Expression::CompileExpression()
{
  m_bBytecodeValid = false;

  if (m_sExpression.IsEmpty())
    return;

  plExpressionParser parser;

  plHybridArray<plExpression::StreamDesc, 4> inputs;
  inputs.PushBack({plMakeHashedString("a"), plProcessingStream::DataType::Float});
  inputs.PushBack({plMakeHashedString("b"), plProcessingStream::DataType::Float});
  inputs.PushBack({plMakeHashedString("c"), plProcessingStream::DataType::Float});
  inputs.PushBack({plMakeHashedString("d"), plProcessingStream::DataType::Float});

  plHybridArray<plExpression::StreamDesc, 1> outputs;
  outputs.PushBack({plMakeHashedString("out"), plProcessingStream::DataType::Float});

  // Build expression: "out = <expression>"
  plStringBuilder sFullExpression;
  sFullExpression.SetFormat("out = {0}", m_sExpression);

  plExpressionParser::Options options;
  plExpressionAST ast;
  if (parser.Parse(sFullExpression, inputs, outputs, options, ast).Failed())
  {
    plLog::Warning("Particle Expression: Failed to parse expression '{0}'", m_sExpression);
    return;
  }

  plExpressionCompiler compiler;
  if (compiler.Compile(ast, m_ByteCode).Failed())
  {
    plLog::Warning("Particle Expression: Failed to compile expression '{0}'", m_sExpression);
    return;
  }

  m_sCompiledExpression = m_sExpression;
  m_bBytecodeValid = true;
}

void plParticleBehavior_Expression::CreateRequiredStreams()
{
  CreateStream("Position", plProcessingStream::DataType::Float4, &m_pStreamPosition, false);
  CreateStream("Velocity", plProcessingStream::DataType::Float3, &m_pStreamVelocity, false);
}

void plParticleBehavior_Expression::QueryOptionalStreams()
{
  m_pStreamSize = GetOwnerSystem()->QueryStream("Size", plProcessingStream::DataType::Half);
  m_pStreamColor = GetOwnerSystem()->QueryStream("Color", plProcessingStream::DataType::Half4);
  m_pStreamLifeTime = GetOwnerSystem()->QueryStream("LifeTime", plProcessingStream::DataType::Half2);
}

float plParticleBehavior_Expression::ExtractValue(plParticleExpressionBinding::Enum binding, plUInt32 uiIndex) const
{
  switch (binding)
  {
    case plParticleExpressionBinding::PositionX:
      return m_pStreamPosition->GetData<plSimdVec4f>()[uiIndex].GetComponent<0>();
    case plParticleExpressionBinding::PositionY:
      return m_pStreamPosition->GetData<plSimdVec4f>()[uiIndex].GetComponent<1>();
    case plParticleExpressionBinding::PositionZ:
      return m_pStreamPosition->GetData<plSimdVec4f>()[uiIndex].GetComponent<2>();
    case plParticleExpressionBinding::VelocityX:
      return m_pStreamVelocity->GetData<plVec3>()[uiIndex].x;
    case plParticleExpressionBinding::VelocityY:
      return m_pStreamVelocity->GetData<plVec3>()[uiIndex].y;
    case plParticleExpressionBinding::VelocityZ:
      return m_pStreamVelocity->GetData<plVec3>()[uiIndex].z;
    case plParticleExpressionBinding::Speed:
      return m_pStreamVelocity->GetData<plVec3>()[uiIndex].GetLength();
    case plParticleExpressionBinding::Size:
    {
      if (m_pStreamSize)
        return static_cast<float>(m_pStreamSize->GetData<plFloat16>()[uiIndex]);
      return 1.0f;
    }
    case plParticleExpressionBinding::LifeFraction:
    {
      if (m_pStreamLifeTime)
      {
        // LifeTime stream is Half2: x = remaining life (seconds), y = 1 / total life
        const plFloat16Vec2& lt = m_pStreamLifeTime->GetData<plFloat16Vec2>()[uiIndex];
        return 1.0f - static_cast<float>(lt.x) * static_cast<float>(lt.y); // fraction consumed
      }
      return 0.0f;
    }
    case plParticleExpressionBinding::ColorR:
    {
      if (m_pStreamColor)
        return static_cast<float>(m_pStreamColor->GetData<plFloat16Vec4>()[uiIndex].x);
      return 1.0f;
    }
    case plParticleExpressionBinding::ColorG:
    {
      if (m_pStreamColor)
        return static_cast<float>(m_pStreamColor->GetData<plFloat16Vec4>()[uiIndex].y);
      return 1.0f;
    }
    case plParticleExpressionBinding::ColorB:
    {
      if (m_pStreamColor)
        return static_cast<float>(m_pStreamColor->GetData<plFloat16Vec4>()[uiIndex].z);
      return 1.0f;
    }
    case plParticleExpressionBinding::ColorA:
    {
      if (m_pStreamColor)
        return static_cast<float>(m_pStreamColor->GetData<plFloat16Vec4>()[uiIndex].w);
      return 1.0f;
    }
    default:
      return 0.0f;
  }
}

void plParticleBehavior_Expression::WriteValue(plParticleExpressionBinding::Enum binding, plUInt32 uiIndex, float fValue)
{
  switch (binding)
  {
    case plParticleExpressionBinding::PositionX:
    {
      plSimdVec4f& pos = m_pStreamPosition->GetWritableData<plSimdVec4f>()[uiIndex];
      pos.SetX(plSimdFloat(fValue));
      break;
    }
    case plParticleExpressionBinding::PositionY:
    {
      plSimdVec4f& pos = m_pStreamPosition->GetWritableData<plSimdVec4f>()[uiIndex];
      pos.SetY(plSimdFloat(fValue));
      break;
    }
    case plParticleExpressionBinding::PositionZ:
    {
      plSimdVec4f& pos = m_pStreamPosition->GetWritableData<plSimdVec4f>()[uiIndex];
      pos.SetZ(plSimdFloat(fValue));
      break;
    }
    case plParticleExpressionBinding::VelocityX:
      m_pStreamVelocity->GetWritableData<plVec3>()[uiIndex].x = fValue;
      break;
    case plParticleExpressionBinding::VelocityY:
      m_pStreamVelocity->GetWritableData<plVec3>()[uiIndex].y = fValue;
      break;
    case plParticleExpressionBinding::VelocityZ:
      m_pStreamVelocity->GetWritableData<plVec3>()[uiIndex].z = fValue;
      break;
    case plParticleExpressionBinding::Speed:
    {
      // rescale the velocity to the new speed; a zero velocity has no direction and stays zero
      plVec3& v = m_pStreamVelocity->GetWritableData<plVec3>()[uiIndex];
      const float fLen = v.GetLength();
      if (fLen > 0.0001f)
      {
        v *= fValue / fLen;
      }
      break;
    }
    case plParticleExpressionBinding::Size:
    {
      if (m_pStreamSize)
        m_pStreamSize->GetWritableData<plFloat16>()[uiIndex] = fValue;
      break;
    }
    case plParticleExpressionBinding::ColorR:
    {
      if (m_pStreamColor)
      {
        plFloat16Vec4& c = m_pStreamColor->GetWritableData<plFloat16Vec4>()[uiIndex];
        c = plFloat16Vec4(plVec4(fValue, static_cast<float>(c.y), static_cast<float>(c.z), static_cast<float>(c.w)));
      }
      break;
    }
    case plParticleExpressionBinding::ColorG:
    {
      if (m_pStreamColor)
      {
        plFloat16Vec4& c = m_pStreamColor->GetWritableData<plFloat16Vec4>()[uiIndex];
        c = plFloat16Vec4(plVec4(static_cast<float>(c.x), fValue, static_cast<float>(c.z), static_cast<float>(c.w)));
      }
      break;
    }
    case plParticleExpressionBinding::ColorB:
    {
      if (m_pStreamColor)
      {
        plFloat16Vec4& c = m_pStreamColor->GetWritableData<plFloat16Vec4>()[uiIndex];
        c = plFloat16Vec4(plVec4(static_cast<float>(c.x), static_cast<float>(c.y), fValue, static_cast<float>(c.w)));
      }
      break;
    }
    case plParticleExpressionBinding::ColorA:
    {
      if (m_pStreamColor)
      {
        plFloat16Vec4& c = m_pStreamColor->GetWritableData<plFloat16Vec4>()[uiIndex];
        c = plFloat16Vec4(plVec4(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), fValue));
      }
      break;
    }
    default:
      break;
  }
}

void plParticleBehavior_Expression::Process(plUInt64 uiNumElements)
{
  PL_PROFILE_SCOPE("PFX: Expression");

  if (!m_bBytecodeValid || uiNumElements == 0)
    return;

  // Marshal particle data into flat float arrays for the expression VM
  const plUInt32 uiCount = static_cast<plUInt32>(uiNumElements);

  plDynamicArray<float> inputA, inputB, inputC, inputD;
  plDynamicArray<float> result;

  inputA.SetCountUninitialized(uiCount);
  inputB.SetCountUninitialized(uiCount);
  inputC.SetCountUninitialized(uiCount);
  inputD.SetCountUninitialized(uiCount);
  result.SetCountUninitialized(uiCount);

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    inputA[i] = ExtractValue(m_InputA, i);
    inputB[i] = ExtractValue(m_InputB, i);
    inputC[i] = ExtractValue(m_InputC, i);
    inputD[i] = ExtractValue(m_InputD, i);
  }

  // Build streams for the VM
  plHybridArray<plProcessingStream, 4> inputs;
  inputs.PushBack(plProcessingStream(plMakeHashedString("a"), plMakeArrayPtr(inputA).ToByteArray(), plProcessingStream::DataType::Float));
  inputs.PushBack(plProcessingStream(plMakeHashedString("b"), plMakeArrayPtr(inputB).ToByteArray(), plProcessingStream::DataType::Float));
  inputs.PushBack(plProcessingStream(plMakeHashedString("c"), plMakeArrayPtr(inputC).ToByteArray(), plProcessingStream::DataType::Float));
  inputs.PushBack(plProcessingStream(plMakeHashedString("d"), plMakeArrayPtr(inputD).ToByteArray(), plProcessingStream::DataType::Float));

  plHybridArray<plProcessingStream, 1> outputs;
  outputs.PushBack(plProcessingStream(plMakeHashedString("out"), plMakeArrayPtr(result).ToByteArray(), plProcessingStream::DataType::Float));

  if (m_VM.Execute(m_ByteCode, inputs, outputs, uiCount).Failed())
  {
    plLog::Warning("Particle Expression: Failed to execute expression '{0}'", m_sExpression);
    m_bBytecodeValid = false;
    return;
  }

  // Write results back to the particle streams
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    WriteValue(m_Output, i, result[i]);
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleBehavior_Expression);
