#pragma once

#include <Foundation/DataProcessing/Stream/ProcessingStreamProcessor.h>
#include <Foundation/Reflection/Reflection.h>
#include <ParticlePlugin/Module/ParticleModule.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

struct plMsgExtractRenderData;

enum plParticleTypeSortingKey
{
  Distortion, // samples the back-buffer, so doing this later would overwrite their result
  Opaque,
  BlendedBackground,
  Additive,
  BlendAdd,
  Blended,
  BlendedForeground,
};

class PL_PARTICLEPLUGIN_DLL plParticleTypeFactory : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleTypeFactory, plReflectedClass);

public:
  virtual const plRTTI* GetTypeType() const = 0;
  virtual void CopyTypeProperties(plParticleType* pObject, bool bFirstTime) const = 0;

  plParticleType* CreateType(plParticleSystemInstance* pOwner) const;

  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const {}

  virtual void Save(plStreamWriter& inout_stream) const = 0;
  virtual void Load(plStreamReader& inout_stream) = 0;

  /// Authoring only: a disabled block is kept in the document but left out of the built
  /// descriptor, so the runtime never sees it and nothing needs to serialize the flag.
  bool m_bEnabled = true;
};

class PL_PARTICLEPLUGIN_DLL plParticleType : public plParticleModule
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleType, plParticleModule);

  friend class plParticleSystemInstance;

public:
  virtual float GetMaxParticleRadius(float fParticleSize) const { return fParticleSize * 0.5f; }

  /// Whether this type manages GPU-side particles that the CPU stream group doesn't track.
  virtual bool HasActiveGPUElements() const { return false; }

  virtual void ExtractTypeRenderData(plMsgExtractRenderData& ref_msg, const plTransform& instanceTransform) const = 0;

protected:
  plParticleType();

  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override {}
  virtual void StepParticleSystem(const plTime& tDiff, plUInt32 uiNumNewParticles) { m_TimeDiff = tDiff; }

  static plUInt32 ComputeSortingKey(plParticleTypeRenderMode::Enum mode, plUInt64 uiResource1Hash, plUInt64 uiResource2Hash);

  plTime m_TimeDiff;
  mutable plUInt64 m_uiLastExtractedFrame;
};
