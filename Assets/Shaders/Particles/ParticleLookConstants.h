#pragma once

// Emissive and depth-blending settings of one particle type. Expects ConstantBufferMacros.h to be included
// already (ParticleSystemConstants.h does), so the same file serves C++ and HLSL without relative includes.

#define PARTICLE_LOOK_FLAG_EMISSIVE_MAP 1
#define PARTICLE_LOOK_FLAG_PER_PARTICLE_EMISSIVE 2
#define PARTICLE_LOOK_FLAG_DISTORTION_TEXTURE 4

CONSTANT_BUFFER2(plParticleLookConstants, 3, BG_DRAW_CALL)
{
  COLOR4F(EmissiveColor);    // linear colour times intensity
  FLOAT1(SoftEdgeDistance);  // metres over which a particle fades into geometry, 0 = off
  FLOAT1(CameraFadeDistance); // metres in front of the camera over which it fades, 0 = off
  UINT1(LookFlags);          // PARTICLE_LOOK_FLAG_*
  FLOAT1(LookPadding0);
};

// The base data's Variation keeps the variation in bits 0-7. Bit 8 marks an Emissive multiplier stored as
// a half in bits 16-31; without it the multiplier is 1, which is what the GPU simulation writes.
#define PARTICLE_VARIATION_EMISSIVE_BIT 0x100u
