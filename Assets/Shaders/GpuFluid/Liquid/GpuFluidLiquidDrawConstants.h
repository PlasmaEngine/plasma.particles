#pragma once

// Push constants of the liquid surface draws (LiquidSurfaceBack.plShader, LiquidSurfaceShade.plShader) and the depth
// copy (LiquidDepthCopy.plShader). Shared between C++ and HLSL.

#include <Shaders/Common/ConstantBufferMacros.h>

BEGIN_PUSH_CONSTANTS(plGpuFluidLiquidDrawConstants)
{
  FLOAT4(GpuFluidDrawOrigin);     // xyz: world position of the domain's minimum corner, w: index of refraction
  FLOAT4(GpuFluidDrawAbsorption); // xyz: absorption coefficient sigma_a (1/m), w: refraction strength (m of offset per m of thickness)
  FLOAT4(GpuFluidDrawScatter);    // xyz: in-scatter colour (albedo of the water's own glow), w: roughness
  FLOAT4(GpuFluidDrawParams);     // x: largest thickness used (m), y: debug view (0 off), z: SSR steps, w: unused
}
END_PUSH_CONSTANTS(plGpuFluidLiquidDrawConstants)
