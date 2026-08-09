#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/GUI/ExposedParameters.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <Foundation/IO/FileSystem/DeferredFileWriter.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Serialization/DdlSerializer.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <GuiFoundation/UIServices/DynamicStringEnum.h>
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
#include <ParticlePlugin/Type/GPU/ParticleGPULowering.h>
#include <ParticlePlugin/Type/Quad/ParticleTypeQuad.h>
#include <ParticlePlugin/Type/Ribbon/ParticleTypeRibbon.h>
#include <ParticlePlugin/Type/Trail/ParticleTypeTrail.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Command/VisualGraphCommands.h>
#include <ToolsFoundation/Project/ToolsProject.h>
#include <ToolsFoundation/Serialization/DocumentObjectConverter.h>

// clang-format off
// v9: blocks moved from root-level graph nodes into the system's ordered context arrays, so the
// authored block order now reaches the descriptor. Forces a re-transform of existing effects.
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEffectAssetDocument, 9, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plParticleEffectAssetDocument::plParticleEffectAssetDocument(plStringView sDocumentPath)
  : plAssetDocument(sDocumentPath, PL_DEFAULT_NEW(plParticleEffectNodeManager), plAssetDocEngineConnection::Simple)
  , m_LightSettings(false)
{
  plVisualizerManager::GetSingleton()->SetVisualizersActive(this, m_bRenderVisualizers);

  // Safe here: construction is never inside a document broadcast, unlike node creation.
  GetObjectManager()->m_PropertyEvents.AddEventHandler(
    plMakeDelegate(&plParticleEffectAssetDocument::ParameterPropertyEventHandler, this));
}

plParticleEffectAssetDocument::~plParticleEffectAssetDocument()
{
  GetObjectManager()->m_PropertyEvents.RemoveEventHandler(
    plMakeDelegate(&plParticleEffectAssetDocument::ParameterPropertyEventHandler, this));
}

void plParticleEffectAssetDocument::UpdateParameterNameEnum() const
{
  plDynamicStringEnum& dynEnum = plDynamicStringEnum::GetDynamicEnum("ParticleParameterNamesEnum");
  dynEnum.Clear();

  const plDocumentObject* pEffectNode = FindEffectNode();
  if (pEffectNode == nullptr)
    return;

  const auto& accessor = pEffectNode->GetTypeAccessor();

  for (const char* szMap : {"FloatParameters", "ColorParameters"})
  {
    plDynamicArray<plVariant> keys;
    accessor.GetKeys(szMap, keys);

    for (const plVariant& key : keys)
    {
      const plString sName = key.ConvertTo<plString>();
      if (!sName.IsEmpty())
        dynEnum.AddValidValue(sName, true);
    }
  }
}

void plParticleEffectAssetDocument::ParameterPropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  if (e.m_pObject == nullptr || !e.m_pObject->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleEffectNode>())
    return;

  if (e.m_sProperty == "FloatParameters" || e.m_sProperty == "ColorParameters")
    UpdateParameterNameEnum();
}

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

/// Children of an owned-pointer array property, in array order.
static void GetArrayChildren(const plDocumentObject* pObject, plStringView sProperty, plDynamicArray<const plDocumentObject*>& out_children)
{
  out_children.Clear();

  const auto& acc = pObject->GetTypeAccessor();
  const plInt32 iCount = acc.GetCount(sProperty);

  for (plInt32 i = 0; i < iCount; ++i)
  {
    const plVariant value = acc.GetValue(sProperty, i);
    if (!value.IsA<plUuid>())
      continue;

    if (const plDocumentObject* pChild = pObject->GetDocumentObjectManager()->GetObject(value.Get<plUuid>()))
      out_children.PushBack(pChild);
  }
}

/// Blocks of one context, in authored order, minus the ones switched off. A disabled block stays
/// in the document but never reaches the runtime.
static void GetContextBlocks(const plDocumentObject* pSystemNode, plParticleContext::Enum context, plDynamicArray<const plDocumentObject*>& out_blocks)
{
  GetArrayChildren(pSystemNode, plParticleContextInfo::Get(context).m_szProperty, out_blocks);

  for (plUInt32 i = out_blocks.GetCount(); i > 0; --i)
  {
    const plVariant enabled = out_blocks[i - 1]->GetTypeAccessor().GetValue("Enabled");

    if (enabled.IsA<bool>() && !enabled.Get<bool>())
      out_blocks.RemoveAtAndCopy(i - 1);
  }
}

/// Values wired into block properties, folded once so the runtime never sees the graph.
///
/// Keyed by the property pin name, which already identifies both the block and the property.
using plParticleFoldedValues = plMap<plString, plVariant>;

/// The node feeding one input pin of an operator, or null when the pin is unwired.
static const plDocumentObject* GetInputSource(
  const plDocumentObject* pNode, plStringView sPinName, const plVisualGraphObjectManager* pManager)
{
  const plVisualGraphPin* pPin = pManager->GetInputPinByName(pNode, sPinName);
  if (pPin == nullptr)
    return nullptr;

  for (const plVisualGraphConnection* pConnection : pManager->GetConnections(*pPin))
    return pConnection->GetSourcePin().GetParent();

  return nullptr;
}

/// Evaluates an operator subgraph down to a single value.
///
/// This is the T0 tier: it runs once when the asset transforms, so nothing about the graph reaches
/// the runtime. The visited set makes a cycle fail to a default rather than recursing forever.
static plVariant EvaluateOperator(const plDocumentObject* pNode, const plVisualGraphObjectManager* pManager,
  const plDocumentObject* pEffectNode, plSet<const plDocumentObject*>& ref_visited)
{
  if (pNode == nullptr || ref_visited.Contains(pNode))
    return plVariant();

  ref_visited.Insert(pNode);
  PL_SCOPE_EXIT(ref_visited.Remove(pNode));

  const auto& accessor = pNode->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  // a stored constant: texture, gradient, curve, colour or plain number
  plStringView sValueProperty;
  const plParticleValueKind::Enum kind = plParticleValueKind::FromNode(pType, sValueProperty);

  if (kind == plParticleValueKind::None)
    return plVariant();

  if (!sValueProperty.IsEmpty())
    return accessor.GetValue(sValueProperty);

  // Resolves one input: the wired subgraph if there is one, otherwise the literal on the node.
  auto readInput = [&](const char* szPin, const char* szLiteral) -> plVariant
  {
    if (const plDocumentObject* pSource = GetInputSource(pNode, szPin, pManager))
    {
      const plVariant value = EvaluateOperator(pSource, pManager, pEffectNode, ref_visited);
      if (value.IsValid())
        return value;
    }

    return szLiteral != nullptr ? accessor.GetValue(szLiteral) : plVariant();
  };

  if (pType->IsDerivedFrom<plParticleToBoolNode>())
  {
    return plVariant(readInput("Value", "Value").ConvertTo<double>() != 0.0);
  }

  if (pType->IsDerivedFrom<plParticleBoolParameterNode>())
  {
    if (pEffectNode == nullptr)
      return plVariant();

    const plString sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      return plVariant();

    const plVariant value = pEffectNode->GetTypeAccessor().GetValue("FloatParameters", plVariant(sName));
    return value.IsValid() ? plVariant(value.ConvertTo<double>() != 0.0) : plVariant();
  }

  if (pType->IsDerivedFrom<plParticleColorParameterNode>())
  {
    if (pEffectNode == nullptr)
      return plVariant();

    const plString sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      return plVariant();

    return pEffectNode->GetTypeAccessor().GetValue("ColorParameters", plVariant(sName));
  }

  if (pType->IsDerivedFrom<plParticleParameterNode>())
  {
    // the folded value is the parameter's default, which lives on the effect node
    if (pEffectNode == nullptr)
      return plVariant();

    const plString sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      return plVariant();

    const plVariant value = pEffectNode->GetTypeAccessor().GetValue("FloatParameters", plVariant(sName));
    return value.IsValid() ? plVariant(value.ConvertTo<double>()) : plVariant();
  }

  if (pType->IsDerivedFrom<plParticleMathNode>())
  {
    const double a = readInput("A", "A").ConvertTo<double>();
    const double b = readInput("B", "B").ConvertTo<double>();

    switch (accessor.GetValue("Operation").ConvertTo<plInt64>())
    {
      case plParticleMathOp::Add:
        return plVariant(a + b);
      case plParticleMathOp::Subtract:
        return plVariant(a - b);
      case plParticleMathOp::Divide:
        return plVariant(b == 0.0 ? 0.0 : a / b);
      case plParticleMathOp::Min:
        return plVariant(plMath::Min(a, b));
      case plParticleMathOp::Max:
        return plVariant(plMath::Max(a, b));
      default:
        return plVariant(a * b);
    }
  }

  if (pType->IsDerivedFrom<plParticleCompareNode>())
  {
    const double a = readInput("A", "A").ConvertTo<double>();
    const double b = readInput("B", "B").ConvertTo<double>();

    switch (accessor.GetValue("Operation").ConvertTo<plInt64>())
    {
      case plParticleCompareOp::Less:
        return plVariant(a < b);
      case plParticleCompareOp::LessOrEqual:
        return plVariant(a <= b);
      case plParticleCompareOp::GreaterOrEqual:
        return plVariant(a >= b);
      case plParticleCompareOp::Equal:
        return plVariant(a == b);
      case plParticleCompareOp::NotEqual:
        return plVariant(a != b);
      default:
        return plVariant(a > b);
    }
  }

  if (pType->IsDerivedFrom<plParticleBranchNode>())
  {
    const plVariant condition = readInput("Condition", nullptr);
    const bool bTaken = condition.IsValid() && condition.ConvertTo<bool>();

    return bTaken ? readInput("True", "True") : readInput("False", "False");
  }

  return plVariant();
}

/// Walks every connection into a system's property pins and evaluates the graph behind it.
static void CollectFoldedValues(const plDocumentObject* pSystemNode, const plVisualGraphObjectManager* pManager,
  const plDocumentObject* pEffectNode, plParticleFoldedValues& out_values)
{
  out_values.Clear();

  for (const auto& pPin : pManager->GetInputPins(pSystemNode))
  {
    const auto& pin = static_cast<const plParticleGraphPin&>(*pPin);
    if (pin.m_Category != plParticleGraphPinCategory::Value)
      continue;

    for (const plVisualGraphConnection* pConnection : pManager->GetConnections(pin))
    {
      plSet<const plDocumentObject*> visited;
      const plVariant value = EvaluateOperator(pConnection->GetSourcePin().GetParent(), pManager, pEffectNode, visited);

      if (value.IsValid())
        out_values[pin.GetName()] = value;

      break; // a property takes a single value
    }
  }
}

/// Allocates the runtime factory matching a block object and copies its authored values across,
/// with any wired value overriding what the block itself stores.
template <typename FactoryBase>
static FactoryBase* AllocateBlockFactory(const plDocumentObject* pBlock, const plParticleFoldedValues* pFolded = nullptr)
{
  const plRTTI* pRtti = pBlock->GetTypeAccessor().GetType();
  auto* pFactory = static_cast<FactoryBase*>(pRtti->GetAllocator()->Allocate<void>().m_pInstance);
  MirrorMemberProperties(pBlock, pFactory);

  if (pFolded != nullptr && !pFolded->IsEmpty())
  {
    plHybridArray<const plAbstractProperty*, 32> properties;
    pRtti->GetAllProperties(properties);

    for (const plAbstractProperty* pProp : properties)
    {
      if (!plParticlePropertyPin::IsWireable(pProp))
        continue;

      auto it = pFolded->Find(plParticlePropertyPin::Make(pBlock->GetGuid(), pProp->GetPropertyName()));
      if (!it.IsValid())
        continue;

      const plVariant current = pBlock->GetTypeAccessor().GetValue(pProp->GetPropertyName());

      // Same-type values go straight in; a number still has to land as float, plTime, variance and
      // so on, which is what the conversion handles.
      plVariant folded = it.Value();
      if (folded.GetType() != current.GetType())
      {
        if (!folded.CanConvertTo(current.GetType()))
          continue;

        folded = folded.ConvertTo(current.GetType());
      }

      plReflectionUtils::SetMemberPropertyValue(static_cast<const plAbstractMemberProperty*>(pProp), pFactory, folded);
    }
  }

  return pFactory;
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

    // Blocks, taken from the system's four context arrays in authored order. Update-block order
    // is significant at runtime, which is exactly why the editor stores it explicitly.
    plDynamicArray<const plDocumentObject*> blocks;

    // Wired values are folded in here, so nothing about the graph reaches the runtime.
    plParticleFoldedValues folded;
    CollectFoldedValues(pSystemNode, pManager, pEffectNode, folded);

    // Spawn — a system can mix several emitters, e.g. an initial burst plus a continuous stream
    GetContextBlocks(pSystemNode, plParticleContext::Spawn, blocks);
    for (const plDocumentObject* pBlock : blocks)
    {
      auto* pFactory = AllocateBlockFactory<plParticleEmitterFactory>(pBlock, &folded);

      // m_EmitterFactories is private, so insert via RTTI array property
      const plAbstractArrayProperty* pArrayProp = static_cast<const plAbstractArrayProperty*>(
        plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
      pArrayProp->Insert(pSysDesc, pArrayProp->GetCount(pSysDesc), &pFactory);
    }

    GetContextBlocks(pSystemNode, plParticleContext::Initialize, blocks);
    for (const plDocumentObject* pBlock : blocks)
      pSysDesc->AddInitializerFactory(AllocateBlockFactory<plParticleInitializerFactory>(pBlock, &folded));

    GetContextBlocks(pSystemNode, plParticleContext::Update, blocks);
    for (const plDocumentObject* pBlock : blocks)
      pSysDesc->AddBehaviorFactory(AllocateBlockFactory<plParticleBehaviorFactory>(pBlock, &folded));

    GetContextBlocks(pSystemNode, plParticleContext::Output, blocks);
    for (const plDocumentObject* pBlock : blocks)
      pSysDesc->AddTypeFactory(AllocateBlockFactory<plParticleTypeFactory>(pBlock, &folded));
  }

  // Event reactions, owned by the effect node
  {
    plDynamicArray<const plDocumentObject*> reactionObjects;
    GetArrayChildren(pEffectNode, "EventReactions", reactionObjects);

    for (const plDocumentObject* pReaction : reactionObjects)
      out_desc.AddEventReaction(AllocateBlockFactory<plParticleEventReactionFactory>(pReaction));
  }
}

plParticleEffectAssetDocument::LoweringResult plParticleEffectAssetDocument::QueryLowering(const plDocumentObject* pSystemNode) const
{
  LoweringResult result;

  if (pSystemNode == nullptr)
    return result;

  // The blocker check reads a descriptor, so build a throwaway one for just this system. Only the
  // behaviors and the render type matter to it, which keeps this cheap enough to run per repaint.
  plParticleSystemDescriptor desc;

  plDynamicArray<const plDocumentObject*> behaviors;
  GetContextBlocks(pSystemNode, plParticleContext::Update, behaviors);
  for (const plDocumentObject* pBlock : behaviors)
    desc.AddBehaviorFactory(AllocateBlockFactory<plParticleBehaviorFactory>(pBlock));

  plDynamicArray<const plDocumentObject*> types;
  GetContextBlocks(pSystemNode, plParticleContext::Output, types);
  for (const plDocumentObject* pBlock : types)
    desc.AddTypeFactory(AllocateBlockFactory<plParticleTypeFactory>(pBlock));

  plParticleGPULowering::BlockerKind kind = plParticleGPULowering::BlockerKind::None;
  plUInt32 uiIndex = 0;
  const plStringView sReason = plParticleGPULowering::FindLoweringBlocker(&desc, &kind, &uiIndex);

  result.m_sReason = sReason;

  // GetContextBlocks skips disabled blocks, so the descriptor index lines up with these arrays.
  if (kind == plParticleGPULowering::BlockerKind::Behavior && uiIndex < behaviors.GetCount())
    result.m_pBlock = behaviors[uiIndex];
  else if (kind == plParticleGPULowering::BlockerKind::Type && uiIndex < types.GetCount())
    result.m_pBlock = types[uiIndex];

  return result;
}

void plParticleEffectAssetDocument::WriteResource(plStreamWriter& inout_stream) const
{
  plParticleEffectDescriptor desc;
  BuildDescriptorFromGraph(desc);
  desc.Save(inout_stream);
}

//////////////////////////////////////////////////////////////////////////
// System templates

static constexpr const char* s_szTemplateFolder = "Particles/Templates";
static constexpr const char* s_szTemplateExtension = "plParticleTemplate";

// static
void plParticleEffectAssetDocument::GetAvailableSystemTemplates(plDynamicArray<SystemTemplate>& out_templates)
{
  out_templates.Clear();

  const plString sProjectDir = plToolsProject::GetSingleton()->GetProjectDirectory();

  plDynamicArray<SystemTemplate> shipped;
  plDynamicArray<SystemTemplate> projectLocal;

  plStringBuilder sPath, sAbsPath, sFile;
  for (plUInt32 uiDir = 0; uiDir < plFileSystem::GetNumDataDirectories(); ++uiDir)
  {
    sPath = plFileSystem::GetDataDirectory(uiDir)->GetDataDirectoryPath();

    if (sPath.IsEmpty() || plFileSystem::ResolveSpecialDirectory(sPath, sAbsPath).Failed())
      continue;

    sAbsPath.AppendPath(s_szTemplateFolder);

    plFileSystemIterator it;
    for (it.StartSearch(sAbsPath, plFileSystemIteratorFlags::ReportFiles); it.IsValid(); it.Next())
    {
      if (!plPathUtils::HasExtension(it.GetStats().m_sName, s_szTemplateExtension))
        continue;

      it.GetStats().GetFullPath(sFile);

      const bool bProjectLocal = sFile.StartsWith_NoCase(sProjectDir);

      SystemTemplate& tmpl = (bProjectLocal ? projectLocal : shipped).ExpandAndGetRef();
      tmpl.m_sFilePath = sFile;
      tmpl.m_sName = plPathUtils::GetFileName(sFile);
      tmpl.m_bProjectLocal = bProjectLocal;
    }
  }

  auto byName = [](const SystemTemplate& lhs, const SystemTemplate& rhs)
  { return lhs.m_sName.Compare_NoCase(rhs.m_sName) < 0; };

  shipped.Sort(byName);
  projectLocal.Sort(byName);

  out_templates.PushBackRange(shipped);
  out_templates.PushBackRange(projectLocal);
}

plStatus plParticleEffectAssetDocument::SaveSystemAsTemplate(const plDocumentObject* pSystemNode, plStringView sName) const
{
  if (pSystemNode == nullptr || !pSystemNode->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
    return plStatus("Only a particle system can be saved as a template.");

  plStringBuilder sFileName;
  plPathUtils::MakeValidFilename(sName, '_', sFileName);
  sFileName.Trim(" ");

  if (sFileName.IsEmpty())
    return plStatus("The template needs a name.");

  plStringBuilder sPath = plToolsProject::GetSingleton()->GetProjectDirectory();
  sPath.AppendPath(s_szTemplateFolder);

  if (plOSFile::CreateDirectoryStructure(sPath).Failed())
    return plStatus(plFmt("Could not create the template folder '{}'.", sPath));

  sPath.AppendPath(sFileName);
  sPath.Append(".", s_szTemplateExtension);

  // The template is the same serialization the clipboard uses: the system node named "root",
  // its blocks as child objects, plus the node-position meta data.
  plAbstractObjectGraph graph;
  plDocumentObjectConverterWriter writer(&graph, GetObjectManager());
  writer.AddObjectToGraph(pSystemNode, "root");
  AttachMetaDataBeforeSaving(graph);

  plDeferredFileWriter file;
  file.SetOutput(sPath);
  plAbstractGraphDdlSerializer::Write(file, &graph, nullptr, false);

  if (file.Close().Failed())
    return plStatus(plFmt("Failed to write template '{}'.", sPath));

  return plStatus(PL_SUCCESS);
}

plStatus plParticleEffectAssetDocument::AddSystemFromTemplate(plStringView sAbsFilePath)
{
  plStringBuilder sGraphText;
  {
    plFileReader file;
    if (file.Open(sAbsFilePath).Failed())
      return plStatus(plFmt("Could not open template '{}'.", sAbsFilePath));

    sGraphText.ReadAll(file);
  }

  const plDocumentObject* pEffectNode = FindEffectNode();
  if (pEffectNode == nullptr)
    return plStatus("The document has no effect node to connect the system to.");

  auto* pManager = static_cast<plParticleEffectNodeManager*>(GetObjectManager());

  // Place the new system to the right of the current systems, on the row of the top-most one.
  plVec2 vPos = plVec2::MakeZero();
  bool bFoundSystem = false;
  for (const plDocumentObject* pChild : pManager->GetRootObject()->GetChildren())
  {
    if (!pChild->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
      continue;

    const plVec2 v = pManager->GetNodePos(pChild);

    if (!bFoundSystem)
    {
      vPos = v;
      bFoundSystem = true;
    }

    vPos.x = plMath::Max(vPos.x, v.x + 500.0f);
    vPos.y = plMath::Min(vPos.y, v.y);
  }

  plCommandHistory* pHistory = GetCommandHistory();
  pHistory->StartTransaction("Add System from Template");

  {
    plPasteObjectsCommand cmd;
    cmd.m_sMimeType = "application/plEditor.ParticleEffectGraph";
    cmd.m_sGraphTextFormat = sGraphText;
    cmd.m_bAllowPickedPosition = false;

    if (pHistory->AddCommand(cmd).Failed())
    {
      pHistory->CancelTransaction();
      return plStatus(plFmt("Failed to instantiate template '{}'.", sAbsFilePath));
    }
  }

  // The paste leaves what it created as the selection; that is how the new nodes are found.
  plHybridArray<const plDocumentObject*, 4> newSystems;
  for (const plDocumentObject* pObject : GetSelectionManager()->GetSelection())
  {
    if (pObject->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
      newSystems.PushBack(pObject);
  }

  if (newSystems.IsEmpty())
  {
    pHistory->CancelTransaction();
    return plStatus(plFmt("'{}' does not contain a particle system.", sAbsFilePath));
  }

  const plRTTI* pConnectionType = pManager->GetConnectionType();

  for (const plDocumentObject* pSystem : newSystems)
  {
    {
      plMoveNodeCommand cmd;
      cmd.m_Object = pSystem->GetGuid();
      cmd.m_NewPos = vPos;
      pHistory->AddCommand(cmd).LogFailure();
      vPos.x += 500.0f;
    }

    const plVisualGraphPin* pSourcePin = pManager->GetOutputPinByName(pSystem, "System");
    const plVisualGraphPin* pTargetPin = pManager->GetInputPinByName(pEffectNode, "Systems");

    if (pSourcePin != nullptr && pTargetPin != nullptr && pManager->GetConnections(*pSourcePin).IsEmpty())
    {
      plNodeCommands::AddAndConnectCommand(pHistory, pConnectionType, *pSourcePin, *pTargetPin).LogFailure();
    }
  }

  pHistory->FinishTransaction();

  // Property pins depend on the pasted blocks; same reason InitializeAfterLoading rebuilds them.
  pManager->RecreateAllSystemPins();

  return plStatus(PL_SUCCESS);
}

static bool IsGraphNode(const plDocumentObject* pObject)
{
  const plRTTI* pType = pObject->GetTypeAccessor().GetType();
  return pType->IsDerivedFrom<plParticleSystemNode>() || pType->IsDerivedFrom<plParticleEffectNode>();
}

bool plParticleEffectAssetDocument::MigrateBlocksIntoContexts()
{
  auto* pManager = static_cast<plParticleEffectNodeManager*>(GetObjectManager());
  const plDocumentObject* pRoot = pManager->GetRootObject();

  // Before the context rework a block was a root-level node wired into a system by a pin.
  plHybridArray<const plDocumentObject*, 32> connections;
  plHybridArray<const plDocumentObject*, 8> mixNodes;
  plHybridArray<const plDocumentObject*, 8> reactions;
  bool bIsLegacy = false;

  for (const plDocumentObject* pChild : pRoot->GetChildren())
  {
    if (pManager->IsConnection(pChild))
    {
      connections.PushBack(pChild);
      continue;
    }

    const plRTTI* pType = pChild->GetTypeAccessor().GetType();

    if (pType->IsDerivedFrom<plParticleMixNode>())
    {
      mixNodes.PushBack(pChild);
      bIsLegacy = true;
    }
    else if (pType->IsDerivedFrom<plParticleEventReactionFactory>())
    {
      reactions.PushBack(pChild);
      bIsLegacy = true;
    }
    else if (plParticleContextInfo::FromFactoryType(pType) != plParticleContext::Count)
    {
      bIsLegacy = true;
    }
  }

  if (!bIsLegacy)
    return false;

  // Work out where everything goes before touching the graph: the moves below invalidate the
  // connections this answer is derived from.
  struct BlockMove
  {
    plUuid m_Block;
    plUuid m_Parent;
    plString m_sProperty;
    plInt32 m_iIndex = 0;
  };

  plHybridArray<BlockMove, 32> moves;
  plSet<const plDocumentObject*> claimed;

  // pin names as they were authored, in context order
  const char* szLegacyPin[plParticleContext::Count] = {"Emitter", "Initializers", "Behaviors", "Renderers"};

  for (const plDocumentObject* pSystemNode : pRoot->GetChildren())
  {
    if (!pSystemNode->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
      continue;

    for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
    {
      plDynamicArray<const plDocumentObject*> directNodes;
      CollectConnectedNodes(pSystemNode, szLegacyPin[uiContext], directNodes);

      plDynamicArray<const plDocumentObject*> leafNodes;
      plSet<const plDocumentObject*> visited;
      for (const plDocumentObject* pNode : directNodes)
        CollectLeafFactories(pNode, pManager, leafNodes, visited);

      plInt32 iIndex = 0;
      for (const plDocumentObject* pLeaf : leafNodes)
      {
        // one object cannot live in two systems; the first system to claim it wins
        if (claimed.Contains(pLeaf))
        {
          plLog::Warning("Particle migration: block '{0}' fed more than one system and was kept only in the first.",
            pLeaf->GetTypeAccessor().GetType()->GetTypeName());
          continue;
        }
        claimed.Insert(pLeaf);

        BlockMove& move = moves.ExpandAndGetRef();
        move.m_Block = pLeaf->GetGuid();
        move.m_Parent = pSystemNode->GetGuid();
        move.m_sProperty = plParticleContextInfo::Get((plParticleContext::Enum)uiContext).m_szProperty;
        move.m_iIndex = iIndex++;
      }
    }
  }

  // Event reactions all belong to the single effect node, so no connection walk is needed.
  if (const plDocumentObject* pEffectNode = FindEffectNode())
  {
    plInt32 iIndex = 0;
    for (const plDocumentObject* pReaction : reactions)
    {
      BlockMove& move = moves.ExpandAndGetRef();
      move.m_Block = pReaction->GetGuid();
      move.m_Parent = pEffectNode->GetGuid();
      move.m_sProperty = "EventReactions";
      move.m_iIndex = iIndex++;
    }
  }

  auto* pHistory = GetCommandHistory();
  pHistory->StartTransaction("Migrate Blocks Into Contexts");

  // Drop every connection that touched a block. System-to-effect wires are the graph and stay.
  for (const plDocumentObject* pConnection : connections)
  {
    const plVisualGraphConnection* pInfo = pManager->GetConnectionIfExists(pConnection);
    if (pInfo != nullptr && IsGraphNode(pInfo->GetSourcePin().GetParent()) && IsGraphNode(pInfo->GetTargetPin().GetParent()))
      continue;

    plRemoveObjectCommand cmd;
    cmd.m_Object = pConnection->GetGuid();
    pHistory->AddCommand(cmd).IgnoreResult();
  }

  for (const BlockMove& move : moves)
  {
    plMoveObjectCommand cmd;
    cmd.m_Object = move.m_Block;
    cmd.m_NewParent = move.m_Parent;
    cmd.m_sParentProperty = move.m_sProperty;
    cmd.m_Index = move.m_iIndex;

    if (pHistory->AddCommand(cmd).Failed())
    {
      plLog::Error("Particle migration: could not move a block into '{0}'.", move.m_sProperty);
    }
  }

  // Mix nodes are gone: a context merges its own blocks.
  for (const plDocumentObject* pMixNode : mixNodes)
  {
    plRemoveObjectCommand cmd;
    cmd.m_Object = pMixNode->GetGuid();
    pHistory->AddCommand(cmd).IgnoreResult();
  }

  pHistory->FinishTransaction();

  plLog::Info("Particle effect '{0}': migrated {1} blocks into system contexts.", GetDocumentPath(), moves.GetCount());
  return true;
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

  if (!bNeedDefaultGraph)
  {
    MigrateBlocksIntoContexts();
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

    // A new effect starts with the minimum that produces visible particles: something that
    // spawns and something that draws. Both go straight into the system's contexts.
    {
      plAddObjectCommand cmd;
      cmd.m_pType = plGetStaticRTTI<plParticleEmitterFactory_Continuous>();
      cmd.m_Parent = systemGuid;
      cmd.m_sParentProperty = plParticleContextInfo::Get(plParticleContext::Spawn).m_szProperty;
      cmd.m_Index = 0;
      pHistory->AddCommand(cmd).IgnoreResult();
    }
    {
      plAddObjectCommand cmd;
      cmd.m_pType = plGetStaticRTTI<plParticleTypeQuadFactory>();
      cmd.m_Parent = systemGuid;
      cmd.m_sParentProperty = plParticleContextInfo::Get(plParticleContext::Output).m_szProperty;
      cmd.m_Index = 0;
      pHistory->AddCommand(cmd).IgnoreResult();
    }

    pHistory->FinishTransaction();
  }

  // A system's pins are created when the node itself is added, which on load happens before any of
  // its blocks exist — so the property pins have to be built once the whole graph is in place.
  // Without this a freshly opened document shows no property pins until a context is edited.
  static_cast<plParticleEffectNodeManager*>(GetObjectManager())->RecreateAllSystemPins();

  UpdateParameterNameEnum();
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

  plParticleEffectNodeManager* pManager = static_cast<plParticleEffectNodeManager*>(GetObjectManager());

  // Connections are resolved by pin name, and a system's property pins only exist once its blocks
  // do — which is not yet true when the node itself was created. Without this every wire into a
  // block fails to resolve here and is silently dropped, so nothing survives a save/load.
  pManager->RecreateAllSystemPins();

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