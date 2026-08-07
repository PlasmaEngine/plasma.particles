#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <Foundation/SimdMath/SimdNoise.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <ParticlePlugin/Resources/VectorFieldResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVectorFieldResource, 1, plRTTIDefaultAllocator<plVectorFieldResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plVectorFieldResource);
// clang-format on

plVectorFieldResource::plVectorFieldResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
}

plVectorFieldResource::~plVectorFieldResource() = default;

plResourceLoadDesc plVectorFieldResource::UnloadData(Unload WhatToUnload)
{
  m_Desc.m_Vectors.Clear();
  m_Desc.m_vDimensions = plVec3U32::MakeZero();

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plVectorFieldResource::UpdateContent(plStreamReader* Stream)
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

  // the only supported file format is FGA (Fluid Grid ASCII); read the rest of the file as text
  plDefaultMemoryStreamStorage storage;
  storage.ReadAll(*Stream);

  plMemoryStreamReader reader(&storage);

  plDynamicArray<char> content;
  content.SetCountUninitialized(storage.GetStorageSize32());
  reader.ReadBytes(content.GetData(), content.GetCount());

  const plStringView sContent(content.GetData(), content.GetData() + content.GetCount());

  if (m_Desc.ParseFga(sContent).Failed())
  {
    plLog::Error("Vector field '{0}' could not be parsed as FGA.", sAbsFilePath);
    res.m_State = plResourceState::LoadedResourceMissing;
  }

  return res;
}

void plVectorFieldResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plVectorFieldResource) + m_Desc.m_Vectors.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plVectorFieldResource, plVectorFieldResourceDescriptor)
{
  m_Desc = descriptor;

  plResourceLoadDesc res;
  res.m_State = plResourceState::Loaded;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;

  return res;
}

//////////////////////////////////////////////////////////////////////////

void plVectorFieldResourceDescriptor::InitializeCurlNoise(plUInt32 uiResolution, plUInt32 uiSeed, float fFrequency, plUInt32 uiOctaves)
{
  const plUInt32 res = plMath::Clamp<plUInt32>(uiResolution, 4, 128);

  m_vDimensions = plVec3U32(res, res, res);
  m_Vectors.SetCountUninitialized(res * res * res);

  // three scalar potential fields on a grid with a one-cell border for the finite differences
  const plUInt32 potRes = res + 2;

  plDynamicArray<float> potentials[3];
  {
    for (plUInt32 c = 0; c < 3; ++c)
    {
      potentials[c].SetCountUninitialized(potRes * potRes * potRes);

      plSimdPerlinNoise noise(uiSeed + c * 0x9E3779B9u);

      const float fToNoise = fFrequency / (float)res;

      for (plUInt32 z = 0; z < potRes; ++z)
      {
        const plSimdVec4f nz = plSimdVec4f(((float)z - 1.0f) * fToNoise);

        for (plUInt32 y = 0; y < potRes; ++y)
        {
          const plSimdVec4f ny = plSimdVec4f(((float)y - 1.0f) * fToNoise);

          for (plUInt32 x = 0; x < potRes; x += 4)
          {
            const plSimdVec4f nx = plSimdVec4f(((float)x - 1.0f) * fToNoise, ((float)x + 0.0f) * fToNoise, ((float)x + 1.0f) * fToNoise, ((float)x + 2.0f) * fToNoise);

            const plSimdVec4f val = noise.NoiseZeroToOne(nx, ny, nz, uiOctaves);

            float vals[4];
            val.Store<4>(vals);

            const plUInt32 uiBase = z * potRes * potRes + y * potRes + x;
            for (plUInt32 i = 0; i < 4 && x + i < potRes; ++i)
            {
              potentials[c][uiBase + i] = vals[i] * 2.0f - 1.0f;
            }
          }
        }
      }
    }
  }

  // curl of the potential: F = (dPz/dy - dPy/dz, dPx/dz - dPz/dx, dPy/dx - dPx/dy)
  // central differences; the potential grid's border supplies the neighbors
  auto pot = [&](plUInt32 c, plUInt32 x, plUInt32 y, plUInt32 z) -> float
  {
    return potentials[c][(z + 1) * potRes * potRes + (y + 1) * potRes + (x + 1)];
  };

  float fMaxLength = 0.0f;

  for (plUInt32 z = 0; z < res; ++z)
  {
    for (plUInt32 y = 0; y < res; ++y)
    {
      for (plUInt32 x = 0; x < res; ++x)
      {
        const float dPz_dy = pot(2, x, y + 1, z) - pot(2, x, y - 1, z);
        const float dPy_dz = pot(1, x, y, z + 1) - pot(1, x, y, z - 1);
        const float dPx_dz = pot(0, x, y, z + 1) - pot(0, x, y, z - 1);
        const float dPz_dx = pot(2, x + 1, y, z) - pot(2, x - 1, y, z);
        const float dPy_dx = pot(1, x + 1, y, z) - pot(1, x - 1, y, z);
        const float dPx_dy = pot(0, x, y + 1, z) - pot(0, x, y - 1, z);

        const plVec3 v(dPz_dy - dPy_dz, dPx_dz - dPz_dx, dPy_dx - dPx_dy);

        m_Vectors[z * res * res + y * res + x] = v;
        fMaxLength = plMath::Max(fMaxLength, v.GetLength());
      }
    }
  }

  // normalize so the strongest swirl has length 1; the behavior's Strength scales it
  if (fMaxLength > 0.0001f)
  {
    const float fInv = 1.0f / fMaxLength;
    for (plVec3& v : m_Vectors)
    {
      v *= fInv;
    }
  }
}

plResult plVectorFieldResourceDescriptor::ParseFga(plStringView sText)
{
  m_vDimensions = plVec3U32::MakeZero();
  m_Vectors.Clear();

  plHybridArray<float, 16> header;
  plDynamicArray<float> values;

  // comma / whitespace separated floats
  {
    const char* szCur = sText.GetStartPointer();
    const char* szEnd = sText.GetEndPointer();

    plStringBuilder sToken;

    auto flushToken = [&]() -> plResult
    {
      if (sToken.IsEmpty())
        return PL_SUCCESS;

      double fValue = 0.0;
      if (plConversionUtils::StringToFloat(sToken, fValue).Failed())
        return PL_FAILURE;

      if (header.GetCount() < 9)
        header.PushBack((float)fValue);
      else
        values.PushBack((float)fValue);

      sToken.Clear();
      return PL_SUCCESS;
    };

    for (; szCur < szEnd; ++szCur)
    {
      const char c = *szCur;

      if (c == ',' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
      {
        if (flushToken().Failed())
          return PL_FAILURE;
      }
      else
      {
        sToken.Append(plStringView(szCur, szCur + 1));
      }
    }

    if (flushToken().Failed())
      return PL_FAILURE;
  }

  if (header.GetCount() < 9)
    return PL_FAILURE;

  const plUInt32 uiDimX = (plUInt32)header[0];
  const plUInt32 uiDimY = (plUInt32)header[1];
  const plUInt32 uiDimZ = (plUInt32)header[2];
  // header[3..8] are the authored world bounds - ignored, the behavior maps the field into the world

  const plUInt32 uiNumVectors = uiDimX * uiDimY * uiDimZ;

  if (uiDimX < 2 || uiDimY < 2 || uiDimZ < 2 || values.GetCount() < uiNumVectors * 3)
    return PL_FAILURE;

  m_vDimensions = plVec3U32(uiDimX, uiDimY, uiDimZ);
  m_Vectors.SetCountUninitialized(uiNumVectors);

  for (plUInt32 i = 0; i < uiNumVectors; ++i)
  {
    m_Vectors[i] = plVec3(values[i * 3 + 0], values[i * 3 + 1], values[i * 3 + 2]);
  }

  return PL_SUCCESS;
}

plVec3 plVectorFieldResourceDescriptor::SampleTrilinear(const plVec3& vNormPos) const
{
  const plUInt32 uiMaxX = m_vDimensions.x - 1;
  const plUInt32 uiMaxY = m_vDimensions.y - 1;
  const plUInt32 uiMaxZ = m_vDimensions.z - 1;

  const float fX = plMath::Clamp(vNormPos.x, 0.0f, 1.0f) * uiMaxX;
  const float fY = plMath::Clamp(vNormPos.y, 0.0f, 1.0f) * uiMaxY;
  const float fZ = plMath::Clamp(vNormPos.z, 0.0f, 1.0f) * uiMaxZ;

  const plUInt32 x0 = plMath::Min((plUInt32)fX, uiMaxX);
  const plUInt32 y0 = plMath::Min((plUInt32)fY, uiMaxY);
  const plUInt32 z0 = plMath::Min((plUInt32)fZ, uiMaxZ);
  const plUInt32 x1 = plMath::Min(x0 + 1, uiMaxX);
  const plUInt32 y1 = plMath::Min(y0 + 1, uiMaxY);
  const plUInt32 z1 = plMath::Min(z0 + 1, uiMaxZ);

  const float wx = fX - x0;
  const float wy = fY - y0;
  const float wz = fZ - z0;

  const plUInt32 uiRowPitch = m_vDimensions.x;
  const plUInt32 uiSlicePitch = m_vDimensions.x * m_vDimensions.y;

  auto at = [&](plUInt32 x, plUInt32 y, plUInt32 z) -> const plVec3&
  {
    return m_Vectors[z * uiSlicePitch + y * uiRowPitch + x];
  };

  const plVec3 c00 = plMath::Lerp(at(x0, y0, z0), at(x1, y0, z0), wx);
  const plVec3 c10 = plMath::Lerp(at(x0, y1, z0), at(x1, y1, z0), wx);
  const plVec3 c01 = plMath::Lerp(at(x0, y0, z1), at(x1, y0, z1), wx);
  const plVec3 c11 = plMath::Lerp(at(x0, y1, z1), at(x1, y1, z1), wx);

  const plVec3 c0 = plMath::Lerp(c00, c10, wy);
  const plVec3 c1 = plMath::Lerp(c01, c11, wy);

  return plMath::Lerp(c0, c1, wz);
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Resources_VectorFieldResource);
