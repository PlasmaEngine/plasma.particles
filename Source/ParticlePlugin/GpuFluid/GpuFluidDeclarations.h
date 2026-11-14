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

/// \brief Replaces a gas volume's simulation with a known analytic content.
///
/// The analytic sphere is a known density written by compute into the same sparse layout the solver writes, so the
/// hand-off to the VDB march can be checked against a baked volume (N0).
struct PL_PARTICLEPLUGIN_DLL plGpuFluidDebugSource
{
  using StorageType = plUInt8;

  enum Enum
  {
    None,           ///< No debug source: the gas solver simulates the volume.
    AnalyticSphere, ///< A sphere of density and temperature, optionally pulsing. No simulation.

    Default = None
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidDebugSource);

/// \brief Shape of a gas source.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidEmitterShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    None,   ///< The source is off.
    Sphere, ///< Radius, with a smooth edge.
    Box,    ///< Half extents, with a smooth edge.

    Default = Sphere
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidEmitterShape);

/// \brief Faces of a fluid volume's box, in its local space. A set bit marks the face open: gas leaves through it and
/// the pressure is zero beyond it. A closed face is a wall.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidFaces
{
  using StorageType = plUInt8;

  enum Enum
  {
    NegX = PL_BIT(0),
    PosX = PL_BIT(1),
    NegY = PL_BIT(2),
    PosY = PL_BIT(3),
    NegZ = PL_BIT(4),
    PosZ = PL_BIT(5),

    /// Smoke over a floor: every face open but the bottom.
    Default = NegX | PosX | NegY | PosY | PosZ
  };

  struct Bits
  {
    StorageType NegX : 1;
    StorageType PosX : 1;
    StorageType NegY : 1;
    StorageType PosY : 1;
    StorageType NegZ : 1;
    StorageType PosZ : 1;
  };
};

PL_DECLARE_FLAGS_OPERATORS(plGpuFluidFaces);
PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGpuFluidFaces);

/// Brick layout shared with the VDB march (VDB_BRICK_* in Shaders/Common/VDBVolume.hlsli).
constexpr plUInt32 plGpuFluidBrickSize = 8;
constexpr plUInt32 plGpuFluidBrickStore = 10;

/// \brief Settings of a gas volume that decide its GPU resources. Changing any of them recreates the volume.
///
/// Versioned with the component that serializes it.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidGasSettings
{
  static constexpr plUInt32 s_uiVersion = 2;

  /// Bricks of the virtual grid per axis. The grid is fixed in the volume's space.
  plVec3U32 m_vBrickGrid = plVec3U32(8, 8, 8);

  /// Most bricks that can hold content at once (the atlas pool).
  plUInt32 m_uiMaxBricks = 512;

  plGpuFluidQuality::Enum m_Quality = plGpuFluidQuality::Default;

  /// The solver runs (false: an analytic debug source writes the atlas instead). The two need different GPU state.
  bool m_bSimulate = true;

  bool operator==(const plGpuFluidGasSettings& other) const
  {
    return m_vBrickGrid == other.m_vBrickGrid && m_uiMaxBricks == other.m_uiMaxBricks && m_Quality == other.m_Quality && m_bSimulate == other.m_bSimulate;
  }
};

/// Gas tick, seconds: 60 Hz, 30 Hz on Low (design plan, section 4). GpuFluid.Debug.StepTime overrides it.
constexpr float plGpuFluidGasTick = 1.0f / 60.0f;
constexpr float plGpuFluidGasTickLow = 1.0f / 30.0f;

/// Most gas ticks one frame runs; beyond that the simulation runs in slow motion.
constexpr plUInt32 plGpuFluidMaxGasTicksPerFrame = 2;

/// \brief Settings of a liquid volume that decide its GPU resources. Changing any of them recreates the volume.
struct PL_PARTICLEPLUGIN_DLL plGpuFluidLiquidSettings
{
  static constexpr plUInt32 s_uiVersion = 1;

  /// Cells of the dense domain per axis, whole bricks of 8.
  plVec3U32 m_vCells = plVec3U32(128, 96, 64);

  /// Most particles the pool holds.
  plUInt32 m_uiMaxParticles = 1024 * 1024;

  plGpuFluidQuality::Enum m_Quality = plGpuFluidQuality::Default;

  /// Edge of a cell, in metres.
  float m_fCellSize = 0.0625f;

  /// Particles a full cell holds at rest (n0): 4 or 8.
  plUInt32 m_uiParticlesPerCell = 4;

  bool operator==(const plGpuFluidLiquidSettings& other) const
  {
    return m_vCells == other.m_vCells && m_uiMaxParticles == other.m_uiMaxParticles && m_Quality == other.m_Quality && m_fCellSize == other.m_fCellSize &&
           m_uiParticlesPerCell == other.m_uiParticlesPerCell;
  }
};
