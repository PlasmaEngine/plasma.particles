#include <ParticlePlugin/ParticlePluginPCH.h>

#include <ParticlePlugin/Behavior/ParticleBehavior_AttractToPosition.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Bounds.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_BoundsSphere.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ColorGradient.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_PullAlong.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Drag.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_FadeOut.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Flies.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SceneForces.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Gravity.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Raycast.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SizeCurve.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Turbulence.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_VectorField.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Velocity.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/System/ParticleSystemInstance.h>
#include <ParticlePlugin/Type/GPU/ParticleGPULowering.h>
#include <ParticlePlugin/Type/GPU/ParticleTypeGPU.h>
#include <ParticlePlugin/Type/Point/ParticleTypePoint.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>
#include <ParticlePlugin/Type/Trail/ParticleTypeTrail.h>

#include <Foundation/Math/Color16f.h>
#include <Foundation/SimdMath/SimdNoise.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Textures/Texture2DResource.h>
#include <RendererCore/Textures/Texture3DResource.h>

namespace
{
  constexpr plUInt32 c_uiLUTWidth = 128;
  constexpr plUInt32 c_uiNoiseSize = 32;

  plTexture2DResourceHandle BakeGradientLUT(const plColorGradientResourceHandle& hGradient, const plColor& tintColor)
  {
    if (!hGradient.IsValid())
      return {};

    plResourceLock<plColorGradientResource> pGradient(hGradient, plResourceAcquireMode::BlockTillLoaded);
    if (pGradient.GetAcquireResult() == plResourceAcquireResult::MissingFallback)
      return {};

    const plColorGradient& gradient = pGradient->GetDescriptor().m_Gradient;

    plStringBuilder sResourceId;
    sResourceId.SetFormat("ParticleGradientLUT_{0}_{1}_{2}", plArgU(pGradient->GetResourceIDHash(), 16, false, 16), pGradient->GetCurrentResourceChangeCounter(),
      plArgU(plHashingUtils::xxHash32(&tintColor, sizeof(tintColor)), 8, false, 16));

    plTexture2DResourceHandle hLUT = plResourceManager::GetExistingResource<plTexture2DResource>(sResourceId);
    if (hLUT.IsValid())
      return hLUT;

    plHybridArray<plFloat16, c_uiLUTWidth * 4> data;
    data.SetCountUninitialized(c_uiLUTWidth * 4);

    for (plUInt32 i = 0; i < c_uiLUTWidth; ++i)
    {
      const float x = (float)i / (float)(c_uiLUTWidth - 1);

      // must match plParticleBehavior_ColorGradient (gamma color + byte alpha, times tint)
      plColor rgba;
      plUInt8 alpha;
      gradient.EvaluateColor(x, rgba);
      gradient.EvaluateAlpha(x, alpha);
      rgba.a = plMath::ColorByteToFloat(alpha);
      rgba *= tintColor;

      data[i * 4 + 0] = rgba.r;
      data[i * 4 + 1] = rgba.g;
      data[i * 4 + 2] = rgba.b;
      data[i * 4 + 3] = rgba.a;
    }

    plGALSystemMemoryDescription initData;
    initData.m_pData = plMakeArrayPtr(reinterpret_cast<const plUInt8*>(data.GetData()), data.GetCount() * sizeof(plFloat16));
    initData.m_uiRowPitch = c_uiLUTWidth * 4 * sizeof(plFloat16);
    initData.m_uiSlicePitch = initData.m_uiRowPitch;

    plTexture2DResourceDescriptor desc;
    desc.m_DescGAL.m_Format = plGALResourceFormat::RGBAHalf;
    desc.m_DescGAL.m_uiWidth = c_uiLUTWidth;
    desc.m_DescGAL.m_uiHeight = 1;
    desc.m_DescGAL.m_uiMipLevelCount = 1;
    desc.m_DescGAL.m_bAllowShaderResourceView = true;
    desc.m_InitialContent = plMakeArrayPtr(&initData, 1);

    return plResourceManager::GetOrCreateResource<plTexture2DResource>(sResourceId, std::move(desc), "baked particle gradient LUT");
  }

  plTexture2DResourceHandle BakeSizeCurveLUT(const plCurve1DResourceHandle& hCurve)
  {
    if (!hCurve.IsValid())
      return {};

    plResourceLock<plCurve1DResource> pCurve(hCurve, plResourceAcquireMode::BlockTillLoaded);
    if (pCurve.GetAcquireResult() == plResourceAcquireResult::MissingFallback || pCurve->GetDescriptor().m_Curves.IsEmpty())
      return {};

    const plCurve1D& curve = pCurve->GetDescriptor().m_Curves[0];

    plStringBuilder sResourceId;
    sResourceId.SetFormat("ParticleSizeCurveLUT_{0}_{1}", plArgU(pCurve->GetResourceIDHash(), 16, false, 16), pCurve->GetCurrentResourceChangeCounter());

    plTexture2DResourceHandle hLUT = plResourceManager::GetExistingResource<plTexture2DResource>(sResourceId);
    if (hLUT.IsValid())
      return hLUT;

    plHybridArray<plFloat16, c_uiLUTWidth> data;
    data.SetCountUninitialized(c_uiLUTWidth);

    for (plUInt32 i = 0; i < c_uiLUTWidth; ++i)
    {
      const float x = (float)i / (float)(c_uiLUTWidth - 1);

      // must match plParticleBehavior_SizeCurve (normalized-position eval, normalized value)
      const double evalPos = curve.ConvertNormalizedPos(x);
      data[i] = (float)curve.NormalizeValue(curve.Evaluate(evalPos));
    }

    plGALSystemMemoryDescription initData;
    initData.m_pData = plMakeArrayPtr(reinterpret_cast<const plUInt8*>(data.GetData()), data.GetCount() * sizeof(plFloat16));
    initData.m_uiRowPitch = c_uiLUTWidth * sizeof(plFloat16);
    initData.m_uiSlicePitch = initData.m_uiRowPitch;

    plTexture2DResourceDescriptor desc;
    desc.m_DescGAL.m_Format = plGALResourceFormat::RHalf;
    desc.m_DescGAL.m_uiWidth = c_uiLUTWidth;
    desc.m_DescGAL.m_uiHeight = 1;
    desc.m_DescGAL.m_uiMipLevelCount = 1;
    desc.m_DescGAL.m_bAllowShaderResourceView = true;
    desc.m_InitialContent = plMakeArrayPtr(&initData, 1);

    return plResourceManager::GetOrCreateResource<plTexture2DResource>(sResourceId, std::move(desc), "baked particle size curve LUT");
  }

  plTexture3DResourceHandle GetTurbulenceNoiseTexture()
  {
    // one shared bake of the same perlin noise the CPU Turbulence behavior samples
    // (seed 12345, 3 channels decorrelated by the same diagonal offsets)
    const char* szResourceId = "ParticleTurbulenceNoise3D";

    plTexture3DResourceHandle hNoise = plResourceManager::GetExistingResource<plTexture3DResource>(szResourceId);
    if (hNoise.IsValid())
      return hNoise;

    plSimdPerlinNoise noise(12345);
    const float kOffset1 = 31.416f;
    const float kOffset2 = 67.123f;

    plDynamicArray<plFloat16> data;
    data.SetCountUninitialized(c_uiNoiseSize * c_uiNoiseSize * c_uiNoiseSize * 4);

    for (plUInt32 z = 0; z < c_uiNoiseSize; ++z)
    {
      for (plUInt32 y = 0; y < c_uiNoiseSize; ++y)
      {
        for (plUInt32 x = 0; x < c_uiNoiseSize; ++x)
        {
          // bake at texel centers - perlin noise is zero at integer lattice points
          const float sx = (float)x + 0.5f;
          const float sy = (float)y + 0.5f;
          const float sz = (float)z + 0.5f;

          const plSimdVec4f samples = noise.NoiseZeroToOne(
            plSimdVec4f(sx, sx + kOffset1, sx + kOffset2, 0.0f),
            plSimdVec4f(sy, sy + kOffset1, sy + kOffset2, 0.0f),
            plSimdVec4f(sz, sz + kOffset1, sz + kOffset2, 0.0f));

          plFloat16* pTexel = &data[((z * c_uiNoiseSize + y) * c_uiNoiseSize + x) * 4];
          pTexel[0] = (float)samples.GetComponent<0>() - 0.5f;
          pTexel[1] = (float)samples.GetComponent<1>() - 0.5f;
          pTexel[2] = (float)samples.GetComponent<2>() - 0.5f;
          pTexel[3] = 0.0f;
        }
      }
    }

    plGALSystemMemoryDescription initData;
    initData.m_pData = plMakeArrayPtr(reinterpret_cast<const plUInt8*>(data.GetData()), data.GetCount() * sizeof(plFloat16));
    initData.m_uiRowPitch = c_uiNoiseSize * 4 * sizeof(plFloat16);
    initData.m_uiSlicePitch = c_uiNoiseSize * c_uiNoiseSize * 4 * sizeof(plFloat16);

    plTexture3DResourceDescriptor desc;
    desc.m_DescGAL.m_Type = plGALTextureType::Texture3D;
    desc.m_DescGAL.m_Format = plGALResourceFormat::RGBAHalf;
    desc.m_DescGAL.m_uiWidth = c_uiNoiseSize;
    desc.m_DescGAL.m_uiHeight = c_uiNoiseSize;
    desc.m_DescGAL.m_uiDepth = c_uiNoiseSize;
    desc.m_DescGAL.m_uiMipLevelCount = 1;
    desc.m_DescGAL.m_bAllowShaderResourceView = true;
    desc.m_InitialContent = plMakeArrayPtr(&initData, 1);

    return plResourceManager::GetOrCreateResource<plTexture3DResource>(szResourceId, std::move(desc), "baked particle turbulence noise");
  }

  plTexture3DResourceHandle BakeVectorFieldTexture(const plVectorFieldResourceHandle& hField)
  {
    if (!hField.IsValid())
      return {};

    plResourceLock<plVectorFieldResource> pField(hField, plResourceAcquireMode::BlockTillLoaded);
    if (pField.GetAcquireResult() == plResourceAcquireResult::MissingFallback || !pField->GetDescriptor().IsValid())
      return {};

    const plVectorFieldResourceDescriptor& field = pField->GetDescriptor();

    plStringBuilder sResourceId;
    sResourceId.SetFormat("ParticleVectorFieldTex_{0}_{1}", plArgU(pField->GetResourceIDHash(), 16, false, 16), pField->GetCurrentResourceChangeCounter());

    plTexture3DResourceHandle hTex = plResourceManager::GetExistingResource<plTexture3DResource>(sResourceId);
    if (hTex.IsValid())
      return hTex;

    const plVec3U32 dims = field.m_vDimensions;

    plDynamicArray<plFloat16> data;
    data.SetCountUninitialized(dims.x * dims.y * dims.z * 4);

    for (plUInt32 i = 0; i < field.m_Vectors.GetCount(); ++i)
    {
      const plVec3& v = field.m_Vectors[i];
      plFloat16* pTexel = &data[i * 4];
      pTexel[0] = v.x;
      pTexel[1] = v.y;
      pTexel[2] = v.z;
      pTexel[3] = 0.0f;
    }

    plGALSystemMemoryDescription initData;
    initData.m_pData = plMakeArrayPtr(reinterpret_cast<const plUInt8*>(data.GetData()), data.GetCount() * sizeof(plFloat16));
    initData.m_uiRowPitch = dims.x * 4 * sizeof(plFloat16);
    initData.m_uiSlicePitch = dims.x * dims.y * 4 * sizeof(plFloat16);

    plTexture3DResourceDescriptor desc;
    desc.m_DescGAL.m_Type = plGALTextureType::Texture3D;
    desc.m_DescGAL.m_Format = plGALResourceFormat::RGBAHalf;
    desc.m_DescGAL.m_uiWidth = dims.x;
    desc.m_DescGAL.m_uiHeight = dims.y;
    desc.m_DescGAL.m_uiDepth = dims.z;
    desc.m_DescGAL.m_uiMipLevelCount = 1;
    desc.m_DescGAL.m_bAllowShaderResourceView = true;
    desc.m_InitialContent = plMakeArrayPtr(&initData, 1);

    return plResourceManager::GetOrCreateResource<plTexture3DResource>(sResourceId, std::move(desc), "baked particle vector field");
  }

  // A lowered quad system is drawn by the ordinary quad renderer from structs the simulate
  // dispatch writes, so every quad feature works; only the CPU-side last-position tracking
  // behind the particle-direction orientation has no GPU equivalent.
  plStringView CheckQuadTypeLowerable(const plParticleTypeQuadFactory* pQuad)
  {
    PL_IGNORE_UNUSED(pQuad);
    return {};
  }
} // namespace

plStringView plParticleGPULowering::FindLoweringBlocker(
  const plParticleSystemDescriptor* pDescriptor, BlockerKind* out_pKind, plUInt32* out_uiIndex)
{
  // A small helper so every early-out reports where the blocker was found.
  auto blocked = [&](BlockerKind kind, plUInt32 uiIndex, plStringView sReason) -> plStringView
  {
    if (out_pKind != nullptr)
      *out_pKind = kind;
    if (out_uiIndex != nullptr)
      *out_uiIndex = uiIndex;
    return sReason;
  };

  if (out_pKind != nullptr)
    *out_pKind = BlockerKind::None;
  if (out_uiIndex != nullptr)
    *out_uiIndex = 0;

  // ---- render type ----
  const auto& typeFactories = pDescriptor->GetTypeFactories();

  if (typeFactories.GetCount() != 1)
    return blocked(BlockerKind::Type, 0, "Multiple render types");

  if (const auto* pQuad = plDynamicCast<const plParticleTypeQuadFactory*>(typeFactories[0]))
  {
    const plStringView sReason = CheckQuadTypeLowerable(pQuad);
    if (!sReason.IsEmpty())
      return blocked(BlockerKind::Type, 0, sReason);
  }
  else if (plDynamicCast<const plParticleTypePointFactory*>(typeFactories[0]) == nullptr &&
           plDynamicCast<const plParticleTypeTrailFactory*>(typeFactories[0]) == nullptr)
  {
    return blocked(BlockerKind::Type, 0, typeFactories[0]->GetDynamicRTTI()->GetTypeName());
  }

  // ---- behaviors: every module needs a GPU equivalent, one instance of each kind ----
  plSet<const plRTTI*> seenKinds;
  plUInt32 uiBehaviorIndex = 0;

  for (const plParticleBehaviorFactory* pFactory : pDescriptor->GetBehaviorFactories())
  {
    const plRTTI* pRtti = pFactory->GetDynamicRTTI();
    const plUInt32 uiIndex = uiBehaviorIndex++;

    if (seenKinds.Contains(pRtti) && pRtti != plGetStaticRTTI<plParticleBehaviorFactory_Gravity>())
      return blocked(BlockerKind::Behavior, uiIndex, "Duplicate behavior (the GPU runs one instance of each kind)");
    seenKinds.Insert(pRtti);

    if (const auto* pDrag = plDynamicCast<const plParticleBehaviorFactory_Drag*>(pFactory))
    {
      if (pDrag->m_bSizeRelative)
        return blocked(BlockerKind::Behavior, uiIndex, "Size-relative drag");
    }
    else if (const auto* pRaycast = plDynamicCast<const plParticleBehaviorFactory_Raycast*>(pFactory))
    {
      // the GPU approximates collision against the depth buffer and the scene SDF, which cannot
      // raise a CPU-side event or honour a physics collision layer
      if (!pRaycast->m_sOnCollideEvent.IsEmpty())
        return blocked(BlockerKind::Behavior, uiIndex, "Raycast collision event");
    }
    else if (plDynamicCast<const plParticleBehaviorFactory_Velocity*>(pFactory) || plDynamicCast<const plParticleBehaviorFactory_Gravity*>(pFactory) ||
             plDynamicCast<const plParticleBehaviorFactory_ColorGradient*>(pFactory) || plDynamicCast<const plParticleBehaviorFactory_SizeCurve*>(pFactory) ||
             plDynamicCast<const plParticleBehaviorFactory_FadeOut*>(pFactory) || plDynamicCast<const plParticleBehaviorFactory_Turbulence*>(pFactory) ||
             plDynamicCast<const plParticleBehaviorFactory_VectorField*>(pFactory) || plDynamicCast<const plParticleBehaviorFactory_Bounds*>(pFactory) ||
             plDynamicCast<const plParticleBehaviorFactory_AttractToPosition*>(pFactory) || plDynamicCast<const plParticleBehaviorFactory_PullAlong*>(pFactory) ||
             plDynamicCast<const plParticleBehaviorFactory_BoundsSphere*>(pFactory) || plDynamicCast<const plParticleBehaviorFactory_SceneForces*>(pFactory) ||
             plDynamicCast<const plParticleBehaviorFactory_Flies*>(pFactory))
    {
      // lowerable
    }
    else
    {
      return blocked(BlockerKind::Behavior, uiIndex, pRtti->GetTypeName());
    }
  }

  return {};
}

void plParticleGPULowering::ConfigureLoweredType(plParticleTypeGPU* pType, const plParticleSystemDescriptor* pDescriptor, plParticleSystemInstance* pOwner)
{
  plParticleEffectInstance* pEffect = pOwner->GetOwnerEffect();

  pType->m_bLowered = true;
  pType->m_uiFeatureFlags = 0;
  pType->m_uiMaxGPUParticles = plMath::Max<plUInt32>(32, (plUInt32)pOwner->GetMaxParticles());
  pType->m_fGravityFactor = 0.0f; // no Gravity behavior means no gravity

  // ---- render type ----
  if (const auto* pQuad = plDynamicCast<const plParticleTypeQuadFactory*>(pDescriptor->GetTypeFactories()[0]))
  {
    // rendered through the normal quad renderer, so the whole quad feature set carries over
    pType->m_CPURenderPath = plParticleTypeGPU::CPURenderPath::Quad;
    pType->m_bRenderAsQuad = true;
    pType->m_GPURenderType = plGPUParticleRenderType::Billboard; // vertex count per particle only

    pType->m_RenderMode = pQuad->m_RenderMode;
    pType->m_QuadOrientation = pQuad->m_Orientation;
    pType->m_TextureAtlasType = pQuad->m_TextureAtlasType;
    pType->m_uiNumSpritesX = pQuad->m_uiNumSpritesX;
    pType->m_uiNumSpritesY = pQuad->m_uiNumSpritesY;
    pType->m_sTintColorParameter = plTempHashedString(pQuad->m_sTintColorParameter.GetData());
    pType->m_fDistortionStrength = pQuad->m_fDistortionStrength;
    pType->m_fStretch = pQuad->m_fStretch;
    pType->m_LightingMode = pQuad->m_LightingMode;
    pType->m_fNormalCurvature = pQuad->m_fNormalCurvature;
    pType->m_fLightDirectionality = pQuad->m_fLightDirectionality;
    pType->m_fSixWayAbsorption = pQuad->m_fSixWayAbsorption;

    // A GPU system can light once per particle in the scatter dispatch instead of at each of the
    // four quad corners, which is both cheaper and independent of overdraw. The render side then
    // draws fullbright because the light is already folded into the particle colour.
    if (pQuad->m_LightingMode == plParticleLightingMode::VertexLit)
    {
      pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_PER_PARTICLE_LIGHTING;
      pType->m_LightingMode = plParticleLightingMode::Fullbright;
    }

    if (!pQuad->m_sTexture.IsEmpty())
    {
      pType->m_hTexture = plResourceManager::LoadResource<plTexture2DResource>(pQuad->m_sTexture);
    }

    if (!pQuad->m_sDistortionTexture.IsEmpty())
    {
      pType->m_hDistortionTexture = plResourceManager::LoadResource<plTexture2DResource>(pQuad->m_sDistortionTexture);
    }

    if (!pQuad->m_sSixWayMapA.IsEmpty())
    {
      pType->m_hSixWayMapA = plResourceManager::LoadResource<plTexture2DResource>(pQuad->m_sSixWayMapA);
    }

    if (!pQuad->m_sSixWayMapB.IsEmpty())
    {
      pType->m_hSixWayMapB = plResourceManager::LoadResource<plTexture2DResource>(pQuad->m_sSixWayMapB);
    }

    if (pQuad->m_bUseCustomMaterial && !pQuad->m_sCustomMaterial.IsEmpty())
    {
      pType->m_hCustomMaterial = plResourceManager::LoadResource<plMaterialResource>(pQuad->m_sCustomMaterial);
    }
  }
  else if (const auto* pTrail = plDynamicCast<const plParticleTypeTrailFactory*>(pDescriptor->GetTypeFactories()[0]))
  {
    // the trail renderer reads the same base data plus its point list, which the simulate
    // dispatch repacks out of the GPU trail ring
    pType->m_CPURenderPath = plParticleTypeGPU::CPURenderPath::Trail;
    pType->m_bRenderAsQuad = true; // writes the shared base data
    pType->m_GPURenderType = plGPUParticleRenderType::Trail;
    pType->m_QuadOrientation = plQuadParticleOrientation::Billboard;

    pType->m_RenderMode = pTrail->m_RenderMode;
    pType->m_uiMaxTrailPoints = plMath::Clamp<plUInt32>(pTrail->m_uiMaxPoints, 4, 64);
    pType->m_TextureAtlasType = pTrail->m_TextureAtlasType;
    pType->m_uiNumSpritesX = pTrail->m_uiNumSpritesX;
    pType->m_uiNumSpritesY = pTrail->m_uiNumSpritesY;
    pType->m_sTintColorParameter = plTempHashedString(pTrail->m_sTintColorParameter.GetData());
    pType->m_fDistortionStrength = pTrail->m_fDistortionStrength;
    pType->m_LightingMode = pTrail->m_LightingMode;
    pType->m_fNormalCurvature = pTrail->m_fNormalCurvature;
    pType->m_fLightDirectionality = pTrail->m_fLightDirectionality;

    if (!pTrail->m_sTexture.IsEmpty())
    {
      pType->m_hTexture = plResourceManager::LoadResource<plTexture2DResource>(pTrail->m_sTexture);
    }

    if (!pTrail->m_sDistortionTexture.IsEmpty())
    {
      pType->m_hDistortionTexture = plResourceManager::LoadResource<plTexture2DResource>(pTrail->m_sDistortionTexture);
    }

    if (pTrail->m_bUseCustomMaterial && !pTrail->m_sCustomMaterial.IsEmpty())
    {
      pType->m_hCustomMaterial = plResourceManager::LoadResource<plMaterialResource>(pTrail->m_sCustomMaterial);
    }
  }
  else
  {
    // points share the quad base + billboard structs, drawn by the point renderer
    pType->m_CPURenderPath = plParticleTypeGPU::CPURenderPath::Point;
    pType->m_bRenderAsQuad = true;
    pType->m_QuadOrientation = plQuadParticleOrientation::Billboard;
    pType->m_GPURenderType = plGPUParticleRenderType::Point;
    pType->m_RenderMode = plParticleTypeRenderMode::Blended;
  }

  // ---- behaviors ----
  for (const plParticleBehaviorFactory* pFactory : pDescriptor->GetBehaviorFactories())
  {
    if (const auto* pGravity = plDynamicCast<const plParticleBehaviorFactory_Gravity*>(pFactory))
    {
      pType->m_fGravityFactor += pGravity->m_fGravityFactor;
    }
    else if (const auto* pVelocity = plDynamicCast<const plParticleBehaviorFactory_Velocity*>(pFactory))
    {
      pType->m_fRiseSpeed = pVelocity->m_fRiseSpeed;
      pType->m_fFriction = pVelocity->m_fFriction;
      pType->m_fWindStrength = pVelocity->m_fWindInfluence;

      if (pVelocity->m_fWindInfluence > 0.0f)
      {
        pEffect->RequestWindSamples();
      }
    }
    else if (const auto* pRaycast = plDynamicCast<const plParticleBehaviorFactory_Raycast*>(pFactory))
    {
      // depth-buffer + SDF collision is the GPU's stand-in for per-particle physics raycasts;
      // the collision layer has no equivalent and is ignored
      pType->m_bEnableDepthCollision = true;
      pType->m_bEnableSDFCollision = true;
      pType->m_CollisionReaction = pRaycast->m_Reaction;
      pType->m_fCollisionBounceFactor = pRaycast->m_fBounceFactor;
      pType->m_fCollisionSlideFactor = pRaycast->m_fSlideFactor;

      // the CPU casts a ray as long as the particle is wide; the GPU tests a depth slab of the
      // same thickness, so the size factor carries over directly
      pType->m_fCollisionThickness = plMath::Max(pRaycast->m_fSizeFactor, 0.01f);
    }
    else if (const auto* pDrag = plDynamicCast<const plParticleBehaviorFactory_Drag*>(pFactory))
    {
      // same parameter fold as plParticleBehaviorFactory_Drag::CopyBehaviorProperties
      const float fDragScale = plMath::Max(pEffect->GetFloatParameter(plTempHashedString(pDrag->m_sDragParameter.GetData()), 1.0f), 0.0f);
      pType->m_fLinearDrag = pDrag->m_fDrag * fDragScale;
    }
    else if (const auto* pGradient = plDynamicCast<const plParticleBehaviorFactory_ColorGradient*>(pFactory))
    {
      pType->m_hGradientLUT = BakeGradientLUT(pGradient->m_hGradient, pGradient->m_TintColor);

      if (pType->m_hGradientLUT.IsValid())
      {
        pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_COLOR_GRADIENT;

        if (pGradient->m_GradientMode == plParticleColorGradientMode::Speed)
        {
          pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_COLOR_GRADIENT_SPEED;
          pType->m_fColorMaxSpeed = plMath::Max(pGradient->m_fMaxSpeed, 0.01f);
        }
      }
    }
    else if (const auto* pSizeCurve = plDynamicCast<const plParticleBehaviorFactory_SizeCurve*>(pFactory))
    {
      pType->m_hSizeCurveLUT = BakeSizeCurveLUT(pSizeCurve->m_hCurve);

      if (pType->m_hSizeCurveLUT.IsValid())
      {
        pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_SIZE_CURVE;
        pType->m_fSizeBase = pSizeCurve->m_fBaseSize;
        pType->m_fSizeScale = pSizeCurve->m_fCurveScale;
      }
    }
    else if (const auto* pFadeOut = plDynamicCast<const plParticleBehaviorFactory_FadeOut*>(pFactory))
    {
      pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_FADE_OUT;
      pType->m_fFadeStartAlpha = pFadeOut->m_fStartAlpha;
      pType->m_fFadeExponent = pFadeOut->m_fExponent;
    }
    else if (const auto* pTurbulence = plDynamicCast<const plParticleBehaviorFactory_Turbulence*>(pFactory))
    {
      pType->m_hTurbulenceNoise = GetTurbulenceNoiseTexture();

      if (pType->m_hTurbulenceNoise.IsValid())
      {
        pType->m_uiFeatureFlags |= pTurbulence->m_bAffectVelocity ? GPU_PARTICLE_FEATURE_TURBULENCE : GPU_PARTICLE_FEATURE_TURBULENCE_ON_POSITION;
        pType->m_fTurbStrength = pTurbulence->m_fStrength;
        pType->m_fTurbFrequency = pTurbulence->m_fFrequency;
        pType->m_vTurbScrollSpeed = pTurbulence->m_vScrollSpeed;
        pType->m_uiTurbOctaves = plMath::Clamp<plUInt8>(pTurbulence->m_uiOctaves, 1, 4);
      }
    }
    else if (const auto* pVectorField = plDynamicCast<const plParticleBehaviorFactory_VectorField*>(pFactory))
    {
      // resolve the field resource exactly like the CPU behavior factory does
      plVectorFieldResourceHandle hField;

      if (pVectorField->m_Source == plParticleVectorFieldSource::File)
      {
        if (!pVectorField->m_sFile.IsEmpty())
        {
          hField = plResourceManager::LoadResource<plVectorFieldResource>(pVectorField->m_sFile);
        }
      }
      else
      {
        plStringBuilder sResourceId;
        sResourceId.SetFormat("VectorFieldCurl_{0}_{1}_{2}_{3}", pVectorField->m_uiResolution, pVectorField->m_uiSeed, pVectorField->m_fFrequency, pVectorField->m_uiOctaves);

        hField = plResourceManager::GetExistingResource<plVectorFieldResource>(sResourceId);

        if (!hField.IsValid())
        {
          plVectorFieldResourceDescriptor desc;
          desc.InitializeCurlNoise(pVectorField->m_uiResolution, pVectorField->m_uiSeed, pVectorField->m_fFrequency, pVectorField->m_uiOctaves);

          hField = plResourceManager::CreateResource<plVectorFieldResource>(sResourceId, std::move(desc), "baked particle curl noise");
        }
      }

      pType->m_hVectorFieldTexture = BakeVectorFieldTexture(hField);

      if (pType->m_hVectorFieldTexture.IsValid())
      {
        pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_VECTOR_FIELD;

        if (pVectorField->m_Mode == plParticleVectorFieldMode::Velocity)
          pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_VECTOR_FIELD_VELOCITY;

        if (pVectorField->m_Mapping == plParticleVectorFieldMapping::Tile)
          pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_VECTOR_FIELD_TILE;

        pType->m_vVectorFieldSize = pVectorField->m_vFieldSize.CompMax(plVec3(0.01f));
        pType->m_vVectorFieldOffset = pVectorField->m_vPositionOffset;

        const float fStrengthScale = plMath::Max(pEffect->GetFloatParameter(plTempHashedString(pVectorField->m_sStrengthParameter.GetData()), 1.0f), 0.0f);
        pType->m_fVectorFieldStrength = pVectorField->m_fStrength * fStrengthScale;
      }
    }
    else if (const auto* pAttractor = plDynamicCast<const plParticleBehaviorFactory_AttractToPosition*>(pFactory))
    {
      pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_ATTRACTOR;

      if (!pAttractor->m_bAffectVelocity)
        pType->m_uiFeatureFlags |= GPU_PARTICLE_FEATURE_ATTRACTOR_ON_POSITION;

      pType->m_bAttractorAtEffectOrigin = pAttractor->m_Target == plParticleAttractorTarget::EffectOrigin;
      pType->m_vAttractorPos = pAttractor->m_vCustomPosition;
      pType->m_vAttractorAxis = pAttractor->m_vAxis.GetNormalized();
      pType->m_fAttractorForce = pAttractor->m_bRepel ? -pAttractor->m_fForce : pAttractor->m_fForce;
      pType->m_fAttractorMaxDist = pAttractor->m_fMaxDistance;
      pType->m_fAttractorMinDist = pAttractor->m_fMinDistance;
      pType->m_uiAttractorShape = (plUInt8)pAttractor->m_Shape.GetValue();
    }
    else if (const auto* pPullAlong = plDynamicCast<const plParticleBehaviorFactory_PullAlong*>(pFactory))
    {
      pType->m_fPullAlongStrength = pPullAlong->m_fStrength;
    }
    else if (const auto* pSceneForces = plDynamicCast<const plParticleBehaviorFactory_SceneForces*>(pFactory))
    {
      pType->m_bSceneForces = true;
      pType->m_fSceneForceInfluence = pSceneForces->m_fInfluence;
      pType->m_sSceneForceParameter = plTempHashedString(pSceneForces->m_sInfluenceParameter.GetData());
    }
    else if (const auto* pFlies = plDynamicCast<const plParticleBehaviorFactory_Flies*>(pFactory))
    {
      pType->m_bFlies = true;
      pType->m_fFliesSpeed = pFlies->m_fSpeed;
      pType->m_fFliesPathLength = pFlies->m_fPathLength;
      pType->m_fFliesMaxEmitterDistance = pFlies->m_fMaxEmitterDistance;
      pType->m_FliesMaxSteeringAngle = pFlies->m_MaxSteeringAngle;
    }
    else if (const auto* pSphere = plDynamicCast<const plParticleBehaviorFactory_BoundsSphere*>(pFactory))
    {
      pType->m_uiFeatureFlags |= (pSphere->m_OutOfBoundsMode == plParticleSphereOutOfBoundsMode::Kill) ? GPU_PARTICLE_FEATURE_SPHERE_KILL : GPU_PARTICLE_FEATURE_SPHERE_CONSTRAIN;
      pType->m_vSphereOffset = pSphere->m_vCenterOffset;
      pType->m_fSphereRadius = plMath::Max(pSphere->m_fRadius, 0.01f);
    }
    else if (const auto* pBounds = plDynamicCast<const plParticleBehaviorFactory_Bounds*>(pFactory))
    {
      pType->m_uiFeatureFlags |= (pBounds->m_OutOfBoundsMode == plParticleOutOfBoundsMode::Die) ? GPU_PARTICLE_FEATURE_BOUNDS_DIE : GPU_PARTICLE_FEATURE_BOUNDS_TELEPORT;
      pType->m_vBoundsOffset = pBounds->m_vPositionOffset;
      pType->m_vBoundsExtents = pBounds->m_vBoxExtents.CompMax(plVec3(0.01f));
    }
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Type_GPU_ParticleGPULowering);
