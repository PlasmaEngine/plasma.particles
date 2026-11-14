#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/Time/Time.h>
#include <ParticlePlugin/GpuFluid/Gpu/GpuFluidReadback.h>
#include <RendererFoundation/Device/ReadbackLock.h>

namespace
{
  /// A slot reserved at record time whose pass never ran (a graph dropped on a lost device, say) is given back after
  /// this many frames instead of blocking the ring forever.
  constexpr plUInt64 s_uiStaleReservationFrames = 8;
} // namespace

plGpuFluidReadbackRing::plGpuFluidReadbackRing() = default;

plGpuFluidReadbackRing::~plGpuFluidReadbackRing()
{
  Reset();
}

plUInt32 plGpuFluidReadbackRing::ReserveSlot(plUInt64 uiFrame)
{
  for (plUInt32 i = 0; i < s_uiSlotCount; ++i)
  {
    Slot& slot = m_Slots[i];

    if (slot.m_State == State::Reserved && slot.m_uiFrame + s_uiStaleReservationFrames < uiFrame)
      slot.m_State = State::Free;

    if (slot.m_State == State::Free)
    {
      slot.m_uiFrame = uiFrame;
      slot.m_State = State::Reserved;
      return i;
    }
  }

  return plInvalidIndex;
}

void plGpuFluidReadbackRing::Submit(plUInt32 uiSlot, plGALCommandEncoder& ref_encoder, plGALBufferHandle hBuffer)
{
  if (uiSlot >= s_uiSlotCount)
    return;

  Slot& slot = m_Slots[uiSlot];
  if (slot.m_State != State::Reserved)
    return;

  // A zero fence means the readback buffer could not be created; the slot is free again for the next frame.
  slot.m_State = slot.m_Helper.ReadbackBuffer(ref_encoder, hBuffer) != 0 ? State::InFlight : State::Free;
}

bool plGpuFluidReadbackRing::Poll(plDynamicArray<plUInt8>& out_data, plUInt64& out_uiFrame)
{
  bool bAny = false;
  plUInt64 uiNewest = 0;

  for (Slot& slot : m_Slots)
  {
    if (slot.m_State != State::InFlight || slot.m_Helper.GetReadbackResult(plTime::MakeZero()) != plGALAsyncResult::Ready)
      continue;

    plArrayPtr<const plUInt8> memory;
    {
      plReadbackBufferLock lock = slot.m_Helper.LockBuffer(memory);
      if (lock && (!bAny || slot.m_uiFrame > uiNewest))
      {
        out_data = memory;
        uiNewest = slot.m_uiFrame;
        bAny = true;
      }
    }

    slot.m_State = State::Free;
  }

  out_uiFrame = uiNewest;
  return bAny;
}

void plGpuFluidReadbackRing::Reset()
{
  for (Slot& slot : m_Slots)
  {
    slot.m_Helper.Reset();
    slot.m_State = State::Free;
  }
}

PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_GpuFluid_Gpu_GpuFluidReadback);
