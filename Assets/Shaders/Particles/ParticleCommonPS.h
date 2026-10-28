#pragma once

// Pulled in first, and before the lighting headers below, because it defines
// SHADOW_FORCE_LAST_CASCADE ahead of including them and carries the shared particle lighting
// functions the per-pixel and six-way paths call.
#include <Shaders/Particles/ParticleCommonVS.h>

#include <Shaders/Materials/MaterialInterpolator.h>
#include <Shaders/Particles/ParticleSystemConstants.h>

#if SHADING_QUALITY == SHADING_QUALITY_NORMAL
#  include <Shaders/Common/Lighting.h>
#elif SHADING_QUALITY == SHADING_QUALITY_SIMPLIFIED
#  include <Shaders/Common/LightingSimplified.h>
#else
#  error "Unknown shading quality configuration."
#endif

// Weights for the six baked light responses. The maps are baked with the light placed on each
// camera-relative axis in turn, so the light direction is resolved against the camera basis.
//
// Channel order follows the positive/negative pairing every exporter uses, which is why the maps
// are conventionally named _P and _N:
//   positive map .rgb = light from +right, +up, +front
//   negative map .rgb = light from -right, -up, -front  (i.e. left, down, back)
//
// lightDir points from the surface towards the light.
void ComputeSixWayWeights(float3 lightDir, out float3 weightsPos, out float3 weightsNeg)
{
  const float3 d = float3(dot(lightDir, GetCameraDirRight()), dot(lightDir, GetCameraDirUp()), dot(lightDir, GetCameraDirForwards()));

  weightsPos = max(d, 0.0);
  weightsNeg = max(-d, 0.0);
}

float CalcCloseToGeometryFadeOut(float4 position)
{
  if (SoftEdgeDistance <= 0.0)
    return 1.0;

  return DepthFade(position.xyw, SoftEdgeDistance);
}

float CalcCloseToCameraFadeOut(float4 position)
{
  if (CameraFadeDistance <= 0.0)
    return 1.0;

  return saturate(position.w / CameraFadeDistance);
}

// The type's emissive map times its colour and intensity; black without an emissive map.
float3 SampleParticleEmissive(Texture2D emissiveMap, SamplerState emissiveSampler, float2 texCoord, float fPerParticle)
{
  if ((LookFlags & PARTICLE_LOOK_FLAG_EMISSIVE_MAP) == 0)
    return 0.0;

  return emissiveMap.Sample(emissiveSampler, texCoord).rgb * EmissiveColor.rgb * fPerParticle;
}

float CalcProximityFadeOut(float4 position)
{
  return CalcCloseToGeometryFadeOut(position) * CalcCloseToCameraFadeOut(position);
}
