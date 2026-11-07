#pragma once

#include <Core/Curves/Curve1DResource.h>
#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>
#include <ParticlePlugin/Behavior/ParticleBehavior.h>
#include <ParticlePlugin/System/ParticleProgram.h>

/// The curves of a Size Curve block baked into tables of their values mapped to 0..1 (a flat curve reads 1), shared by
/// every system that runs the block.
class PL_PARTICLEPLUGIN_DLL plParticleSizeCurveTables : public plRefCounted
{
public:
  static constexpr plUInt32 s_uiTableSize = 128;

  /// Loads the three curves (uniform or X, Y, Z), blocking until they are there; an invalid or missing one has no table.
  void Bake(const plCurve1DResourceHandle& hCurveX, const plCurve1DResourceHandle& hCurveY, const plCurve1DResourceHandle& hCurveZ);

  /// True when a curve these tables were baked from has been reloaded since.
  bool IsOutdated() const;

  bool HasCurve(plUInt32 uiAxis) const { return m_bValid[uiAxis]; }

  /// The value of one curve at t (0 to 1 across its range); 0 without a table.
  float Sample(plUInt32 uiAxis, float t) const;

private:
  plCurve1DResourceHandle m_hCurves[3];
  plUInt32 m_uiChangeCounters[3] = {};
  bool m_bValid[3] = {};
  float m_Values[3][s_uiTableSize] = {};
};

class PL_PARTICLEPLUGIN_DLL plParticleBehaviorFactory_SizeCurve final : public plParticleBehaviorFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehaviorFactory_SizeCurve, plParticleBehaviorFactory);

public:
  virtual const plRTTI* GetBehaviorType() const override;
  virtual void CopyBehaviorProperties(plParticleBehavior* pObject, bool bFirstTime) const override;
  virtual plParticleProgramData* GetParticleProgram() override { return &m_Program; }

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  /// The baked curves, shared by every instance of the block. The first call bakes them, and so does a call after a
  /// curve was reloaded.
  plSharedPtr<plParticleSizeCurveTables> GetTables() const;

  plEnum<plParticleSizeMode> m_Mode;
  float m_fBaseSize;
  float m_fCurveScale;
  plCurve1DResourceHandle m_hCurve; ///< the uniform curve, or X in PerAxis mode
  plCurve1DResourceHandle m_hCurveY;
  plCurve1DResourceHandle m_hCurveZ;

  /// Per-particle wires into CurveScale.
  plParticleProgramData m_Program;

private:
  mutable plSharedPtr<plParticleSizeCurveTables> m_pTables;
};

/// Uniform mode writes Size. PerAxis mode writes the SizeScale multipliers instead and leaves Size alone.
///
/// Every particle is updated every step while the effect is visible; an invisible effect updates a slice of them per
/// step, so its particles are close to right when it shows again.
class PL_PARTICLEPLUGIN_DLL plParticleBehavior_SizeCurve final : public plParticleBehavior
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleBehavior_SizeCurve, plParticleBehavior);

public:
  plEnum<plParticleSizeMode> m_Mode;
  float m_fBaseSize;
  float m_fCurveScale;
  const plParticleBehaviorFactory_SizeCurve* m_pFactory = nullptr;
  const plParticleProgramData* m_pProgram = nullptr;

  virtual void CreateRequiredStreams() override;

protected:
  friend class plParticleBehaviorFactory_SizeCurve;

  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;
  virtual void Process(plUInt64 uiNumElements) override;

  plProcessingStream* m_pStreamLifeTime = nullptr;
  plProcessingStream* m_pStreamSize = nullptr;
  plProcessingStream* m_pStreamSizeScale = nullptr;
  plParticleProgramRunner m_Runner;
  plSharedPtr<plParticleSizeCurveTables> m_pTables;
  plUInt8 m_uiFirstToUpdate = 0;
  plUInt8 m_uiCurrentUpdateInterval = 1;
};
