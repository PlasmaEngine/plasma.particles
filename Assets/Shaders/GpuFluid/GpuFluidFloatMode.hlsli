#pragma once

// The float mode of every GpuFluid compute kernel. Vulkan lets a shader that declares no SignedZeroInfNanPreserve mode
// treat NaN and infinity as undefined, and the NaN guards (GpuFluidIsNonFinite, GpuFluidKeepNonFinite) then lose the
// values they exist to count: on an RTX 4080 an injected NaN was never counted while an injected infinity was. DX12
// and Metal keep them already. Put GPU_FLUID_KERNEL_CAPS before the entry point and GPU_FLUID_KERNEL_MODES first in it.
#if defined(VULKAN)
#  define GPU_FLUID_KERNEL_CAPS [[vk::ext_capability(4466)]] /* SignedZeroInfNanPreserve */
#  define GPU_FLUID_KERNEL_MODES vk::ext_execution_mode(4461, 32); /* SignedZeroInfNanPreserve, 32-bit floats */
#else
#  define GPU_FLUID_KERNEL_CAPS
#  define GPU_FLUID_KERNEL_MODES
#endif
