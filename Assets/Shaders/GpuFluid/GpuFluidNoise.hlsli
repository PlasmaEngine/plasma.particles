#pragma once

// Deterministic noise for GpuFluid sources: integer hashing, so the same position and time give the same value on
// every backend and every run.

uint GpuFluidHash(uint3 v)
{
  // PCG3D (Jarzynski and Olano, "Hash Functions for GPU Rendering").
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  return v.x ^ v.y ^ v.z;
}

/// Lattice value in [-1, 1].
float GpuFluidLatticeValue(int3 cell, uint seed)
{
  return float(GpuFluidHash(uint3(cell) + seed * uint3(0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u)) & 0xFFFFu) * (2.0 / 65535.0) - 1.0;
}

/// Value noise in [-1, 1] with its analytic gradient (xyz = gradient, w = value), quintic interpolation.
float4 GpuFluidValueNoise(float3 p, uint seed)
{
  const float3 cellFloor = floor(p);
  const int3 c = int3(cellFloor);
  const float3 f = p - cellFloor;

  const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  const float3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);

  const float a = GpuFluidLatticeValue(c + int3(0, 0, 0), seed);
  const float b = GpuFluidLatticeValue(c + int3(1, 0, 0), seed);
  const float cc = GpuFluidLatticeValue(c + int3(0, 1, 0), seed);
  const float d = GpuFluidLatticeValue(c + int3(1, 1, 0), seed);
  const float e = GpuFluidLatticeValue(c + int3(0, 0, 1), seed);
  const float g = GpuFluidLatticeValue(c + int3(1, 0, 1), seed);
  const float h = GpuFluidLatticeValue(c + int3(0, 1, 1), seed);
  const float k = GpuFluidLatticeValue(c + int3(1, 1, 1), seed);

  const float k0 = a;
  const float k1 = b - a;
  const float k2 = cc - a;
  const float k3 = e - a;
  const float k4 = a - b - cc + d;
  const float k5 = a - cc - e + h;
  const float k6 = a - b - e + g;
  const float k7 = -a + b + cc - d + e - g - h + k;

  const float value = k0 + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x + k7 * u.x * u.y * u.z;
  const float3 gradient = du * float3(
    k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
    k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
    k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);

  return float4(gradient, value);
}

/// Divergence-free noise: the curl of a vector potential made of three value noises. Components roughly in [-2, 2].
float3 GpuFluidCurlNoise(float3 p)
{
  const float3 g0 = GpuFluidValueNoise(p, 1u).xyz;
  const float3 g1 = GpuFluidValueNoise(p + float3(31.4, -17.2, 7.9), 2u).xyz;
  const float3 g2 = GpuFluidValueNoise(p + float3(-11.7, 23.1, -41.3), 3u).xyz;
  return float3(g2.y - g1.z, g0.z - g2.x, g1.x - g0.y);
}
