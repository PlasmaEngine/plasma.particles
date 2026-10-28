#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/GUI/ExposedParameters.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphCompiler.h>
#include <Foundation/IO/FileSystem/DeferredFileWriter.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Serialization/DdlSerializer.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <GuiFoundation/UIServices/DynamicStringEnum.h>
#include <GuiFoundation/PropertyGrid/VisualizerManager.h>
#include <GuiFoundation/EngineUi/EngineGraphView.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_ColorGradient.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_KillIf.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Raycast.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_OnEvent.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SetAttribute.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_SizeCurve.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Velocity.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_Continuous.h>
#include <ParticlePlugin/Events/ParticleEventReaction.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_CylinderPosition.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_RandomColor.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_RandomSize.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SpherePosition.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_VelocityCone.h>
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
// v11: plasma.particles 1.6 events (On Event per event, inherit, Event inputs, Blackboard nodes).
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plParticleEffectAssetDocument, 11, plRTTINoAllocator)
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

namespace
{
  /// Calls fn for every object of the document below pObject: nodes, and the blocks inside systems.
  template <typename FN>
  void VisitObjects(const plDocumentObject* pObject, FN fn)
  {
    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      fn(pChild);
      VisitObjects(pChild, fn);
    }
  }

  void AddEventName(plDynamicArray<plString>& ref_events, const plVariant& name)
  {
    const plString sName = name.ConvertTo<plString>();
    if (!sName.IsEmpty() && !ref_events.Contains(sName))
      ref_events.PushBack(sName);
  }
} // namespace

void plParticleEffectAssetDocument::GetRaisedEvents(plDynamicArray<plString>& out_events) const
{
  out_events.Clear();

  VisitObjects(GetObjectManager()->GetRootObject(), [&](const plDocumentObject* pObject)
    {
      // the Events blocks; a disabled one raises nothing
      if (pObject->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleEventRaiserBlock>() && pObject->GetTypeAccessor().GetValue("Enabled").ConvertTo<bool>())
        AddEventName(out_events, pObject->GetTypeAccessor().GetValue("EventName"));
    });
}

void plParticleEffectAssetDocument::UpdateEventNameEnum() const
{
  // what the effect raises comes first, then names only something listens to
  plDynamicArray<plString> events;
  GetRaisedEvents(events);

  VisitObjects(GetObjectManager()->GetRootObject(), [&](const plDocumentObject* pObject)
    {
      if (pObject->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleEmitterFactory_OnEvent>())
        AddEventName(events, pObject->GetTypeAccessor().GetValue("EventName"));
    });

  plDynamicStringEnum& dynEnum = plDynamicStringEnum::GetDynamicEnum("ParticleEventNamesEnum");
  dynEnum.Clear();

  for (const plString& sEvent : events)
    dynEnum.AddValidValue(sEvent, true);
}

void plParticleEffectAssetDocument::ParameterPropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  if (e.m_pObject == nullptr || !e.m_pObject->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleEffectNode>())
    return;

  if (e.m_sProperty == "FloatParameters" || e.m_sProperty == "ColorParameters")
    UpdateParameterNameEnum();
}

/// The emissive and depth-blending properties of the quad, trail and ribbon types. Custom materials and distortion bring
/// no emissive map; the colour, intensity and parameter only matter once a map is set.
static void SetLookPropertyStates(plPropertyMetaStateEvent& e, bool bNoEmissive)
{
  auto& props = *e.m_pPropertyStates;

  const bool bHasMap = !e.m_pObject->GetTypeAccessor().GetValue("EmissiveMap").ConvertTo<plString>().IsEmpty();

  props["EmissiveMap"].m_Visibility = bNoEmissive ? plPropertyUiState::Invisible : plPropertyUiState::Default;
  for (const char* szProperty : {"EmissiveColor", "EmissiveIntensity", "EmissiveColorParam"})
    props[szProperty].m_Visibility = (bNoEmissive || !bHasMap) ? plPropertyUiState::Invisible : plPropertyUiState::Default;
}

void plParticleEffectAssetDocument::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  // The parameter-name dropdown is one enum for all open effects. Refilling it from the document of
  // the node being shown keeps it from listing another effect's parameters.
  const plRTTI* pShownType = e.m_pObject->GetTypeAccessor().GetType();
  if (pShownType->IsDerivedFrom<plParticleParameterNode>() || pShownType->IsDerivedFrom<plParticleColorParameterNode>() || pShownType->IsDerivedFrom<plParticleBoolParameterNode>())
  {
    if (const auto* pDocument = plDynamicCast<const plParticleEffectAssetDocument*>(e.m_pObject->GetDocumentObjectManager()->GetDocument()))
      pDocument->UpdateParameterNameEnum();
  }

  // the event-name dropdown is shared the same way; it lists the events of the shown object's effect
  if (pShownType->IsDerivedFrom<plParticleSystemNode>() || pShownType->IsDerivedFrom<plParticleEmitterFactory_OnEvent>() || pShownType->IsDerivedFrom<plParticleBehaviorFactory_Raycast>())
  {
    if (const auto* pDocument = plDynamicCast<const plParticleEffectAssetDocument*>(e.m_pObject->GetDocumentObjectManager()->GetDocument()))
      pDocument->UpdateEventNameEnum();
  }

  // Set Attribute shows the one value its target takes, and the custom attribute only for that target
  if (pShownType->IsDerivedFrom<plParticleInitializerFactory_SetAttribute>() || pShownType->IsDerivedFrom<plParticleBehaviorFactory_Attribute>())
  {
    auto& props = *e.m_pPropertyStates;
    const auto& accessor = e.m_pObject->GetTypeAccessor();

    plParticleAttributeSinkSettings settings;
    settings.m_Target = (plParticleSinkTarget::Enum)accessor.GetValue("Target").ConvertTo<plInt64>();
    settings.m_Component = (plParticleSinkComponent::Enum)accessor.GetValue("Component").ConvertTo<plInt64>();
    settings.m_CustomType = (plParticleCustomAttributeType::Enum)accessor.GetValue("CustomType").ConvertTo<plInt64>();

    const plStringView sValue = settings.GetValuePropertyName();
    for (const char* szValue : {"Number", "Vector", "Color"})
      props[szValue].m_Visibility = (sValue == szValue) ? plPropertyUiState::Default : plPropertyUiState::Invisible;

    // a number target has nothing to pick a component of
    props["Component"].m_Visibility = settings.GetTargetComponents() > 1 ? plPropertyUiState::Default : plPropertyUiState::Invisible;

    const bool bCustom = settings.m_Target == plParticleSinkTarget::Custom;
    props["CustomAttribute"].m_Visibility = bCustom ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["CustomType"].m_Visibility = bCustom ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }

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

    const bool bDistortion = renderMode == plParticleTypeRenderMode::Distortion;

    props["Deviation"].m_Visibility = plPropertyUiState::Invisible;
    props["DistortionTexture"].m_Visibility = (bDistortion && !useMaterial) ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["DistortionStrength"].m_Visibility = bDistortion ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    SetLookPropertyStates(e, bDistortion || useMaterial);
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

    const bool bDistortion = renderMode == plParticleTypeRenderMode::Distortion;

    props["DistortionTexture"].m_Visibility = (bDistortion && !useMaterial) ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["DistortionStrength"].m_Visibility = bDistortion ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    SetLookPropertyStates(e, bDistortion || useMaterial);
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
    SetLookPropertyStates(e, useMaterial);
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plParticleInitializerFactory_RandomSize>())
  {
    auto& props = *e.m_pPropertyStates;

    const bool bPerAxis = e.m_pObject->GetTypeAccessor().GetValue("Mode").ConvertTo<plInt64>() == plParticleSizeMode::PerAxis;

    props["Aspect"].m_Visibility = bPerAxis ? plPropertyUiState::Invisible : plPropertyUiState::Default;
    props["AxisScale"].m_Visibility = bPerAxis ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["SameRandomForAllAxes"].m_Visibility = bPerAxis ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plParticleBehaviorFactory_SizeCurve>())
  {
    auto& props = *e.m_pPropertyStates;

    const bool bPerAxis = e.m_pObject->GetTypeAccessor().GetValue("Mode").ConvertTo<plInt64>() == plParticleSizeMode::PerAxis;

    props["SizeCurveY"].m_Visibility = bPerAxis ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["SizeCurveZ"].m_Visibility = bPerAxis ? plPropertyUiState::Default : plPropertyUiState::Invisible;
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

/// A wire whose value changes while the effect runs: an output of the system's per-frame program,
/// or of the per-particle program of the block it drives.
struct plParticleFrameWire
{
  plString m_sPin;
  const plDocumentObject* m_pBlock = nullptr;
  plString m_sProperty;
  const plDocumentObject* m_pSource = nullptr;
  plParticleValueKind::Enum m_TargetKind = plParticleValueKind::None;
  plParticleTier m_Tier = plParticleTier::Frame;
};

/// Walks every connection into a system's property pins. Every wire is folded: a constant one to its
/// value, a per-frame one to its start value, so the blocks start right even before the program runs.
/// The per-frame and per-particle ones are also listed, for the programs.
static void CollectWiredValues(const plDocumentObject* pSystemNode, const plVisualGraphObjectManager* pManager,
  plParticleGraphCompiler& ref_compiler, plParticleFoldedValues& out_values, plDynamicArray<plParticleFrameWire>& out_frameWires, bool bReportProblems = true)
{
  out_values.Clear();
  out_frameWires.Clear();

  for (const auto& pPin : pManager->GetInputPins(pSystemNode))
  {
    const auto& pin = static_cast<const plParticleGraphPin&>(*pPin);
    if (pin.m_Category != plParticleGraphPinCategory::Value)
      continue;

    for (const plVisualGraphConnection* pConnection : pManager->GetConnections(pin))
    {
      const plDocumentObject* pSource = pConnection->GetSourcePin().GetParent();

      const plVariant value = ref_compiler.Fold(pSource);
      if (value.IsValid())
      {
        out_values[pin.GetName()] = value;
      }
      else if (bReportProblems)
      {
        const plString sIssue = ref_compiler.GetIssue(pSource);
        plLog::Warning("A wire into '{}' has no value and is ignored. {}", pin.GetName(), sIssue);
      }

      if (ref_compiler.GetTier(pSource) != plParticleTier::Constant)
      {
        plStringBuilder sProperty;
        if (const plDocumentObject* pBlock = plParticlePropertyPin::FindBlock(pSystemNode, pin.GetName(), sProperty))
        {
          if (const plAbstractProperty* pProp = pBlock->GetTypeAccessor().GetType()->FindPropertyByName(sProperty))
          {
            plParticleFrameWire& wire = out_frameWires.ExpandAndGetRef();
            wire.m_sPin = pin.GetName();
            wire.m_pBlock = pBlock;
            wire.m_sProperty = sProperty;
            wire.m_pSource = pSource;
            wire.m_TargetKind = plParticleValueKind::FromProperty(pProp);
            wire.m_Tier = ref_compiler.GetTier(pSource);
          }
        }
      }

      break; // a property takes a single value
    }
  }
}

/// Turns a folded value into what a property of the given current value takes. Numbers, vectors and
/// colours go through the same conversion the runtime uses for driven values, so a wire lands the
/// same way whether it is folded or evaluated per frame.
static plVariant ConvertWiredValue(const plVariant& value, const plVariant& current)
{
  plVec4 v(0.0f);
  bool bNumeric = true;

  if (value.IsA<plVec3>())
    v = value.Get<plVec3>().GetAsVec4(0.0f);
  else if (value.IsA<plColor>())
  {
    const plColor c = value.Get<plColor>();
    v.Set(c.r, c.g, c.b, c.a);
  }
  else if (value.IsA<bool>())
    v.x = value.Get<bool>() ? 1.0f : 0.0f;
  else if (value.IsNumber())
    v.x = value.ConvertTo<float>();
  else
    bNumeric = false;

  if (bNumeric)
  {
    const plVariant converted = plParticleMakeDrivenValue(current, v);
    if (converted.IsValid())
      return converted;
  }

  // assets and texts: same type, or whatever the variant conversion gives
  if (value.GetType() == current.GetType())
    return value;

  return value.CanConvertTo(current.GetType()) ? value.ConvertTo(current.GetType()) : plVariant();
}

/// The per-particle program a block can carry, or null for blocks that take no per-particle wires.
static plParticleProgramData* GetSinkProgram(plReflectedClass* pFactory, const plParticleAttributeSinkSettings*& out_pSettings, bool& out_bInitialize)
{
  out_pSettings = nullptr;
  out_bInitialize = false;

  if (auto* pInitializer = plDynamicCast<plParticleInitializerFactory_SetAttribute*>(pFactory))
  {
    out_pSettings = &pInitializer->m_Settings;
    out_bInitialize = true;
    return &pInitializer->m_Settings.m_Program;
  }

  if (auto* pBehavior = plDynamicCast<plParticleBehaviorFactory_Attribute*>(pFactory))
  {
    out_pSettings = &pBehavior->m_Settings;
    return &pBehavior->m_Settings.m_Program;
  }

  if (auto* pKill = plDynamicCast<plParticleBehaviorFactory_KillIf*>(pFactory))
    return &pKill->m_Program;

  // the blocks with properties that read their value per particle
  if (auto* pBehavior = plDynamicCast<plParticleBehaviorFactory*>(pFactory))
    return pBehavior->GetParticleProgram();

  if (auto* pInitializer = plDynamicCast<plParticleInitializerFactory*>(pFactory))
  {
    out_bInitialize = true;
    return pInitializer->GetParticleProgram();
  }

  return nullptr;
}

/// True when a per-particle wire into this property of the block can be evaluated.
static bool TakesParticleWire(const plReflectedClass* pFactory, const plParticleAttributeSinkSettings* pSettings, plStringView sProperty)
{
  // a Set Attribute only reads the value its target takes
  if (pSettings != nullptr)
    return sProperty == pSettings->GetValuePropertyName();

  if (plDynamicCast<const plParticleBehaviorFactory_KillIf*>(pFactory) != nullptr)
    return sProperty == "Condition";

  const plAbstractProperty* pProp = pFactory->GetDynamicRTTI()->FindPropertyByName(sProperty);
  return pProp != nullptr && pProp->GetAttributeByType<plParticlePerParticleAttribute>() != nullptr;
}

/// Compiles the per-particle wires of one system into the programs of the blocks they drive: one
/// program per block, run at the block's place, so the authored order holds.
static void BuildParticlePrograms(const plVisualGraphObjectManager* pManager, const plDocumentObject* pEffectNode,
  const plDynamicArray<plParticleFrameWire>& wires, const plHashTable<plUuid, plReflectedClass*>& sinkFactories, double fSystemLifetime)
{
  plMap<plUuid, plHybridArray<const plParticleFrameWire*, 4>> wiresPerBlock;
  for (const plParticleFrameWire& wire : wires)
  {
    if (wire.m_Tier == plParticleTier::Particle)
      wiresPerBlock[wire.m_pBlock->GetGuid()].PushBack(&wire);
  }

  for (auto it = wiresPerBlock.GetIterator(); it.IsValid(); ++it)
  {
    plReflectedClass* const* ppFactory = sinkFactories.GetValue(it.Key());
    if (ppFactory == nullptr) // disabled, or not in Initialize or Update
    {
      for (const plParticleFrameWire* pWire : it.Value())
        plLog::Warning("A per-particle wire into '{}' is ignored: only Initialize and Update blocks take per-particle values.", pWire->m_sProperty);
      continue;
    }

    const plParticleAttributeSinkSettings* pSettings = nullptr;
    bool bInitialize = false;
    plParticleProgramData* pProgram = GetSinkProgram(*ppFactory, pSettings, bInitialize);

    if (pProgram == nullptr)
    {
      for (const plParticleFrameWire* pWire : it.Value())
        plLog::Warning("A per-particle wire into '{}' is ignored: this block takes no per-particle values.", pWire->m_sProperty);
      continue;
    }

    plParticleGraphCompiler compiler(pManager, pEffectNode);
    compiler.SetParticleContext(bInitialize, fSystemLifetime);

    pProgram->m_Outputs.Clear();

    for (const plParticleFrameWire* pWire : it.Value())
    {
      if (!TakesParticleWire(*ppFactory, pSettings, pWire->m_sProperty))
      {
        plLog::Warning("A per-particle wire into '{}' is ignored: that property takes constant and per-frame values.", pWire->m_sProperty);
        continue;
      }

      const plUInt32 uiOutput = compiler.AddOutput(pWire->m_pSource, pWire->m_TargetKind);
      if (uiOutput == plInvalidIndex)
      {
        plLog::Warning("The per-particle wire into '{}' cannot be evaluated; the block uses its own value.", pWire->m_sProperty);
        continue;
      }

      // outputs are numbered in the order they were added, which is this array's order
      PL_ASSERT_DEV(uiOutput == pProgram->m_Outputs.GetCount(), "Program outputs out of step");
      pProgram->m_Outputs.PushBack(pWire->m_sProperty);
    }

    if (!pProgram->m_Outputs.IsEmpty() && compiler.Compile(pProgram->m_ByteCode).Failed())
    {
      plLog::Error("A per-particle graph does not compile; its block uses its own values.");
      pProgram->m_Outputs.Clear();
      pProgram->m_ByteCode.Clear();
    }

    pProgram->m_Assets = compiler.GetAssets();
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

      auto it = pFolded->Find(plParticlePropertyPin::Make(pBlock, pProp->GetPropertyName()));
      if (!it.IsValid())
        continue;

      const plVariant current = pBlock->GetTypeAccessor().GetValue(pProp->GetPropertyName());

      // a number still has to land as float, plTime, variance and so on
      const plVariant folded = ConvertWiredValue(it.Value(), current);
      if (!folded.IsValid())
        continue;

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
    pSysDesc->m_sLifeScaleParameter = sysAcc.GetValue("LifeScaleParam").ConvertTo<plString>();
    pSysDesc->m_SimulationTarget = (plParticleSimulationTarget::Enum)sysAcc.GetValue("SimulationTarget").ConvertTo<plInt32>();

    // Blocks, taken from the system's four context arrays in authored order. Update-block order
    // is significant at runtime, which is exactly why the editor stores it explicitly.
    plDynamicArray<const plDocumentObject*> blocks;

    // Every wire is folded in here. The ones that change while the effect runs also become outputs
    // of the system's per-frame program further down.
    plParticleGraphCompiler compiler(pManager, pEffectNode);
    plParticleFoldedValues folded;
    plDynamicArray<plParticleFrameWire> frameWires;
    CollectWiredValues(pSystemNode, pManager, compiler, folded, frameWires);

    // where each block lands in the descriptor, for the program's driven properties
    plHashTable<plUuid, plParticleDrivenProperty> blockSlots;
    auto recordSlot = [&](const plDocumentObject* pBlock, plParticleDrivenProperty::Module module, plUInt32 uiIndex)
    {
      plParticleDrivenProperty& slot = blockSlots[pBlock->GetGuid()];
      slot.m_Module = module;
      slot.m_uiModuleIndex = (plUInt16)uiIndex;
    };

    // Spawn — a system can mix several emitters, e.g. an initial burst plus a continuous stream
    GetContextBlocks(pSystemNode, plParticleContext::Spawn, blocks);
    for (plUInt32 i = 0; i < blocks.GetCount(); ++i)
    {
      auto* pFactory = AllocateBlockFactory<plParticleEmitterFactory>(blocks[i], &folded);

      // m_EmitterFactories is private, so insert via RTTI array property
      const plAbstractArrayProperty* pArrayProp = static_cast<const plAbstractArrayProperty*>(
        plGetStaticRTTI<plParticleSystemDescriptor>()->FindPropertyByName("Emitters"));
      pArrayProp->Insert(pSysDesc, pArrayProp->GetCount(pSysDesc), &pFactory);
      recordSlot(blocks[i], plParticleDrivenProperty::Module::Emitter, i);
    }

    // the blocks that can take per-particle wires, for their programs further down
    plHashTable<plUuid, plReflectedClass*> sinkFactories;

    GetContextBlocks(pSystemNode, plParticleContext::Initialize, blocks);
    for (plUInt32 i = 0; i < blocks.GetCount(); ++i)
    {
      plParticleInitializerFactory* pFactory = AllocateBlockFactory<plParticleInitializerFactory>(blocks[i], &folded);
      pSysDesc->AddInitializerFactory(pFactory);
      recordSlot(blocks[i], plParticleDrivenProperty::Module::Initializer, i);
      sinkFactories[blocks[i]->GetGuid()] = pFactory;
    }

    GetContextBlocks(pSystemNode, plParticleContext::Update, blocks);
    for (plUInt32 i = 0; i < blocks.GetCount(); ++i)
    {
      plParticleBehaviorFactory* pFactory = AllocateBlockFactory<plParticleBehaviorFactory>(blocks[i], &folded);
      pSysDesc->AddBehaviorFactory(pFactory);
      recordSlot(blocks[i], plParticleDrivenProperty::Module::Behavior, i);
      sinkFactories[blocks[i]->GetGuid()] = pFactory;
    }

    GetContextBlocks(pSystemNode, plParticleContext::Output, blocks);
    for (plUInt32 i = 0; i < blocks.GetCount(); ++i)
    {
      pSysDesc->AddTypeFactory(AllocateBlockFactory<plParticleTypeFactory>(blocks[i], &folded));
      recordSlot(blocks[i], plParticleDrivenProperty::Module::Type, i);
    }

    BuildEventRaisers(pSystemNode, *pSysDesc);

    // the per-frame program: one output per wire that changes once per step
    plDynamicArray<plParticleDrivenProperty> driven;
    for (const plParticleFrameWire& wire : frameWires)
    {
      if (wire.m_Tier != plParticleTier::Frame)
        continue;

      const plParticleDrivenProperty* pSlot = blockSlots.GetValue(wire.m_pBlock->GetGuid());
      if (pSlot == nullptr) // a disabled block never reaches the runtime
        continue;

      const plUInt32 uiOutput = compiler.AddOutput(wire.m_pSource, wire.m_TargetKind);
      if (uiOutput == plInvalidIndex)
      {
        plLog::Warning("The wire into '{}' cannot be evaluated per frame; it keeps its start value.", wire.m_sProperty);
        continue;
      }

      plParticleDrivenProperty& property = driven.ExpandAndGetRef();
      property = *pSlot;
      property.m_uiOutput = (plUInt16)uiOutput;
      property.m_sProperty = wire.m_sProperty;
    }

    if (!driven.IsEmpty())
    {
      if (compiler.Compile(pSysDesc->m_FrameProgram.m_ByteCode).Succeeded())
      {
        pSysDesc->m_FrameProgram.m_Driven = driven;
        pSysDesc->m_FrameProgram.m_Assets = compiler.GetAssets();
      }
      else
      {
        plLog::Error("The per-frame graph of system '{}' does not compile; its wires keep their start values.", sysAcc.GetValue("Name").ConvertTo<plString>());
        pSysDesc->m_FrameProgram.m_ByteCode.Clear();
      }
    }

    BuildParticlePrograms(pManager, pEffectNode, frameWires, sinkFactories, pSysDesc->m_LifeTime.m_Value.GetSeconds());
  }

  // Event reactions, owned by the effect node
  {
    plDynamicArray<const plDocumentObject*> reactionObjects;
    GetArrayChildren(pEffectNode, "EventReactions", reactionObjects);

    for (const plDocumentObject* pReaction : reactionObjects)
      out_desc.AddEventReaction(AllocateBlockFactory<plParticleEventReactionFactory>(pReaction));
  }
}

void plParticleEffectAssetDocument::BuildEventRaisers(const plDocumentObject* pSystemNode, plParticleSystemDescriptor& ref_desc) const
{
  // the runtime keeps a system's events as names separated by ';': its death events on the descriptor,
  // its collision events on each Raycast
  plStringBuilder sDeath, sCollide;

  plDynamicArray<const plDocumentObject*> raisers;
  GetContextBlocks(pSystemNode, plParticleContext::Events, raisers);

  for (const plDocumentObject* pRaiser : raisers)
  {
    const plString sName = pRaiser->GetTypeAccessor().GetValue("EventName").ConvertTo<plString>();
    if (sName.IsEmpty())
      continue;

    plStringBuilder& sNames = pRaiser->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleRaiseOnCollisionBlock>() ? sCollide : sDeath;
    if (!sNames.IsEmpty())
      sNames.Append(";");
    sNames.Append(sName);
  }

  ref_desc.m_sOnDeathEvent = sDeath;

  bool bHasRaycast = false;
  for (plParticleBehaviorFactory* pBehavior : ref_desc.GetBehaviorFactories())
  {
    if (auto* pRaycast = plDynamicCast<plParticleBehaviorFactory_Raycast*>(pBehavior))
    {
      pRaycast->m_sOnCollideEvent = sCollide;
      bHasRaycast = true;
    }
  }

  if (!sCollide.IsEmpty() && !bHasRaycast)
    plLog::Warning("System '{}' raises '{}' on collision but has no Raycast in Update; nothing raises it.", pSystemNode->GetTypeAccessor().GetValue("Name").ConvertTo<plString>(), sCollide);
}

plParticleEffectAssetDocument::LoweringResult plParticleEffectAssetDocument::QueryLowering(const plDocumentObject* pSystemNode) const
{
  LoweringResult result;

  if (pSystemNode == nullptr)
    return result;

  // The blocker check reads a descriptor, so build a throwaway one for just this system. Only the
  // behaviors with their programs and the render type matter to it, which keeps this cheap enough to
  // run per repaint. The graph shows what is wrong with a wire itself, so the compile stays out of the log.
  plMuteLog mute;
  plLogSystemScope muteScope(&mute);
  plParticleSystemDescriptor desc;

  // wired values decide lowering too (a wire can switch a setting the GPU cannot do)
  const auto* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());
  plParticleGraphCompiler compiler(pManager, FindEffectNode());
  plParticleFoldedValues folded;
  plDynamicArray<plParticleFrameWire> frameWires;
  CollectWiredValues(pSystemNode, pManager, compiler, folded, frameWires, false);

  // initializers matter for what they write at spawn (per-axis size, CPU-only Set Attribute targets)
  plDynamicArray<const plDocumentObject*> initializers;
  GetContextBlocks(pSystemNode, plParticleContext::Initialize, initializers);
  for (const plDocumentObject* pBlock : initializers)
    desc.AddInitializerFactory(AllocateBlockFactory<plParticleInitializerFactory>(pBlock, &folded));

  plDynamicArray<const plDocumentObject*> behaviors;
  plHashTable<plUuid, plReflectedClass*> sinkFactories;
  GetContextBlocks(pSystemNode, plParticleContext::Update, behaviors);
  for (const plDocumentObject* pBlock : behaviors)
  {
    plParticleBehaviorFactory* pFactory = AllocateBlockFactory<plParticleBehaviorFactory>(pBlock, &folded);
    desc.AddBehaviorFactory(pFactory);
    sinkFactories[pBlock->GetGuid()] = pFactory;
  }

  // the per-particle programs decide too: their size, what they read, how many there are
  const double fLifetime = pSystemNode->GetTypeAccessor().GetValue("LifeTime").Get<plVarianceTypeTime>().m_Value.GetSeconds();
  BuildParticlePrograms(pManager, FindEffectNode(), frameWires, sinkFactories, fLifetime);

  plDynamicArray<const plDocumentObject*> types;
  GetContextBlocks(pSystemNode, plParticleContext::Output, types);
  for (const plDocumentObject* pBlock : types)
    desc.AddTypeFactory(AllocateBlockFactory<plParticleTypeFactory>(pBlock, &folded));

  plParticleGPULowering::BlockerKind kind = plParticleGPULowering::BlockerKind::None;
  plUInt32 uiIndex = 0;
  const plString sReason = plParticleGPULowering::FindLoweringBlocker(&desc, &kind, &uiIndex);

  result.m_sReason = sReason;

  // GetContextBlocks skips disabled blocks, so the descriptor index lines up with these arrays.
  if (kind == plParticleGPULowering::BlockerKind::Initializer && uiIndex < initializers.GetCount())
    result.m_pBlock = initializers[uiIndex];
  else if (kind == plParticleGPULowering::BlockerKind::Behavior && uiIndex < behaviors.GetCount())
    result.m_pBlock = behaviors[uiIndex];
  else if (kind == plParticleGPULowering::BlockerKind::Type && uiIndex < types.GetCount())
    result.m_pBlock = types[uiIndex];

  return result;
}

plParticleEffectAssetDocument::ParticleProgramInfo plParticleEffectAssetDocument::QueryParticlePrograms(const plDocumentObject* pSystemNode) const
{
  ParticleProgramInfo info;

  if (pSystemNode == nullptr)
    return info;

  // the graph shows what is wrong with a wire itself, so the transform's warnings stay out of the log
  plMuteLog mute;
  plLogSystemScope muteScope(&mute);

  const auto* pManager = static_cast<const plVisualGraphObjectManager*>(GetObjectManager());
  const plDocumentObject* pEffectNode = FindEffectNode();

  plParticleGraphCompiler compiler(pManager, pEffectNode);
  plParticleFoldedValues folded;
  plDynamicArray<plParticleFrameWire> wires;
  CollectWiredValues(pSystemNode, pManager, compiler, folded, wires, false);

  // throwaway factories of the Initialize and Update blocks, the only ones that carry programs
  plHybridArray<plReflectedClass*, 16> factories;
  plHashTable<plUuid, plReflectedClass*> sinkFactories;
  plDynamicArray<const plDocumentObject*> blocks;

  GetContextBlocks(pSystemNode, plParticleContext::Initialize, blocks);
  for (const plDocumentObject* pBlock : blocks)
  {
    plReflectedClass* pFactory = AllocateBlockFactory<plParticleInitializerFactory>(pBlock, &folded);
    factories.PushBack(pFactory);
    sinkFactories[pBlock->GetGuid()] = pFactory;
  }

  GetContextBlocks(pSystemNode, plParticleContext::Update, blocks);
  for (const plDocumentObject* pBlock : blocks)
  {
    plReflectedClass* pFactory = AllocateBlockFactory<plParticleBehaviorFactory>(pBlock, &folded);
    factories.PushBack(pFactory);
    sinkFactories[pBlock->GetGuid()] = pFactory;
  }

  const double fLifetime = pSystemNode->GetTypeAccessor().GetValue("LifeTime").Get<plVarianceTypeTime>().m_Value.GetSeconds();
  BuildParticlePrograms(pManager, pEffectNode, wires, sinkFactories, fLifetime);

  for (plReflectedClass* pFactory : factories)
  {
    const plParticleAttributeSinkSettings* pSettings = nullptr;
    bool bInitialize = false;

    if (const plParticleProgramData* pProgram = GetSinkProgram(pFactory, pSettings, bInitialize); pProgram != nullptr && !pProgram->IsEmpty())
    {
      (bInitialize ? info.m_bInitializeProgram : info.m_bUpdateProgram) = true;
      (bInitialize ? info.m_uiInitializeInstructions : info.m_uiUpdateInstructions) += pProgram->m_ByteCode.GetNumInstructions();
    }

    if (pSettings != nullptr && pSettings->m_Target == plParticleSinkTarget::Custom && !pSettings->m_sCustomAttribute.IsEmpty())
    {
      bool bKnown = false;
      for (const ParticleProgramInfo::CustomAttribute& attribute : info.m_CustomAttributes)
        bKnown = bKnown || attribute.m_sName == pSettings->m_sCustomAttribute;

      if (!bKnown)
      {
        ParticleProgramInfo::CustomAttribute& attribute = info.m_CustomAttributes.ExpandAndGetRef();
        attribute.m_sName = pSettings->m_sCustomAttribute;
        attribute.m_sType = pSettings->m_CustomType == plParticleCustomAttributeType::Vector ? "vector" : (pSettings->m_CustomType == plParticleCustomAttributeType::Color ? "color" : "float");
      }
    }

    pFactory->GetDynamicRTTI()->GetAllocator()->Deallocate(pFactory);
  }

  return info;
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

  // packages installed into the project live in its Plugins folder; their templates count as shipped
  const plString sProjectDir = plToolsProject::GetSingleton()->GetProjectDirectory();
  plStringBuilder sProjectPlugins = sProjectDir;
  sProjectPlugins.AppendPath("Plugins/");

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

      const bool bProjectLocal = sFile.StartsWith_NoCase(sProjectDir) && !sFile.StartsWith_NoCase(sProjectPlugins);

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

plString plParticleEffectAssetDocument::DescribeSystemTemplate(plStringView sAbsFilePath)
{
  plStringBuilder sText;
  {
    plFileReader file;
    if (file.Open(sAbsFilePath).Failed())
      return {};

    sText.ReadAll(file);
  }

  struct Block
  {
    plParticleContext::Enum m_Context;
    plUInt32 m_uiOrder;
    plString m_sName;
  };

  plHybridArray<Block, 16> blocks;

  // every object of the template names its type as  s %t{"TypeName"}
  const char* szSearch = sText.GetData();
  while ((szSearch = plStringUtils::FindSubString(szSearch, "%t{\"")) != nullptr)
  {
    szSearch += 4;
    const char* szEnd = plStringUtils::FindSubString(szSearch, "\"");
    if (szEnd == nullptr)
      break;

    const plString sType(plStringView(szSearch, szEnd));
    const plRTTI* pType = plRTTI::FindTypeByName(sType);
    const plParticleContext::Enum context = plParticleContextInfo::FromFactoryType(pType);

    if (context != plParticleContext::Count)
      blocks.PushBack({context, blocks.GetCount(), plParticleEffectNodeManager::FriendlyTypeName(pType)});

    szSearch = szEnd;
  }

  blocks.Sort([](const Block& a, const Block& b)
    { return a.m_Context != b.m_Context ? a.m_Context < b.m_Context : a.m_uiOrder < b.m_uiOrder; });

  plStringBuilder sDescription;
  for (const Block& block : blocks)
  {
    if (!sDescription.IsEmpty())
      sDescription.Append(", ");
    sDescription.Append(block.m_sName);
  }

  return sDescription;
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

plStatus plParticleEffectAssetDocument::AddSystemOnDeaths(const plDocumentObject* pSource)
{
  const plDocumentObject* pEffectNode = FindEffectNode();
  if (pEffectNode == nullptr || pSource == nullptr)
    return plStatus("The document has no effect node to connect the system to.");

  auto* pManager = static_cast<plParticleEffectNodeManager*>(GetObjectManager());
  plCommandHistory* pHistory = GetCommandHistory();
  pHistory->StartTransaction("Spawn a System on Its Deaths");

  // the source raises an event on every death: its Raise on Death block, or a new one named after it
  plString sEvent;
  for (const plDocumentObject* pBlock : pSource->GetChildren())
  {
    if (pBlock->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleRaiseOnDeathBlock>())
    {
      sEvent = pBlock->GetTypeAccessor().GetValue("EventName").ConvertTo<plString>();
      if (!sEvent.IsEmpty())
        break;
    }
  }

  if (sEvent.IsEmpty())
  {
    plDynamicArray<plString> taken;
    GetRaisedEvents(taken);

    const plString sSource = pSource->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    plStringBuilder sName;
    for (plUInt32 i = 1;; ++i)
    {
      sName.SetFormat(i == 1 ? "{} Death" : "{} Death {}", sSource.IsEmpty() ? plStringView("System") : sSource.GetView(), i);
      if (!taken.Contains(sName))
        break;
    }

    sEvent = sName;

    plAddObjectCommand add;
    add.m_pType = plGetStaticRTTI<plParticleRaiseOnDeathBlock>();
    add.m_Parent = pSource->GetGuid();
    add.m_sParentProperty = plParticleContextInfo::Get(plParticleContext::Events).m_szProperty;
    add.m_Index = -1;
    add.m_NewObjectGuid = plUuid::MakeUuid();
    pHistory->AddCommand(add).LogFailure();

    plSetObjectPropertyCommand cmd;
    cmd.m_Object = add.m_NewObjectGuid;
    cmd.m_sProperty = "EventName";
    cmd.m_NewValue = sEvent.GetData();
    pHistory->AddCommand(cmd).LogFailure();
  }

  // the listening system, right of the source: On Event per event, a cone of new particles, and a quad
  const plUuid systemGuid = plUuid::MakeUuid();
  {
    plAddObjectCommand cmd;
    cmd.m_pType = plGetStaticRTTI<plParticleSystemNode>();
    cmd.m_NewObjectGuid = systemGuid;
    cmd.m_Index = -1;
    pHistory->AddCommand(cmd).LogFailure();
  }

  auto SetProperty = [&](const plUuid& object, const char* szProperty, const plVariant& value)
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = object;
    cmd.m_sProperty = szProperty;
    cmd.m_NewValue = value;
    pHistory->AddCommand(cmd).LogFailure();
  };

  plStringBuilder sName("On ", sEvent);
  SetProperty(systemGuid, "Name", sName.GetData());
  plVarianceTypeTime lifeTime;
  lifeTime.m_Value = plTime::MakeFromSeconds(1.0);
  lifeTime.m_fVariance = 0.25f;
  SetProperty(systemGuid, "LifeTime", plVariant(lifeTime));

  {
    plMoveNodeCommand cmd;
    cmd.m_Object = systemGuid;
    cmd.m_NewPos = pManager->GetNodePos(pSource) + plVec2(500.0f, 0.0f);
    pHistory->AddCommand(cmd).LogFailure();
  }

  auto AddBlock = [&](const plRTTI* pType, plParticleContext::Enum context) -> plUuid
  {
    plAddObjectCommand cmd;
    cmd.m_pType = pType;
    cmd.m_Parent = systemGuid;
    cmd.m_sParentProperty = plParticleContextInfo::Get(context).m_szProperty;
    cmd.m_Index = -1;
    cmd.m_NewObjectGuid = plUuid::MakeUuid();
    pHistory->AddCommand(cmd).LogFailure();
    return cmd.m_NewObjectGuid;
  };

  const plUuid onEvent = AddBlock(plGetStaticRTTI<plParticleEmitterFactory_OnEvent>(), plParticleContext::Spawn);
  SetProperty(onEvent, "EventName", sEvent.GetData());
  SetProperty(onEvent, "MinSpawnCount", (plUInt32)4);
  SetProperty(onEvent, "SpawnCountRange", (plUInt32)2);

  AddBlock(plGetStaticRTTI<plParticleInitializerFactory_VelocityCone>(), plParticleContext::Initialize);
  const plUuid quad = AddBlock(plGetStaticRTTI<plParticleTypeQuadFactory>(), plParticleContext::Output);

  // drawn with the source's texture, so the new particles are visible at once
  for (const plDocumentObject* pBlock : pSource->GetChildren())
  {
    if (pBlock->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleTypeQuadFactory>())
    {
      SetProperty(quad, "Texture", pBlock->GetTypeAccessor().GetValue("Texture"));
      SetProperty(quad, "RenderMode", pBlock->GetTypeAccessor().GetValue("RenderMode"));
      break;
    }
  }

  // wired into the effect like every other system
  if (const plVisualGraphPin* pSourcePin = pManager->GetOutputPinByName(pManager->GetObject(systemGuid), "System"))
  {
    if (const plVisualGraphPin* pTargetPin = pManager->GetInputPinByName(pEffectNode, "Systems"))
      plNodeCommands::AddAndConnectCommand(pHistory, pManager->GetConnectionType(), *pSourcePin, *pTargetPin).LogFailure();
  }

  pHistory->FinishTransaction();
  pManager->RecreateAllSystemPins();

  GetSelectionManager()->SetSelection(pManager->GetObject(systemGuid));
  return plStatus(PL_SUCCESS);
}

static bool IsGraphNode(const plDocumentObject* pObject)
{
  const plRTTI* pType = pObject->GetTypeAccessor().GetType();
  return pType->IsDerivedFrom<plParticleSystemNode>() || pType->IsDerivedFrom<plParticleEffectNode>();
}

bool plParticleEffectAssetDocument::MigrateEventsIntoBlocks()
{
  struct Move
  {
    const plDocumentObject* m_pSystem = nullptr;
    const plDocumentObject* m_pOwner = nullptr; ///< the system (deaths) or the Raycast (collisions)
    const char* m_szProperty = nullptr;
    plString m_sEvent;
    const plRTTI* m_pRaiser = nullptr;
  };

  plHybridArray<Move, 4> moves;

  for (const plDocumentObject* pSystem : GetObjectManager()->GetRootObject()->GetChildren())
  {
    if (!pSystem->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
      continue;

    if (plString sEvent = pSystem->GetTypeAccessor().GetValue("OnDeathEvent").ConvertTo<plString>(); !sEvent.IsEmpty())
      moves.PushBack({pSystem, pSystem, "OnDeathEvent", sEvent, plGetStaticRTTI<plParticleRaiseOnDeathBlock>()});

    for (const plDocumentObject* pBlock : pSystem->GetChildren())
    {
      if (!pBlock->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleBehaviorFactory_Raycast>())
        continue;

      if (plString sEvent = pBlock->GetTypeAccessor().GetValue("OnCollideEvent").ConvertTo<plString>(); !sEvent.IsEmpty())
        moves.PushBack({pSystem, pBlock, "OnCollideEvent", sEvent, plGetStaticRTTI<plParticleRaiseOnCollisionBlock>()});
    }
  }

  if (moves.IsEmpty())
    return false;

  // like the other conversions on load: saving keeps the new form, and it is not an undo step
  plCommandHistory* pHistory = GetCommandHistory();
  pHistory->StartTransaction("Convert Events into Blocks");

  for (const Move& move : moves)
  {
    plAddObjectCommand add;
    add.m_pType = move.m_pRaiser;
    add.m_Parent = move.m_pSystem->GetGuid();
    add.m_sParentProperty = plParticleContextInfo::Get(plParticleContext::Events).m_szProperty;
    add.m_Index = -1;
    add.m_NewObjectGuid = plUuid::MakeUuid();
    pHistory->AddCommand(add).LogFailure();

    plSetObjectPropertyCommand name;
    name.m_Object = add.m_NewObjectGuid;
    name.m_sProperty = "EventName";
    name.m_NewValue = move.m_sEvent.GetData();
    pHistory->AddCommand(name).LogFailure();

    plSetObjectPropertyCommand clear;
    clear.m_Object = move.m_pOwner->GetGuid();
    clear.m_sProperty = move.m_szProperty;
    clear.m_NewValue = "";
    pHistory->AddCommand(clear).LogFailure();
  }

  pHistory->FinishTransaction();
  pHistory->ClearUndoHistory();
  return true;
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
  // Events came after this layout, so they have no pin
  const char* szLegacyPin[plParticleContext::Count] = {"Emitter", "Initializers", "Behaviors", "Renderers", nullptr};

  for (const plDocumentObject* pSystemNode : pRoot->GetChildren())
  {
    if (!pSystemNode->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>())
      continue;

    for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
    {
      if (szLegacyPin[uiContext] == nullptr)
        continue;

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

    // a plain remove would leave the connection registered with its pins, and the next pin rebuild
    // would destroy it a second time
    if (pInfo != nullptr)
    {
      plNodeCommands::DisconnectAndRemoveCommand(pHistory, pConnection->GetGuid()).LogFailure();
    }
    else
    {
      plRemoveObjectCommand cmd;
      cmd.m_Object = pConnection->GetGuid();
      pHistory->AddCommand(cmd).IgnoreResult();
    }
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

    // property pins of the migrated blocks exist from here on, which the conversion wires into
    static_cast<plParticleEffectNodeManager*>(GetObjectManager())->RecreateAllSystemPins();
    ConvertLogicBlocks();
    MigrateEventsIntoBlocks();
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

  if (!pManager->CopySelectedObjects(out_objectGraph))
    return false;

  // the paste gives every object a new guid, but the wires name their blocks by key
  plParticlePropertyPin::StampKeys(out_objectGraph);
  return true;
}

bool plParticleEffectAssetDocument::Paste(const plArrayPtr<PasteInfo>& info, const plAbstractObjectGraph& objectGraph, bool bAllowPickedPosition, plStringView sMimeType)
{
  plVisualGraphObjectManager* pManager = static_cast<plVisualGraphObjectManager*>(GetObjectManager());
  return pManager->PasteObjects(info, objectGraph, plEditorUiGraphView::GetLastPointerGraphPos(), bAllowPickedPosition);
}