#include <ParticlePlugin/ParticlePluginPCH.h>

#include <ParticlePlugin/Type/GPU/GPUParticleRenderer.h>
#include <RendererCore/Pipeline/RenderDataBatch.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererFoundation/Device/Device.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGPUParticleRenderData, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGPUParticleRenderer, 1, plRTTIDefaultAllocator<plGPUParticleRenderer>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

bool plGPUParticleRenderData::CanBatch(const plRenderData& other0) const
{
  const auto& other = plStaticCast<const plGPUParticleRenderData&>(other0);
  return m_RenderMode == other.m_RenderMode && m_hTexture == other.m_hTexture && m_uiGPURenderType == other.m_uiGPURenderType;
}

plGPUParticleRenderer::plGPUParticleRenderer()
{
  m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Particles/GPUParticleRender.plShader");

  m_hGPUConstantBuffer = plRenderContext::CreateConstantBufferStorage<plGPUParticleConstants>();
}

plGPUParticleRenderer::~plGPUParticleRenderer()
{
  plRenderContext::DeleteConstantBufferStorage(m_hGPUConstantBuffer);
}

void plGPUParticleRenderer::GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& out_types) const
{
  out_types.PushBack(plGetStaticRTTI<plGPUParticleRenderData>());
}

void plGPUParticleRenderer::RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const
{
  plRenderContext* pRenderContext = renderViewContext.m_pRenderContext;
  plGALCommandEncoder* pGALCommandEncoder = pRenderContext->GetCommandEncoder();

  TempSystemCB systemConstants(pRenderContext);

  pRenderContext->BindShader(m_hShader);

  for (auto it = batch.GetIterator<plGPUParticleRenderData>(0, batch.GetDataCount()); it.IsValid(); ++it)
  {
    const plGPUParticleRenderData* pRenderData = it;

    if (!pRenderData->m_hParticleBuffer.IsInvalidated() && pRenderData->m_uiActiveSlotCount > 0)
    {
      systemConstants.SetGenericData(pRenderData->m_GlobalTransform, pRenderData->m_TotalEffectLifeTime, 1, 1, 1, 1, 0, 0);

      ConfigureRenderMode(pRenderData, pRenderContext);

      // Set GPU_PARTICLE_TYPE permutation
      switch (pRenderData->m_uiGPURenderType)
      {
        case plGPUParticleRenderType::Point:
          pRenderContext->SetShaderPermutationVariable("GPU_PARTICLE_TYPE", "GPU_PARTICLE_TYPE_POINT");
          break;
        case plGPUParticleRenderType::VelocityAligned:
          pRenderContext->SetShaderPermutationVariable("GPU_PARTICLE_TYPE", "GPU_PARTICLE_TYPE_VELOCITY_ALIGNED");
          break;
        case plGPUParticleRenderType::Trail:
          pRenderContext->SetShaderPermutationVariable("GPU_PARTICLE_TYPE", "GPU_PARTICLE_TYPE_TRAIL");
          break;
        default: // Billboard
          pRenderContext->SetShaderPermutationVariable("GPU_PARTICLE_TYPE", "GPU_PARTICLE_TYPE_BILLBOARD");
          break;
      }

      const plUInt32 uiSlotCount = plMath::Max(pRenderData->m_uiActiveSlotCount, 1u);

      plGALPrimitiveTopology::Enum topology = plGALPrimitiveTopology::Triangles;
      plUInt32 uiPrimitiveCount = uiSlotCount * 2;

      switch (pRenderData->m_uiGPURenderType)
      {
        case plGPUParticleRenderType::Point:
          topology = plGALPrimitiveTopology::Points;
          uiPrimitiveCount = uiSlotCount;
          break;
        case plGPUParticleRenderType::Trail:
          uiPrimitiveCount = uiSlotCount * (pRenderData->m_uiMaxTrailPoints - 1) * 2;
          break;
        default:
          break;
      }

      pRenderContext->BindNullMeshBuffer(topology, uiPrimitiveCount);

      // the render VS reads these from plGPUParticleConstants; bind our own storage with THIS
      // system's values instead of inheriting whatever the compute pass last uploaded
      {
        auto* cb = plRenderContext::GetConstantBufferData<plGPUParticleConstants>(m_hGPUConstantBuffer);
        cb->GPUPartMaxParticles = pRenderData->m_uiMaxParticles;
        cb->GPUPartMaxTrailPoints = pRenderData->m_uiMaxTrailPoints;
        cb->GPUPartTrailWriteIndex = pRenderData->m_uiTrailWriteIndex;
        cb->GPUPartVelocityStretch = pRenderData->m_fVelocityStretch;
      }

      plBindGroupBuilder& bindGroup = pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
      bindGroup.BindBuffer("plGPUParticleConstants", m_hGPUConstantBuffer);
      bindGroup.BindBuffer("gpuParticlesRead", pRenderData->m_hParticleBuffer);
      bindGroup.BindBuffer("gpuAliveListRead", pRenderData->m_hAliveListBuffer);
      bindGroup.BindTexture("ParticleTexture", pRenderData->m_hTexture);

      if (pRenderData->m_uiGPURenderType == plGPUParticleRenderType::Trail && !pRenderData->m_hTrailPositionBuffer.IsInvalidated())
      {
        bindGroup.BindBuffer("gpuTrailPositionsRead", pRenderData->m_hTrailPositionBuffer);
      }

      // indirect draw: the simulate dispatch wrote vertexCount = alive * vertsPerParticle, so
      // exactly the alive set is drawn; falls back to the watermark draw without args
      if (!pRenderData->m_hDrawArgsBuffer.IsInvalidated())
      {
        pRenderContext->DrawMeshBufferIndirect(pRenderData->m_hDrawArgsBuffer, 0).IgnoreResult();
      }
      else
      {
        pRenderContext->DrawMeshBuffer(uiPrimitiveCount).IgnoreResult();
      }
    }
  }
}

void plGPUParticleRenderer::ConfigureRenderMode(const plGPUParticleRenderData* pRenderData, plRenderContext* pRenderContext) const
{
  if (pRenderData->m_RenderMode == plParticleTypeRenderMode::Additive)
  {
    pRenderContext->SetShaderPermutationVariable("PARTICLE_RENDER_MODE", "PARTICLE_RENDER_MODE_ADDITIVE");
  }
  else if (pRenderData->m_RenderMode == plParticleTypeRenderMode::Opaque)
  {
    pRenderContext->SetShaderPermutationVariable("PARTICLE_RENDER_MODE", "PARTICLE_RENDER_MODE_OPAQUE");
  }
  else
  {
    pRenderContext->SetShaderPermutationVariable("PARTICLE_RENDER_MODE", "PARTICLE_RENDER_MODE_BLENDED");
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_GPU_GPUParticleRenderer);