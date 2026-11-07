#pragma once

#include <ParticlePlugin/ParticlePluginDLL.h>

#include <Foundation/Strings/String.h>
#include <Foundation/Types/ArrayPtr.h>

class plParticleBehaviorFactory;
class plParticleSystemDescriptor;
class plParticleTypeFactory;
class plParticleTypeGPU;
class plParticleSystemInstance;

/// Translates a system's CPU module list into a configured plParticleTypeGPU.
///
/// Emitters and initializers keep running on the CPU (they only produce spawn payloads), so
/// only the per-frame behaviors and the render type need translating. A system lowers when its
/// type is a plain billboard quad or a point type and every behavior has a GPU equivalent.
struct PL_PARTICLEPLUGIN_DLL plParticleGPULowering
{
  /// Which of the descriptor's lists the blocking module sits in, so a tool can point at it.
  enum class BlockerKind : plUInt8
  {
    None,
    Behavior,
    Type,
    Initializer,
  };

  /// Returns an empty string when the descriptor can be lowered, otherwise why not: the type name of
  /// the first module that has no GPU equivalent, or what keeps its graph programs off the GPU.
  ///
  /// The optional outputs identify which module it was: the editor uses them to badge the block
  /// rather than only reporting a reason after the effect has already fallen back.
  static plString FindLoweringBlocker(
    const plParticleSystemDescriptor* pDescriptor, BlockerKind* out_pKind = nullptr, plUInt32* out_uiIndex = nullptr);

  /// Configures the (freshly created) GPU type from the descriptor's type + behavior factories.
  /// Bakes gradient / curve LUTs and noise / vector-field textures as shared resources.
  static void ConfigureLoweredType(plParticleTypeGPU* pType, const plParticleSystemDescriptor* pDescriptor, plParticleSystemInstance* pOwner);

  /// Same, from explicit lists. A system with a per-frame program passes its driven copies of the
  /// factories here every time a driven value changes, which re-uploads the values to the GPU.
  static void ConfigureLoweredType(plParticleTypeGPU* pType, plArrayPtr<const plParticleBehaviorFactory* const> behaviors,
    plArrayPtr<const plParticleTypeFactory* const> types, plParticleSystemInstance* pOwner);
};
