#pragma once

#include <Foundation/CodeUtils/Expression/ExpressionByteCode.h>
#include <Foundation/CodeUtils/Expression/ExpressionVM.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Vec4.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

class plParticleSystemInstance;

/// \brief The gradient and curve assets a program samples, by slot.
///
/// The byte code reads gradient slot i through PfxGradient(t, i, component) and curve slot i through
/// PfxCurve(t, i); t runs from 0 to 1 across the asset's own range.
struct PL_PARTICLEPLUGIN_DLL plParticleProgramAssets
{
  bool IsEmpty() const { return m_Gradients.IsEmpty() && m_Curves.IsEmpty(); }

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);

  plHybridArray<plString, 2> m_Gradients;
  plHybridArray<plString, 2> m_Curves;
};

/// \brief A program's gradients and curves baked into tables, for the byte code functions to read.
class PL_PARTICLEPLUGIN_DLL plParticleProgramTables
{
public:
  /// PfxGradient(float t, int slot, int component): one component (r, g, b, a) of a gradient, HDR.
  static plExpressionFunction s_GradientFunc;

  /// PfxCurve(float t, int slot): a curve's value.
  static plExpressionFunction s_CurveFunc;

  static constexpr plUInt32 s_uiTableSize = 64;

  /// Loads the assets, blocking until they are there, and samples each into a table. A missing
  /// gradient reads as white and a missing curve as zero.
  void Bake(const plParticleProgramAssets& assets);

  /// The global data to execute the byte code with; it points the functions at these tables.
  const plExpression::GlobalData& GetGlobalData() const { return m_GlobalData; }

  float SampleGradient(plUInt32 uiSlot, plUInt32 uiComponent, float t) const;
  float SampleCurve(plUInt32 uiSlot, float t) const;

  /// The baked tables, s_uiTableSize entries per slot, for the GPU programs.
  plArrayPtr<const plVec4> GetGradientTables() const { return m_Gradients; }
  plArrayPtr<const float> GetCurveTables() const { return m_Curves; }

private:
  plDynamicArray<plVec4> m_Gradients; ///< s_uiTableSize entries per slot
  plDynamicArray<float> m_Curves;
  plExpression::GlobalData m_GlobalData;
};

/// \brief The per-particle program of one block: its compiled wires, run over the particles.
///
/// The editor compiles every per-particle wire into a block's properties into one program, stored on
/// the block's factory. Output i is named "Out<i>" and computes the property m_Outputs[i].
/// Inputs are particle streams by their own name and type (Position, Velocity, Size, Color, LifeTime,
/// RotationSpeed, Axis, RandomSeed, Custom.<name>) plus any per-frame input (plParticleFrameInput),
/// whose one value is given to every particle.
struct PL_PARTICLEPLUGIN_DLL plParticleProgramData
{
  bool IsEmpty() const { return m_Outputs.IsEmpty() || m_ByteCode.IsEmpty(); }

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);

  plExpressionByteCode m_ByteCode;
  plHybridArray<plString, 2> m_Outputs;
  plParticleProgramAssets m_Assets;
};

/// \brief Marks a block property that takes per-particle wires.
///
/// Only a short list of properties has it (Drag, Gravity Factor, Attract Force, Scene Forces
/// Influence, Size Curve Scale, Color Gradient Tint). The block's factory holds the compiled wires
/// (plParticleBehaviorFactory::GetParticleProgram) and the block reads the value per particle; without
/// a wire it uses its own value, as before.
class PL_PARTICLEPLUGIN_DLL plParticlePerParticleAttribute : public plPropertyAttribute
{
  PL_ADD_DYNAMIC_REFLECTION(plParticlePerParticleAttribute, plPropertyAttribute);
};

/// \brief Runs a block's per-particle program over a range of particles.
///
/// The block calls CreateRequiredStreams from its own CreateRequiredStreams, so every stream the
/// program reads exists, then Run once per step (or per spawn batch) before writing its results.
class PL_PARTICLEPLUGIN_DLL plParticleProgramRunner
{
public:
  plParticleProgramRunner();
  ~plParticleProgramRunner();

  void Configure(const plParticleProgramData* pData);
  bool IsActive() const { return m_pData != nullptr; }

  /// Calls fnCreate(szName, type, ppStream) for every particle stream the program reads. The stream
  /// pointers live in this runner and must not move, so this is only called after Configure.
  template <typename CREATE>
  void CreateRequiredStreams(CREATE fnCreate)
  {
    for (Stream& stream : m_Streams)
    {
      fnCreate(stream.m_sName.GetData(), stream.m_DataType, &stream.m_pStream);
    }
  }

  /// Evaluates the program for the particles [uiStart, uiStart + uiCount).
  bool Run(const plParticleSystemInstance* pSystem, plUInt64 uiStart, plUInt64 uiCount, const plTime& diff);

  /// The results of the last Run for one property: uiComponents floats per particle, or null when the
  /// program does not compute that property.
  const float* GetOutput(plStringView sProperty, plUInt32& out_uiComponents) const;

  /// Run and GetOutput in one, for the first uiCount particles: null when the runner is inactive, the
  /// program fails or does not compute the property.
  const float* RunProperty(const plParticleSystemInstance* pSystem, plUInt64 uiCount, const plTime& diff, plStringView sProperty, plUInt32& out_uiComponents);

private:
  /// One stream of the particle system the program reads, possibly through several split inputs.
  struct Stream
  {
    plString m_sName;
    plProcessingStream::DataType m_DataType = plProcessingStream::DataType::Float;
    plProcessingStream* m_pStream = nullptr; ///< kept up to date by the stream binding
  };

  /// One input of the byte code, in its order. The compiler gives only scalar inputs.
  struct Input
  {
    plHashedString m_sName;    ///< as the byte code names it, "Position.x" for a split input
    plString m_sSource;        ///< the stream or frame input it reads, "Position"
    plProcessingStream::DataType m_DataType = plProcessingStream::DataType::Float;
    bool m_bFrameInput = false;
    plUInt32 m_uiStream = 0;   ///< particle inputs: index into m_Streams
    plInt32 m_iComponent = -1; ///< the component of a split input
  };

  struct Output
  {
    plString m_sProperty;
    plUInt32 m_uiComponents = 1;
    plUInt32 m_uiOffset = 0; ///< first float in m_OutputData
  };

  /// Where one scalar output of the byte code lands, in its order.
  struct OutputTarget
  {
    plUInt32 m_uiOutput = plInvalidIndex; ///< index into m_Outputs; invalid writes to scratch
    plUInt32 m_uiComponent = 0;
  };

  const plParticleProgramData* m_pData = nullptr;
  plExpressionVM m_VM;
  plParticleProgramTables m_Tables;

  plDynamicArray<Stream> m_Streams;
  plDynamicArray<Input> m_Inputs;
  plHybridArray<Output, 2> m_Outputs;
  plHybridArray<OutputTarget, 8> m_OutputTargets;
  plDynamicArray<float> m_FrameData;
  plDynamicArray<float> m_OutputData;
  plDynamicArray<plProcessingStream> m_InputViews;
  plDynamicArray<plProcessingStream> m_OutputViews;
};

/// \brief The compiler splits vector inputs into one input per component ("Position.x"). Returns the
/// name of the whole input and the component (0-3), or -1 for an input that was not split.
PL_PARTICLEPLUGIN_DLL plStringView plParticleSplitInputComponent(plStringView sName, plInt32& out_iComponent);

/// \brief Whether a program input name is a per-frame input (plParticleFrameInput) rather than a particle stream.
PL_PARTICLEPLUGIN_DLL bool plParticleIsFrameInput(plStringView sName);

/// \brief Reads one per-frame input by name (plParticleFrameInput) for a system; false for unknown names.
PL_PARTICLEPLUGIN_DLL bool plParticleReadFrameInput(plStringView sName, const plParticleSystemInstance* pSystem, const plTime& diff, plVec4& out_vValue);
