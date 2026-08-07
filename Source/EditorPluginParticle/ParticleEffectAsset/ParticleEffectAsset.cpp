#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/GUI/ExposedParameters.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <GuiFoundation/PropertyGrid/VisualizerManager.h>
#include <GuiFoundation/VisualGraph/Scene.moc.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ColorGradient.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SizeCurve.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Velocity.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_Continuous.h>
#include <ParticlePlugin/Events/ParticleEventReaction.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_CylinderPosition.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_RandomColor.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SpherePosition.h>
#include <ParticlePlugin/System/ParticleSystemDescriptor.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>
#include <ParticlePlugin/Type/Ribbon/ParticleTypeRibbon.h>
#include <ParticlePlugin/Type/Trail/ParticleTypeTrail.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Command/VisualGraphCommands.h>
#include <ToolsFoundation/Serialization/DocumentObjectConverter.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEffectAssetDocument, 8, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleEffectAssetDocument::plParticleEffectAssetDocument(plStringView sDocumentPath)
  : plAssetDocument(sDocumentPath, PL_DEFAULT_NEW(plParticleEffectNodeManager), plAssetDocEngineConnection::Simple)
  , m_LightSettings(false)
{
  plVisualizerManager::GetSingleton()->SetVisualizersActive(this, m_bRenderVisualizers);
}

plParticleEffectAssetDocument::~plParticleEffectAssetDocument() = default;

void plParticleEffectAssetDocument::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plParticleEffectNode>())
  {
    auto& props = *e.m_pPropertyStates;

    bool bShared = e.m_pObject->GetTypeAccessor().GetValue("AlwaysShared").ConvertTo<bool>();

    props["SimulateInLocalSpace"].m_Visibility = bShared ? plPropertyUiState::Disabled : plPropertyUiState::Default;
    props["ApplyOwnerVelocity"].m_Visibility = bShared ? plPropertyUiState::Disabled : plPropertyUiState::Default;
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plParticleTypeQuadFactory>())
  {
    auto& props = *e.m_pPropertyStates;

    bool useMaterial = e.m_pObject->GetTypeAccessor().GetValue("UseCustomMaterial").ConvertTo<bool>();
    plInt64 orientation = e.m_pObject->GetTypeAccessor().GetValue("Orientation").ConvertTo<plInt64>();
    plInt64 renderMode = e.m_pObject->GetTypeAccessor().GetValue("RenderMode").ConvertTo<plInt64>();
    plInt64 lightingMode = e.m_pObject->GetTypeAccessor().GetValue("LightingMode").ConvertTo<plInt64>();
    plInt64 textureAtlas = e.m_pObject->GetTypeAccessor().GetValue("TextureAtlas").ConvertTo<plInt64>();

    props["Deviation"].m_Visibility = plPropertyUiState::Invisible;
    props["DistortionTexture"].m_Visibility = plPropertyUiState::Invisible;
    props["DistortionStrength"].m_Visibility = plPropertyUiState::Invisible;
    props["ParticleStretch"].m_Visibility =
      (orientation == plQuadParticleOrientation::FixedAxis_EmitterDir || orientation == plQuadParticleOrientation::FixedAxis_ParticleDir)
        ? plPropertyUiState::Default
        : plPropertyUiState::Invisible;
    props["NumSpritesX"].m_Visibility = (textureAtlas == (int)plParticleTextureAtlasType::None) ? plPropertyUiState::Invisible : plPropertyUiState::Default;
    props["NumSpritesY"].m_Visibility = (textureAtlas == (int)plParticleTextureAtlasType::None) ? plPropertyUiState::Invisible : plPropertyUiState::Default;
    props["NormalCurvature"].m_Visibility = plPropertyUiState::Invisible;
    props["LightDirectionality"].m_Visibility = plPropertyUiState::Invisible;
    props["Texture"].m_Visibility = useMaterial ? plPropertyUiState::Invisible : plPropertyUiState::Default;
    props["CustomMaterial"].m_Visibility = useMaterial ? plPropertyUiState::Default : plPropertyUiState::Invisible;

    if (orientation == plQuadParticleOrientation::Fixed_EmitterDir || orientation == plQuadParticleOrientation::Fixed_WorldUp)
    {
      props["Deviation"].m_Visibility = plPropertyUiState::Default;
    }

    if (lightingMode == plParticleLightingMode::VertexLit)
    {
      props["NormalCurvature"].m_Visibility = plPropertyUiState::Default;
      props["LightDirectionality"].m_Visibility = plPropertyUiState::Default;
    }
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plParticleTypeTrailFactory>())
  {
    auto& props = *e.m_pPropertyStates;

    bool useMaterial = e.m_pObject->GetTypeAccessor().GetValue("UseCustomMaterial").ConvertTo<bool>();
    plInt64 renderMode = e.m_pObject->GetTypeAccessor().GetValue("RenderMode").ConvertTo<plInt64>();
    plInt64 lightingMode = e.m_pObject->GetTypeAccessor().GetValue("LightingMode").ConvertTo<plInt64>();
    plInt64 textureAtlas = e.m_pObject->GetTypeAccessor().GetValue("TextureAtlas").ConvertTo<plInt64>();

    props["DistortionTexture"].m_Visibility = plPropertyUiState::Invisible;
    props["DistortionStrength"].m_Visibility = plPropertyUiState::Invisible;
    props["NumSpritesX"].m_Visibility =
      (textureAtlas == (int)plParticleTextureAtlasType::None) ? plPropertyUiState::Invisible : plPropertyUiState::Default;
    props["NumSpritesY"].m_Visibility =
      (textureAtlas == (int)plParticleTextureAtlasType::None) ? plPropertyUiState::Invisible : plPropertyUiState::Default;
    props["NormalCurvature"].m_Visibility = plPropertyUiState::Invisible;
    props["LightDirectionality"].m_Visibility = plPropertyUiState::Invisible;
    props["Texture"].m_Visibility = useMaterial ? plPropertyUiState::Invisible : plPropertyUiState::Default;
    props["CustomMaterial"].m_Visibility = useMaterial ? plPropertyUiState::Default : plPropertyUiState::Invisible;

    if (lightingMode == plParticleLightingMode::VertexLit)
    {
      props["NormalCurvature"].m_Visibility = plPropertyUiState::Default;
      props["LightDirectionality"].m_Visibility = plPropertyUiState::Default;
    }
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plParticleTypeRibbonFactory>())
  {
    auto& props = *e.m_pPropertyStates;

    bool useMaterial = e.m_pObject->GetTypeAccessor().GetValue("UseCustomMaterial").ConvertTo<bool>();
    plInt64 lightingMode = e.m_pObject->GetTypeAccessor().GetValue("LightingMode").ConvertTo<plInt64>();
    plInt64 uvMode = e.m_pObject->GetTypeAccessor().GetValue("UvMode").ConvertTo<plInt64>();

    props["TileLength"].m_Visibility = (uvMode == plParticleRibbonUvMode::Tiled) ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["NormalCurvature"].m_Visibility = (lightingMode == plParticleLightingMode::VertexLit) ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["LightDirectionality"].m_Visibility = (lightingMode == plParticleLightingMode::VertexLit) ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["Texture"].m_Visibility = useMaterial ? plPropertyUiState::Invisible : plPropertyUiState::Default;
    props["CustomMaterial"].m_Visibility = useMaterial ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plParticleBehaviorFactory_ColorGradient>())
  {
    auto& props = *e.m_pPropertyStates;

    plInt64 gradientSource = e.m_pObject->GetTypeAccessor().GetValue("GradientSource").ConvertTo<plInt64>();
    plInt64 mode = e.m_pObject->GetTypeAccessor().GetValue("ColorGradientMode").ConvertTo<plInt64>();

    props["GradientMaxSpeed"].m_Visibility = (mode == plParticleColorGradientMode::Speed) ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plParticleInitializerFactory_CylinderPosition>())
  {
    auto& props = *e.m_pPropertyStates;

    bool bSetVelocity = e.m_pObject->GetTypeAccessor().GetValue("SetVelocity").ConvertTo<bool>();

    props["Speed"].m_Visibility = bSetVelocity ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plParticleInitializerFactory_SpherePosition>())
  {
    auto& props = *e.m_pPropertyStates;

    bool bSetVelocity = e.m_pObject->GetTypeAccessor().GetValue("SetVelocity").ConvertTo<bool>();

    props["Speed"].m_Visibility = bSetVelocity ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }
}

//////////////////////////////////////////////////////////////////////////

const plDocumentObject* plParticleEffectAssetDocument::FindEffectNode() const
{
  const auto* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());
  const plDocumentObject* pRoot = GetObjectManager()->GetRootObject();

  for (const plDocumentObject* pChild : pRoot->GetChildren())
  {
    if (pChild->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleEffectNode>())
      return pChild;
  }

  return nullptr;
}

void plParticleEffectAssetDocument::CollectConnectedNodes(const plDocumentObject* pNode, plStringView sPinName, plDynamicArray<const plDocumentObject*>& out_nodes) const
{
  out_nodes.Clear();

  const auto* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());

  const plVisualGraphPin* pPin = pManager->GetInputPinByName(pNode, sPinName);
  if (pPin == nullptr)
    return;

  auto connections = pManager->GetConnections(*pPin);
  for (const plVisualGraphConnection* pConn : connections)
  {
    const plVisualGraphPin& otherPin = (&pConn->GetSourcePin() == pPin) ? pConn->GetTargetPin() : pConn->GetSourcePin();
    out_nodes.PushBack(otherPin.GetParent());
  }
}

static void MirrorMemberProperties(const plDocumentObject* pDocObj, plReflectedClass* pNativeObj)
{
  const plRTTI* pType = pDocObj->GetTypeAccessor().GetType();

  plHybridArray<const plAbstractProperty*, 32> properties;
  pType->GetAllProperties(properties);

  for (auto pProp : properties)
  {
    if (pProp->GetCategory() != plPropertyCategory::Member)
      continue;

    if (pProp->GetFlags().IsSet(plPropertyFlags::ReadOnly))
      continue;

    plVariant value = pDocObj->GetTypeAccessor().GetValue(pProp->GetPropertyName());
    if (value.IsValid())
    {
      plReflectionUtils::SetMemberPropertyValue(static_cast<const plAbstractMemberProperty*>(pProp), pNativeObj, value);
    }
  }
}

static void CollectLeafFactories(
  const plDocumentObject* pNode,
  const plVisualGraphObjectManager* pManager,
  plDynamicArray<const plDocumentObject*>& out_factories,
  plSet<const plDocumentObject*>& ref_visited)
{
  if (ref_visited.Contains(pNode))
    return;
  ref_visited.Insert(pNode);

  const plRTTI* pType = pNode->GetTypeAccessor().GetType();

  if (pType->IsDerivedFrom<plParticleMixNode>())
  {
    const char* inputNames[] = {"Input 1", "Input 2", "Input 3", "Input 4"};
    for (int i = 0; i < 4; ++i)
    {
      const plVisualGraphPin* pPin = pManager->GetInputPinByName(pNode, inputNames[i]);
      if (pPin == nullptr)
        continue;

      auto connections = pManager->GetConnections(*pPin);
      for (const plVisualGraphConnection* pConn : connections)
      {
        const plVisualGraphPin& otherPin = (&pConn->GetSourcePin() == pPin) ? pConn->GetTargetPin() : pConn->GetSourcePin();
        CollectLeafFactories(otherPin.GetParent(), pManager, out_factories, ref_visited);
      }
    }
  }
  else
  {
    out_factories.PushBack(pNode);
  }
}

void plParticleEffectAssetDocument::BuildDescriptorFromGraph(plParticleEffectDescriptor& out_desc) const
{
  out_desc.ClearSystems();
  out_desc.ClearEventReactions();

  const auto* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());

  const plDocumentObject* pEffectNode = FindEffectNode();
  if (pEffectNode == nullptr)
    return;

  // Copy effect-level properties from the effect node
  {
    const auto& acc = pEffectNode->GetTypeAccessor();
    out_desc.m_InvisibleUpdateRate = (plEffectInvisibleUpdateRate::Enum)acc.GetValue("WhenInvisible").ConvertTo<plInt64>();
    out_desc.m_bAlwaysShared = acc.GetValue("AlwaysShared").ConvertTo<bool>();
    out_desc.m_bSimulateInLocalSpace = acc.GetValue("SimulateInLocalSpace").ConvertTo<bool>();
    out_desc.m_fApplyInstanceVelocity = acc.GetValue("ApplyOwnerVelocity").ConvertTo<float>();
    out_desc.m_PreSimulateDuration = acc.GetValue("PreSimulateDuration").ConvertTo<plTime>();
    out_desc.m_vNumWindSamples = acc.GetValue("NumWindSamples").Get<plVec3U32>();
    out_desc.m_fFadeOutStartDistance = acc.GetValue("FadeOutStartDistance").ConvertTo<float>();
    out_desc.m_fFadeOutEndDistance = acc.GetValue("FadeOutEndDistance").ConvertTo<float>();
    out_desc.m_Importance = (plParticleEffectImportance::Enum)acc.GetValue("Importance").ConvertTo<plInt64>();
    out_desc.m_fFixedTickHz = acc.GetValue("FixedTickHz").ConvertTo<float>();
    out_desc.m_uiMaxTicksPerFrame = acc.GetValue("MaxTicksPerFrame").ConvertTo<plUInt8>();

    // Copy map properties
    out_desc.m_FloatParameters.Clear();
    out_desc.m_ColorParameters.Clear();

    const plRTTI* pEffectNodeType = plGetStaticRTTI<plParticleEffectNode>();
    plHybridArray<const plAbstractProperty*, 32> allProps;
    pEffectNodeType->GetAllProperties(allProps);

    for (auto pProp : allProps)
    {
      if (pProp->GetCategory() == plPropertyCategory::Map)
      {
        auto* pMapProp = static_cast<const plAbstractMapProperty*>(pProp);

        if (plStringUtils::IsEqual(pProp->GetPropertyName(), "FloatParameters"))
        {
          plDynamicArray<plVariant> keys;
          acc.GetKeys(pProp->GetPropertyName(), keys);
          for (auto& key : keys)
          {
            plString sKey = key.ConvertTo<plString>();
            plVariant val = acc.GetValue(pProp->GetPropertyName(), key);
            out_desc.m_FloatParameters[sKey] = val.ConvertTo<float>();
          }
        }
        else if (plStringUtils::IsEqual(pProp->GetPropertyName(), "ColorParameters"))
        {
          plDynamicArray<plVariant> keys;
          acc.GetKeys(pProp->GetPropertyName(), keys);
          for (auto& key : keys)
          {
            plString sKey = key.ConvertTo<plString>();
            plVariant val = acc.GetValue(pProp->GetPropertyName(), key);
            out_desc.m_ColorParameters[sKey] = val.Get<plColor>();
          }
        }
      }
    }
  }

  // Collect system nodes connected to the effect node
  plDynamicArray<const plDocumentObject*> systemNodes;
  CollectConnectedNodes(pEffectNode, "Systems", systemNodes);

  for (const plDocumentObject* pSystemNode : systemNodes)
  {
    plParticleSystemDescriptor* pSysDesc = PL_DEFAULT_NEW(plParticleSystemDescriptor);
    out_desc.AddParticleSystem(pSysDesc);

    // Copy system-level properties
    const auto& sysAcc = pSystemNode->GetTypeAccessor();
    pSysDesc->m_bVisible = sysAcc.GetValue("Visible").ConvertTo<bool>();
    pSysDesc->m_LifeTime = sysAcc.GetValue("LifeTime").Get<plVarianceTypeTime>();
    pSysDesc->m_sOnDeathEvent = sysAcc.GetValue("OnDeathEvent").ConvertTo<plString>();
    pSysDesc->m_sLifeScaleParameter = sysAcc.GetValue("LifeScaleParam").ConvertTo<plString>();
    pSysDesc->m_SimulationTarget = (plParticleSimulationTarget::Enum)sysAcc.GetValue("SimulationTarget").ConvertTo<plInt32>();

    // Emitters (a system can mix several, e.g. an initial burst plus a continuous stream)
    {
      plDynamicArray<const plDocumentObject*> emitterNodes;
      CollectConnectedNodes(pSystemNode, "Emitter", emitterNodes);

      for (const plDocumentObject* pEmitterNode : emitterNodes)
      {
        const plRTTI* pRtti = pEmitterNode->GetTypeAccessor().GetType();
        plParticleEmitterFactory* pFactory = static_cast<plParticleEmitterFactory*>(pRtti->GetAllocator()->Allocate<void>().m_pInstance);
        MirrorMemberProperties(pEmitterNode, pFactory);

        // m_EmitterFactories is private, so insert via RTTI array property
        const plAbstractArrayProperty* pArrayProp = static_cast<const plAbstractArrayProperty*>(
          plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
        pArrayProp->Insert(pSysDesc, pArrayProp->GetCount(pSysDesc), &pFactory);
      }
    }

    // Initializers (expand through mix nodes)
    {
      plDynamicArray<const plDocumentObject*> directNodes;
      CollectConnectedNodes(pSystemNode, "Initializers", directNodes);

      plDynamicArray<const plDocumentObject*> leafNodes;
      plSet<const plDocumentObject*> visited;
      for (const plDocumentObject* pNode : directNodes)
        CollectLeafFactories(pNode, pManager, leafNodes, visited);

      for (const plDocumentObject* pInitNode : leafNodes)
      {
        const plRTTI* pRtti = pInitNode->GetTypeAccessor().GetType();
        plParticleInitializerFactory* pFactory = static_cast<plParticleInitializerFactory*>(pRtti->GetAllocator()->Allocate<void>().m_pInstance);
        MirrorMemberProperties(pInitNode, pFactory);
        pSysDesc->AddInitializerFactory(pFactory);
      }
    }

    // Behaviors (expand through mix nodes)
    {
      plDynamicArray<const plDocumentObject*> directNodes;
      CollectConnectedNodes(pSystemNode, "Behaviors", directNodes);

      plDynamicArray<const plDocumentObject*> leafNodes;
      plSet<const plDocumentObject*> visited;
      for (const plDocumentObject* pNode : directNodes)
        CollectLeafFactories(pNode, pManager, leafNodes, visited);

      for (const plDocumentObject* pBehaviorNode : leafNodes)
      {
        const plRTTI* pRtti = pBehaviorNode->GetTypeAccessor().GetType();
        plParticleBehaviorFactory* pFactory = static_cast<plParticleBehaviorFactory*>(pRtti->GetAllocator()->Allocate<void>().m_pInstance);
        MirrorMemberProperties(pBehaviorNode, pFactory);
        pSysDesc->AddBehaviorFactory(pFactory);
      }
    }

    // Renderers/Types (expand through mix nodes)
    {
      plDynamicArray<const plDocumentObject*> directNodes;
      CollectConnectedNodes(pSystemNode, "Renderers", directNodes);

      plDynamicArray<const plDocumentObject*> leafNodes;
      plSet<const plDocumentObject*> visited;
      for (const plDocumentObject* pNode : directNodes)
        CollectLeafFactories(pNode, pManager, leafNodes, visited);

      for (const plDocumentObject* pTypeNode : leafNodes)
      {
        const plRTTI* pRtti = pTypeNode->GetTypeAccessor().GetType();
        plParticleTypeFactory* pFactory = static_cast<plParticleTypeFactory*>(pRtti->GetAllocator()->Allocate<void>().m_pInstance);
        MirrorMemberProperties(pTypeNode, pFactory);
        pSysDesc->AddTypeFactory(pFactory);
      }
    }
  }

  // Event reactions connected to the effect node
  {
    plDynamicArray<const plDocumentObject*> reactionNodes;
    CollectConnectedNodes(pEffectNode, "EventReactions", reactionNodes);

    for (const plDocumentObject* pReactionNode : reactionNodes)
    {
      const plRTTI* pRtti = pReactionNode->GetTypeAccessor().GetType();
      plParticleEventReactionFactory* pFactory = static_cast<plParticleEventReactionFactory*>(pRtti->GetAllocator()->Allocate<void>().m_pInstance);
      MirrorMemberProperties(pReactionNode, pFactory);
      out_desc.AddEventReaction(pFactory);
    }
  }
}

void plParticleEffectAssetDocument::WriteResource(plStreamWriter& inout_stream) const
{
  plParticleEffectDescriptor desc;
  BuildDescriptorFromGraph(desc);
  desc.Save(inout_stream);
}

void plParticleEffectAssetDocument::InitializeAfterLoading(bool bFirstTimeCreation)
{
  SUPER::InitializeAfterLoading(bFirstTimeCreation);

  AddSyncObject(&m_LightSettings);

  bool bNeedDefaultGraph = bFirstTimeCreation;

  if (!bFirstTimeCreation && FindEffectNode() == nullptr)
  {
    // Old-format document loaded (from plSimpleAssetDocument era).
    // Remove legacy objects and create a fresh default graph.
    auto* pRoot = GetObjectManager()->GetRootObject();
    plHybridArray<const plDocumentObject*, 16> oldChildren;
    for (auto* pChild : pRoot->GetChildren())
      oldChildren.PushBack(pChild);

    if (!oldChildren.IsEmpty())
    {
      GetCommandHistory()->StartTransaction("Remove Legacy Objects");
      for (auto* pChild : oldChildren)
      {
        plRemoveObjectCommand cmd;
        cmd.m_Object = pChild->GetGuid();
        GetCommandHistory()->AddCommand(cmd).IgnoreResult();
      }
      GetCommandHistory()->FinishTransaction();
    }

    bNeedDefaultGraph = true;
  }

  if (bNeedDefaultGraph)
  {
    auto* pHistory = GetCommandHistory();
    auto* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());
    const plRTTI* pConnectionType = pManager->GetConnectionType();

    pHistory->StartTransaction("Init Particle Effect");

    // Create the effect node
    plUuid effectGuid = plUuid::MakeUuid();
    {
      plAddObjectCommand cmd;
      cmd.m_pType = plGetStaticRTTI<plParticleEffectNode>();
      cmd.m_NewObjectGuid = effectGuid;
      cmd.m_Index = -1;
      pHistory->AddCommand(cmd).IgnoreResult();
    }
    {
      plMoveNodeCommand cmd;
      cmd.m_Object = effectGuid;
      cmd.m_NewPos = plVec2(400, 0);
      pHistory->AddCommand(cmd).IgnoreResult();
    }

    // Create a default system node
    plUuid systemGuid = plUuid::MakeUuid();
    {
      plAddObjectCommand cmd;
      cmd.m_pType = plGetStaticRTTI<plParticleSystemNode>();
      cmd.m_NewObjectGuid = systemGuid;
      cmd.m_Index = -1;
      pHistory->AddCommand(cmd).IgnoreResult();
    }
    {
      plSetObjectPropertyCommand cmd;
      cmd.m_Object = systemGuid;
      cmd.m_sProperty = "Name";
      cmd.m_NewValue = "Default";
      pHistory->AddCommand(cmd).IgnoreResult();
    }
    {
      plMoveNodeCommand cmd;
      cmd.m_Object = systemGuid;
      cmd.m_NewPos = plVec2(0, 0);
      pHistory->AddCommand(cmd).IgnoreResult();
    }

    // Connect system -> effect
    {
      const plVisualGraphPin* pSourcePin = pManager->GetOutputPinByName(GetObjectManager()->GetObject(systemGuid), "System");
      const plVisualGraphPin* pTargetPin = pManager->GetInputPinByName(GetObjectManager()->GetObject(effectGuid), "Systems");
      if (pSourcePin && pTargetPin)
        plNodeCommands::AddAndConnectCommand(pHistory, pConnectionType, *pSourcePin, *pTargetPin).IgnoreResult();
    }

    // Create default emitter (Continuous)
    plUuid emitterGuid = plUuid::MakeUuid();
    {
      plAddObjectCommand cmd;
      cmd.m_pType = plGetStaticRTTI<plParticleEmitterFactory_Continuous>();
      cmd.m_NewObjectGuid = emitterGuid;
      cmd.m_Index = -1;
      pHistory->AddCommand(cmd).IgnoreResult();
    }
    {
      plMoveNodeCommand cmd;
      cmd.m_Object = emitterGuid;
      cmd.m_NewPos = plVec2(-400, -100);
      pHistory->AddCommand(cmd).IgnoreResult();
    }
    {
      const plVisualGraphPin* pSourcePin = pManager->GetOutputPinByName(GetObjectManager()->GetObject(emitterGuid), "Emitter");
      const plVisualGraphPin* pTargetPin = pManager->GetInputPinByName(GetObjectManager()->GetObject(systemGuid), "Emitter");
      if (pSourcePin && pTargetPin)
        plNodeCommands::AddAndConnectCommand(pHistory, pConnectionType, *pSourcePin, *pTargetPin).IgnoreResult();
    }

    // Create default Quad renderer
    plUuid quadGuid = plUuid::MakeUuid();
    {
      plAddObjectCommand cmd;
      cmd.m_pType = plGetStaticRTTI<plParticleTypeQuadFactory>();
      cmd.m_NewObjectGuid = quadGuid;
      cmd.m_Index = -1;
      pHistory->AddCommand(cmd).IgnoreResult();
    }
    {
      plMoveNodeCommand cmd;
      cmd.m_Object = quadGuid;
      cmd.m_NewPos = plVec2(-400, 200);
      pHistory->AddCommand(cmd).IgnoreResult();
    }
    {
      const plVisualGraphPin* pSourcePin = pManager->GetOutputPinByName(GetObjectManager()->GetObject(quadGuid), "Renderer");
      const plVisualGraphPin* pTargetPin = pManager->GetInputPinByName(GetObjectManager()->GetObject(systemGuid), "Renderers");
      if (pSourcePin && pTargetPin)
        plNodeCommands::AddAndConnectCommand(pHistory, pConnectionType, *pSourcePin, *pTargetPin).IgnoreResult();
    }

    pHistory->FinishTransaction();
  }
}

void plParticleEffectAssetDocument::TriggerRestartEffect()
{
  plParticleEffectAssetEvent e;
  e.m_pDocument = this;
  e.m_Type = plParticleEffectAssetEvent::RestartEffect;
  m_Events.Broadcast(e);
}

void plParticleEffectAssetDocument::SetAutoRestart(bool bEnable)
{
  if (m_bAutoRestart == bEnable)
    return;

  m_bAutoRestart = bEnable;

  plParticleEffectAssetEvent e;
  e.m_pDocument = this;
  e.m_Type = plParticleEffectAssetEvent::AutoRestartChanged;
  m_Events.Broadcast(e);
}

void plParticleEffectAssetDocument::SetSimulationPaused(bool bPaused)
{
  if (m_bSimulationPaused == bPaused)
    return;

  m_bSimulationPaused = bPaused;

  plParticleEffectAssetEvent e;
  e.m_pDocument = this;
  e.m_Type = plParticleEffectAssetEvent::SimulationSpeedChanged;
  m_Events.Broadcast(e);
}

void plParticleEffectAssetDocument::SetSimulationSpeed(float fSpeed)
{
  if (m_fSimulationSpeed == fSpeed)
    return;

  m_fSimulationSpeed = fSpeed;

  plParticleEffectAssetEvent e;
  e.m_pDocument = this;
  e.m_Type = plParticleEffectAssetEvent::SimulationSpeedChanged;
  m_Events.Broadcast(e);
}

void plParticleEffectAssetDocument::SetRenderVisualizers(bool b)
{
  if (m_bRenderVisualizers == b)
    return;

  m_bRenderVisualizers = b;

  plVisualizerManager::GetSingleton()->SetVisualizersActive(this, m_bRenderVisualizers);

  plParticleEffectAssetEvent e;
  e.m_pDocument = this;
  e.m_Type = plParticleEffectAssetEvent::RenderVisualizersChanged;
  m_Events.Broadcast(e);
}

plResult plParticleEffectAssetDocument::ComputeObjectTransformation(const plDocumentObject* pObject, plTransform& out_result) const
{
  out_result.SetIdentity();
  return PL_SUCCESS;
}

void plParticleEffectAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  // Build a temporary descriptor to extract dependency info
  plParticleEffectDescriptor desc;
  BuildDescriptorFromGraph(desc);

  for (const auto& system : desc.GetParticleSystems())
  {
    for (const auto& type : system->GetTypeFactories())
    {
      if (auto* pType = plDynamicCast<plParticleTypeQuadFactory*>(type))
      {
        if (pType->m_bUseCustomMaterial)
          pInfo->m_TransformDependencies.Remove(pType->m_sTexture);
        else
          pInfo->m_TransformDependencies.Remove(pType->m_sCustomMaterial);
      }

      if (auto* pType = plDynamicCast<plParticleTypeTrailFactory*>(type))
      {
        if (pType->m_bUseCustomMaterial)
          pInfo->m_TransformDependencies.Remove(pType->m_sTexture);
        else
          pInfo->m_TransformDependencies.Remove(pType->m_sCustomMaterial);
      }
    }
  }

  if (!desc.m_bAlwaysShared)
  {
    plExposedParameters* pExposedParams = PL_DEFAULT_NEW(plExposedParameters);
    for (auto it = desc.m_FloatParameters.GetIterator(); it.IsValid(); ++it)
    {
      plExposedParameter* param = PL_DEFAULT_NEW(plExposedParameter);
      pExposedParams->m_Parameters.PushBack(param);
      param->m_sName = it.Key();
      param->m_DefaultValue = it.Value();
    }
    for (auto it = desc.m_ColorParameters.GetIterator(); it.IsValid(); ++it)
    {
      plExposedParameter* param = PL_DEFAULT_NEW(plExposedParameter);
      pExposedParams->m_Parameters.PushBack(param);
      param->m_sName = it.Key();
      param->m_DefaultValue = it.Value();
    }

    pInfo->m_MetaInfo.PushBack(pExposedParams);
  }
}

plTransformStatus plParticleEffectAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag,
  const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  WriteResource(stream);
  return plStatus(PL_SUCCESS);
}

plTransformStatus plParticleEffectAssetDocument::InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo)
{
  plStatus status = plAssetDocument::RemoteCreateThumbnail(ThumbnailInfo);
  return status;
}

//////////////////////////////////////////////////////////////////////////
// Visual graph metadata

void plParticleEffectAssetDocument::InternalGetMetaDataHash(const plDocumentObject* pObject, plUInt64& inout_uiHash) const
{
  const plVisualGraphObjectManager* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());
  pManager->GetMetaDataHash(pObject, inout_uiHash);
}

void plParticleEffectAssetDocument::AttachMetaDataBeforeSaving(plAbstractObjectGraph& graph) const
{
  SUPER::AttachMetaDataBeforeSaving(graph);
  const plVisualGraphObjectManager* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());
  pManager->AttachMetaDataBeforeSaving(graph);
}

void plParticleEffectAssetDocument::RestoreMetaDataAfterLoading(const plAbstractObjectGraph& graph, bool bUndoable)
{
  SUPER::RestoreMetaDataAfterLoading(graph, bUndoable);
  plVisualGraphObjectManager* pManager = static_cast<plVisualGraphObjectManager*>(GetObjectManager());
  pManager->RestoreMetaDataAfterLoading(graph, bUndoable);
}

//////////////////////////////////////////////////////////////////////////
// Copy/Paste

void plParticleEffectAssetDocument::GetSupportedMimeTypesForPasting(plHybridArray<plString, 4>& out_MimeTypes) const
{
  out_MimeTypes.PushBack("application/plEditor.ParticleEffectGraph");
}

bool plParticleEffectAssetDocument::CopySelectedObjects(plAbstractObjectGraph& out_objectGraph, plStringBuilder& out_MimeType) const
{
  out_MimeType = "application/plEditor.ParticleEffectGraph";
  const plVisualGraphObjectManager* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());
  return pManager->CopySelectedObjects(out_objectGraph);
}

bool plParticleEffectAssetDocument::Paste(const plArrayPtr<PasteInfo>& info, const plAbstractObjectGraph& objectGraph, bool bAllowPickedPosition, plStringView sMimeType)
{
  plVisualGraphObjectManager* pManager = static_cast<plVisualGraphObjectManager*>(GetObjectManager());
  return pManager->PasteObjects(info, objectGraph, plQtVisualGraphScene::GetLastMouseInteractionPos(), bAllowPickedPosition);
}