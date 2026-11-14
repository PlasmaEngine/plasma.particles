#pragma once

// Push constants of the liquid debug point renderer (LiquidDebugPoints.plShader). Shared between C++ and HLSL.

#include <Shaders/Common/ConstantBufferMacros.h>

BEGIN_PUSH_CONSTANTS(plGpuFluidLiquidDebugConstants)
{
  FLOAT4(GpuFluidDebugParams); // x: point radius (m), y: speed that maps to the top of the colour ramp (m/s), z: 1 colour by sleep instead of speed, w: unused
}
END_PUSH_CONSTANTS(plGpuFluidLiquidDebugConstants)
