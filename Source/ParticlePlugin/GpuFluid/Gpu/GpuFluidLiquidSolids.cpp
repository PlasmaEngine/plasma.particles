#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Time/Stopwatch.h>
#include <Foundation/Utilities/GraphicsUtils.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidLiquidSolids.h>
#include <RendererCore/Meshes/CpuMeshResource.h>
#include <Shaders/GpuFluid/Liquid/GpuFluidLiquidConstants.h>

void plGpuFluidLiquidSolidBake::GatherSourceMeshes(const plWorldGeoExtractionUtil::MeshObjectList& objects, const plVec3& vDomainMin, const plVec3& vDomainMax, float fMargin, plDynamicArray<SourceMesh>& out_meshes)
{
  const plBoundingBox domain = plBoundingBox::MakeFromMinMax(vDomainMin - plVec3(fMargin), vDomainMax + plVec3(fMargin));

  for (const plWorldGeoExtractionUtil::MeshObject& object : objects)
  {
    plResourceLock<plCpuMeshResource> pCpuMesh(object.m_hMeshResource, plResourceAcquireMode::BlockTillLoaded_NeverFail);
    if (pCpuMesh.GetAcquireResult() != plResourceAcquireResult::Final)
      continue;

    const plMeshBufferResourceDescriptor& meshBuffer = pCpuMesh->GetDescriptor().MeshBufferDesc();
    const plArrayPtr<const plVec3> positions = meshBuffer.GetPositionData();
    const plUInt32 uiVertices = meshBuffer.GetVertexCount();
    if (positions.IsEmpty() || uiVertices == 0)
      continue;

    const plMat4 transform = object.m_GlobalTransform.GetAsMat4();

    // Culled as a whole by its world box; triangles are culled one by one in Build().
    plBoundingBox bounds = plBoundingBox::MakeInvalid();
    SourceMesh mesh;
    mesh.m_Positions.SetCountUninitialized(uiVertices);
    for (plUInt32 i = 0; i < uiVertices; ++i)
    {
      mesh.m_Positions[i] = transform.TransformPosition(positions[i]);
      bounds.ExpandToInclude(mesh.m_Positions[i]);
    }
    if (!bounds.Overlaps(domain))
      continue;

    // A mirroring transform turns the winding over; the pseudonormals need it counter-clockwise from outside.
    const bool bFlip = plGraphicsUtils::IsTriangleFlipRequired(transform.GetRotationalPart());
    const plUInt32 uiTriangles = meshBuffer.HasIndexBuffer() ? meshBuffer.GetPrimitiveCount() : uiVertices / 3;
    mesh.m_Indices.Reserve(uiTriangles * 3);
    const plArrayPtr<const plUInt8> indexData = meshBuffer.GetIndexBufferData();
    for (plUInt32 t = 0; t < uiTriangles; ++t)
    {
      plUInt32 tri[3];
      for (plUInt32 k = 0; k < 3; ++k)
      {
        const plUInt32 uiCorner = t * 3 + k;
        if (!meshBuffer.HasIndexBuffer())
          tri[k] = uiCorner;
        else if (meshBuffer.Uses32BitIndices())
          tri[k] = reinterpret_cast<const plUInt32*>(indexData.GetPtr())[uiCorner];
        else
          tri[k] = reinterpret_cast<const plUInt16*>(indexData.GetPtr())[uiCorner];
      }
      if (tri[0] >= uiVertices || tri[1] >= uiVertices || tri[2] >= uiVertices)
        continue;
      mesh.m_Indices.PushBack(tri[bFlip ? 2 : 0]);
      mesh.m_Indices.PushBack(tri[1]);
      mesh.m_Indices.PushBack(tri[bFlip ? 0 : 2]);
    }

    out_meshes.PushBack(std::move(mesh));
  }
}

namespace
{
  struct WeldKey
  {
    PL_DECLARE_POD_TYPE();
    plInt64 x, y, z;
    bool operator==(const WeldKey& o) const { return x == o.x && y == o.y && z == o.z; }
  };

  struct WeldKeyHash
  {
    static plUInt32 Hash(const WeldKey& k)
    {
      plUInt64 h = static_cast<plUInt64>(k.x) * 0x9E3779B97F4A7C15ull;
      h ^= static_cast<plUInt64>(k.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
      h ^= static_cast<plUInt64>(k.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
      return static_cast<plUInt32>(h ^ (h >> 32));
    }
    static bool Equal(const WeldKey& a, const WeldKey& b) { return a == b; }
  };

  plVec3 SafeNormalized(const plVec3& v, const plVec3& vFallback)
  {
    const float fLength = v.GetLength();
    return fLength > 1e-12f ? v / fLength : vFallback;
  }

  float CornerAngle(const plVec3& p, const plVec3& a, const plVec3& b)
  {
    const plVec3 u = SafeNormalized(a - p, plVec3::MakeZero());
    const plVec3 v = SafeNormalized(b - p, plVec3::MakeZero());
    return plMath::ACos(plMath::Clamp(u.Dot(v), -1.0f, 1.0f)).GetRadian();
  }
} // namespace

void plGpuFluidLiquidSolidBake::Build(const plDynamicArray<SourceMesh>& meshes, const plVec3& vDomainOrigin, const plVec3U32& vCells, float fCellSize, float fBandCells)
{
  plStopwatch timer;

  m_vCells = vCells;
  m_vBricks = plVec3U32((vCells.x + 7) / 8, (vCells.y + 7) / 8, (vCells.z + 7) / 8);
  m_fCellSize = fCellSize;
  m_fBand = fBandCells * fCellSize;
  m_Triangles.Clear();
  m_Indices.Clear();
  m_BakeBricks.Clear();
  m_uiMeshObjects = meshes.GetCount();
  m_uiSourceTriangles = 0;

  const plVec3 vDomainSize = plVec3(static_cast<float>(vCells.x), static_cast<float>(vCells.y), static_cast<float>(vCells.z)) * fCellSize;
  const plBoundingBox domain = plBoundingBox::MakeFromMinMax(plVec3(-m_fBand), vDomainSize + plVec3(m_fBand));
  const float fWeldStep = 1.0e-5f; // metres: corners closer than this are one vertex

  plDynamicArray<plBoundingBox> triangleBoxes;

  for (const SourceMesh& mesh : meshes)
  {
    const plUInt32 uiTriangles = mesh.m_Indices.GetCount() / 3;
    m_uiSourceTriangles += uiTriangles;

    // Weld: corners at the same position are one vertex, so edges and vertices find all their faces (a flat-shaded
    // mesh repeats each corner once per face).
    plHashTable<WeldKey, plUInt32, WeldKeyHash> weld;
    plDynamicArray<plVec3> vertices;
    plDynamicArray<plUInt32> remap;
    remap.SetCountUninitialized(mesh.m_Positions.GetCount());
    for (plUInt32 i = 0; i < mesh.m_Positions.GetCount(); ++i)
    {
      const plVec3 p = mesh.m_Positions[i] - vDomainOrigin;
      const WeldKey key = {static_cast<plInt64>(plMath::Round(p.x / fWeldStep)), static_cast<plInt64>(plMath::Round(p.y / fWeldStep)), static_cast<plInt64>(plMath::Round(p.z / fWeldStep))};
      plUInt32 uiIndex = 0;
      if (!weld.TryGetValue(key, uiIndex))
      {
        uiIndex = vertices.GetCount();
        vertices.PushBack(p);
        weld.Insert(key, uiIndex);
      }
      remap[i] = uiIndex;
    }

    // Faces (degenerate ones dropped), angle-weighted vertex normals, edge normals as the sum of the faces' normals.
    struct Face
    {
      plUInt32 v[3];
      plVec3 n;
    };
    plDynamicArray<Face> faces;
    faces.Reserve(uiTriangles);
    plDynamicArray<plVec3> vertexNormals;
    vertexNormals.SetCount(vertices.GetCount(), plVec3::MakeZero());
    plHashTable<plUInt64, plVec3> edgeNormals;
    auto EdgeKey = [](plUInt32 a, plUInt32 b) { return a < b ? (plUInt64(a) << 32) | b : (plUInt64(b) << 32) | a; };

    for (plUInt32 t = 0; t < uiTriangles; ++t)
    {
      Face face;
      for (plUInt32 k = 0; k < 3; ++k)
        face.v[k] = remap[mesh.m_Indices[t * 3 + k]];
      if (face.v[0] == face.v[1] || face.v[1] == face.v[2] || face.v[0] == face.v[2])
        continue;
      const plVec3& a = vertices[face.v[0]];
      const plVec3& b = vertices[face.v[1]];
      const plVec3& c = vertices[face.v[2]];
      const plVec3 cross = (b - a).CrossRH(c - a);
      if (cross.GetLength() < 1e-12f)
        continue;
      face.n = cross.GetNormalized();
      faces.PushBack(face);

      for (plUInt32 k = 0; k < 3; ++k)
      {
        const plVec3& p = vertices[face.v[k]];
        vertexNormals[face.v[k]] += CornerAngle(p, vertices[face.v[(k + 1) % 3]], vertices[face.v[(k + 2) % 3]]) * face.n;
        bool bExisted = false;
        plVec3& edge = edgeNormals.FindOrAdd(EdgeKey(face.v[k], face.v[(k + 1) % 3]), &bExisted);
        edge = bExisted ? edge + face.n : face.n;
      }
    }

    for (const Face& face : faces)
    {
      const plVec3& a = vertices[face.v[0]];
      const plVec3& b = vertices[face.v[1]];
      const plVec3& c = vertices[face.v[2]];
      plBoundingBox box = plBoundingBox::MakeFromPoints(&a, 1);
      box.ExpandToInclude(b);
      box.ExpandToInclude(c);
      if (!box.Overlaps(domain))
        continue;

      const plUInt32 uiBase = m_Triangles.GetCount();
      m_Triangles.SetCount(uiBase + s_uiFloatsPerTriangle);
      float* pOut = &m_Triangles[uiBase];
      auto Put = [&](const plVec3& v)
      {
        pOut[0] = v.x;
        pOut[1] = v.y;
        pOut[2] = v.z;
        pOut += 3;
      };
      Put(a);
      Put(b);
      Put(c);
      Put(face.n);
      for (plUInt32 k = 0; k < 3; ++k)
      {
        const plVec3* pEdge = edgeNormals.GetValue(EdgeKey(face.v[k], face.v[(k + 1) % 3]));
        Put(SafeNormalized(pEdge != nullptr ? *pEdge : face.n, face.n));
      }
      for (plUInt32 k = 0; k < 3; ++k)
        Put(SafeNormalized(vertexNormals[face.v[k]], face.n));
      const plUInt32 uiObject = static_cast<plUInt32>(&mesh - meshes.GetData());
      plMemoryUtils::Copy(reinterpret_cast<plUInt32*>(pOut), &uiObject, 1);
      pOut[1] = 0.0f;

      triangleBoxes.PushBack(box);
    }
  }

  // Bins: every triangle whose box, grown by the band, touches a brick goes to that brick, so a cell finds every
  // triangle closer than the band in its own brick's list. Counted, then filled (one array, brick after brick).
  const plUInt32 uiBrickCount = m_vBricks.x * m_vBricks.y * m_vBricks.z;
  m_BrickBins.SetCount(uiBrickCount * 2, 0u);
  const float fBrickSize = 8.0f * fCellSize;
  auto BrickRange = [&](const plBoundingBox& box, plVec3U32& out_min, plVec3U32& out_max) -> bool
  {
    const plVec3 vMin = (box.m_vMin - plVec3(m_fBand)) / fBrickSize;
    const plVec3 vMax = (box.m_vMax + plVec3(m_fBand)) / fBrickSize;
    for (plUInt32 a = 0; a < 3; ++a)
    {
      const float fCount = static_cast<float>(a == 0 ? m_vBricks.x : (a == 1 ? m_vBricks.y : m_vBricks.z));
      if (vMax.GetData()[a] < 0.0f || vMin.GetData()[a] >= fCount)
        return false;
      out_min.GetData()[a] = static_cast<plUInt32>(plMath::Clamp(plMath::Floor(vMin.GetData()[a]), 0.0f, fCount - 1.0f));
      out_max.GetData()[a] = static_cast<plUInt32>(plMath::Clamp(plMath::Floor(vMax.GetData()[a]), 0.0f, fCount - 1.0f));
    }
    return true;
  };

  for (plUInt32 pass = 0; pass < 2; ++pass)
  {
    if (pass == 1)
    {
      // Counts to offsets; the second pass counts again from zero.
      plUInt32 uiOffset = 0;
      for (plUInt32 b = 0; b < uiBrickCount; ++b)
      {
        const plUInt32 uiCount = m_BrickBins[b * 2 + 1];
        m_BrickBins[b * 2 + 0] = uiOffset;
        m_BrickBins[b * 2 + 1] = 0;
        uiOffset += uiCount;
        if (uiCount > 0)
          m_BakeBricks.PushBack(b);
      }
      m_Indices.SetCountUninitialized(uiOffset);
    }

    for (plUInt32 t = 0; t < triangleBoxes.GetCount(); ++t)
    {
      plVec3U32 vMin, vMax;
      if (!BrickRange(triangleBoxes[t], vMin, vMax))
        continue;
      for (plUInt32 z = vMin.z; z <= vMax.z; ++z)
      {
        for (plUInt32 y = vMin.y; y <= vMax.y; ++y)
        {
          for (plUInt32 x = vMin.x; x <= vMax.x; ++x)
          {
            const plUInt32 b = x + m_vBricks.x * (y + m_vBricks.y * z);
            if (pass == 1)
              m_Indices[m_BrickBins[b * 2 + 0] + m_BrickBins[b * 2 + 1]] = t;
            ++m_BrickBins[b * 2 + 1];
          }
        }
      }
    }
  }

  m_uiMaxBinCount = 0;
  for (plUInt32 b = 0; b < uiBrickCount; ++b)
    m_uiMaxBinCount = plMath::Max(m_uiMaxBinCount, m_BrickBins[b * 2 + 1]);

  m_fBuildMs = timer.GetRunningTotal().GetMilliseconds();
}

void plGpuFluidLiquidSolidBakeJob::Run()
{
  plSharedPtr<plGpuFluidLiquidSolidBake> pBake = PL_DEFAULT_NEW(plGpuFluidLiquidSolidBake);
  pBake->Build(m_Meshes, m_vDomainOrigin, m_vCells, m_fCellSize, GPU_FLUID_LIQUID_SDF_BAND_CELLS);
  pBake->m_uiMeshObjects = m_Meshes.GetCount();
  m_pResult = pBake;
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidLiquidSolids);
