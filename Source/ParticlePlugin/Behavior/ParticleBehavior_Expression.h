#pragma once

#include <Foundation/CodeUtils/Expression/ExpressionByteCode.h>
#include <Foundation/CodeUtils/Expression/ExpressionVM.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>

/// Which particle stream an expression variable maps to
struct PL_PARTICLEPLUGIN_DLL plParticleExpressionBinding
{
  using StorageType = plUInt8;

  enum Enum
  {
    PositionX,
    PositionY,
    PositionZ,
    VelocityX,
    VelocityY,
    VelocityZ,
    Speed,
    Size,
    LifeFraction,
    ColorR,
    ColorG,
    ColorB,
    ColorA,

    Default = PositionX
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleExpressionBinding);

/// Evaluates a math expression per-particle to modify a chosen attribute.
///
/// Variables a, b, c, d are mapped to particle streams. The result is written
/// to the output stream. Uses the plExpressionVM for SIMD-batched evaluation.
///
/// Example: Expression = "a * sin(b * 3.14159)" with a = Size, b = LifeFraction
/// would create a pulsing size effect over the particle's lifetime.
class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_Expression final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_Expression, plParticleBehaviorFactory);

public:
  plParticleBehaviorFactory_Expression();

  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plString m_sExpression;
  plEnum<plParticleExpressionBinding> m_InputA;
  plEnum<plParticleExpressionBinding> m_InputB;
  plEnum<plParticleExpressionBinding> m_InputC;
  plEnum<plParticleExpressionBinding> m_InputD;
  plEnum<plParticleExpressionBinding> m_Output;
};

class PL_PARTICLEPLUGIN_DLL plParticleBehavior_Expression final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_Expression, plParticleBehavior);

public:
  plString m_sExpression;
  plEnum<plParticleExpressionBinding> m_InputA;
  plEnum<plParticleExpressionBinding> m_InputB;
  plEnum<plParticleExpressionBinding> m_InputC;
  plEnum<plParticleExpressionBinding> m_InputD;
  plEnum<plParticleExpressionBinding> m_Output;

  void CompileExpression();

protected:
  virtual void CreateRequiredStreams() override;
  virtual void QueryOptionalStreams() override;
  virtual void Process(plUInt64 uiNumElements) override;

  float ExtractValue(plParticleExpressionBinding::Enum binding, plUInt32 uiIndex) const;
  void WriteValue(plParticleExpressionBinding::Enum binding, plUInt32 uiIndex, float fValue);

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamColor = nullptr;
  plProcessingStream* m_pStreamLifeTime = nullptr;

  plExpressionByteCode m_ByteCode;
  plExpressionVM m_VM;
  bool m_bBytecodeValid = false;
  plString m_sCompiledExpression; ///< The expression that was last successfully compiled
};
