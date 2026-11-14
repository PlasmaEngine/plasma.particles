#pragma once

#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/RendererFoundationDLL.h>

class plRenderGraph;

/// \brief The water depth of every column of a flood basin at its current level, from the CPU's solid profile
/// (plGpuFluidBasinColumns). A depth of 0 or less: the water does not reach the column (a wall, furniture).
class PL_PARTICLEPLUGIN_DLL plGpuFluidBasinDepths : public plRefCounted
{
public:
  plDynamicArray<float> m_Depths;
};

/// What a flood basin's wave surface does in one frame, gathered on the update thread.
struct plGpuFluidBasinFrameInput
{
  /// A new value steps the waves; the same value again (a frame rendered without an update) steps nothing.
  plUInt64 m_uiTick = 0;
  plUInt32 m_uiSteps = 1;          ///< wave steps this frame
  float m_fStepDt = 1.0f / 60.0f;  ///< seconds per step
  float m_fDamping = 0.02f;        ///< share of the wave motion lost per step
  float m_fLevel = 0.0f;           ///< water level above the basin's floor (m)
  float m_fGravity = 9.81f;
  float m_fMaxWaveHeight = 0.15f;  ///< m, heights are clamped to it
  float m_fMaxSpeedDepth = 1.0f;   ///< m, the deepest water the wave speed uses (deeper water would need more steps)

  plSharedPtr<const plGpuFluidBasinDepths> m_pDepths;

  /// Bumps added before the steps: xy centre (m, from the basin's corner), z radius (m), w height (m, negative pushes down).
  plHybridArray<plVec4, 16> m_Impulses;
};

/// The surface mesh in the liquid surface pass's layout (plGpuFluidLiquidSurfaceRenderData).
struct plGpuFluidBasinPublish
{
  plGALBufferHandle m_hVertices;
  plGALBufferHandle m_hQuads;
  plGALBufferHandle m_hDrawArgs;
  plUInt32 m_uiQuads = 0;
};

/// \brief GPU side of a flood basin: the wave heights of its columns, stepped every frame, and the surface mesh drawn
/// at the basin's level by plGpuFluidLiquidPass. Render thread only.
class PL_PARTICLEPLUGIN_DLL plGpuFluidBasinSolver
{
public:
  plGpuFluidBasinSolver();
  ~plGpuFluidBasinSolver();

  bool Create(const plVec2U32& vColumns, float fColumnSize);
  void Destroy();
  bool IsCreated() const { return m_bCreated; }

  /// Adds the frame's passes to the graph. False when nothing was recorded.
  bool Record(plRenderGraph& ref_graph, const plGpuFluidBasinFrameInput& input);

  /// True once a frame was recorded: the mesh then holds a surface.
  bool HasRecorded() const { return m_bRecorded; }
  void GetPublish(plGpuFluidBasinPublish& out_publish) const;

private:
  plGALBufferHandle CreateBuffer(plUInt32 uiBytes, const char* szName, plArrayPtr<const plUInt8> initialData, bool bWritable, bool bIndirect);

  bool m_bCreated = false;
  bool m_bRecorded = false;
  plVec2U32 m_vColumns = plVec2U32::MakeZero();
  float m_fColumnSize = 0.05f;
  plUInt64 m_uiLastTick = 0;
  bool m_bHasTick = false;

  plGALBufferHandle m_hHeight[2]; ///< the current heights and the ones before, swapped every step
  plUInt32 m_uiCurrent = 0;
  plGALBufferHandle m_hDepth;
  plSharedPtr<const plGpuFluidBasinDepths> m_pUploadedDepths;
  plGALBufferHandle m_hVertices;
  plGALBufferHandle m_hQuads;
  plGALBufferHandle m_hDrawArgs;

  plShaderResourceHandle m_hWaveShader;
  plShaderResourceHandle m_hImpulseShader;
  plShaderResourceHandle m_hMeshShader;
};
