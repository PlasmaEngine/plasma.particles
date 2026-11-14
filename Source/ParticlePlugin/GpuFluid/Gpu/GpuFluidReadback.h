#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <ParticlePlugin/ParticlePluginDLL.h>
#include <RendererFoundation/Resources/ReadbackHelper.h>

class plGALCommandEncoder;

/// \brief GPU to CPU copies of a small buffer, retired by fence, at most three in flight.
///
/// Nothing ever waits: a frame whose three slots are all still in flight simply takes no readback, and the CPU side
/// sees results two to three frames late. Render thread only, except Submit, which runs inside a transfer pass of the
/// frame that reserved the slot.
class PL_PARTICLEPLUGIN_DLL plGpuFluidReadbackRing
{
public:
  static constexpr plUInt32 s_uiSlotCount = 3;

  plGpuFluidReadbackRing();
  ~plGpuFluidReadbackRing();

  /// At record time: a free slot for this frame's copy, or plInvalidIndex when all are in flight.
  plUInt32 ReserveSlot(plUInt64 uiFrame);

  /// Inside the transfer pass: copies hBuffer into the reserved slot.
  void Submit(plUInt32 uiSlot, plGALCommandEncoder& ref_encoder, plGALBufferHandle hBuffer);

  /// Retires every slot whose fence has passed. When at least one did, the bytes of the newest go into out_data and
  /// out_uiFrame says which frame they were recorded in.
  bool Poll(plDynamicArray<plUInt8>& out_data, plUInt64& out_uiFrame);

  /// Frees the readback buffers. Render thread, once the GPU is done with them.
  void Reset();

private:
  enum State : plInt32
  {
    Free,
    Reserved,
    InFlight,
  };

  struct Slot
  {
    plGALReadbackBufferHelper m_Helper;
    plAtomicInteger32 m_State; // State, starts at Free (0)
    plUInt64 m_uiFrame = 0;
  };

  Slot m_Slots[s_uiSlotCount];
};
