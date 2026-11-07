#include <Shaders/Particles/ParticleCommonVS.h>
#include <Shaders/Particles/RibbonShaderData.h>

#if PARTICLE_RENDER_MODE == PARTICLE_RENDER_MODE_NONE

struct TMP_VS_IN
{
  float3 Position : POSITION;
  float2 TexCoord0 : TEXCOORD0;
};

struct TMP_VS_OUT
{
  float4 Position : SV_Position;
  float2 TexCoord0 : TEXCOORD0;
  float4 Color0 : COLOR0;
  float FogAmount : FOG;
};

TMP_VS_OUT main(TMP_VS_IN input)
{
  float4 outPosition = mul(GetWorldToScreenMatrix(), float4(input.Position, 1.0));

  TMP_VS_OUT ret;
  ret.Position = outPosition;
  ret.TexCoord0 = input.TexCoord0;
  ret.Color0 = float4(1, 1, 1, 1);
  ret.FogAmount = 1.0; // 1 == no fog

  return ret;
}

#else

// The whole drawcall is ONE ribbon: NumUsedTrailPoints points connected in order,
// two triangles per segment, expanded around the camera in the vertex shader.
VS_OUT main(uint VertexID : SV_VertexID, uint InstanceID : SV_InstanceID)
{
#  if CAMERA_MODE == CAMERA_MODE_STEREO
  s_ActiveCameraEyeIndex = InstanceID % 2;
#  endif

  VS_OUT ret;
#  if CAMERA_MODE == CAMERA_MODE_STEREO
  ret.RenderTargetArrayIndex = InstanceID;
#  endif

  int segmentIndexOffset[6] = {0, 1, 1, 0, 1, 0};

  int segmentIndex = VertexID / 6;
  int vertexSubIndex = VertexID % 6;
  int pointIndex = segmentIndex + segmentIndexOffset[vertexSubIndex];

  if (segmentIndex + 1 >= NumUsedTrailPoints)
  {
    // degenerate the vertices of unused segments
    ret.Position = float4(0, 0, 0, 0);
    ret.TexCoord0 = float2(0, 0);
    ret.Color0 = float4(0, 0, 0, 0);
    ret.FogAmount = 1.0;
    ret.Life = 0.0;
    ret.Variation = 0.0;

    return ret;
  }

  plRibbonPointShaderData point0 = ribbonPointData[pointIndex];
  UNPACKHALF2(pointU, pointLife, point0.UAndLife);

  int prevPointIdx = max(pointIndex - 1, 0);
  int nextPointIdx = min(pointIndex + 1, NumUsedTrailPoints - 1);

  float3 positionP = ribbonPointData[prevPointIdx].Position;
  float3 positionN = ribbonPointData[nextPointIdx].Position;

  // average tangent through this point; the band always faces the camera
  float3 dirAlong = mul(ObjectToWorldMatrix, float4(positionN - positionP, 0)).xyz;
  float3 dirUp = normalize(cross(dirAlong, GetCameraDirForwards())) * point0.Width;

  float4 offsetUp = float4(dirUp * (QuadPosOffsets[vertexSubIndex].y - 0.5), 0);

  float4 worldPosition = mul(ObjectToWorldMatrix, float4(point0.Position, 1)) + offsetUp;
  float4 screenPosition = mul(GetWorldToScreenMatrix(), worldPosition);

  ret.Position = screenPosition;
  ret.TexCoord0.x = pointU;
  ret.TexCoord0.y = 1.0 - QuadPosOffsets[vertexSubIndex].y;
  ret.Color0 = UNPACKCOLOR4H(point0.Color);

  float3 centerNormal = normalize(cross(normalize(dirAlong), normalize(dirUp)));
  float3 cornerNormal = normalize(offsetUp.xyz);
  float3 normal = normalize(lerp(centerNormal, cornerNormal, NormalCurvature));

#  if PARTICLE_LIGHTING_MODE == PARTICLE_LIGHTING_MODE_VERTEX_LIT
  float3 diffuseLight = CalculateParticleLighting(screenPosition, worldPosition.xyz, normal);
#  else
  float3 diffuseLight = 1;
#  endif

#  if RENDER_PASS == RENDER_PASS_EDITOR
  if (RenderPass == EDITOR_RENDER_PASS_DIFFUSE_LIT_ONLY)
  {
    ret.Color0 = float4(diffuseLight * 0.5, 1);
  }
  else if (RenderPass == EDITOR_RENDER_PASS_PIXEL_NORMALS || RenderPass == EDITOR_RENDER_PASS_VERTEX_NORMALS)
  {
    ret.Color0 = float4(normal, 1);
  }
#  else
  ret.Color0.rgb *= diffuseLight;
#  endif

  ret.FogAmount = GetFogAmount(worldPosition.xyz);
  ret.Life = pointLife;
  ret.Variation = 0.0;

  return ret;
}

#endif
