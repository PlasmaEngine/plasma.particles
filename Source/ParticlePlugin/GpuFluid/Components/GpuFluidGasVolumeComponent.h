#pragma once

#include <Core/World/World.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidGpuManager.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>
#include <RendererCore/Volumes/VDBVolumeComponent.h>

struct plMsgUpdateLocalBounds;
struct plMsgExtractRenderData;

using plGpuFluidGasVolumeComponentManager = plComponentManagerSimple<class plGpuFluidGasVolumeComponent, plComponentUpdateType::Always>;

/// \brief A box of simulated gas, rendered by the VDB march straight from the GPU.
///
/// The volume is a sparse grid of 8 cubed bricks fixed in the object's space: Extents over VoxelSize, rounded up to
/// whole bricks. Its atlas and brick table live on the GPU and are handed to the march every frame as
/// plVDBVolumeGpuRenderData, with no resource and no copy in between.
///
/// P0: there is no solver yet. The content comes from the analytic debug source, a sphere of density and temperature
/// that can pulse, which is what checks the hand-off against a baked volume.
class PL_PARTICLEPLUGIN_DLL plGpuFluidGasVolumeComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidGasVolumeComponent, plComponent, plGpuFluidGasVolumeComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plGpuFluidGasVolumeComponent

public:
  plGpuFluidGasVolumeComponent();
  ~plGpuFluidGasVolumeComponent();

  /// Authored size of the box, in metres. The grid rounds it up to whole bricks.
  plVec3 m_vExtents = plVec3(4.0f);                                   // [ property ]
  float m_fVoxelSize = 0.0625f;                                       // [ property ] metres
  plUInt32 m_uiMaxBricks = 512;                                       // [ property ] atlas pool
  plEnum<plGpuFluidQuality> m_Quality;                                // [ property ]

  plEnum<plGpuFluidDebugSource> m_DebugSource;                        // [ property ]
  float m_fSphereRadius = 1.4f;                                       // [ property ] metres
  float m_fSphereSoftness = 0.5f;                                     // [ property ] metres from full density to none
  float m_fPulseAmplitude = 0.15f;                                    // [ property ] fraction of the radius
  float m_fPulsePeriod = 2.5f;                                        // [ property ] seconds
  float m_fPeakDensity = 1.0f;                                        // [ property ] atlas units, before DensityScale
  float m_fPeakTemperature = 0.8f;                                    // [ property ] 0..1
  float m_fAllocThreshold = 1e-3f;                                    // [ property ] peak density a brick needs to be kept

  float m_fDensityScale = 4.0f;                                       // [ property ]
  plColor m_Albedo = plColor::White;                                  // [ property ]
  plColor m_EmissionColor = plColor::Black;                           // [ property ]
  float m_fEmissionScale = 0.0f;                                      // [ property ]
  float m_fAnisotropy = 0.2f;                                         // [ property ]
  plEnum<plVDBVolumeRenderMode> m_RenderMode;                         // [ property ]

  /// Sets the pulse clock, so a scene looks the same whatever ran before it.
  void ResetClock(plTime time = plTime::MakeZero()) { m_PulseTime = time; }

  /// Bricks per axis of the virtual grid.
  plVec3U32 GetBrickGrid() const;

  /// The box the grid actually covers: the extents rounded up to whole bricks.
  plVec3 GetGridExtents() const;

  /// What the GPU counted, two to three frames late. False until the first readback.
  bool GetStats(plGpuFluidGasStats& out_stats) const;

  /// Whether the volume was handed to the march in the last extraction.
  bool WasPublished() const { return m_bPublished; }

protected:
  void Update();
  void OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg);
  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;

private:
  plGpuFluidGasSettings ComputeSettings() const;
  void EnsureVolume();
  void ReleaseVolume();

  plGpuFluidGpuManager* m_pManager = nullptr;
  plGpuFluidVolumeId m_VolumeId = 0;
  plGpuFluidGasSettings m_ActiveSettings;
  plTime m_PulseTime;

  /// The grid extents the owner's local bounds were last computed from; a change to Extents or VoxelSize refreshes them.
  plVec3 m_vBoundsExtents = plVec3::MakeZero();
  mutable bool m_bPublished = false;
};
