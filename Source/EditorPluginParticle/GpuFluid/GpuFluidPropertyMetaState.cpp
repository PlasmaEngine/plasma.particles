#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/GpuFluid/GpuFluidPropertyMetaState.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <ToolsFoundation/Object/DocumentObjectBase.h>

namespace
{
  // Values of plGpuFluidColliderShape and plGpuFluidEmitterShape (ParticlePlugin/GpuFluid).
  enum ColliderShape
  {
    ColliderSphere,
    ColliderBox,
    ColliderCapsule,
    ColliderPlane
  };

  enum EmitterShape
  {
    EmitterNone,
    EmitterSphere,
    EmitterBox
  };

  plUInt32 GetEnum(const plDocumentObject* pObject, plStringView sProperty)
  {
    return static_cast<plUInt32>(pObject->GetTypeAccessor().GetValue(sProperty).ConvertTo<plInt64>());
  }

  plPropertyUiState::Visibility Shown(bool bShown)
  {
    return bShown ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }

  void EventHandler(plPropertyMetaStateEvent& e)
  {
    static const plRTTI* pCollider = plRTTI::FindTypeByName("plGpuFluidColliderComponent");
    static const plRTTI* pGasEmitter = plRTTI::FindTypeByName("plGpuFluidGasEmitterComponent");
    static const plRTTI* pLiquidSource = plRTTI::FindTypeByName("plGpuFluidLiquidSourceComponent");

    const plRTTI* pType = e.m_pObject->GetTypeAccessor().GetType();
    auto& props = *e.m_pPropertyStates;

    if (pType == pCollider && pCollider != nullptr)
    {
      // Each shape shows only the sizes it uses.
      const plUInt32 uiShape = GetEnum(e.m_pObject, "Shape");
      props["HalfExtents"].m_Visibility = Shown(uiShape == ColliderBox);
      props["Radius"].m_Visibility = Shown(uiShape == ColliderSphere || uiShape == ColliderCapsule);
      props["HalfHeight"].m_Visibility = Shown(uiShape == ColliderCapsule);

      const bool bStatic = e.m_pObject->GetTypeAccessor().GetValue("Static").ConvertTo<bool>();
      props["Static"].m_sNewLabelText = bStatic ? "Static (baked into liquid)" : "Static (off: gas only)";
    }
    else if (pType == pGasEmitter && pGasEmitter != nullptr)
    {
      const plUInt32 uiShape = GetEnum(e.m_pObject, "Shape");
      props["Radius"].m_Visibility = Shown(uiShape == EmitterSphere);
      props["HalfExtents"].m_Visibility = Shown(uiShape == EmitterBox);
    }
    else if (pType == pLiquidSource && pLiquidSource != nullptr)
    {
      // With no flow rate set, the label says what the disc pours: its area times the speed.
      const float fRadius = e.m_pObject->GetTypeAccessor().GetValue("Radius").ConvertTo<float>();
      const float fSpeed = e.m_pObject->GetTypeAccessor().GetValue("Speed").ConvertTo<float>();
      const float fFlowRate = e.m_pObject->GetTypeAccessor().GetValue("FlowRate").ConvertTo<float>();
      if (fFlowRate <= 0.0f)
      {
        const float fLitres = plMath::Pi<float>() * fRadius * fRadius * fSpeed * 1000.0f;
        plStringBuilder sLabel;
        // No colon: the property grid cuts a label at one.
        sLabel.SetFormat("Flow Rate (auto, {} l/s)", plArgF(fLitres, 1));
        props["FlowRate"].m_sNewLabelText = sLabel;
      }
    }
  }
} // namespace

void plGpuFluidPropertyMetaState::Register()
{
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(EventHandler);
}

void plGpuFluidPropertyMetaState::Unregister()
{
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(EventHandler);
}
