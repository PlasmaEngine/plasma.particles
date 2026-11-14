#include <ParticlePlugin/ParticlePluginPCH.h>

#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidQuality, 1)
  PL_ENUM_CONSTANT(plGpuFluidQuality::Low),
  PL_ENUM_CONSTANT(plGpuFluidQuality::Medium),
  PL_ENUM_CONSTANT(plGpuFluidQuality::High),
  PL_ENUM_CONSTANT(plGpuFluidQuality::Ultra),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidKind, 1)
  PL_ENUM_CONSTANT(plGpuFluidKind::Gas),
  PL_ENUM_CONSTANT(plGpuFluidKind::Liquid),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plGpuFluidDebugSource, 1)
  PL_ENUM_CONSTANT(plGpuFluidDebugSource::None),
  PL_ENUM_CONSTANT(plGpuFluidDebugSource::AnalyticSphere),
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_GpuFluidDeclarations);
