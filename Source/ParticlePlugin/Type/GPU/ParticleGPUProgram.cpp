#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/CodeUtils/Expression/ExpressionByteCode.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Math/Random.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_AttractToPosition.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ColorGradient.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Drag.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Gravity.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_KillIf.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SceneForces.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SizeCurve.h>
#include <ParticlePlugin/System/ParticleProgram.h>
#include <ParticlePlugin/Type/GPU/ParticleGPUProgram.h>

using OpCode = plExpressionByteCode::OpCode;

// the shader's op numbers are the byte code's
static_assert(GPU_OP_FIRST_UNARY == OpCode::FirstUnary && GPU_OP_ABS_F == OpCode::AbsF_R && GPU_OP_FTOI == OpCode::FToI_R && GPU_OP_LAST_UNARY == OpCode::LastUnary);
static_assert(GPU_OP_FIRST_BINARY == OpCode::FirstBinary && GPU_OP_ADD_F == OpCode::AddF_RR && GPU_OP_OR_B == OpCode::OrB_RR && GPU_OP_LAST_BINARY == OpCode::LastBinary);
static_assert(GPU_OP_FIRST_BINARY_CONSTANT == OpCode::FirstBinaryWithConstant && GPU_OP_LAST_BINARY_CONSTANT == OpCode::LastBinaryWithConstant);
static_assert(GPU_OP_ADD_F + GPU_OP_CONSTANT_OFFSET == OpCode::AddF_RC && GPU_OP_OR_B + GPU_OP_CONSTANT_OFFSET == OpCode::OrB_RC);
static_assert(GPU_OP_SEL_F == OpCode::SelF_RRR && GPU_OP_SEL_B == OpCode::SelB_RRR);
static_assert(GPU_OP_MOV_R == OpCode::MovX_R && GPU_OP_MOV_C == OpCode::MovX_C && GPU_OP_LOAD_F == OpCode::LoadF && GPU_OP_LOAD_I == OpCode::LoadI);
static_assert(GPU_OP_STORE_F == OpCode::StoreF && GPU_OP_STORE_I == OpCode::StoreI && GPU_OP_CALL == OpCode::Call && GPU_OP_COUNT == OpCode::Count);
static_assert(GPU_PROGRAM_TABLE_SIZE == plParticleProgramTables::s_uiTableSize);
static_assert(GPU_PROGRAM_TARGET_POSITION == plParticleSinkTarget::Position && GPU_PROGRAM_TARGET_ROTATION_SPEED == plParticleSinkTarget::RotationSpeed);

namespace
{
  constexpr plUInt32 s_uiNoOutput = 0xFFFFFFFFu;

  /// "Gravity" for plParticleBehaviorFactory_Gravity, as the block reads in the graph.
  plString GetBlockName(const plParticleBehaviorFactory* pFactory)
  {
    if (plDynamicCast<const plParticleBehaviorFactory_Attribute*>(pFactory))
      return "Set Attribute";
    if (plDynamicCast<const plParticleBehaviorFactory_KillIf*>(pFactory))
      return "Kill If";

    plStringBuilder sType = pFactory->GetDynamicRTTI()->GetTypeName();
    sType.TrimWordStart("plParticleBehaviorFactory_");

    plStringBuilder sName;
    for (plStringIterator it = sType.GetIteratorFront(); it.IsValid(); ++it)
    {
      const plUInt32 c = it.GetCharacter();
      if (!sName.IsEmpty() && c >= 'A' && c <= 'Z')
        sName.Append(" ");
      sName.Append(c);
    }

    return sName;
  }

  bool IsSink(const plParticleBehaviorFactory* pFactory)
  {
    return plDynamicCast<const plParticleBehaviorFactory_Attribute*>(pFactory) != nullptr || plDynamicCast<const plParticleBehaviorFactory_KillIf*>(pFactory) != nullptr;
  }

  /// The program record of one block, before the layout is known.
  struct PendingProgram
  {
    const plParticleBehaviorFactory* m_pFactory = nullptr;
    const plParticleProgramData* m_pProgram = nullptr; ///< null: the literal alone (a Set Attribute without a wire)
    plUInt32 m_uiBlock = 0;                             ///< its index among the Update blocks
    plUInt32 m_uiKind = GPU_PROGRAM_KIND_PROPERTY;
    plUInt32 m_uiTarget = 0;
    plUInt32 m_uiComponent = 0;
    plUInt32 m_uiMode = 0;
    plString m_sOutput; ///< the property the record's value is
    plVec4 m_vLiteral = plVec4::MakeZero();
  };

  /// What a block contributes: a record, nothing, or why it cannot run on the GPU.
  plString DescribeBlock(const plParticleBehaviorFactory* pFactory, plDynamicArray<PendingProgram>& ref_programs)
  {
    const plParticleProgramData* pProgram = pFactory->GetParticleProgram();
    const bool bHasProgram = pProgram != nullptr && !pProgram->IsEmpty();

    PendingProgram program;
    program.m_pFactory = pFactory;
    program.m_pProgram = bHasProgram ? pProgram : nullptr;

    if (const auto* pAttribute = plDynamicCast<const plParticleBehaviorFactory_Attribute*>(pFactory))
    {
      const plParticleAttributeSinkSettings& settings = pAttribute->m_Settings;
      if (settings.m_Target == plParticleSinkTarget::Custom)
        return "Set Attribute writes a custom attribute, which the GPU does not keep";
      if (settings.m_Target > plParticleSinkTarget::RotationSpeed)
        return "Set Attribute writes Rotation, Facing, Size Per Axis or Emissive, which GPU systems do not keep";

      program.m_uiKind = GPU_PROGRAM_KIND_SET_ATTRIBUTE;
      program.m_uiTarget = settings.m_Target.GetValue();
      program.m_uiComponent = settings.m_Component.GetValue();
      program.m_uiMode = settings.m_Mode.GetValue();
      program.m_sOutput = settings.GetValuePropertyName();

      const plUInt32 uiComponents = settings.GetValueComponents();
      if (uiComponents == 3)
        program.m_vLiteral = settings.m_vVector.GetAsVec4(0.0f);
      else if (uiComponents == 4)
        program.m_vLiteral.Set(settings.m_Color.r, settings.m_Color.g, settings.m_Color.b, settings.m_Color.a);
      else
        program.m_vLiteral.x = settings.m_fNumber;

      ref_programs.PushBack(program);
      return {};
    }

    if (const auto* pKill = plDynamicCast<const plParticleBehaviorFactory_KillIf*>(pFactory))
    {
      // without a wire the condition holds for all particles or for none
      if (!bHasProgram && !pKill->m_bCondition)
        return {};

      program.m_uiKind = GPU_PROGRAM_KIND_KILL_IF;
      program.m_sOutput = "Condition";
      program.m_vLiteral.x = pKill->m_bCondition ? 1.0f : 0.0f;
      ref_programs.PushBack(program);
      return {};
    }

    if (!bHasProgram)
      return {};

    if (const auto* pDrag = plDynamicCast<const plParticleBehaviorFactory_Drag*>(pFactory))
    {
      program.m_uiTarget = GPU_PROGRAM_PROPERTY_DRAG;
      program.m_sOutput = "Drag";
      program.m_vLiteral.x = pDrag->m_fDrag;
    }
    else if (const auto* pGravity = plDynamicCast<const plParticleBehaviorFactory_Gravity*>(pFactory))
    {
      program.m_uiTarget = GPU_PROGRAM_PROPERTY_GRAVITY_FACTOR;
      program.m_sOutput = "GravityFactor";
      program.m_vLiteral.x = pGravity->m_fGravityFactor;
    }
    else if (const auto* pAttract = plDynamicCast<const plParticleBehaviorFactory_AttractToPosition*>(pFactory))
    {
      program.m_uiTarget = GPU_PROGRAM_PROPERTY_ATTRACT_FORCE;
      program.m_sOutput = "Force";
      program.m_vLiteral.x = pAttract->m_fForce;
    }
    else if (const auto* pForces = plDynamicCast<const plParticleBehaviorFactory_SceneForces*>(pFactory))
    {
      program.m_uiTarget = GPU_PROGRAM_PROPERTY_SCENE_INFLUENCE;
      program.m_sOutput = "Influence";
      program.m_vLiteral.x = pForces->m_fInfluence;
    }
    else if (const auto* pSize = plDynamicCast<const plParticleBehaviorFactory_SizeCurve*>(pFactory))
    {
      program.m_uiTarget = GPU_PROGRAM_PROPERTY_CURVE_SCALE;
      program.m_sOutput = "CurveScale";
      program.m_vLiteral.x = pSize->m_fCurveScale;
    }
    else if (const auto* pGradient = plDynamicCast<const plParticleBehaviorFactory_ColorGradient*>(pFactory))
    {
      program.m_uiTarget = GPU_PROGRAM_PROPERTY_TINT;
      program.m_sOutput = "TintColor";
      program.m_vLiteral.Set(pGradient->m_TintColor.r, pGradient->m_TintColor.g, pGradient->m_TintColor.b, pGradient->m_TintColor.a);
    }
    else
    {
      plStringBuilder sReason;
      sReason.SetFormat("{} has a per-particle program the GPU cannot run", GetBlockName(pFactory));
      return sReason;
    }

    ref_programs.PushBack(program);
    return {};
  }

  plUInt32 GetSource(plStringView sStream)
  {
    if (sStream == "Position")
      return GPU_PROGRAM_SOURCE_POSITION;
    if (sStream == "Velocity")
      return GPU_PROGRAM_SOURCE_VELOCITY;
    if (sStream == "Size")
      return GPU_PROGRAM_SOURCE_SIZE;
    if (sStream == "Color")
      return GPU_PROGRAM_SOURCE_COLOR;
    if (sStream == "LifeTime")
      return GPU_PROGRAM_SOURCE_LIFETIME;
    if (sStream == "RotationSpeed")
      return GPU_PROGRAM_SOURCE_ROTATION_SPEED;
    if (sStream == "Axis")
      return GPU_PROGRAM_SOURCE_AXIS;
    if (sStream == "RandomSeed")
      return GPU_PROGRAM_SOURCE_RANDOM_SEED;
    return plInvalidIndex;
  }

  plUInt32 GetFunction(plStringView sMangledName)
  {
    // the byte code names a function with its argument types: "Random_II"
    plStringView sName = sMangledName;
    if (const char* szTypes = sMangledName.FindSubString("_"))
      sName = plStringView(sMangledName.GetStartPointer(), szTypes);

    if (sName == "Random")
      return GPU_PROGRAM_FUNCTION_RANDOM;
    if (sName == "PerlinNoise")
      return GPU_PROGRAM_FUNCTION_PERLIN_NOISE;
    if (sName == "PfxGradient")
      return GPU_PROGRAM_FUNCTION_GRADIENT;
    if (sName == "PfxCurve")
      return GPU_PROGRAM_FUNCTION_CURVE;
    return plInvalidIndex;
  }

  /// Walks the byte code: every instruction must be one the kernel knows, with operands in range.
  bool ValidateByteCode(const plExpressionByteCode& byteCode)
  {
    const plUInt32 uiRegs = byteCode.GetNumTempRegisters();
    const plUInt32 uiInputs = byteCode.GetInputs().GetCount();
    const plUInt32 uiOutputs = byteCode.GetOutputs().GetCount();
    const plUInt32 uiFunctions = byteCode.GetFunctions().GetCount();
    const plArrayPtr<const plUInt32> code = byteCode.GetByteCode();

    plUInt32 pc = 0;
    auto take = [&](plUInt32& out_uiValue) -> bool
    {
      if (pc >= code.GetCount())
        return false;
      out_uiValue = code[pc++];
      return true;
    };
    auto reg = [&]() -> bool
    {
      plUInt32 r = 0;
      return take(r) && r < uiRegs;
    };

    while (pc < code.GetCount())
    {
      const plUInt32 op = code[pc++];
      plUInt32 uiValue = 0;

      if (op > OpCode::FirstUnary && op < OpCode::LastUnary)
      {
        if (!reg() || !reg())
          return false;
      }
      else if (op > OpCode::FirstBinary && op < OpCode::LastBinary)
      {
        if (!reg() || !reg() || !reg())
          return false;
      }
      else if (op > OpCode::FirstBinaryWithConstant && op < OpCode::LastBinaryWithConstant)
      {
        if (!reg() || !reg() || !take(uiValue))
          return false;
      }
      else if (op > OpCode::FirstTernary && op < OpCode::LastTernary)
      {
        if (!reg() || !reg() || !reg() || !reg())
          return false;
      }
      else if (op == OpCode::MovX_R)
      {
        if (!reg() || !reg())
          return false;
      }
      else if (op == OpCode::MovX_C)
      {
        if (!reg() || !take(uiValue))
          return false;
      }
      else if (op == OpCode::LoadF || op == OpCode::LoadI)
      {
        if (!reg() || !take(uiValue) || uiValue >= uiInputs)
          return false;
      }
      else if (op == OpCode::StoreF || op == OpCode::StoreI)
      {
        if (!take(uiValue) || uiValue >= uiOutputs || !reg())
          return false;
      }
      else if (op == OpCode::Call)
      {
        plUInt32 uiArgs = 0;
        if (!take(uiValue) || uiValue >= uiFunctions || !reg() || !take(uiArgs) || uiArgs > GPU_PROGRAM_MAX_FUNCTION_ARGS)
          return false;

        for (plUInt32 a = 0; a < uiArgs; ++a)
        {
          if (!reg())
            return false;
        }
      }
      else
      {
        return false;
      }
    }

    return true;
  }

  /// Whether a program can run in the kernel at all: within the limits, with known instructions, functions and inputs.
  bool FitsGPU(const plParticleProgramData& program)
  {
    const plExpressionByteCode& byteCode = program.m_ByteCode;
    if (program.IsEmpty() || byteCode.GetNumInstructions() > GPU_PROGRAM_MAX_INSTRUCTIONS || byteCode.GetNumTempRegisters() > GPU_PROGRAM_MAX_REGISTERS || !ValidateByteCode(byteCode))
      return false;

    for (const plExpression::FunctionDesc& function : byteCode.GetFunctions())
    {
      if (GetFunction(function.m_sName.GetView()) == plInvalidIndex)
        return false;
    }

    for (const plExpression::StreamDesc& input : byteCode.GetInputs())
    {
      plInt32 iComponent = -1;
      const plStringView sSource = plParticleSplitInputComponent(input.m_sName.GetView(), iComponent);
      if (!plParticleIsFrameInput(sSource) && GetSource(sSource) == plInvalidIndex)
        return false;
    }

    return true;
  }

  /// The permutation table of the VM's plSimdPerlinNoise (seed 12345), rebuilt the way its constructor builds it.
  void BuildPerlinPermutations(plUInt8* out_pTable)
  {
    for (plUInt32 i = 0; i < 256; ++i)
      out_pTable[i] = static_cast<plUInt8>(i);

    plRandom rnd;
    rnd.Initialize(12345);

    for (plUInt32 i = 255; i > 0; --i)
    {
      const plUInt32 uiRandomIndex = rnd.UIntInRange(256);
      plMath::Swap(out_pTable[i], out_pTable[uiRandomIndex]);
    }
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// Building

plString plParticleGPUProgramSet::FindBlocker(plArrayPtr<const plParticleBehaviorFactory* const> behaviors, plUInt32* out_pBlock)
{
  plParticleGPUProgramSet set;
  return set.Build(behaviors, out_pBlock);
}

plString plParticleGPUProgramSet::Build(plArrayPtr<const plParticleBehaviorFactory* const> behaviors, plUInt32* out_pBlock, const plParticleProgramData* pSpy)
{
  plUInt32 uiBlockStorage = 0;
  plUInt32& uiBlock = out_pBlock != nullptr ? *out_pBlock : uiBlockStorage;
  uiBlock = plInvalidIndex;

  m_Words.Clear();
  m_Data.Clear();
  m_FrameInputs.Clear();
  m_uiProgramCount = 0;
  m_uiNumInstructions = 0;
  m_uiMaxRegisters = 0;
  m_bReadsRandomSeed = false;
  m_bTintProgram = false;
  m_bUsesPerlinNoise = false;
  m_bHasSpy = false;

  plStringBuilder sReason;

  // Set Attribute and Kill If run after the built-in blocks on the GPU, so they must come after them (D24)
  plUInt32 uiLastBuiltIn = plInvalidIndex;
  for (plUInt32 i = 0; i < behaviors.GetCount(); ++i)
  {
    if (!IsSink(behaviors[i]))
      uiLastBuiltIn = i;
  }

  plUInt32 uiGravityBlocks = 0;
  plUInt32 uiGravityProgramBlock = plInvalidIndex;

  plDynamicArray<PendingProgram> programs;
  for (plUInt32 i = 0; i < behaviors.GetCount(); ++i)
  {
    const plParticleBehaviorFactory* pFactory = behaviors[i];

    if (plDynamicCast<const plParticleBehaviorFactory_Gravity*>(pFactory) != nullptr)
    {
      ++uiGravityBlocks;
      if (const plParticleProgramData* pProgram = pFactory->GetParticleProgram(); pProgram != nullptr && !pProgram->IsEmpty())
        uiGravityProgramBlock = i;
    }

    const plUInt32 uiBefore = programs.GetCount();
    const plString sBlock = DescribeBlock(pFactory, programs);
    if (!sBlock.IsEmpty())
    {
      uiBlock = i;
      return sBlock;
    }

    if (programs.GetCount() > uiBefore)
      programs.PeekBack().m_uiBlock = i;

    if (IsSink(pFactory) && programs.GetCount() > uiBefore && uiLastBuiltIn != plInvalidIndex && i < uiLastBuiltIn)
    {
      uiBlock = i;
      sReason.SetFormat("{} is above {}; on the GPU, Set Attribute and Kill If run after the other blocks, so move it below them", GetBlockName(pFactory), GetBlockName(behaviors[uiLastBuiltIn]));
      return sReason;
    }
  }

  if (uiGravityProgramBlock != plInvalidIndex && uiGravityBlocks > 1)
  {
    uiBlock = uiGravityProgramBlock;
    return "Several Gravity blocks and a per-particle Gravity Factor";
  }

  if (programs.GetCount() > GPU_PROGRAM_MAX_PROGRAMS)
  {
    uiBlock = programs[GPU_PROGRAM_MAX_PROGRAMS].m_uiBlock;
    sReason.SetFormat("{} programs in Update; the GPU runs at most {}", programs.GetCount(), GPU_PROGRAM_MAX_PROGRAMS);
    return sReason;
  }

  // the editor's spy goes last, when it fits; it never keeps the system off the GPU
  if (pSpy != nullptr && FitsGPU(*pSpy))
  {
    PendingProgram& spy = programs.ExpandAndGetRef();
    spy.m_pFactory = nullptr;
    spy.m_pProgram = pSpy;
    spy.m_uiKind = GPU_PROGRAM_KIND_SPY;
    spy.m_uiBlock = plInvalidIndex;
    spy.m_sOutput = "Spy";
    m_bHasSpy = true;
  }

  if (programs.IsEmpty())
    return {};

  // every program within the limits (D23)
  for (const PendingProgram& program : programs)
  {
    if (program.m_pProgram == nullptr)
      continue;

    uiBlock = program.m_uiBlock;

    const plExpressionByteCode& byteCode = program.m_pProgram->m_ByteCode;
    if (byteCode.GetNumInstructions() > GPU_PROGRAM_MAX_INSTRUCTIONS)
    {
      sReason.SetFormat("The {} program has {} instructions; the GPU takes at most {}", GetBlockName(program.m_pFactory), byteCode.GetNumInstructions(), GPU_PROGRAM_MAX_INSTRUCTIONS);
      return sReason;
    }

    if (byteCode.GetNumTempRegisters() > GPU_PROGRAM_MAX_REGISTERS)
    {
      sReason.SetFormat("The {} program uses {} registers; the GPU has {}", GetBlockName(program.m_pFactory), byteCode.GetNumTempRegisters(), GPU_PROGRAM_MAX_REGISTERS);
      return sReason;
    }

    if (!ValidateByteCode(byteCode))
    {
      sReason.SetFormat("The {} program has an instruction the GPU does not know", GetBlockName(program.m_pFactory));
      return sReason;
    }

    for (const plExpression::FunctionDesc& function : byteCode.GetFunctions())
    {
      const plUInt32 uiFunction = GetFunction(function.m_sName.GetView());
      if (uiFunction == plInvalidIndex)
      {
        sReason.SetFormat("The {} program calls {}, which the GPU does not have", GetBlockName(program.m_pFactory), function.m_sName);
        return sReason;
      }

      m_bUsesPerlinNoise |= uiFunction == GPU_PROGRAM_FUNCTION_PERLIN_NOISE;
    }
  }

  uiBlock = plInvalidIndex;

  // ---- data: the Perlin table first, then each program's literal and tables, then the frame inputs ----
  plUInt32 uiPermOffset = 0xFFFFFFFFu;
  if (m_bUsesPerlinNoise)
  {
    plUInt8 permutations[256];
    BuildPerlinPermutations(permutations);

    uiPermOffset = m_Data.GetCount();
    for (plUInt8 value : permutations)
      m_Data.PushBack((float)value);
  }

  m_uiProgramCount = programs.GetCount();
  m_Words.SetCount(GPU_PROGRAM_HEADER_SIZE + m_uiProgramCount * GPU_PROGRAM_RECORD_SIZE, 0u);
  m_Words[GPU_PROGRAM_HEADER_COUNT] = m_uiProgramCount;
  m_Words[GPU_PROGRAM_HEADER_PERM] = uiPermOffset;

  for (plUInt32 p = 0; p < programs.GetCount(); ++p)
  {
    const PendingProgram& program = programs[p];
    const plUInt32 uiRecord = GPU_PROGRAM_HEADER_SIZE + p * GPU_PROGRAM_RECORD_SIZE;

    m_Words[uiRecord + GPU_PROGRAM_RECORD_KIND] = program.m_uiKind;
    m_Words[uiRecord + GPU_PROGRAM_RECORD_TARGET] = program.m_uiTarget;
    m_Words[uiRecord + GPU_PROGRAM_RECORD_COMPONENT] = program.m_uiComponent;
    m_Words[uiRecord + GPU_PROGRAM_RECORD_MODE] = program.m_uiMode;

    m_Words[uiRecord + GPU_PROGRAM_RECORD_LITERAL] = m_Data.GetCount();
    m_Data.PushBack(program.m_vLiteral.x);
    m_Data.PushBack(program.m_vLiteral.y);
    m_Data.PushBack(program.m_vLiteral.z);
    m_Data.PushBack(program.m_vLiteral.w);

    m_bTintProgram |= program.m_uiKind == GPU_PROGRAM_KIND_PROPERTY && program.m_uiTarget == GPU_PROGRAM_PROPERTY_TINT;

    if (program.m_pProgram == nullptr)
    {
      // the literal alone: an empty code range
      const plUInt32 uiHere = m_Words.GetCount();
      m_Words[uiRecord + GPU_PROGRAM_RECORD_CODE_START] = uiHere;
      m_Words[uiRecord + GPU_PROGRAM_RECORD_CODE_END] = uiHere;
      m_Words[uiRecord + GPU_PROGRAM_RECORD_INPUTS] = uiHere;
      m_Words[uiRecord + GPU_PROGRAM_RECORD_OUTPUTS] = uiHere;
      m_Words[uiRecord + GPU_PROGRAM_RECORD_FUNCTIONS] = uiHere;
      continue;
    }

    const plParticleProgramData& data = *program.m_pProgram;
    const plExpressionByteCode& byteCode = data.m_ByteCode;

    // the stats show the effect's own programs, not the editor's spy
    if (program.m_uiKind != GPU_PROGRAM_KIND_SPY)
    {
      m_uiNumInstructions += byteCode.GetNumInstructions();
      m_uiMaxRegisters = plMath::Max(m_uiMaxRegisters, byteCode.GetNumTempRegisters());
    }

    // baked gradient and curve tables, as the CPU runner samples them
    if (!data.m_Assets.IsEmpty())
    {
      plParticleProgramTables tables;
      tables.Bake(data.m_Assets);

      m_Words[uiRecord + GPU_PROGRAM_RECORD_GRADIENTS] = m_Data.GetCount();
      m_Words[uiRecord + GPU_PROGRAM_RECORD_NUM_GRADIENTS] = tables.GetGradientTables().GetCount() / GPU_PROGRAM_TABLE_SIZE;
      for (const plVec4& v : tables.GetGradientTables())
      {
        m_Data.PushBack(v.x);
        m_Data.PushBack(v.y);
        m_Data.PushBack(v.z);
        m_Data.PushBack(v.w);
      }

      m_Words[uiRecord + GPU_PROGRAM_RECORD_CURVES] = m_Data.GetCount();
      m_Words[uiRecord + GPU_PROGRAM_RECORD_NUM_CURVES] = tables.GetCurveTables().GetCount() / GPU_PROGRAM_TABLE_SIZE;
      for (float f : tables.GetCurveTables())
        m_Data.PushBack(f);
    }

    // inputs: a particle value, or a per-frame value the CPU writes into the data each frame
    m_Words[uiRecord + GPU_PROGRAM_RECORD_INPUTS] = m_Words.GetCount();
    for (const plExpression::StreamDesc& input : byteCode.GetInputs())
    {
      plInt32 iComponent = -1;
      const plStringView sSource = plParticleSplitInputComponent(input.m_sName.GetView(), iComponent);

      if (plParticleIsFrameInput(sSource))
      {
        FrameInput& frame = m_FrameInputs.ExpandAndGetRef();
        frame.m_sName = sSource;
        frame.m_iComponent = iComponent;
        frame.m_uiDataOffset = m_Data.GetCount();
        m_Data.PushBack(0.0f);

        m_Words.PushBack(GPU_PROGRAM_SOURCE_FRAME | (frame.m_uiDataOffset << 16));
        continue;
      }

      const plUInt32 uiSource = GetSource(sSource);
      if (uiSource == plInvalidIndex)
      {
        if (sSource.StartsWith("Custom."))
          sReason.SetFormat("The {} program reads the custom attribute '{}', which the GPU does not keep", GetBlockName(program.m_pFactory), sSource.GetSubString(7, 1000));
        else
          sReason.SetFormat("The {} program reads {}, which the GPU does not keep", GetBlockName(program.m_pFactory), sSource);

        uiBlock = program.m_uiBlock;
        m_uiProgramCount = 0;
        return sReason;
      }

      m_bReadsRandomSeed |= uiSource == GPU_PROGRAM_SOURCE_RANDOM_SEED;
      m_Words.PushBack(uiSource | ((plUInt32)plMath::Max(iComponent, 0) << 8));
    }

    // outputs: "Out<i>.c" is component c of property i; only the record's own property has a place
    m_Words[uiRecord + GPU_PROGRAM_RECORD_OUTPUTS] = m_Words.GetCount();
    for (const plExpression::StreamDesc& output : byteCode.GetOutputs())
    {
      plInt32 iComponent = -1;
      plStringView sBase = plParticleSplitInputComponent(output.m_sName.GetView(), iComponent);

      plUInt32 uiWord = s_uiNoOutput;
      if (sBase.StartsWith("Out"))
      {
        plUInt32 uiIndex = 0;
        plStringBuilder sIndex = sBase.GetSubString(3, 8);
        if (plConversionUtils::StringToUInt(sIndex, uiIndex).Succeeded() && uiIndex < data.m_Outputs.GetCount() && data.m_Outputs[uiIndex] == program.m_sOutput)
          uiWord = (plUInt32)plMath::Max(iComponent, 0);
      }

      m_Words.PushBack(uiWord);
    }

    m_Words[uiRecord + GPU_PROGRAM_RECORD_FUNCTIONS] = m_Words.GetCount();
    for (const plExpression::FunctionDesc& function : byteCode.GetFunctions())
      m_Words.PushBack(GetFunction(function.m_sName.GetView()));

    m_Words[uiRecord + GPU_PROGRAM_RECORD_CODE_START] = m_Words.GetCount();
    m_Words.PushBackRange(byteCode.GetByteCode());
    m_Words[uiRecord + GPU_PROGRAM_RECORD_CODE_END] = m_Words.GetCount();
  }

  if (m_Words.GetCount() > GPU_PROGRAM_MAX_WORDS || m_Data.GetCount() > GPU_PROGRAM_MAX_DATA)
  {
    m_uiProgramCount = 0;
    return "The programs in Update are too large for the GPU";
  }

  return {};
}

void plParticleGPUProgramSet::FillData(const plParticleSystemInstance* pSystem, const plTime& diff, plArrayPtr<float> out_data) const
{
  if (out_data.GetCount() < m_Data.GetCount())
    return;

  plMemoryUtils::Copy(out_data.GetPtr(), m_Data.GetData(), m_Data.GetCount());

  if (pSystem == nullptr)
    return;

  for (const FrameInput& frame : m_FrameInputs)
  {
    plVec4 vValue = plVec4::MakeZero();
    if (plParticleReadFrameInput(frame.m_sName, pSystem, diff, vValue))
      out_data[frame.m_uiDataOffset] = frame.m_iComponent < 0 ? vValue.x : vValue.GetData()[frame.m_iComponent & 3];
  }
}

//////////////////////////////////////////////////////////////////////////
// Reference interpreter: GPUParticleProgramVM.h in C++

namespace
{
  struct Interpreter
  {
    plArrayPtr<const plUInt32> m_Words;
    plArrayPtr<const float> m_Data;

    plUInt32 Word(plUInt32 i) const { return i < m_Words.GetCount() ? m_Words[i] : 0u; }
    float Data(plUInt32 i) const { return i < m_Data.GetCount() ? m_Data[i] : 0.0f; }

    static plUInt32 AsUInt(float f) { return *reinterpret_cast<const plUInt32*>(&f); }
    static float AsFloat(plUInt32 u) { return *reinterpret_cast<const float*>(&u); }
    static plInt32 AsInt(plUInt32 u) { return static_cast<plInt32>(u); }
    static plUInt32 Bool(bool b) { return b ? 0xFFFFFFFFu : 0u; }

    // the shader's two packed words are a plColorLinear16f in C++
    static plVec4 UnpackColor(const plGPUParticle& p) { return plVec4(p.Color.r, p.Color.g, p.Color.b, p.Color.a); }

    static void PackColor(plGPUParticle& p, const plVec4& c)
    {
      p.Color.r = c.x;
      p.Color.g = c.y;
      p.Color.b = c.z;
      p.Color.a = c.w;
    }

    static plVec3 UnpackAxis(plUInt32 packed)
    {
      auto component = [](plUInt32 v) -> float
      {
        plInt32 i = (plInt32)(v & 0x3FF);
        if (i > 511)
          i -= 1024;
        return (float)i / 511.0f;
      };
      return plVec3(component(packed), component(packed >> 10), component(packed >> 20));
    }

    static plUInt32 RandomUInt(plInt32 iPosition, plUInt32 uiSeed)
    {
      plUInt32 mangled = (plUInt32)iPosition;
      mangled *= 0xb5297a4du;
      mangled += uiSeed;
      mangled ^= (mangled >> 8);
      mangled += 0x68e31da4u;
      mangled ^= (mangled << 8);
      mangled *= 0x1b56c4e9u;
      mangled ^= (mangled >> 8);
      return mangled;
    }

    plInt32 Permute(plUInt32 uiPerm, plInt32 v) const { return (plInt32)Data(uiPerm + (plUInt32)(v & 255)); }

    static float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

    static float Grad(plInt32 hash, float x, float y, float z)
    {
      const plInt32 h = hash & 15;
      const float u = h < 8 ? x : y;
      const float v = h < 4 ? y : ((h == 12 || h == 14) ? x : z);
      return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
    }

    float Perlin(plUInt32 uiPerm, float x, float y, float z) const
    {
      const float xf = plMath::Floor(x), yf = plMath::Floor(y), zf = plMath::Floor(z);
      const plInt32 X = (plInt32)xf & 255, Y = (plInt32)yf & 255, Z = (plInt32)zf & 255;
      x -= xf;
      y -= yf;
      z -= zf;

      const float u = Fade(x), v = Fade(y), w = Fade(z);
      const plInt32 A = Permute(uiPerm, X) + Y, AA = Permute(uiPerm, A) + Z, AB = Permute(uiPerm, A + 1) + Z;
      const plInt32 B = Permute(uiPerm, X + 1) + Y, BA = Permute(uiPerm, B) + Z, BB = Permute(uiPerm, B + 1) + Z;

      const float c000 = Grad(Permute(uiPerm, AA), x, y, z);
      const float c100 = Grad(Permute(uiPerm, BA), x - 1, y, z);
      const float c010 = Grad(Permute(uiPerm, AB), x, y - 1, z);
      const float c110 = Grad(Permute(uiPerm, BB), x - 1, y - 1, z);
      const float c001 = Grad(Permute(uiPerm, AA + 1), x, y, z - 1);
      const float c101 = Grad(Permute(uiPerm, BA + 1), x - 1, y, z - 1);
      const float c011 = Grad(Permute(uiPerm, AB + 1), x, y - 1, z - 1);
      const float c111 = Grad(Permute(uiPerm, BB + 1), x - 1, y - 1, z - 1);

      return plMath::Lerp(plMath::Lerp(plMath::Lerp(c000, c100, u), plMath::Lerp(c010, c110, u), v), plMath::Lerp(plMath::Lerp(c001, c101, u), plMath::Lerp(c011, c111, u), v), w);
    }

    float PerlinZeroToOne(plUInt32 uiPerm, float x, float y, float z, plUInt32 uiOctaves) const
    {
      float result = 0.0f, amplitude = 1.0f;
      plUInt32 uiOffset = 0;
      uiOctaves = plMath::Max(uiOctaves, 1u);
      for (plUInt32 i = 0; i < uiOctaves && i < 8; ++i)
      {
        const float scale = (float)(1u << i);
        result += Perlin(uiPerm, x * scale + (float)Permute(uiPerm, (plInt32)uiOffset), y * scale + (float)Permute(uiPerm, (plInt32)uiOffset + 1), z * scale + (float)Permute(uiPerm, (plInt32)uiOffset + 2)) * amplitude;
        amplitude *= 0.5f;
        uiOffset += 23;
      }
      return result * 0.5f + 0.5f;
    }

    float SampleTable(plUInt32 uiOffset, plUInt32 uiStride, plUInt32 uiComponent, float t) const
    {
      const float pos = plMath::Saturate(t) * (GPU_PROGRAM_TABLE_SIZE - 1);
      const plUInt32 i0 = plMath::Min((plUInt32)pos, (plUInt32)(GPU_PROGRAM_TABLE_SIZE - 2));
      return plMath::Lerp(Data(uiOffset + i0 * uiStride + uiComponent), Data(uiOffset + (i0 + 1) * uiStride + uiComponent), pos - (float)i0);
    }

    plUInt32 Call(plUInt32 uiRecord, plUInt32 uiFunction, plUInt32 uiNumArgs, const plUInt32* pArgs) const
    {
      switch (uiFunction)
      {
        case GPU_PROGRAM_FUNCTION_RANDOM:
          return AsUInt((float)RandomUInt(AsInt(pArgs[0]), uiNumArgs >= 2 ? pArgs[1] : 0u) * (1.0f / 4294967296.0f));

        case GPU_PROGRAM_FUNCTION_PERLIN_NOISE:
          return AsUInt(PerlinZeroToOne(Word(GPU_PROGRAM_HEADER_PERM), AsFloat(pArgs[0]), AsFloat(pArgs[1]), AsFloat(pArgs[2]), uiNumArgs >= 4 ? (plUInt32)AsInt(pArgs[3]) : 1u));

        case GPU_PROGRAM_FUNCTION_GRADIENT:
        {
          const plUInt32 uiSlot = (plUInt32)AsInt(pArgs[1]);
          if (uiSlot >= Word(uiRecord + GPU_PROGRAM_RECORD_NUM_GRADIENTS))
            return AsUInt(1.0f);
          const plUInt32 uiOffset = Word(uiRecord + GPU_PROGRAM_RECORD_GRADIENTS) + uiSlot * GPU_PROGRAM_TABLE_SIZE * 4;
          return AsUInt(SampleTable(uiOffset, 4, (plUInt32)AsInt(pArgs[2]) & 3u, AsFloat(pArgs[0])));
        }

        case GPU_PROGRAM_FUNCTION_CURVE:
        {
          const plUInt32 uiSlot = (plUInt32)AsInt(pArgs[1]);
          if (uiSlot >= Word(uiRecord + GPU_PROGRAM_RECORD_NUM_CURVES))
            return AsUInt(0.0f);
          const plUInt32 uiOffset = Word(uiRecord + GPU_PROGRAM_RECORD_CURVES) + uiSlot * GPU_PROGRAM_TABLE_SIZE;
          return AsUInt(SampleTable(uiOffset, 1, 0, AsFloat(pArgs[0])));
        }

        default:
          return 0u;
      }
    }

    static plUInt32 Unary(plUInt32 op, plUInt32 a)
    {
      const float f = AsFloat(a);
      const plInt32 i = AsInt(a);

      switch (op)
      {
        case GPU_OP_ABS_F: return AsUInt(plMath::Abs(f));
        case GPU_OP_ABS_I: return (plUInt32)plMath::Abs(i);
        case GPU_OP_SQRT_F: return AsUInt(plMath::Sqrt(f));
        case GPU_OP_EXP_F: return AsUInt(plMath::Exp(f));
        case GPU_OP_LN_F: return AsUInt(plMath::Ln(f));
        case GPU_OP_LOG2_F: return AsUInt(plMath::Log2(f));
        case GPU_OP_LOG2_I: return (plUInt32)(i <= 0 ? 0 : (plInt32)plMath::Log2i((plUInt32)i));
        case GPU_OP_LOG10_F: return AsUInt(plMath::Log10(f));
        case GPU_OP_POW2_F: return AsUInt(plMath::Pow2(f));
        case GPU_OP_SIN_F: return AsUInt(plMath::Sin(plAngle::MakeFromRadian(f)));
        case GPU_OP_COS_F: return AsUInt(plMath::Cos(plAngle::MakeFromRadian(f)));
        case GPU_OP_TAN_F: return AsUInt(plMath::Tan(plAngle::MakeFromRadian(f)));
        case GPU_OP_ASIN_F: return AsUInt(plMath::ASin(f).GetRadian());
        case GPU_OP_ACOS_F: return AsUInt(plMath::ACos(f).GetRadian());
        case GPU_OP_ATAN_F: return AsUInt(plMath::ATan(f).GetRadian());
        case GPU_OP_ROUND_F: return AsUInt(plMath::Round(f));
        case GPU_OP_FLOOR_F: return AsUInt(plMath::Floor(f));
        case GPU_OP_CEIL_F: return AsUInt(plMath::Ceil(f));
        case GPU_OP_TRUNC_F: return AsUInt(plMath::Trunc(f));
        case GPU_OP_NOT_I: return ~a;
        case GPU_OP_NOT_B: return Bool(a == 0u);
        case GPU_OP_ITOF: return AsUInt((float)i);
        case GPU_OP_FTOI: return (plUInt32)(plInt32)f;
        default: return 0u;
      }
    }

    static plUInt32 Binary(plUInt32 op, plUInt32 a, plUInt32 b)
    {
      const float fa = AsFloat(a), fb = AsFloat(b);
      const plInt32 ia = AsInt(a), ib = AsInt(b);

      switch (op)
      {
        case GPU_OP_ADD_F: return AsUInt(fa + fb);
        case GPU_OP_ADD_I: return (plUInt32)(ia + ib);
        case GPU_OP_SUB_F: return AsUInt(fa - fb);
        case GPU_OP_SUB_I: return (plUInt32)(ia - ib);
        case GPU_OP_MUL_F: return AsUInt(fa * fb);
        case GPU_OP_MUL_I: return (plUInt32)(ia * ib);
        case GPU_OP_DIV_F: return AsUInt(fa / fb);
        case GPU_OP_DIV_I: return (plUInt32)(ib != 0 ? ia / ib : 0);
        case GPU_OP_MIN_F: return AsUInt(plMath::Min(fa, fb));
        case GPU_OP_MIN_I: return (plUInt32)plMath::Min(ia, ib);
        case GPU_OP_MAX_F: return AsUInt(plMath::Max(fa, fb));
        case GPU_OP_MAX_I: return (plUInt32)plMath::Max(ia, ib);
        case GPU_OP_SHL_I: return (plUInt32)(ia << (ib & 31));
        case GPU_OP_SHR_I: return (plUInt32)(ia >> (ib & 31));
        case GPU_OP_AND_I: return a & b;
        case GPU_OP_XOR_I: return a ^ b;
        case GPU_OP_OR_I: return a | b;
        case GPU_OP_EQ_F: return Bool(fa == fb);
        case GPU_OP_EQ_I: return Bool(ia == ib);
        case GPU_OP_EQ_B: return Bool((a != 0u) == (b != 0u));
        case GPU_OP_NEQ_F: return Bool(fa != fb);
        case GPU_OP_NEQ_I: return Bool(ia != ib);
        case GPU_OP_NEQ_B: return Bool((a != 0u) != (b != 0u));
        case GPU_OP_LT_F: return Bool(fa < fb);
        case GPU_OP_LT_I: return Bool(ia < ib);
        case GPU_OP_LEQ_F: return Bool(fa <= fb);
        case GPU_OP_LEQ_I: return Bool(ia <= ib);
        case GPU_OP_GT_F: return Bool(fa > fb);
        case GPU_OP_GT_I: return Bool(ia > ib);
        case GPU_OP_GEQ_F: return Bool(fa >= fb);
        case GPU_OP_GEQ_I: return Bool(ia >= ib);
        case GPU_OP_AND_B: return Bool(a != 0u && b != 0u);
        case GPU_OP_OR_B: return Bool(a != 0u || b != 0u);
        default: return 0u;
      }
    }

    plUInt32 Input(plUInt32 uiMap, const plGPUParticle& p, plUInt32 uiSeed) const
    {
      const plUInt32 uiComponent = (uiMap >> 8) & 0xFF;

      switch (uiMap & 0xFF)
      {
        case GPU_PROGRAM_SOURCE_POSITION: return AsUInt(uiComponent < 3 ? p.Position.GetData()[uiComponent] : 0.0f);
        case GPU_PROGRAM_SOURCE_VELOCITY: return AsUInt(p.Velocity.GetData()[plMath::Min(uiComponent, 2u)]);
        case GPU_PROGRAM_SOURCE_SIZE: return AsUInt(p.Size);
        case GPU_PROGRAM_SOURCE_COLOR: return AsUInt(UnpackColor(p).GetData()[uiComponent & 3]);
        case GPU_PROGRAM_SOURCE_LIFETIME: return AsUInt(uiComponent == 0 ? p.Life / plMath::Max(p.MaxLife, 1e-20f) : p.MaxLife);
        case GPU_PROGRAM_SOURCE_ROTATION_SPEED: return AsUInt(p.RotationSpeed);
        case GPU_PROGRAM_SOURCE_AXIS: return AsUInt(UnpackAxis(p.AxisPacked).GetData()[plMath::Min(uiComponent, 2u)]);
        case GPU_PROGRAM_SOURCE_RANDOM_SEED: return uiSeed;
        default: return AsUInt(Data(uiMap >> 16));
      }
    }

    plVec4 Run(plUInt32 uiRecord, const plGPUParticle& p, plUInt32 uiSeed) const
    {
      plUInt32 R[GPU_PROGRAM_MAX_REGISTERS] = {};

      const plUInt32 uiLiteral = Word(uiRecord + GPU_PROGRAM_RECORD_LITERAL);
      plVec4 value(Data(uiLiteral), Data(uiLiteral + 1), Data(uiLiteral + 2), Data(uiLiteral + 3));

      const plUInt32 uiInputs = Word(uiRecord + GPU_PROGRAM_RECORD_INPUTS);
      const plUInt32 uiOutputs = Word(uiRecord + GPU_PROGRAM_RECORD_OUTPUTS);
      const plUInt32 uiFunctions = Word(uiRecord + GPU_PROGRAM_RECORD_FUNCTIONS);

      plUInt32 pc = Word(uiRecord + GPU_PROGRAM_RECORD_CODE_START);
      const plUInt32 end = Word(uiRecord + GPU_PROGRAM_RECORD_CODE_END);

      for (plUInt32 step = 0; pc < end && step < GPU_PROGRAM_MAX_INSTRUCTIONS; ++step)
      {
        const plUInt32 op = Word(pc++);

        if (op > GPU_OP_FIRST_UNARY && op < GPU_OP_LAST_UNARY)
        {
          const plUInt32 t = Word(pc++), a = Word(pc++);
          R[t & 31] = Unary(op, R[a & 31]);
        }
        else if (op > GPU_OP_FIRST_BINARY && op < GPU_OP_LAST_BINARY)
        {
          const plUInt32 t = Word(pc++), a = Word(pc++), b = Word(pc++);
          R[t & 31] = Binary(op, R[a & 31], R[b & 31]);
        }
        else if (op > GPU_OP_FIRST_BINARY_CONSTANT && op < GPU_OP_LAST_BINARY_CONSTANT)
        {
          const plUInt32 t = Word(pc++), a = Word(pc++), c = Word(pc++);
          R[t & 31] = Binary(op - GPU_OP_CONSTANT_OFFSET, R[a & 31], c);
        }
        else if (op >= GPU_OP_SEL_F && op <= GPU_OP_SEL_B)
        {
          const plUInt32 t = Word(pc++), a = Word(pc++), b = Word(pc++), c = Word(pc++);
          R[t & 31] = R[a & 31] != 0u ? R[b & 31] : R[c & 31];
        }
        else if (op == GPU_OP_MOV_R)
        {
          const plUInt32 t = Word(pc++), a = Word(pc++);
          R[t & 31] = R[a & 31];
        }
        else if (op == GPU_OP_MOV_C)
        {
          const plUInt32 t = Word(pc++);
          R[t & 31] = Word(pc++);
        }
        else if (op == GPU_OP_LOAD_F || op == GPU_OP_LOAD_I)
        {
          const plUInt32 t = Word(pc++), uiInput = Word(pc++);
          R[t & 31] = Input(Word(uiInputs + uiInput), p, uiSeed);
        }
        else if (op == GPU_OP_STORE_F || op == GPU_OP_STORE_I)
        {
          const plUInt32 uiOutput = Word(pc++), t = Word(pc++);
          const plUInt32 uiComponent = Word(uiOutputs + uiOutput);
          const float f = op == GPU_OP_STORE_F ? AsFloat(R[t & 31]) : (float)AsInt(R[t & 31]);
          if (uiComponent < 4)
            value.GetData()[uiComponent] = f;
        }
        else if (op == GPU_OP_CALL)
        {
          const plUInt32 uiFunction = Word(pc++), t = Word(pc++), uiNumArgs = Word(pc++);
          plUInt32 args[GPU_PROGRAM_MAX_FUNCTION_ARGS] = {};
          for (plUInt32 a = 0; a < uiNumArgs; ++a)
          {
            const plUInt32 r = Word(pc++);
            if (a < GPU_PROGRAM_MAX_FUNCTION_ARGS)
              args[a] = R[r & 31];
          }

          R[t & 31] = Call(uiRecord, Word(uiFunctions + uiFunction), uiNumArgs, args);
        }
        else
        {
          break;
        }
      }

      return value;
    }
  };

  float ApplySinkMode(plUInt32 uiMode, float fCurrent, float fValue, float fDeltaTime)
  {
    if (uiMode == plParticleSinkMode::Add)
      return fCurrent + fValue * fDeltaTime;
    if (uiMode == plParticleSinkMode::Multiply)
      return fCurrent * fValue;
    return fValue;
  }

  plVec4 ApplySink(plUInt32 uiMode, plUInt32 uiComponent, plUInt32 uiNumComponents, bool bNumber, const plVec4& vCurrent, const plVec4& vValue, float fDeltaTime)
  {
    const plUInt32 uiFirst = uiComponent > 0 ? uiComponent - 1 : 0;
    const plUInt32 uiLast = uiComponent > 0 ? plMath::Min(uiComponent, uiNumComponents) : uiNumComponents;

    plVec4 vResult = vCurrent;
    for (plUInt32 c = uiFirst; c < uiLast; ++c)
      vResult.GetData()[c] = ApplySinkMode(uiMode, vCurrent.GetData()[c], bNumber ? vValue.x : vValue.GetData()[c], fDeltaTime);

    return vResult;
  }
} // namespace

plParticleGPUProgramSet::Properties plParticleGPUProgramSet::RunPropertyPrograms(plArrayPtr<const plUInt32> words, plArrayPtr<const float> data, const plGPUParticle& p, plUInt32 uiSeed)
{
  Interpreter vm{words, data};
  Properties props;

  for (plUInt32 i = 0; i < vm.Word(GPU_PROGRAM_HEADER_COUNT); ++i)
  {
    const plUInt32 uiRecord = GPU_PROGRAM_HEADER_SIZE + i * GPU_PROGRAM_RECORD_SIZE;
    if (vm.Word(uiRecord + GPU_PROGRAM_RECORD_KIND) != GPU_PROGRAM_KIND_PROPERTY)
      continue;

    const plUInt32 uiProperty = vm.Word(uiRecord + GPU_PROGRAM_RECORD_TARGET);
    if (uiProperty >= GPU_PROGRAM_PROPERTY_COUNT)
      continue;

    const plVec4 vValue = vm.Run(uiRecord, p, uiSeed);
    props.m_uiMask |= 1u << uiProperty;

    if (uiProperty == GPU_PROGRAM_PROPERTY_TINT)
      props.m_vTint = vValue;
    else if (uiProperty == GPU_PROGRAM_PROPERTY_GRAVITY_FACTOR || uiProperty == GPU_PROGRAM_PROPERTY_ATTRACT_FORCE)
      props.m_fValues[uiProperty] = vValue.x;
    else
      props.m_fValues[uiProperty] = plMath::Max(vValue.x, 0.0f);
  }

  return props;
}

bool plParticleGPUProgramSet::RunSinkPrograms(plArrayPtr<const plUInt32> words, plArrayPtr<const float> data, plGPUParticle& ref_p, plUInt32 uiSeed, float fDeltaTime)
{
  Interpreter vm{words, data};

  for (plUInt32 i = 0; i < vm.Word(GPU_PROGRAM_HEADER_COUNT); ++i)
  {
    const plUInt32 uiRecord = GPU_PROGRAM_HEADER_SIZE + i * GPU_PROGRAM_RECORD_SIZE;
    const plUInt32 uiKind = vm.Word(uiRecord + GPU_PROGRAM_RECORD_KIND);

    if (uiKind == GPU_PROGRAM_KIND_KILL_IF)
    {
      if (vm.Run(uiRecord, ref_p, uiSeed).x != 0.0f)
        return false;
    }
    else if (uiKind == GPU_PROGRAM_KIND_SET_ATTRIBUTE)
    {
      const plUInt32 uiTarget = vm.Word(uiRecord + GPU_PROGRAM_RECORD_TARGET);
      const plUInt32 uiComponent = vm.Word(uiRecord + GPU_PROGRAM_RECORD_COMPONENT);
      const plUInt32 uiMode = vm.Word(uiRecord + GPU_PROGRAM_RECORD_MODE);
      const plVec4 vValue = vm.Run(uiRecord, ref_p, uiSeed);
      const bool bNumber = uiComponent > 0 || uiTarget == GPU_PROGRAM_TARGET_SIZE || uiTarget == GPU_PROGRAM_TARGET_ROTATION_SPEED;

      if (uiTarget == GPU_PROGRAM_TARGET_POSITION)
        ref_p.Position = ApplySink(uiMode, uiComponent, 3, bNumber, ref_p.Position.GetAsVec4(0), vValue, fDeltaTime).GetAsVec3();
      else if (uiTarget == GPU_PROGRAM_TARGET_VELOCITY)
        ref_p.Velocity = ApplySink(uiMode, uiComponent, 3, bNumber, ref_p.Velocity.GetAsVec4(0), vValue, fDeltaTime).GetAsVec3();
      else if (uiTarget == GPU_PROGRAM_TARGET_SIZE)
        ref_p.Size = ApplySink(uiMode, 0, 1, true, plVec4(ref_p.Size, 0, 0, 0), vValue, fDeltaTime).x;
      else if (uiTarget == GPU_PROGRAM_TARGET_COLOR)
        Interpreter::PackColor(ref_p, ApplySink(uiMode, uiComponent, 4, bNumber, Interpreter::UnpackColor(ref_p), vValue, fDeltaTime));
      else if (uiTarget == GPU_PROGRAM_TARGET_ROTATION_SPEED)
        ref_p.RotationSpeed = ApplySink(uiMode, 0, 1, true, plVec4(ref_p.RotationSpeed, 0, 0, 0), vValue, fDeltaTime).x;
    }
  }

  return true;
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_GPU_ParticleGPUProgram);
