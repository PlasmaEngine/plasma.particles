#pragma once

#include <Core/World/WorldModule.h>
#include <EnginePluginParticle/EnginePluginParticleDLL.h>
#include <Foundation/Math/Float16.h>
#include <ParticlePlugin/System/ParticleProgram.h>

class plParticleComponent;
class plParticleSystemInstance;
class plParticleSpyMsgToEngine;
class plParticleTypeGPU;

/// The particle editor's value spy, in the preview world.
///
/// After the simulation of each step it runs the spied node's program over the particles of its
/// system, keeps min, average, max and a histogram, and tints the particles by the value. The colours
/// go back before the next step simulates, so the effect itself never sees the tint; only what is
/// rendered in between does. A system on the GPU computes the values in its kernel and tints what it
/// draws; they are read back about four times a second.
class PL_ENGINEPLUGINPARTICLE_DLL plParticlePreviewModule : public plWorldModule
{
  PL_DECLARE_WORLD_MODULE();
  PL_ADD_DYNAMIC_REFLECTION(plParticlePreviewModule, plWorldModule);

public:
  plParticlePreviewModule(plWorld* pWorld);
  ~plParticlePreviewModule();

  virtual void Initialize() override;

  void SetPreviewComponent(plParticleComponent* pComponent) { m_pComponent = pComponent; }

  /// Starts spying on the node of the message, or stops when its program is empty.
  void SetSpy(const plParticleSpyMsgToEngine& msg);

  /// Puts back the colours the tint replaced. Called before the effect is simulated outside the
  /// world update, as the timeline and the thumbnail do.
  void RestoreColors();

  struct SpyResult
  {
    plUuid m_Node;
    plUInt32 m_uiCount = 0;
    float m_fMin = 0.0f;
    float m_fAvg = 0.0f;
    float m_fMax = 0.0f;
    plHybridArray<plUInt32, 16> m_Bins;
    bool m_bOnGPU = false; ///< read back from the GPU, a few frames late
  };

  const SpyResult& GetSpyResult() const { return m_Result; }

  static constexpr plUInt32 s_uiSpyBins = 14;

private:
  void BeforeSimulation(const plWorldModule::UpdateContext& context);
  void AfterSimulation(const plWorldModule::UpdateContext& context);

  plParticleSystemInstance* GetSpiedSystem() const;
  plParticleTypeGPU* GetSpiedGPUType() const;
  void Tint(plParticleSystemInstance* pSystem, const float* pValues, plUInt32 uiCount);

  /// The spy on a system that runs on the GPU: its kernel computes the values, read back four times a second.
  void SpyOnGPU();

  /// Count, min, average, max and the histogram of the values.
  void ComputeResult(const float* pValues, plUInt32 uiCount, plUInt32 uiStride);

  plParticleComponent* m_pComponent = nullptr;

  plUuid m_SpyNode;
  plUInt32 m_uiSpySystem = 0;
  bool m_bSpyPerFrame = false;
  plParticleProgramData m_SpyProgram;
  plParticleProgramRunner m_SpyRunner;
  SpyResult m_Result;

  // the GPU system the spy program was given to, and its last readback
  plParticleTypeGPU* m_pSpyType = nullptr;
  plDynamicArray<float> m_GPUValues;

  // the colours the tint replaced, of this system, this many particles
  const plParticleSystemInstance* m_pTintedSystem = nullptr;
  plDynamicArray<plFloat16Vec4> m_SavedColors;
};
