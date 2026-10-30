#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <ParticlePlugin/Resources/ParticleEffectResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEffectResource, 1, plRTTIDefaultAllocator<plParticleEffectResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plParticleEffectResource);
// clang-format on

plParticleEffectResource::plParticleEffectResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
}

plParticleEffectResource::~plParticleEffectResource() = default;

plResourceLoadDesc plParticleEffectResource::UnloadData(Unload WhatToUnload)
{
  /// \todo Clear something
  // m_Desc.m_System1

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plParticleEffectResource::UpdateContent(plStreamReader* Stream)
{
  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;

  if (Stream == nullptr)
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  plStringBuilder sAbsFilePath;
  (*Stream) >> sAbsFilePath;

  plAssetFileHeader AssetHash;
  AssetHash.Read(*Stream).IgnoreResult();

  m_Desc.Load(*Stream);

  return res;
}

void plParticleEffectResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  /// \todo Better statistics
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plParticleEffectResource) + sizeof(plParticleEffectResourceDescriptor);
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plParticleEffectResource, plParticleEffectResourceDescriptor)
{
  // The descriptor owns its systems and modules through raw pointers, so a member-wise copy would
  // share them with the caller's descriptor and free them twice. Copy through the binary format.
  plDefaultMemoryStreamStorage storage;
  {
    plMemoryStreamWriter writer(&storage);
    descriptor.Save(writer);
  }
  {
    plMemoryStreamReader reader(&storage);
    m_Desc.Load(reader);
  }

  plResourceLoadDesc res;
  res.m_State = plResourceState::Loaded;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;

  return res;
}

void plParticleEffectResourceDescriptor::Save(plStreamWriter& inout_stream) const
{
  m_Effect.Save(inout_stream);
}

void plParticleEffectResourceDescriptor::Load(plStreamReader& inout_stream)
{
  m_Effect.Load(inout_stream);
}



PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Resources_ParticleEffectResource);
