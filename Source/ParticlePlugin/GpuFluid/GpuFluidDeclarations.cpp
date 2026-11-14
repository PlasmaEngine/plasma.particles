#include <ParticlePlugin/ParticlePluginPCH.h>

#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidQuality, 1)
  PL_ENUM_CONSTANT(plGpuFluidQuality::Low),
  PL_ENUM_CONSTANT(plGpuFluidQuality::Medium),
  PL_ENUM_CONSTANT(plGpuFluidQuality::High),
  PL_ENUM_CONSTANT(plGpuFluidQuality::Ultra),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidGasTickRate, 1)
  PL_ENUM_CONSTANT(plGpuFluidGasTickRate::Quality),
  PL_ENUM_CONSTANT(plGpuFluidGasTickRate::Hz30),
  PL_ENUM_CONSTANT(plGpuFluidGasTickRate::Hz60),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidKind, 1)
  PL_ENUM_CONSTANT(plGpuFluidKind::Gas),
  PL_ENUM_CONSTANT(plGpuFluidKind::Liquid),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidDebugSource, 1)
  PL_ENUM_CONSTANT(plGpuFluidDebugSource::None),
  PL_ENUM_CONSTANT(plGpuFluidDebugSource::AnalyticSphere),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidEmitterShape, 1)
  PL_ENUM_CONSTANT(plGpuFluidEmitterShape::None),
  PL_ENUM_CONSTANT(plGpuFluidEmitterShape::Sphere),
  PL_ENUM_CONSTANT(plGpuFluidEmitterShape::Box),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_BITFLAGS(plGpuFluidFaces, 1)
  PL_BITFLAGS_CONSTANTS(plGpuFluidFaces::NegX, plGpuFluidFaces::PosX, plGpuFluidFaces::NegY, plGpuFluidFaces::PosY, plGpuFluidFaces::NegZ, plGpuFluidFaces::PosZ)
PL_END_STATIC_REFLECTED_BITFLAGS;
// clang-format on

// The gas tiers, rewritten from the P2 Release measurements on a base M4 (plan amendment A1.1: the 5 ms gate missed with
// the design plan's estimates, Medium at 60 Hz measured 8.35 ms a frame). The CSVs are in Utilities/GpuFluid/Timing/,
// the table with its cost model in Utilities/GpuFluid/README.md ("Gas tiers").
//
// Measured cost (sparse gate scene, about 990 active bricks, two plumes): a Medium tick (scalar MacCormack, velocity
// semi-Lagrangian, vorticity, two V(2,2) cycles) is about 4.0 us per active brick, plus about 0.7 us with the velocity's
// MacCormack correction; Alloc and Finalize add about 0.25 ms a frame at 1000 bricks.
//
// - Low: 30 Hz, semi-Lagrangian, no vorticity, one V-cycle.
// - Medium: 30 Hz (one tick per 30 fps frame, one every other 60 fps frame), the only rate at which the 1536 brick pool
//   60% full stays under 5 ms a frame; at 60 Hz the same pool costs 8.4 ms at 30 fps. Velocity MacCormack stays off: it
//   adds about 0.7 ms a tick and gains gas in closed boxes (N4).
// - High (M4 Pro target, not measurable here): 60 Hz with velocity MacCormack. On a base M4 the model gives about
//   18 ms a frame at 30 fps for the 3072 pool 60% full, so it is not a base M4 tier.
// - Ultra: capture only.
static const plGpuFluidGasTier s_GasTiers[] = {
  {512, 1.0f / 30.0f, 1, false, false, false},
  {1536, 1.0f / 30.0f, 2, true, false, true},
  {3072, 1.0f / 60.0f, 2, true, true, true},
  {6144, 1.0f / 60.0f, 3, true, true, true},
};

const plGpuFluidGasTier& plGpuFluidGetGasTier(plGpuFluidQuality::Enum quality)
{
  const plUInt32 uiIndex = plMath::Min(static_cast<plUInt32>(quality), static_cast<plUInt32>(PL_ARRAY_SIZE(s_GasTiers) - 1));
  return s_GasTiers[uiIndex];
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_GpuFluidDeclarations);
