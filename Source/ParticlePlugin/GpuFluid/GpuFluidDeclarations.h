#pragma once

#include <Foundation/Math/Vec3.h>
#include <Foundation/Reflection/Reflection.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

/// \brief Quality tier of a GpuFluid volume: pool sizes, tick rate, solver iterations.
///
/// Ultra is meant for offline captures, not for realtime use.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidQuality
{
  using StorageType = plUInt8;

  enum Enum
  {
    Low,
    Medium,
    High,
    Ultra,

    Default = Medium
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidQuality);

/// \brief What a GpuFluid volume simulates.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidKind
{
  using StorageType = plUInt8;

  enum Enum
  {
    Gas,    ///< Sparse brick grid: smoke, fire, explosions. Rendered by the VDB march.
    Liquid, ///< Dense MAC domain with particles: water. Rendered as an extracted surface.

    Default = Gas
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidKind);

/// \brief Where a gas volume's content comes from until the solver exists.
///
/// P0 has no solver: the analytic sphere is a known density written by compute into the same sparse layout the
/// solver will write, so the hand-off to the VDB march can be checked against a baked volume.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidDebugSource
{
  using StorageType = plUInt8;

  enum Enum
  {
    None,           ///< Nothing is written. The volume publishes nothing.
    AnalyticSphere, ///< A sphere of density and temperature, optionally pulsing.

    Default = AnalyticSphere
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidDebugSource);

/// Brick layout shared with the VDB march (VDB_BRICK_* in Shaders/Common/VDBVolume.hlsli).
constexpr plUInt32 plGpuFluidBrickSize = 8;
constexpr plUInt32 plGpuFluidBrickStore = 10;

/// \brief Settings of a gas volume that decide its GPU resources. Changing any of them recreates the volume.
///
/// Versioned with the component that serializes it.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidGasSettings
{
  static constexpr plUInt32 s_uiVersion = 1;

  /// Bricks of the virtual grid per axis. The grid is fixed in the volume's space.
  plVec3U32 m_vBrickGrid = plVec3U32(8, 8, 8);

  /// Most bricks that can hold content at once (the atlas pool).
  plUInt32 m_uiMaxBricks = 512;

  plGpuFluidQuality::Enum m_Quality = plGpuFluidQuality::Default;

  bool operator==(const plGpuFluidGasSettings& other) const
  {
    return m_vBrickGrid == other.m_vBrickGrid && m_uiMaxBricks == other.m_uiMaxBricks && m_Quality == other.m_Quality;
  }
};

/// \brief Settings of a liquid volume that decide its GPU resources. Not used before P6.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidSettings
{
  static constexpr plUInt32 s_uiVersion = 1;

  /// Cells of the dense domain per axis.
  plVec3U32 m_vCells = plVec3U32(128, 96, 64);

  /// Most particles the pool holds.
  plUInt32 m_uiMaxParticles = 1024 * 1024;

  plGpuFluidQuality::Enum m_Quality = plGpuFluidQuality::Default;
};
