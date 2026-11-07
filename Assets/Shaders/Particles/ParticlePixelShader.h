#include <Shaders/Common/StandardMacros.h>
#include <Shaders/Particles/ParticleCommonPS.h>

void FillCustomGlobals();
float GetParticleOpacity();
float3 GetParticleColor();

// optional, for shaders that define the matching PARTICLE_HAS_* macro
float3 GetParticleEmissive();
float2 GetParticleDistortionOffset();

#if PARTICLE_RENDER_MODE == PARTICLE_RENDER_MODE_NONE
struct TMP_PS_IN
{
  float4 Position : SV_Position;
  float2 TexCoord0 : TEXCOORD0;
  float4 Color0 : COLOR0;
  float FogAmount : FOG;
  float Life : TEXCOORD1;
  float Variation : TEXCOORD2;
};

struct PS_GLOBALS
{
  TMP_PS_IN Input;
};
static PS_GLOBALS G;

float4 main(TMP_PS_IN Input)
  : SV_Target
{
  G.Input = Input;
  FillCustomGlobals();
  return float4(PreExpose(GetParticleColor()), 1.0);
}

#else

struct PS_GLOBALS
{
  PS_IN Input;
};
static PS_GLOBALS G;

float4 main(PS_IN Input)
  : SV_Target
{
  G.Input = Input;
  FillCustomGlobals();

#  if CAMERA_MODE == CAMERA_MODE_STEREO
  s_ActiveCameraEyeIndex = Input.RenderTargetArrayIndex;
#  endif

  float opacity = GetParticleOpacity();
  float proximityFade = 1.0;
#  if !(defined(RENDER_PASS) && RENDER_PASS == RENDER_PASS_OVERDRAW) && CAMERA_MODE != CAMERA_MODE_ORTHO
  proximityFade = CalcProximityFadeOut(Input.Position);
  opacity *= proximityFade;
#  endif
  if (opacity <= 1.0 / 255.0)
  {
    discard;
  }

#  if PARTICLE_RENDER_MODE == PARTICLE_RENDER_MODE_DISTORTION
  // The distortion pass adds up screen offsets into an RG target; it has no depth target, so occluded pixels are
  // rejected against the scene depth here.
#    if CAMERA_MODE != CAMERA_MODE_ORTHO
  if (SampleSceneDepth(Input.Position.xy) < Input.Position.w)
  {
    discard;
  }
#    endif
#    if defined(PARTICLE_HAS_DISTORTION_OFFSET)
  return float4(GetParticleDistortionOffset() * opacity, 0.0, 0.0);
#    else
  return float4(0.0, 0.0, 0.0, 0.0);
#    endif
#  endif

#  if defined(RENDER_PASS) && RENDER_PASS == RENDER_PASS_OVERDRAW
  return float4(0.04, 0.01, 0.001, 0.0);
#  elif defined(RENDER_PASS) && RENDER_PASS == RENDER_PASS_GEOMETRY_ID
  return float4(0.08, 0.08, 0.08, 1.0);
#  endif

  float3 finalColor = GetParticleColor();

#  if defined(PARTICLE_HAS_EMISSIVE)
  finalColor += GetParticleEmissive();
#  endif

#  if PARTICLE_RENDER_MODE == PARTICLE_RENDER_MODE_BLENDADD
  // Blend Add adds its colour without scaling it by alpha, so the soft edges have to fade the colour as well
  finalColor *= proximityFade;
#  endif

#  if PARTICLE_RENDER_MODE == PARTICLE_RENDER_MODE_OPAQUE
  opacity = 1.0;
#  endif

// fog behavior
#  if PARTICLE_RENDER_MODE == PARTICLE_RENDER_MODE_ADDITIVE
  // for additive blending, color shouldn't be adjusted, just fade out
  opacity *= Input.FogAmount;
#  else
  finalColor = ApplyFog(finalColor, Input.FogAmount);
#  endif

#  if defined(RENDER_PASS) && RENDER_PASS == RENDER_PASS_EDITOR
  if (RenderPass == EDITOR_RENDER_PASS_DIFFUSE_LIT_ONLY)
  {
    return float4(SrgbToLinear(Input.Color0.rgb * Exposure), 1);
  }
  else if (RenderPass == EDITOR_RENDER_PASS_TEXCOORDS_UV0)
  {
    return float4(SrgbToLinear(float3(frac(Input.TexCoord0.xy), 0)), 1);
  }
  else if (RenderPass == EDITOR_RENDER_PASS_DIFFUSE_COLOR || RenderPass == EDITOR_RENDER_PASS_DIFFUSE_COLOR_RANGE)
  {
    return float4(finalColor, 1);
  }
  else if (RenderPass == EDITOR_RENDER_PASS_PIXEL_NORMALS || RenderPass == EDITOR_RENDER_PASS_VERTEX_NORMALS)
  {
    return float4(SrgbToLinear(normalize(Input.Color0.rgb) * 0.5 + 0.5), 1);
  }
  else if (RenderPass == EDITOR_RENDER_PASS_OCCLUSION)
  {
    return 1;
  }
  else if (RenderPass == EDITOR_RENDER_PASS_DEPTH)
  {
    float depth = Input.Position.w * ClipPlanes.z;
    return float4(SrgbToLinear(depth), 1);
  }
  else
  {
    return float4(0, 0, 0, 1);
  }
#  else
#    if !(defined(RENDER_PASS) && (RENDER_PASS == RENDER_PASS_WIREFRAME || RENDER_PASS == RENDER_PASS_PICKING || RENDER_PASS == RENDER_PASS_PICKING_WIREFRAME))
  // Radiance enters scene color pre-exposed; diagnostic passes keep the raw colour.
  finalColor = PreExpose(finalColor);
#    endif
  return float4(finalColor, opacity);
#  endif
}

#endif
