#pragma once

#include <Core/Curves/ColorGradientResource.h>
#include <Core/Curves/Curve1DResource.h>
#include <Foundation/CodeUtils/Expression/ExpressionAST.h>
#include <Foundation/CodeUtils/Expression/ExpressionByteCode.h>
#include <Foundation/CodeUtils/Expression/ExpressionVM.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Vec4.h>
#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

class plParticleSystemInstance;
class plParticleProgramTables;
class plCurve1D;
class plColorGradient;

/// \brief The gradient and curve tables a program samples, by slot.
///
/// The byte code reads gradient slot i through PfxGradient(t, i, component) and curve slot i through
/// PfxCurve(t, i); t runs from 0 to 1. The asset slots come first, in list order, with t across the asset's
/// own range. The inline tables the editor baked follow them, s_uiTableSize entries per slot.
struct PL_PARTICLEPLUGIN_DLL plParticleProgramAssets
{
  bool IsEmpty() const { return m_Gradients.IsEmpty() && m_Curves.IsEmpty() && m_InlineGradients.IsEmpty() && m_InlineCurves.IsEmpty(); }

  plUInt32 GetNumGradientSlots() const;
  plUInt32 GetNumCurveSlots() const;

  /// Whether curve asset slot i reads its values mapped to 0..1, the way Size Curve does (a flat curve reads 1).
  bool IsCurveNormalized(plUInt32 uiSlot) const { return uiSlot < m_CurveNormalized.GetCount() && m_CurveNormalized[uiSlot] != 0; }

  void Save(plStreamWriter& inout_stream) const;

  /// bInlineTables: the data has the inline tables and the curve normalization flags (program format 3 and up).
  void Load(plStreamReader& inout_stream, bool bInlineTables);

  /// The baked tables, shared by every runner of this program. The first call bakes them, which loads the
  /// assets; Load starts loading them, so this rarely waits. A later call bakes again once an asset was reloaded.
  plSharedPtr<plParticleProgramTables> GetTables() const;

  plHybridArray<plString, 2> m_Gradients;
  plHybridArray<plString, 2> m_Curves;
  plHybridArray<plUInt8, 2> m_CurveNormalized; ///< per curve asset slot: 1 to read it mapped to 0..1
  plDynamicArray<plVec4> m_InlineGradients;    ///< s_uiTableSize entries per slot, after the gradient asset slots
  plDynamicArray<float> m_InlineCurves;        ///< s_uiTableSize entries per slot, after the curve asset slots

private:
  void PreloadAssets();

  mutable plSharedPtr<plParticleProgramTables> m_pTables;
  plHybridArray<plColorGradientResourceHandle, 2> m_hPreloadedGradients;
  plHybridArray<plCurve1DResourceHandle, 2> m_hPreloadedCurves;
};

/// \brief A program's gradients and curves baked into tables, for the byte code functions to read.
class PL_PARTICLEPLUGIN_DLL plParticleProgramTables : public plRefCounted
{
public:
  /// PfxGradient(float t, int slot, int component): one component (r, g, b, a) of a gradient, HDR.
  static plExpressionFunction s_GradientFunc;

  /// PfxCurve(float t, int slot): a curve's value.
  static plExpressionFunction s_CurveFunc;

  static constexpr plUInt32 s_uiTableSize = 64;

  /// Loads the assets, blocking until they are there, and samples each into a table, then appends the
  /// inline tables. A missing gradient reads as white and a missing curve as zero.
  void Bake(const plParticleProgramAssets& assets);

  /// True when an asset these tables were baked from has been reloaded since.
  bool IsOutdated() const;

  /// The global data to execute the byte code with; it points the functions at these tables.
  const plExpression::GlobalData& GetGlobalData() const { return m_GlobalData; }

  float SampleGradient(plUInt32 uiSlot, plUInt32 uiComponent, float t) const;
  float SampleCurve(plUInt32 uiSlot, float t) const;

  /// The baked tables, s_uiTableSize entries per slot, for the GPU programs.
  plArrayPtr<const plVec4> GetGradientTables() const { return m_Gradients; }
  plArrayPtr<const float> GetCurveTables() const { return m_Curves; }

  /// A curve's value at t (0 to 1 across its own range). bNormalized maps the values to 0..1 between the
  /// curve's lowest and highest value, and a flat curve reads 1.
  static float SampleCurveAsset(const plCurve1D& curve, float t, bool bNormalized);

  /// Fills out_table (s_uiTableSize values) with SampleCurveAsset across t = 0 to 1.
  static void BakeCurve(const plCurve1D& curve, bool bNormalized, plArrayPtr<float> out_table);

  /// Fills out_table (s_uiTableSize colours, HDR) across the gradient's own range.
  static void BakeGradient(const plColorGradient& gradient, plArrayPtr<plVec4> out_table);

private:
  plDynamicArray<plVec4> m_Gradients; ///< s_uiTableSize entries per slot
  plDynamicArray<float> m_Curves;
  plExpression::GlobalData m_GlobalData;

  /// The asset slots' resources and the change counters they were baked at.
  plHybridArray<plColorGradientResourceHandle, 2> m_hGradients;
  plHybridArray<plCurve1DResourceHandle, 2> m_hCurves;
  plHybridArray<plUInt32, 4> m_ChangeCounters; ///< gradients first, then curves
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
  plSharedPtr<plParticleProgramTables> m_pTables; ///< shared with every runner of the same program

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

/// \brief The expressions of the graph's operator nodes, built the same way for the editor's compiler and for tests.
///
/// Every one lowers to instructions and functions both the CPU VM and the GPU simulate kernel have. Angles are radians
/// here; the graph converts its degrees around them. Numbers broadcast against vectors where an operation mixes them.
namespace plParticleProgramOps
{
  using Node = plExpressionAST::Node;

  PL_PARTICLEPLUGIN_DLL Node* Number(plExpressionAST& ref_ast, float fValue);

  /// The value repeated into uiComponents components; a single component returns it as it is.
  PL_PARTICLEPLUGIN_DLL Node* Broadcast(plExpressionAST& ref_ast, Node* pValue, plUInt32 uiComponents);

  /// a / b, and 0 where b is 0.
  PL_PARTICLEPLUGIN_DLL Node* SafeDivide(plExpressionAST& ref_ast, Node* a, Node* b);

  /// 1 where x is at or above the edge, else 0.
  PL_PARTICLEPLUGIN_DLL Node* Step(plExpressionAST& ref_ast, Node* pEdge, Node* pX);

  /// The smooth 0-to-1 ramp of x between two edges: t*t*(3 - 2t), or t*t*t*(t*(6t - 15) + 10) with bSmoother. Equal
  /// edges step.
  PL_PARTICLEPLUGIN_DLL Node* SmoothStep(plExpressionAST& ref_ast, Node* pEdge0, Node* pEdge1, Node* pX, bool bSmoother);

  /// (Value - A) / (B - A), 0 where A equals B, clamped to 0..1 with bClamp.
  PL_PARTICLEPLUGIN_DLL Node* InverseLerp(plExpressionAST& ref_ast, Node* a, Node* b, Node* pValue, bool bClamp);

  /// Value moved from the In range to the Out range, clamped to the Out range with bClamp.
  PL_PARTICLEPLUGIN_DLL Node* Remap(plExpressionAST& ref_ast, Node* pValue, Node* pInMin, Node* pInMax, Node* pOutMin, Node* pOutMax, bool bClamp);

  /// -1, 0 or 1.
  PL_PARTICLEPLUGIN_DLL Node* Sign(plExpressionAST& ref_ast, Node* pX);

  /// The angle of (x, y) from the x axis, -pi..pi, and 0 at the origin.
  PL_PARTICLEPLUGIN_DLL Node* ATan2(plExpressionAST& ref_ast, Node* pY, Node* pX);

  PL_PARTICLEPLUGIN_DLL Node* Distance(plExpressionAST& ref_ast, Node* a, Node* b);

  /// The part of a vector along another one; zero when that one is zero.
  PL_PARTICLEPLUGIN_DLL Node* ProjectOnto(plExpressionAST& ref_ast, Node* pVector, Node* pOnto);

  /// A vector rotated about an axis by an angle, right-handed; the vector itself when the axis is zero.
  PL_PARTICLEPLUGIN_DLL Node* RotateAboutAxis(plExpressionAST& ref_ast, Node* pVector, Node* pAxis, Node* pRadians);

  /// Random(RandomSeed, iSeed): 0..1, the same for a particle all its life.
  PL_PARTICLEPLUGIN_DLL Node* ParticleRandom(plExpressionAST& ref_ast, Node* pRandomSeed, plInt32 iSeed);

  /// The seed of one component of a random vector or colour.
  PL_PARTICLEPLUGIN_DLL plInt32 GetComponentSeed(plInt32 iSeed, plUInt32 uiComponent);

  /// Min + (Max - Min) x a random value, with its own random value per component (uiComponents 1, 3 or 4).
  PL_PARTICLEPLUGIN_DLL Node* RandomRange(plExpressionAST& ref_ast, Node* pRandomSeed, plInt32 iSeed, Node* pMin, Node* pMax, plUInt32 uiComponents);

  /// A whole number from Min to Max, both included; Min and Max are rounded first.
  PL_PARTICLEPLUGIN_DLL Node* RandomInteger(plExpressionAST& ref_ast, Node* pRandomSeed, plInt32 iSeed, Node* pMin, Node* pMax);

  enum class RandomVectorShape : plUInt8
  {
    InSphere, ///< anywhere in a ball of the radius
    OnSphere, ///< on its surface
    InBox,    ///< in a cube from -radius to +radius
    InCone,   ///< a direction of the radius' length, at most the cone angle off +Z
  };

  PL_PARTICLEPLUGIN_DLL Node* RandomVector(plExpressionAST& ref_ast, Node* pRandomSeed, plInt32 iSeed, RandomVectorShape shape, Node* pRadius, float fConeRadians);

  /// Perlin noise at Position x Frequency, 0..1, summed over Octaves (rounded and kept in 1..8).
  PL_PARTICLEPLUGIN_DLL Node* Noise(plExpressionAST& ref_ast, Node* pPosition, Node* pFrequency, Node* pOctaves);

  /// Three decorrelated noise channels at Position x Frequency, each -1..1.
  PL_PARTICLEPLUGIN_DLL Node* VectorNoise(plExpressionAST& ref_ast, Node* pPosition, Node* pFrequency, plInt32 iOctaves);

  /// The effect's frame: its origin, axes and scale in simulation space.
  struct Frame
  {
    Node* m_pPosition = nullptr;
    Node* m_pForward = nullptr;
    Node* m_pRight = nullptr;
    Node* m_pUp = nullptr;
    Node* m_pScale = nullptr;
  };

  /// A position from the effect's local frame into simulation space (bToWorld) or back; scale included.
  PL_PARTICLEPLUGIN_DLL Node* TransformPosition(plExpressionAST& ref_ast, const Frame& frame, Node* pVector, bool bToWorld);

  /// A direction from the effect's local frame into simulation space (bToWorld) or back; rotation only.
  PL_PARTICLEPLUGIN_DLL Node* TransformDirection(plExpressionAST& ref_ast, const Frame& frame, Node* pVector, bool bToWorld);

  /// PfxCurve(t, slot).
  PL_PARTICLEPLUGIN_DLL Node* SampleCurve(plExpressionAST& ref_ast, Node* pT, Node* pSlot);

  /// The four PfxGradient(t, slot, component) as a colour.
  PL_PARTICLEPLUGIN_DLL Node* SampleGradient(plExpressionAST& ref_ast, Node* pT, Node* pSlot);
} // namespace plParticleProgramOps

/// \brief Reads one per-frame input by name (plParticleFrameInput) for a system; false for unknown names.
PL_PARTICLEPLUGIN_DLL bool plParticleReadFrameInput(plStringView sName, const plParticleSystemInstance* pSystem, const plTime& diff, plVec4& out_vValue);
