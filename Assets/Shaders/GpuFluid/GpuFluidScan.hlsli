#pragma once

// GpuFluidScan: a generic exclusive prefix sum over uint counts, and an indirect-argument writer that folds group
// counts above 65535 into a second dimension. Shared between C++ (the push constant layout) and HLSL.
//
// Layout of a scan of N counts (see plGpuFluidScan::ComputeLayout):
// - level 0 scans the input into the output in blocks of GPU_FLUID_SCAN_BLOCK and writes one sum per block into the
//   scratch buffer;
// - while a level has more than one block, the next level scans those sums in place in the scratch buffer;
// - the single block of the top level writes the grand total into its own sum slot;
// - the add pass then adds the scanned block sums back, top-down.
// Every sum is an integer, so the result does not depend on the order in which groups run.

#include <Shaders/Common/ConstantBufferMacros.h>

#define GPU_FLUID_SCAN_THREADS 256
#define GPU_FLUID_SCAN_PER_THREAD 4
#define GPU_FLUID_SCAN_BLOCK (GPU_FLUID_SCAN_THREADS * GPU_FLUID_SCAN_PER_THREAD)

// Dispatches have at most this many groups per dimension (D3D12, Vulkan minimum). Larger counts fold into y.
#define GPU_FLUID_SCAN_MAX_GROUPS_X 65535

// A shader has one push-constant block (one root-constant register on D3D12, one push-constant block on Vulkan), so
// shaders declare this one only when they are scan kernels.
#if !PL_ENABLED(PLATFORM_SHADER) || defined(GPU_FLUID_SCAN_KERNEL)

/// Push constants of the scan kernels. Offsets are in bytes.
BEGIN_PUSH_CONSTANTS(plGpuFluidScanConstants)
{
  UINT4(GpuFluidScanRange);   // x: element count of this level, y: input offset, z: output offset, w: block-sum offset
  UINT4(GpuFluidScanArgs);    // args writer: x count offset, y items per group, z args offset, w unused
}
END_PUSH_CONSTANTS(plGpuFluidScanConstants)

#endif

#if PL_ENABLED(PLATFORM_SHADER)

/// The linear group index of a dispatch whose arguments were written by the args writer: group (x, y) of a dispatch
/// that is argsX groups wide.
uint GpuFluidLinearGroup(uint3 groupId, uint argsX)
{
  return groupId.x + groupId.y * argsX;
}

#endif
