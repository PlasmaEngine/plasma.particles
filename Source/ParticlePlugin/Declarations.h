#pragma once

#include <Core/ResourceManager/ResourceHandle.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Strings/HashedString.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Id.h>
#include <Foundation/Types/RefCounted.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

class plWorld;
class plParticleSystemDescriptor;
class plParticleEventReactionFactory;
class plParticleEventReaction;
class plParticleEmitter;
class plParticleInitializer;
class plParticleBehavior;
class plParticleType;
class plProcessingStreamGroup;
class plProcessingStream;
class plRandom;
struct plParticleEvent;
class plParticleEffectDescriptor;
class plParticleWorldModule;
class plParticleEffectInstance;
class plParticleSystemInstance;
struct plRenderViewContext;
class plRenderPipelinePass;
class plParticleFinalizer;
class plParticleFinalizerFactory;

using plParticleEffectResourceHandle = plTypedResourceHandle<class plParticleEffectResource>;

using plParticleEffectId = plGenericId<22, 10>;

/// \brief A handle to a particle effect
class PL_PARTICLEPLUGIN_DLL plParticleEffectHandle
{
  PL_DECLARE_HANDLE_TYPE(plParticleEffectHandle, plParticleEffectId);
};


struct PL_PARTICLEPLUGIN_DLL plParticleSystemState
{
  enum Enum
  {
    Active,
    EmittersFinished,
    OnlyReacting,
    Inactive,
  };
};

class PL_PARTICLEPLUGIN_DLL plParticleStreamBinding
{
public:
  void UpdateBindings(const plProcessingStreamGroup* pGroup) const;
  void Clear() { m_Bindings.Clear(); }

private:
  friend class plParticleSystemInstance;

  struct Binding
  {
    plString m_sName;
    plProcessingStream** m_ppStream;
  };

  plHybridArray<Binding, 4> m_Bindings;
};

//////////////////////////////////////////////////////////////////////////

struct PL_PARTICLEPLUGIN_DLL plParticleTypeRenderMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Additive,
    Blended,
    Opaque,
    Distortion,
    BlendedBackground,
    BlendedForeground,
    BlendAdd,
    Default = Additive
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleTypeRenderMode);

//////////////////////////////////////////////////////////////////////////

struct PL_PARTICLEPLUGIN_DLL plParticleLightingMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Fullbright,
    VertexLit,
    SixWay,   ///< blends six baked directional light responses, giving a flat card internal volume
    PerPixel, ///< full clustered lighting per pixel; for small bright particles, not for smoke
    Default = Fullbright
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleLightingMode);

//////////////////////////////////////////////////////////////////////////

/// \brief What to do when an effect is not visible.
struct PL_PARTICLEPLUGIN_DLL plEffectInvisibleUpdateRate
{
  using StorageType = plUInt8;

  enum Enum
  {
    FullUpdate,
    Max20fps,
    Max10fps,
    Max5fps,
    Pause,
    Discard,

    Default = Max10fps
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plEffectInvisibleUpdateRate);

//////////////////////////////////////////////////////////////////////////

/// \brief Whether an effect may be throttled when the global particle budget is exceeded.
struct PL_PARTICLEPLUGIN_DLL plParticleEffectImportance
{
  using StorageType = plUInt8;

  enum Enum
  {
    Cosmetic, ///< spawn counts scale down when the world exceeds 'Particles.MaxParticles'
    Gameplay, ///< never throttled - the effect carries gameplay-relevant information

    Default = Cosmetic
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleEffectImportance);

//////////////////////////////////////////////////////////////////////////

struct PL_PARTICLEPLUGIN_DLL plParticleTextureAtlasType
{
  using StorageType = plUInt8;

  enum Enum
  {
    None,

    RandomVariations,
    FlipbookAnimation,
    RandomYAnimatedX,

    Default = None
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleTextureAtlasType);

//////////////////////////////////////////////////////////////////////////

struct PL_PARTICLEPLUGIN_DLL plParticleColorGradientMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Age,
    Speed,

    Default = Age
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleColorGradientMode);


//////////////////////////////////////////////////////////////////////////

struct PL_PARTICLEPLUGIN_DLL plParticleOutOfBoundsMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Teleport,
    Die,

    Default = Teleport
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleOutOfBoundsMode);

//////////////////////////////////////////////////////////////////////////

////// Render type for GPU particles
struct PL_PARTICLEPLUGIN_DLL plGPUParticleRenderType
{
  using StorageType = plUInt8;

  enum Enum
  {
    Billboard,       ///< Camera-facing billboard quads
    Point,           ///< Single-pixel points
    VelocityAligned, ///< Quads stretched along velocity direction
    Trail,           ///< Ribbon trails following particle paths

    Default = Billboard
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plGPUParticleRenderType);

//////////////////////////////////////////////////////////////////////////

/// Where a particle system's per-frame simulation runs.
///
/// Spawning always happens on the CPU. With GPU / Auto, the system's behaviors are lowered
/// into the GPU simulate shader when every module has a GPU equivalent; Auto falls back to
/// CPU silently, GPU falls back with a warning naming the blocking module.
struct PL_PARTICLEPLUGIN_DLL plParticleSimulationTarget
{
  using StorageType = plUInt8;

  enum Enum
  {
    CPU,
    GPU,
    Auto,

    Default = CPU
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleSimulationTarget);

//////////////////////////////////////////////////////////////////////////

struct plParticleEffectFloatParam
{
  PL_DECLARE_POD_TYPE();
  plHashedString m_sName;
  float m_Value;
};

struct plParticleEffectColorParam
{
  PL_DECLARE_POD_TYPE();
  plHashedString m_sName;
  plColor m_Value;
};

class plParticleEffectParameters final : public plRefCounted
{
public:
  plHybridArray<plParticleEffectFloatParam, 2> m_FloatParams;
  plHybridArray<plParticleEffectColorParam, 2> m_ColorParams;
};
