#pragma once

#include <Core/World/World.h>
#include <ParticlePlugin/GpuFluid/GpuFluidDeclarations.h>

struct plMsgSetPlaying;

using plGpuFluidGasEmitterComponentManager = plComponentManagerSimple<class plGpuFluidGasEmitterComponent, plComponentUpdateType::Always>;

/// \brief Emits gas (density and heat) into every gas volume whose box holds the object's position.
///
/// A sphere of Radius or a box of HalfExtents (in the gas volume's axes; the emitter's rotation turns only its
/// velocity), with a smooth edge of Softness. Velocity is in the object's space and drives the gas inside the shape.
/// plMsgSetPlaying starts and stops it. While the world is not simulating (editing in the editor), an emitter outside
/// every gas volume draws itself red.
class PL_PARTICLEPLUGIN_DLL plGpuFluidGasEmitterComponent final : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGpuFluidGasEmitterComponent, plComponent, plGpuFluidGasEmitterComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  //////////////////////////////////////////////////////////////////////////
  // plGpuFluidGasEmitterComponent

public:
  plGpuFluidGasEmitterComponent();
  ~plGpuFluidGasEmitterComponent();

  plEnum<plGpuFluidEmitterShape> m_Shape;                     // [ property ] Sphere or Box (None: off)
  float m_fRadius = 0.5f;                                     // [ property ] metres (sphere)
  plVec3 m_vHalfExtents = plVec3(0.4f, 0.4f, 0.2f);           // [ property ] metres (box)
  float m_fSoftness = 0.2f;                                   // [ property ] metres
  float m_fRate = 5.0f;                                       // [ property ] density per second
  float m_fTemperature = 1.0f;                                // [ property ]
  plVec3 m_vVelocity = plVec3(0.0f, 0.0f, 2.0f);              // [ property ] m/s, object space
  float m_fNoise = 0.9f;                                      // [ property ]
  float m_fNoiseScale = 0.3f;                                 // [ property ] metres
  bool m_bEmitting = true;                                    // [ property ]

protected:
  void Update();
  void OnMsgSetPlaying(plMsgSetPlaying& ref_msg);
};
