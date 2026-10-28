#pragma once

#include <Foundation/CodeUtils/Expression/ExpressionByteCode.h>
#include <Foundation/CodeUtils/Expression/ExpressionVM.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Vec4.h>
#include <Foundation/Strings/StringBuilder.h>
#include <Foundation/Threading/Mutex.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <ParticlePlugin/System/ParticleProgram.h>

class plAbstractMemberProperty;
class plParticleModule;
class plParticleSystemDescriptor;
class plParticleSystemInstance;
class plReflectedClass;

/// \brief Names of the inputs a per-frame graph program can read.
///
/// The editor names the program's input streams with these; the runtime fills each one before the
/// program runs. Effect parameters are read by name, so they use a prefix instead of a fixed name.
namespace plParticleFrameInput
{
  constexpr const char* Time = "Time";                       ///< seconds on the effect's own clock
  constexpr const char* DeltaTime = "DeltaTime";             ///< length of the current step
  constexpr const char* EffectPosition = "EffectPosition";   ///< Float3, simulation space
  constexpr const char* EffectForward = "EffectForward";     ///< Float3, the effect's +X
  constexpr const char* EffectUp = "EffectUp";               ///< Float3, the effect's +Z
  constexpr const char* EffectVelocity = "EffectVelocity";   ///< Float3, the owner's velocity
  constexpr const char* CameraPosition = "CameraPosition";   ///< Float3, the world's main camera
  constexpr const char* CameraDistance = "CameraDistance";   ///< effect origin to the main camera
  constexpr const char* ParticleCount = "ParticleCount";     ///< alive particles at the start of the step
  constexpr const char* SpawnScale = "SpawnScale";           ///< LOD x budget x Particles.SpawnCountScale
  constexpr const char* FloatParameterPrefix = "Param.";     ///< + name: an effect float parameter
  constexpr const char* ColorParameterPrefix = "ColorParam."; ///< + name: an effect colour parameter, Float4
  constexpr const char* BlackboardPrefix = "Blackboard.";     ///< + "<blackboard>|<entry>": a global blackboard entry, Float4
  constexpr const char* BlackboardFoundPrefix = "BlackboardFound."; ///< + the same: 1 when the entry exists, else 0
} // namespace plParticleFrameInput

/// \brief The name of the input that reads a global blackboard's entry (or, with bFound, whether it exists).
/// An empty blackboard name is the "Global" blackboard.
PL_PARTICLEPLUGIN_DLL void plParticleMakeBlackboardInput(plStringView sBlackboard, plStringView sEntry, bool bFound, plStringBuilder& out_sName);

/// \brief Whether "<blackboard>|<entry>" names an entry of a global blackboard that a Blackboard node can read.
PL_PARTICLEPLUGIN_DLL bool plParticleHasBlackboardEntry(plStringView sPath);

/// \brief Particle systems update on task threads and plBlackboard has no locking of its own, so the
/// particle code reads and writes blackboards under this mutex.
PL_PARTICLEPLUGIN_DLL plMutex& plParticleGetBlackboardMutex();

/// \brief One block property whose value comes from a per-frame program output.
struct PL_PARTICLEPLUGIN_DLL plParticleDrivenProperty
{
  enum class Module : plUInt8
  {
    Emitter,
    Initializer,
    Behavior,
    Type,
  };

  Module m_Module = Module::Emitter;
  plUInt16 m_uiModuleIndex = 0; ///< index into the descriptor's list of that module kind
  plUInt16 m_uiOutput = 0;      ///< the program output named "Out<index>"
  plString m_sProperty;         ///< reflected member property of the module's factory

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief The per-frame part of a system's graph: one program and the properties it drives.
///
/// Built by the editor from the wires whose sources change over time (effect parameters, time,
/// the effect's transform). Wires from constants are folded into the blocks instead and never
/// appear here.
class PL_PARTICLEPLUGIN_DLL plParticleFrameProgram
{
public:
  bool IsEmpty() const { return m_Driven.IsEmpty() || m_ByteCode.IsEmpty(); }

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);

  plExpressionByteCode m_ByteCode;
  plDynamicArray<plParticleDrivenProperty> m_Driven;
  plParticleProgramAssets m_Assets;
};

/// \brief Builds the value a driven property is set to from a program output.
///
/// Keeps the property's type: a number lands as float, int, time, angle (in degrees) or the value
/// of a variance pair, whose variance stays as authored. Vectors and colours take as many
/// components as they have. Returns an invalid variant for types that cannot be driven.
PL_PARTICLEPLUGIN_DLL plVariant plParticleMakeDrivenValue(const plVariant& current, const plVec4& vValue);

/// \brief Runs a system's per-frame program and writes its outputs into the driven blocks.
///
/// Every driven block gets a private copy of its factory. The program's outputs are written into
/// that copy, and the block takes them through its ordinary property copy, so any number property
/// can be driven without the block knowing about it. A system lowered to the GPU reconfigures its
/// GPU type the same way, which uploads the new values with the next simulation step.
class PL_PARTICLEPLUGIN_DLL plParticleFrameProgramRunner
{
public:
  plParticleFrameProgramRunner();
  ~plParticleFrameProgramRunner();

  /// Prepares the program of pTemplate for pSystem, whose modules must already be created.
  void Configure(const plParticleSystemDescriptor* pTemplate, plParticleSystemInstance* pSystem, bool bLoweredToGPU);

  void Clear();
  bool IsActive() const { return m_pProgram != nullptr; }

  /// Runs the program once and pushes changed values into the blocks. Call before the modules step.
  void Evaluate(const plTime& diff);

private:
  struct Input
  {
    plString m_sSource;      ///< the frame input it reads; the byte code may read one component of it
    plInt32 m_iComponent = -1; ///< the component of a split input, or -1 for the whole value
    plUInt32 m_uiComponents = 1;
    plUInt32 m_uiOffset = 0; ///< first float in m_InputData
  };

  struct Clone
  {
    plParticleDrivenProperty::Module m_Module = plParticleDrivenProperty::Module::Emitter;
    plUInt16 m_uiModuleIndex = 0;
    plReflectedClass* m_pFactory = nullptr; ///< owned
    plParticleModule* m_pModule = nullptr;  ///< null when the block runs inside the lowered GPU type
    bool m_bDirty = false;
  };

  struct Driven
  {
    const plAbstractMemberProperty* m_pProperty = nullptr;
    plUInt32 m_uiClone = 0;
    plUInt32 m_uiOutputOffsets[4] = {}; ///< the float in m_OutputData of each component
    plUInt32 m_uiComponents = 1;
    plVec4 m_vLastValue = plVec4(plMath::NaN<float>());
  };

  void FillInputs(const plTime& diff);
  void ApplyLowered();

  const plParticleFrameProgram* m_pProgram = nullptr;
  const plParticleSystemDescriptor* m_pTemplate = nullptr;
  plParticleSystemInstance* m_pSystem = nullptr;

  plExpressionVM m_VM;
  plParticleProgramTables m_Tables;

  // four floats per component, so the VM may run a whole SIMD lane on one instance
  plDynamicArray<float> m_InputData;
  plDynamicArray<float> m_OutputData;
  plDynamicArray<Input> m_Inputs;
  plDynamicArray<plProcessingStream> m_InputStreams;
  plDynamicArray<plProcessingStream> m_OutputStreams;

  plHybridArray<Clone, 4> m_Clones;
  plHybridArray<Driven, 8> m_Driven;
};
