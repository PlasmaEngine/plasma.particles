#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Vec4.h>
#include <Foundation/Strings/String.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

#include <RendererCore/../../../Data/Base/Shaders/Particles/GPUParticleData.h>
#include <RendererCore/../../../Data/Base/Shaders/Particles/GPUParticleProgram.h>

class plParticleBehaviorFactory;
class plParticleSystemInstance;

/// \brief The graph programs of a lowered system, in the form the simulate kernel runs them
/// (Data/Base/Shaders/Particles/GPUParticleProgram.h, GPUParticleProgramVM.h).
///
/// Built from the Update blocks' per-particle programs: the byte code the CPU VM runs, with maps from
/// its inputs, outputs and functions to what the kernel has. Property programs give the six
/// per-particle block properties their value; Set Attribute and Kill If blocks become records that
/// run after the built-in blocks, so they must come after them in Update.
class PL_PARTICLEPLUGIN_DLL plParticleGPUProgramSet
{
public:
  /// Builds the set from the system's Update blocks. Returns why they cannot run on the GPU, or an empty
  /// string when they can (also when there is nothing to run); out_pBlock gets the index of the block
  /// the reason is about, or plInvalidIndex. pSpy adds the particle editor's spy program (output "Spy")
  /// as a last record, outside the limit on programs; a spy that does not fit the GPU is left out.
  plString Build(plArrayPtr<const plParticleBehaviorFactory* const> behaviors, plUInt32* out_pBlock = nullptr, const plParticleProgramData* pSpy = nullptr);

  /// Whether the set has the spy's record.
  bool HasSpy() const { return m_bHasSpy; }

  /// The reason Build would give, for the lowering check.
  static plString FindBlocker(plArrayPtr<const plParticleBehaviorFactory* const> behaviors, plUInt32* out_pBlock = nullptr);

  bool IsEmpty() const { return m_uiProgramCount == 0; }
  plUInt32 GetProgramCount() const { return m_uiProgramCount; }

  /// Whether a program reads the particles' random seed, which the GPU then keeps per slot.
  bool ReadsRandomSeed() const { return m_bReadsRandomSeed; }

  /// Whether a Color Gradient's tint is computed per particle (its LUT is then baked without the tint).
  bool HasTintProgram() const { return m_bTintProgram; }

  plUInt32 GetNumInstructions() const { return m_uiNumInstructions; }
  plUInt32 GetMaxRegisters() const { return m_uiMaxRegisters; }

  plArrayPtr<const plUInt32> GetWords() const { return m_Words; }

  /// The data buffer of one frame: the built data with the per-frame inputs filled in for the system
  /// (left at zero without one).
  void FillData(const plParticleSystemInstance* pSystem, const plTime& diff, plArrayPtr<float> out_data) const;
  plUInt32 GetDataCount() const { return m_Data.GetCount(); }

  /// \name Reference
  /// The kernel's interpreter in C++, run on one particle. Tests compare it with the CPU VM.
  ///@{

  struct Properties
  {
    plUInt32 m_uiMask = 0; ///< bit per GPU_PROGRAM_PROPERTY_*
    float m_fValues[GPU_PROGRAM_PROPERTY_COUNT] = {};
    plVec4 m_vTint = plVec4(1.0f);
  };

  static Properties RunPropertyPrograms(plArrayPtr<const plUInt32> words, plArrayPtr<const float> data, const plGPUParticle& p, plUInt32 uiSeed);

  /// False when a Kill If removes the particle.
  static bool RunSinkPrograms(plArrayPtr<const plUInt32> words, plArrayPtr<const float> data, plGPUParticle& ref_p, plUInt32 uiSeed, float fDeltaTime);

  ///@}

private:
  struct FrameInput
  {
    plString m_sName;
    plInt32 m_iComponent = -1;
    plUInt32 m_uiDataOffset = 0;
  };

  plString AddProgram(const plParticleBehaviorFactory* pFactory, plUInt32 uiIndex);

  plDynamicArray<plUInt32> m_Words;
  plDynamicArray<float> m_Data;
  plDynamicArray<FrameInput> m_FrameInputs;
  plUInt32 m_uiProgramCount = 0;
  plUInt32 m_uiNumInstructions = 0;
  plUInt32 m_uiMaxRegisters = 0;
  bool m_bReadsRandomSeed = false;
  bool m_bTintProgram = false;
  bool m_bUsesPerlinNoise = false;
  bool m_bHasSpy = false;
};
