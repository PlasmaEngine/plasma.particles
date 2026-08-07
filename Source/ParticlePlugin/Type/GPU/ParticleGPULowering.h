#pragma once

#include <ParticlePlugin/ParticlePluginDLL.h>

#include <Foundation/Strings/StringView.h>

class plParticleSystemDescriptor;
class plParticleTypeGPU;
class plParticleSystemInstance;

/// Translates a system's CPU module list into a configured plParticleTypeGPU.
///
/// Emitters and initializers keep running on the CPU (they only produce spawn payloads), so
/// only the per-frame behaviors and the render type need translating. A system lowers when its
/// type is a plain billboard quad or a point type and every behavior has a GPU equivalent.
struct PL_PARTICLEPLUGIN_DLL plParticleGPULowering
{
  /// Returns an empty view when the descriptor can be lowered, otherwise the type name of the
  /// first module that has no GPU equivalent.
  static plStringView FindLoweringBlocker(const plParticleSystemDescriptor* pDescriptor);

  /// Configures the (freshly created) GPU type from the descriptor's type + behavior factories.
  /// Bakes gradient / curve LUTs and noise / vector-field textures as shared resources.
  static void ConfigureLoweredType(plParticleTypeGPU* pType, const plParticleSystemDescriptor* pDescriptor, plParticleSystemInstance* pOwner);
};
