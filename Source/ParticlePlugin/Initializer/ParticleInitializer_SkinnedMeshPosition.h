#pragma once

#include <Foundation/Types/VarianceTypes.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_MeshPosition.h>
#include <RendererCore/Meshes/CpuMeshResource.h>

class PL_PARTICLEPLUGIN_DLL plParticleInitializerFactory_SkinnedMeshPosition final : public plParticleInitializerFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializerFactory_SkinnedMeshPosition, plParticleInitializerFactory);

public:
  plParticleInitializerFactory_SkinnedMeshPosition();

  virtual const plRTTI* GetInitializerType() const override;
  virtual void CopyInitializerProperties(plParticleInitializer* pInitializer, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

public:
  plString m_sMesh;
  plEnum<plParticleMeshSpawnMode> m_SpawnMode;
  bool m_bSetVelocity = false;
  plVarianceTypeFloat m_Speed;
};

/// Spawns particles on the CURRENT pose of an animated mesh.
///
/// The mesh must match the one rendered by the plAnimatedMeshComponent that the particle
/// component references (or finds on its owner hierarchy) - the sampled bind-pose points are
/// skinned with that component's bone matrices, snapshotted once per frame. The surface normal
/// goes into the Axis stream; with SetVelocity, particles move along the normal.
class PL_PARTICLEPLUGIN_DLL plParticleInitializer_SkinnedMeshPosition final : public plParticleInitializer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializer_SkinnedMeshPosition, plParticleInitializer);

public:
  plCpuMeshResourceHandle m_hMesh;
  plEnum<plParticleMeshSpawnMode> m_SpawnMode;
  bool m_bSetVelocity = false;
  plVarianceTypeFloat m_Speed;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  bool UpdateMeshSamplingData(const plCpuMeshResource& mesh);

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamAxis = nullptr;

  // area CDF over the bind-pose triangles, used for area-weighted surface sampling
  plDynamicArray<float> m_TriangleAreaCdf;
  float m_fTotalArea = 0.0f;
  plUInt64 m_uiCdfMeshIdHash = 0;
  plUInt32 m_uiCdfChangeCounter = 0;
};
