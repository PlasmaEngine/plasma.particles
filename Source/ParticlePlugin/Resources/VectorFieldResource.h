#pragma once

#include <Core/ResourceManager/Resource.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Vec3.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

using plVectorFieldResourceHandle = plTypedResourceHandle<class plVectorFieldResource>;

/// A 3D grid of direction vectors, sampled by the Vector Field particle behavior.
///
/// The grid is unit-less: the behavior decides what world-space box the field maps onto.
/// Data is stored x-fastest (x, then y, then z slices).
struct PL_PARTICLEPLUGIN_DLL plVectorFieldResourceDescriptor
{
  plVec3U32 m_vDimensions = plVec3U32::MakeZero();
  plDynamicArray<plVec3> m_Vectors;

  /// Fills the grid with divergence-free curl noise - the classic swirling smoke/dust motion.
  void InitializeCurlNoise(plUInt32 uiResolution, plUInt32 uiSeed, float fFrequency, plUInt32 uiOctaves);

  /// Parses the FGA (Fluid Grid ASCII) format: dimX,dimY,dimZ, min xyz, max xyz, then dim^3 vectors.
  /// The authored bounds are ignored - the particle behavior supplies the world-space mapping.
  plResult ParseFga(plStringView sText);

  /// Trilinear sample at a normalized [0;1]^3 position, clamped at the borders.
  plVec3 SampleTrilinear(const plVec3& vNormPos) const;

  bool IsValid() const { return m_vDimensions.x >= 2 && m_vDimensions.y >= 2 && m_vDimensions.z >= 2 && m_Vectors.GetCount() == m_vDimensions.x * m_vDimensions.y * m_vDimensions.z; }
};

/// Loads .fga files directly (reference them by path); procedural fields are created
/// through the descriptor (see plParticleBehavior_VectorField's curl-noise source).
class PL_PARTICLEPLUGIN_DLL plVectorFieldResource final : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plVectorFieldResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plVectorFieldResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plVectorFieldResource, plVectorFieldResourceDescriptor);

public:
  plVectorFieldResource();
  ~plVectorFieldResource();

  const plVectorFieldResourceDescriptor& GetDescriptor() const { return m_Desc; }

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* Stream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  plVectorFieldResourceDescriptor m_Desc;
};
