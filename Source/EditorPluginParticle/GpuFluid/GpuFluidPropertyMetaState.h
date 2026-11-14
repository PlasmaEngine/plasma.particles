#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>

/// \brief The property panel of the GpuFluid components: each collider and gas emitter shape shows only the sizes it
/// uses, a liquid source's flow rate label says what it pours when left at 0.
class plGpuFluidPropertyMetaState
{
public:
  static void Register();
  static void Unregister();
};
