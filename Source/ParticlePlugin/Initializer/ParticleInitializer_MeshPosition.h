#pragma once

#include <Foundation/Types/VarianceTypes.h>
#include <ParticlePlugin/Initializer/ParticleInitializer.h>
#include <RendererCore/Meshes/CpuMeshResource.h>

struct PL_PARTICLEPLUGIN_DLL plParticleMeshSpawnMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Surface, ///< area-weighted random point on the mesh surface
    Vertex,  ///< random mesh vertex

    Default = Surface
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleMeshSpawnMode);

class PL_PARTICLEPLUGIN_DLL plParticleInitializerFactory_MeshPosition final : public plParticleInitializerFactory
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializerFactory_MeshPosition, plParticleInitializerFactory);

public:
  plParticleInitializerFactory_MeshPosition();

  virtual const plRTTI* GetInitializerType() const override;
  virtual void CopyInitializerProperties(plParticleInitializer* pInitializer, bool bFirstTime) const override;

  virtual void Save(plStreamWriter& inout_stream) const override;
  virtual void Load(plStreamReader& inout_stream) override;

  virtual void QueryFinalizerDependencies(plSet<const plRTTI*>& inout_finalizerDeps) const override;

public:
  plString m_sMesh;
  plEnum<plParticleMeshSpawnMode> m_SpawnMode;
  plVec3 m_vPositionOffset;
  float m_fScale;
  bool m_bSetVelocity; // along the surface normal
  plVarianceTypeFloat m_Speed;
};


/// Spawns particles on the surface (or the vertices) of a mesh.
///
/// The surface normal at the spawn point is written to the 'Axis' stream, so quad types using
/// the Fixed: Start Axis orientation align to the mesh surface. With SetVelocity, particles
/// move along the normal.
class PL_PARTICLEPLUGIN_DLL plParticleInitializer_MeshPosition final : public plParticleInitializer
{
  PL_ADD_DYNAMIC_REFLECTION(plParticleInitializer_MeshPosition, plParticleInitializer);

public:
  plCpuMeshResourceHandle m_hMesh;
  plEnum<plParticleMeshSpawnMode> m_SpawnMode;
  plVec3 m_vPositionOffset;
  float m_fScale = 1.0f;
  bool m_bSetVelocity = false;
  plVarianceTypeFloat m_Speed;

  virtual void CreateRequiredStreams() override;

protected:
  virtual void InitializeElements(plUInt64 uiStartIndex, plUInt64 uiNumElements) override;

  /// Rebuilds the triangle-area CDF if the mesh resource changed. Returns false if the mesh is not usable.
  bool UpdateMeshSamplingData(const plCpuMeshResource& mesh);

  plProcessingStream* m_pStreamPosition = nullptr;
  plProcessingStream* m_pStreamVelocity = nullptr;
  plProcessingStream* m_pStreamAxis = nullptr;

  // area CDF over all triangles, used for area-weighted surface sampling
  plDynamicArray<float> m_TriangleAreaCdf;
  float m_fTotalArea = 0.0f;
  plUInt64 m_uiCdfMeshIdHash = 0;      // which mesh the CDF was built for
  plUInt32 m_uiCdfChangeCounter = 0;   // resource change counter at build time (mesh hot-reload)
};
