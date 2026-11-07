#pragma once

#include <Core/Curves/Curve1DResource.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/System/ParticleProgram.h>

class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_SizeCurve final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_SizeCurve, plParticleBehaviorFactory);

public:
  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual plParticleProgramData* GetParticleProgram() override { return &m_Program; }

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleSizeMode> m_Mode;
  float m_fBaseSize;
  float m_fCurveScale;
  plCurve1DResourceHandle m_hCurve; ///< the uniform curve, or X in PerAxis mode
  plCurve1DResourceHandle m_hCurveY;
  plCurve1DResourceHandle m_hCurveZ;

  /// Per-particle wires into CurveScale.
  plParticleProgramData m_Program;
};

/// Uniform mode writes Size. PerAxis mode writes the SizeScale multipliers instead and leaves Size alone.
class PL_PARTICLEPLUGIN_DLL plParticleBehavior_SizeCurve final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_SizeCurve, plParticleBehavior);

public:
  plEnum<plParticleSizeMode> m_Mode;
  float m_fBaseSize;
  float m_fCurveScale;
  plCurve1DResourceHandle m_hCurve;
  plCurve1DResourceHandle m_hCurveY;
  plCurve1DResourceHandle m_hCurveZ;
  const plParticleProgramData* m_pProgram = nullptr;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamLifeTime = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamSizeScale = nullptr;
  plParticleProgramRunner m_Runner;
  plUInt8 m_uiFirstToUpdate = 0;
  plUInt8 m_uiCurrentUpdateInterval = 8;
};
