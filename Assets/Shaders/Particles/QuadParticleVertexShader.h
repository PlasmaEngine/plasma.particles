#include <Shaders/Particles/ParticleCommonVS.h>

#if PARTICLE_QUAD_MODE == PARTICLE_QUAD_MODE_BILLBOARD
#  include <Shaders/Particles/BillboardQuadParticleShaderData.h>
#else
#  include <Shaders/Particles/TangentQuadParticleShaderData.h>
#endif

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
  float Life : TEXCOORD1;
  float Variation : TEXCOORD2;
};

TMP_VS_OUT main(TMP_VS_IN input)
{
  float4 outPosition = mul(GetWorldToScreenMatrix(), float4(input.Position, 1.0));

  TMP_VS_OUT ret;
  ret.Position = outPosition;
  ret.TexCoord0 = input.TexCoord0;
  ret.Color0 = float4(1, 1, 1, 1);
  ret.FogAmount = 1.0;  // 1 == no fog
  ret.Life = 1.0;       // 1 == just spawned
  ret.Variation = 42.0; // purely random value

  return ret;
}

#else

VS_OUT main(uint VertexID : SV_VertexID, uint InstanceID : SV_InstanceID)
{
#  if CAMERA_MODE == CAMERA_MODE_STEREO
  s_ActiveCameraEyeIndex = InstanceID % 2;
#  endif

  VS_OUT ret;
#if CAMERA_MODE == CAMERA_MODE_STEREO
  ret.RenderTargetArrayIndex = InstanceID;
#endif

  uint dataIndex = CalcQuadParticleDataIndex(VertexID);
  uint vertexIndex = CalcQuadParticleVertexIndex(VertexID);

  plBaseParticleShaderData baseParticle = particleBaseData[dataIndex];
  UNPACKHALF2(particleLife, particleSize, baseParticle.LifeAndSize);
  ret.Color0 = UNPACKCOLOR4H(baseParticle.Color);

  Quad quad;

#  if PARTICLE_QUAD_MODE == PARTICLE_QUAD_MODE_BILLBOARD

  plBillboardQuadParticleShaderData billboardData = particleBillboardQuadData[dataIndex];
  UNPACKHALF2(rotationOffset, rotationSpeed, billboardData.RotationOffsetAndSpeed);
  UNPACKHALF2(particleScaleX, particleScaleY, billboardData.Scale);
  quad = CalcQuadOutputPositionAsBillboard(vertexIndex, billboardData.Position, rotationOffset, rotationSpeed, particleSize, float2(particleScaleX, particleScaleY));

#  elif PARTICLE_QUAD_MODE == PARTICLE_QUAD_MODE_TANGENTS

  plTangentQuadParticleShaderData tangentData = particleTangentQuadData[dataIndex];
  quad = CalcQuadOutputPositionWithTangents(vertexIndex, tangentData.Position.xyz, tangentData.TangentX, tangentData.TangentZ, particleSize);

#  elif PARTICLE_QUAD_MODE == PARTICLE_QUAD_MODE_AXIS_ALIGNED

  plTangentQuadParticleShaderData tangentData = particleTangentQuadData[dataIndex];
  quad = CalcQuadOutputPositionWithAlignedAxis(vertexIndex, tangentData.Position.xyz, tangentData.TangentX, tangentData.TangentZ, particleSize);

#  endif

  ret.Position = quad.screenPosition;

  float fVariation = (baseParticle.Variation & 255) / 255.0;

#  if PARTICLE_QUAD_MODE == PARTICLE_QUAD_MODE_AXIS_ALIGNED
  ret.TexCoord0 = ComputeAtlasTexCoordRandomAnimated(QuadTexCoordsAxisAligned[vertexIndex], TextureAtlasVariationFramesX, TextureAtlasVariationFramesY, fVariation, TextureAtlasFlipbookFramesX, TextureAtlasFlipbookFramesY, 1.0f - particleLife);
#  else
  ret.TexCoord0 = ComputeAtlasTexCoordRandomAnimated(QuadTexCoordsBillboard[vertexIndex], TextureAtlasVariationFramesX, TextureAtlasVariationFramesY, fVariation, TextureAtlasFlipbookFramesX, TextureAtlasFlipbookFramesY, 1.0f - particleLife);
#  endif

#  if PARTICLE_LIGHTING_MODE == PARTICLE_LIGHTING_MODE_VERTEX_LIT
  float3 diffuseLight = CalculateParticleLighting(quad.screenPosition, quad.worldPosition, quad.normal);
#  elif PARTICLE_LIGHTING_MODE == PARTICLE_LIGHTING_MODE_SIX_WAY
  // the pixel shader does the shading; the vertex stage only resolves which light drives it
  float3 keyLightDir;
  float3 keyLightColor;
  float3 totalLight = CalculateParticleLightingEx(quad.screenPosition, quad.worldPosition, quad.normal, keyLightDir, keyLightColor);

  ret.KeyLightDir = keyLightDir;
  ret.KeyLightColor = keyLightColor;
  ret.AmbientLight = max(totalLight - keyLightColor, 0.0);

  float3 diffuseLight = 1;
#  elif PARTICLE_LIGHTING_MODE == PARTICLE_LIGHTING_MODE_PER_PIXEL
  // hand the surface to the pixel shader and light it there
  ret.LitWorldPos = quad.worldPosition;
  ret.LitNormal = quad.normal;

  float3 diffuseLight = 1;
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
    ret.Color0 = float4(quad.normal, 1);
  }
#  else
  ret.Color0.rgb *= diffuseLight;
#  endif

  ret.FogAmount = GetFogAmount(quad.worldPosition.xyz);
  ret.Life = particleLife;
  ret.Variation = fVariation;
#  if defined(PARTICLE_HAS_EMISSIVE)
  ret.Emissive = UnpackParticleEmissive(baseParticle.Variation);
#  endif
  return ret;
}

#endif
