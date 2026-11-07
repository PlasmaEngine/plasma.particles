#pragma once

#include <Core/ResourceManager/ResourceHandle.h>
#include <Foundation/Types/VarianceTypes.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>

using plCurve1DResourceHandle = plTypedResourceHandle<class plCurve1DResource>;

class PL_PARTICLEPLUGIN_DLL plParticleInitializerFactory_RandomSize final : public plParticleInitializerFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializerFactory_RandomSize, plParticleInitializerFactory);

public:
  virtual const plRTTI* GetInitializerType() const override;
  virtual void CopyInitializerProperties(plParticleInitializer* pInitializer, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  plEnum<plParticleSizeMode> m_Mode;
  plVarianceTypeFloat m_Size;
  plVarianceTypeFloat m_Aspect; // Uniform only: width/height ratio; != 1 stretches quads along their local X
  plVec3 m_vSizeAxes = plVec3(1.0f); // PerAxis only: X/Y/Z multipliers of Size
  bool m_bSameRandomForAllAxes = false;
  plCurve1DResourceHandle m_hCurve;
};


class PL_PARTICLEPLUGIN_DLL plParticleInitializer_RandomSize final : public plParticleInitializer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializer_RandomSize, plParticleInitializer);

public:
  plEnum<plParticleSizeMode> m_Mode;
  plVarianceTypeFloat m_Size;
  plVarianceTypeFloat m_Aspect;
  plVec3 m_vSizeAxes = plVec3(1.0f);
  bool m_bSameRandomForAllAxes = false;
  plCurve1DResourceHandle m_hCurve;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  /// The curve's normalized value at a random position, or 1 without a curve.
  void ComputeCurveFactors(plUInt64 uiStartIndex, plUInt64 uiNumElements, plFloat16* pOut);

  plProcessingStream* m_pStreamSize;
  plProcessingStream* m_pStreamSizeScale = nullptr;
};
