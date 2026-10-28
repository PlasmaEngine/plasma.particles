#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectGraphView.h>
#include <EditorUi/Widgets.h>
#include <Foundation/IO/OSFile.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Raycast.h>
#include <ParticlePlugin/Emitter/ParticleEmitter_OnEvent.h>
#include <RendererCore/Textures/Texture2DResource.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Selection/SelectionManager.h>

// Layout and retained elements of the particle graph's custom nodes. All sizes are graph units.

namespace
{
  constexpr plUInt32 s_uiTitleSize = plEditorUi::FontSize + 1;
  constexpr plUInt32 s_uiSmallSize = plEditorUi::FontSize - 1;
  constexpr plUInt32 s_uiChipSize = plEditorUi::FontSize - 2;
  constexpr float s_fPad = 8.0f;
  constexpr float s_fCornerRadius = 7.0f;
  constexpr float s_fTitleLine = 18.0f;
  constexpr float s_fBadgeHeight = 16.0f;
  constexpr float s_fPinHit = 8.0f;

  constexpr float s_fEffectWidth = 300.0f;
  constexpr float s_fEffectHeight = 52.0f;
  constexpr float s_fEffectStrip = 3.0f;

  constexpr float s_fSystemWidth = 300.0f;
  constexpr float s_fContextHeader = 22.0f;
  constexpr float s_fBlockRow = 24.0f;
  constexpr float s_fValueRow = 20.0f;
  constexpr float s_fAddRow = 20.0f;
  constexpr float s_fContextGap = 8.0f;
  constexpr float s_fAccentWidth = 3.0f;
  constexpr float s_fVerdictHeight = 18.0f;
  constexpr float s_fEventNoteHeight = 18.0f;
  constexpr float s_fAttributeRow = 17.0f;

  /// Events: the raising and listening rows and the wires between them.
  const plColorGammaUB s_EventColor(224, 163, 92);

  constexpr float s_fOperatorWidth = 190.0f;
  constexpr float s_fMaxCodeWidth = 360.0f;
  constexpr float s_fCodeLine = 15.0f;
  constexpr float s_fPreviewBand = 46.0f;
  constexpr float s_fMaxPreview = 240.0f;

  plColorGammaUB Mix(plColorGammaUB a, plColorGammaUB b, float t)
  {
    return plColorGammaUB(plMath::Lerp(plColor(a), plColor(b), t));
  }

  plColorGammaUB WithAlpha(plColorGammaUB color, plUInt8 uiAlpha)
  {
    color.a = uiAlpha;
    return color;
  }

  void Place(plUiElement* pElement, const plRectFloat& rect)
  {
    pElement->m_vAnchorMin = plVec2::MakeZero();
    pElement->m_vAnchorMax = plVec2::MakeZero();
    pElement->m_vOffsets = plVec4(rect.x, rect.y, rect.width, rect.height);
  }

  plUiPanel* AddPanel(plUiElement* pParent, const plRectFloat& rect, plColorGammaUB color, float fRadius = 0.0f)
  {
    plUiPanel* pPanel = PL_DEFAULT_NEW(plUiPanel);
    Place(pPanel, rect);
    pPanel->m_Color = color;
    pPanel->m_vCornerRadius = plVec4(fRadius);
    pParent->m_Children.PushBack(pPanel);
    return pPanel;
  }

  plUiPanel* AddFrame(plUiElement* pParent, const plRectFloat& rect, plColorGammaUB border, float fRadius, plColorGammaUB fill = plColorGammaUB(0, 0, 0, 0))
  {
    plUiPanel* pFrame = AddPanel(pParent, rect, fill, fRadius);
    pFrame->m_fBorderWidth = 1.0f;
    pFrame->m_BorderColor = border;
    return pFrame;
  }

  plUiText* AddText(plUiElement* pParent, const plRectFloat& rect, plStringView sText, plUInt32 uiSize, plColorGammaUB color, plUiTextHAlign::Enum align = plUiTextHAlign::Left)
  {
    plUiText* pText = PL_DEFAULT_NEW(plUiText);
    Place(pText, rect);
    pText->m_sText = sText;
    pText->m_hFont = plEditorUi::GetDefaultFont();
    pText->m_uiFontSize = uiSize;
    pText->m_Color = color;
    pText->m_HAlign = align;
    pText->m_VAlign = plUiTextVAlign::Center;
    pText->m_Overflow = plUiTextOverflow::Ellipsis;
    pParent->m_Children.PushBack(pText);
    return pText;
  }

  /// A plate that lights up under the pointer. It has no name, so the graph view still routes its
  /// presses to the controller (which needs the drags and right-clicks) instead of treating it as a control.
  plUiButton* AddHoverPlate(plUiElement* pParent, const plRectFloat& rect, plColorGammaUB color, float fRadius)
  {
    plUiButton* pPlate = PL_DEFAULT_NEW(plUiButton);
    Place(pPlate, rect);
    pPlate->m_Color = color;
    pPlate->m_HoverColor = Mix(color, plEditorUi::Foreground(), 0.08f);
    pPlate->m_PressedColor = pPlate->m_HoverColor;
    pPlate->m_vCornerRadius = plVec4(fRadius);
    pParent->m_Children.PushBack(pPlate);
    return pPlate;
  }

  plRectFloat Inset(const plRectFloat& rect, float fLeft, float fRight)
  {
    return plRectFloat(rect.x + fLeft, rect.y, plMath::Max(rect.width - fLeft - fRight, 0.0f), rect.height);
  }

  plVisualGraphPinPlacement& AddPin(plDynamicArray<plVisualGraphPinPlacement>& out_pins, const plVisualGraphPin* pPin, const plVec2& vCenter)
  {
    plVisualGraphPinPlacement& placement = out_pins.ExpandAndGetRef();
    placement.m_pPin = pPin;
    placement.m_vCenter = vCenter;
    placement.m_HitRect = plRectFloat(vCenter.x - s_fPinHit, vCenter.y - s_fPinHit, 2.0f * s_fPinHit, 2.0f * s_fPinHit);
    return placement;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// Layout

plParticleEffectGraphController::NodeType plParticleEffectGraphController::GetNodeType(const plDocumentObject* pNode) const
{
  const plRTTI* pType = pNode->GetTypeAccessor().GetType();

  if (pType->IsDerivedFrom<plParticleEffectNode>())
    return NodeType::Effect;

  if (pType->IsDerivedFrom<plParticleSystemNode>())
    return NodeType::System;

  plStringView sValueProperty;
  if (plParticleValueKind::FromNode(pType, sValueProperty) != plParticleValueKind::None)
    return NodeType::Operator;

  // pre-rework blocks and mix nodes at the root only exist until the document migrates them
  return NodeType::None;
}

const plParticleEffectGraphController::NodeLayout* plParticleEffectGraphController::FindLayout(const plDocumentObject* pNode) const
{
  return m_Layouts.GetValue(pNode);
}

void plParticleEffectGraphController::LayoutCustomNode(const plDocumentObject* pNode, plVec2& out_vSize, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const
{
  // layouts of removed nodes are dropped once they outnumber the graph's objects
  const auto& children = m_pManager->GetRootObject()->GetChildren();
  if (m_Layouts.GetCount() > children.GetCount() + 8)
  {
    for (auto it = m_Layouts.GetIterator(); it.IsValid();)
    {
      if (children.Contains(const_cast<plDocumentObject*>(it.Key())))
        ++it;
      else
        it = m_Layouts.Remove(it);
    }
  }

  NodeLayout& layout = m_Layouts[pNode];
  layout = NodeLayout();
  layout.m_Type = GetNodeType(pNode);

  switch (layout.m_Type)
  {
    case NodeType::Effect:
      LayoutEffect(pNode, layout, out_pins);
      break;
    case NodeType::System:
      LayoutSystem(pNode, layout, out_pins);
      break;
    case NodeType::Operator:
      LayoutOperator(pNode, layout, out_pins);
      break;
    case NodeType::None:
      break;
  }

  out_vSize = layout.m_vSize;
}

void plParticleEffectGraphController::LayoutEffect(const plDocumentObject* pNode, NodeLayout& ref_layout, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const
{
  ref_layout.m_vSize.Set(s_fEffectWidth, s_fEffectHeight);
  ref_layout.m_fHeaderHeight = s_fEffectHeight;
  ref_layout.m_sTitle = "Effect";
  ref_layout.m_sSubtitle = plParticleGraphEditing::GetEffectSubtitle(pNode);

  // systems arrive on the top edge; the retired EventReactions pin stays hidden, reactions are an
  // owned array edited in the properties dock
  if (const plVisualGraphPin* pSystems = m_pManager->GetInputPinByName(pNode, "Systems"))
    AddPin(out_pins, pSystems, plVec2(s_fEffectWidth * 0.5f, 0.0f));
}

void plParticleEffectGraphController::LayoutSystem(const plDocumentObject* pNode, NodeLayout& ref_layout, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const
{
  const float w = s_fSystemWidth;

  ref_layout.m_sTitle = plParticleGraphEditing::GetSystemTitle(pNode);
  ref_layout.m_sSubtitle = plParticleGraphEditing::GetSimulationTargetLabel(pNode);
  ref_layout.m_fHeaderHeight = 6.0f + s_fTitleLine + 2.0f + s_fBadgeHeight + 6.0f;

  float y = ref_layout.m_fHeaderHeight + 6.0f;

  // a CPU system is never asked to lower, so there is nothing to report for it
  const plInt64 iTarget = pNode->GetTypeAccessor().GetValue("SimulationTarget").ConvertTo<plInt64>();
  ref_layout.m_bTargetsGpu = iTarget != (plInt64)plParticleSimulationTarget::CPU;

  if (ref_layout.m_bTargetsGpu)
  {
    ref_layout.m_VerdictRect = plRectFloat(s_fPad, y, w - 2.0f * s_fPad, s_fVerdictHeight);
    y += s_fVerdictHeight + 4.0f;
  }

  LayoutSystemPrograms(pNode, ref_layout, y);

  y += 4.0f;
  LayoutSystemContexts(pNode, ref_layout, y);

  ref_layout.m_vSize.Set(w, y - s_fContextGap + s_fPad);

  if (const plVisualGraphPin* pOutput = m_pManager->GetOutputPinByName(pNode, "System"))
    AddPin(out_pins, pOutput, plVec2(w * 0.5f, ref_layout.m_vSize.y));

  LayoutSystemPins(pNode, ref_layout, out_pins);
}

void plParticleEffectGraphController::LayoutSystemPrograms(const plDocumentObject* pNode, NodeLayout& ref_layout, float& ref_fY) const
{
  const auto* pDocument = plDynamicCast<const plParticleEffectAssetDocument*>(m_pManager->GetDocument());
  if (pDocument == nullptr)
    return;

  const plParticleEffectAssetDocument::ParticleProgramInfo info = pDocument->QueryParticlePrograms(pNode);

  plStringBuilder sProgram;

  if (info.m_bInitializeProgram)
  {
    sProgram.SetFormat("program: {} ops", info.m_uiInitializeInstructions);
    ref_layout.m_Contexts[plParticleContext::Initialize].m_sProgram = sProgram;
  }

  if (info.m_bUpdateProgram)
  {
    sProgram.SetFormat("program: {} ops", info.m_uiUpdateInstructions);
    ref_layout.m_Contexts[plParticleContext::Update].m_sProgram = sProgram;
  }

  if (info.m_CustomAttributes.IsEmpty())
    return;

  // the custom attributes: a title row with the count and "+ Add", then one row per attribute
  const float w = s_fSystemWidth - 2.0f * s_fPad;
  const float fTop = ref_fY;

  ref_layout.m_AttributesTitleRect = plRectFloat(s_fPad + 4.0f, ref_fY, w - 8.0f, s_fAttributeRow);
  ref_layout.m_AddAttributeRect = plRectFloat(s_fPad + w - 48.0f, ref_fY, 44.0f, s_fAttributeRow);
  ref_fY += s_fAttributeRow;

  for (const auto& attribute : info.m_CustomAttributes)
  {
    AttributeLayout& row = ref_layout.m_Attributes.ExpandAndGetRef();
    row.m_sName = attribute.m_sName;
    row.m_sType = attribute.m_sType;
    row.m_Rect = plRectFloat(s_fPad, ref_fY, w, s_fAttributeRow);
    ref_fY += s_fAttributeRow;
  }

  ref_fY += 4.0f;
  ref_layout.m_AttributesRect = plRectFloat(s_fPad, fTop, w, ref_fY - fTop);
}

void plParticleEffectGraphController::LayoutSystemContexts(const plDocumentObject* pNode, NodeLayout& ref_layout, float& ref_fY) const
{
  const float w = s_fSystemWidth;
  const auto* pDocument = plDynamicCast<const plParticleEffectAssetDocument*>(m_pManager->GetDocument());

  const plDocumentObject* pBlocker = nullptr;
  if (ref_layout.m_bTargetsGpu && pDocument != nullptr)
  {
    const auto lowering = pDocument->QueryLowering(pNode);
    ref_layout.m_sLoweringReason = lowering.m_sReason;
    pBlocker = lowering.m_pBlock;
  }

  const SystemState* pState = m_SystemStates.GetValue(pNode->GetGuid());
  plHybridArray<const plDocumentObject*, 8> blocks;

  // an On Event says when nothing in the effect raises its event
  plDynamicArray<plString> raisedEvents;
  if (pDocument != nullptr)
    pDocument->GetRaisedEvents(raisedEvents);

  // Raise on Collision takes the collisions of the system's Raycast blocks
  bool bHasRaycast = false;
  plParticleGraphEditing::GetContextBlocks(m_pManager, pNode, plParticleContext::Update, blocks);
  for (const plDocumentObject* pBlock : blocks)
    bHasRaycast |= pBlock->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleBehaviorFactory_Raycast>();

  // a wired row shows what the wire carries instead of the block's own, unused, value
  plParticleGraphCompiler compiler(m_pManager, FindEffectNode());
  auto describeWiredRows = [&](const plDocumentObject* pBlock, plArrayPtr<ValueRowLayout> rows)
  {
    for (ValueRowLayout& row : rows)
    {
      const plVisualGraphPin* pPin = m_pManager->GetInputPinByName(pNode, plParticlePropertyPin::Make(pBlock, row.m_pProp->GetPropertyName()));
      if (pPin == nullptr)
        continue;

      for (const plVisualGraphConnection* pConnection : m_pManager->GetConnections(*pPin))
      {
        const plDocumentObject* pSource = pConnection->GetSourcePin().GetParent();

        row.m_bDriven = true;
        row.m_DrivenTier = compiler.GetTier(pSource);
        row.m_bDrivenUnresolved = !compiler.Fold(pSource).IsValid();
        row.m_sDrivenBy = compiler.Describe(pSource);
        break;
      }
    }
  };

  for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
  {
    ContextLayout& context = ref_layout.m_Contexts[uiContext];
    context.m_bCollapsed = pState != nullptr && pState->m_bCollapsed[uiContext];

    const float fTop = ref_fY;
    context.m_HeaderRect = plRectFloat(s_fPad, ref_fY, w - 2.0f * s_fPad, s_fContextHeader);
    ref_fY += s_fContextHeader;

    if (!context.m_sProgram.IsEmpty())
    {
      const float fChipWidth = plEditorUi::MeasureText(context.m_sProgram, s_uiChipSize) + 12.0f;
      context.m_ProgramRect = plRectFloat(context.m_HeaderRect.Right() - 6.0f - fChipWidth, context.m_HeaderRect.y + 3.0f, fChipWidth, context.m_HeaderRect.height - 6.0f);
    }

    plParticleGraphEditing::GetContextBlocks(m_pManager, pNode, (plParticleContext::Enum)uiContext, blocks);

    for (const plDocumentObject* pBlock : blocks)
    {
      BlockLayout& block = context.m_Blocks.ExpandAndGetRef();
      block.m_Block = pBlock->GetGuid();
      block.m_sTitle = plParticleGraphEditing::GetBlockDisplayName(pBlock->GetTypeAccessor().GetType());
      block.m_sDetail = plParticleGraphEditing::GetSinkTargetLabel(pBlock);
      block.m_sSummary = block.m_sDetail.IsEmpty() ? plParticleGraphEditing::GetBlockSummary(pBlock) : plString();
      block.m_bEnabled = plParticleGraphEditing::IsBlockEnabled(pBlock);
      block.m_bExpanded = m_ExpandedBlocks.Contains(block.m_Block);
      block.m_bBlocksLowering = pBlock == pBlocker;
      block.m_bConverted = pDocument != nullptr && pDocument->IsConverted(block.m_Block);
      LayoutBlockEvents(pBlock, raisedEvents, bHasRaycast, block);

      if (block.m_bExpanded && !context.m_bCollapsed)
      {
        plParticleGraphEditing::CollectBlockRows<ValueRowLayout>(pBlock, block.m_Rows);
        describeWiredRows(pBlock, block.m_Rows);

        const auto& accessor = pBlock->GetTypeAccessor();
        for (ValueRowLayout& row : block.m_Rows)
        {
          row.m_bPerParticle = plParticleGraphEditing::IsPerParticleProperty(row.m_pProp);

          // a Set Attribute component reads as the target names it: A of a colour, Z of a position
          if (!block.m_sDetail.IsEmpty() && plStringUtils::IsEqual(row.m_pProp->GetPropertyName(), "Component"))
            row.m_sValue = plParticleGraphEditing::GetSinkComponentLabel(accessor.GetValue("Target").ConvertTo<plInt64>(), accessor.GetValue("CustomType").ConvertTo<plInt64>(), row.m_Original.ConvertTo<plInt64>());
        }
      }
    }

    if (!context.m_bCollapsed)
    {
      ref_fY += 2.0f;

      const float fRowLeft = s_fPad + s_fAccentWidth;
      const float fRowWidth = w - 2.0f * s_fPad - s_fAccentWidth;

      for (BlockLayout& block : context.m_Blocks)
      {
        block.m_Rect = plRectFloat(fRowLeft, ref_fY, fRowWidth, s_fBlockRow - 2.0f);
        block.m_EnableRect = plRectFloat(block.m_Rect.Right() - 18.0f, block.m_Rect.y + (block.m_Rect.height - 12.0f) * 0.5f, 12.0f, 12.0f);
        ref_fY += s_fBlockRow;

        // values get the wider share: enum names such as "Flipbook Animation" are long
        LayoutValueRows(block.m_Rows, block.m_Rect.x + 10.0f, block.m_Rect.width - 12.0f, s_fValueRow, 0.58f, ref_fY);

        if (block.m_bExpanded)
          ref_fY += 3.0f;
      }

      context.m_AddRect = plRectFloat(fRowLeft, ref_fY, fRowWidth, s_fAddRow);
      ref_fY += s_fAddRow + 4.0f;
    }

    context.m_Rect = plRectFloat(s_fPad, fTop, w - 2.0f * s_fPad, ref_fY - fTop);
    ref_fY += s_fContextGap;

    // a GPU system's events come back from the GPU; the note sits under the blocks that raise them
    if (uiContext == plParticleContext::Events && ref_layout.m_bTargetsGpu && ref_layout.m_sLoweringReason.IsEmpty())
    {
      plHybridArray<plString, 4> events;
      for (const BlockLayout& block : context.m_Blocks)
      {
        if (!block.m_sRaises.IsEmpty() && !events.Contains(block.m_sRaises))
          events.PushBack(block.m_sRaises);
      }

      if (!events.IsEmpty())
      {
        plStringBuilder sNames;
        for (plUInt32 i = 0; i < events.GetCount(); ++i)
          sNames.Append(i == 0 ? "" : (i + 1 == events.GetCount() ? " and " : ", "), events[i]);

        plStringBuilder sNote;
        sNote.SetFormat("{} {} read back from the GPU, 2\xE2\x80\x93" "3 frames late.", sNames, events.GetCount() == 1 ? "is" : "are"); // –
        ref_layout.m_sEventNote = sNote;
        ref_layout.m_EventNoteRect = plRectFloat(s_fPad, ref_fY - s_fContextGap + 2.0f, w - 2.0f * s_fPad, s_fEventNoteHeight);
        ref_fY += s_fEventNoteHeight + 2.0f;
      }
    }
  }
}

void plParticleEffectGraphController::LayoutBlockEvents(const plDocumentObject* pBlock, plArrayPtr<const plString> raised, bool bHasRaycast, BlockLayout& ref_block) const
{
  const auto& accessor = pBlock->GetTypeAccessor();
  const plRTTI* pType = accessor.GetType();

  if (pType->IsDerivedFrom<plParticleEventRaiserBlock>())
  {
    ref_block.m_sRaises = accessor.GetValue("EventName").ConvertTo<plString>();

    if (ref_block.m_sRaises.IsEmpty())
    {
      ref_block.m_sEvent = "name the event";
      ref_block.m_bEventUnmatched = true;
    }
    else if (pType->IsDerivedFrom<plParticleRaiseOnCollisionBlock>() && !bHasRaycast)
    {
      // nothing finds the collisions, so the event is never raised; no wires leave it
      ref_block.m_sEvent = "needs a Raycast in Update";
      ref_block.m_bEventUnmatched = true;
      ref_block.m_sRaises.Clear();
    }
    else
    {
      ref_block.m_sEvent = ref_block.m_sRaises;
    }
  }
  else if (pType->IsDerivedFrom<plParticleEmitterFactory_OnEvent>())
  {
    ref_block.m_sListens = accessor.GetValue("EventName").ConvertTo<plString>();

    if (ref_block.m_sListens.IsEmpty())
    {
      ref_block.m_sEvent = "pick an event";
      ref_block.m_bEventUnmatched = true;
    }
    else if (!raised.Contains(ref_block.m_sListens))
    {
      ref_block.m_sEvent = plStringBuilder(ref_block.m_sListens, " \xC2\xB7 nothing raises it"); // ·
      ref_block.m_bEventUnmatched = true;
    }
    else
    {
      const bool bPerEvent = accessor.GetValue("Spawn").ConvertTo<plInt64>() == plParticleOnEventSpawn::PerEvent;
      ref_block.m_sEvent = plStringBuilder(ref_block.m_sListens, bPerEvent ? " \xC2\xB7 per event" : " \xC2\xB7 once per frame"); // ·
    }
  }
}

void plParticleEffectGraphController::GetExtraWires(plDynamicArray<plVisualGraphExtraWire>& out_wires) const
{
  // every block that raises an event is wired to every On Event that spawns on it, in any system
  struct End
  {
    const plDocumentObject* m_pNode = nullptr;
    plVec2 m_vOffset;
    plString m_sEvent;
  };

  plHybridArray<End, 8> raisers;
  plHybridArray<End, 8> listeners;

  for (auto it = m_Layouts.GetIterator(); it.IsValid(); ++it)
  {
    const NodeLayout& layout = it.Value();
    if (layout.m_Type != NodeType::System)
      continue;

    for (const ContextLayout& context : layout.m_Contexts)
    {
      for (const BlockLayout& block : context.m_Blocks)
      {
        // a folded context hides its rows; the wires end on its header then
        const plRectFloat& rect = context.m_bCollapsed ? context.m_HeaderRect : block.m_Rect;
        const float fY = rect.y + rect.height * 0.5f;

        if (!block.m_sRaises.IsEmpty())
          raisers.PushBack({it.Key(), plVec2(layout.m_vSize.x, fY), block.m_sRaises});

        if (!block.m_sListens.IsEmpty())
          listeners.PushBack({it.Key(), plVec2(0.0f, fY), block.m_sListens});
      }
    }
  }

  for (const End& raiser : raisers)
  {
    for (const End& listener : listeners)
    {
      if (raiser.m_sEvent != listener.m_sEvent || raiser.m_pNode == listener.m_pNode)
        continue;

      plVisualGraphExtraWire& wire = out_wires.ExpandAndGetRef();
      wire.m_pSourceNode = raiser.m_pNode;
      wire.m_vSourceOffset = raiser.m_vOffset;
      wire.m_pTargetNode = listener.m_pNode;
      wire.m_vTargetOffset = listener.m_vOffset;
      wire.m_Color = s_EventColor;
      wire.m_sLabel = raiser.m_sEvent;
    }
  }
}

void plParticleEffectGraphController::LayoutValueRows(plArrayPtr<ValueRowLayout> rows, float fLeft, float fWidth, float fRowHeight, float fValueShare, float& ref_fY) const
{
  for (ValueRowLayout& row : rows)
  {
    row.m_Rect = plRectFloat(fLeft, ref_fY, fWidth, fRowHeight - 1.0f);
    row.m_ValueRect = plRectFloat(fLeft + fWidth * (1.0f - fValueShare), ref_fY, fWidth * fValueShare, fRowHeight - 1.0f);

    // a variance pair splits the value area into value and deviation, each scrubbable on its own
    if (row.m_bHasVariance)
    {
      const float fHalf = row.m_ValueRect.width * 0.5f;
      row.m_ValueRect2 = plRectFloat(row.m_ValueRect.x + fHalf, row.m_ValueRect.y, fHalf, row.m_ValueRect.height);
      row.m_ValueRect.width = fHalf - 4.0f;
    }

    ref_fY += fRowHeight;
  }
}

void plParticleEffectGraphController::LayoutSystemPins(const plDocumentObject* pNode, const NodeLayout& layout, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const
{
  // the legacy category pins stay hidden; property pins sit on the row they drive, and an unconnected
  // one only appears while its block is open, so a closed system stays clean
  plStringBuilder sProperty;

  for (const auto& pPin : m_pManager->GetInputPins(pNode))
  {
    const plDocumentObject* pBlock = plParticlePropertyPin::FindBlock(pNode, pPin->GetName(), sProperty);
    if (pBlock == nullptr)
      continue;

    plVec2 vAnchor;
    bool bRowVisible = false;
    if (!FindPinAnchor(layout, pBlock->GetGuid(), sProperty, vAnchor, bRowVisible))
      continue;

    if (!bRowVisible && !m_pManager->HasConnections(*pPin))
      continue;

    AddPin(out_pins, pPin.Borrow(), vAnchor);
  }
}

bool plParticleEffectGraphController::FindPinAnchor(const NodeLayout& layout, const plUuid& block, plStringView sProperty, plVec2& out_vPosition, bool& out_bRowVisible) const
{
  out_bRowVisible = false;

  for (const ContextLayout& context : layout.m_Contexts)
  {
    for (const BlockLayout& row : context.m_Blocks)
    {
      if (row.m_Block != block)
        continue;

      // a folded context has no rows at all, so its blocks' pins fold onto its header
      if (context.m_bCollapsed)
      {
        out_vPosition.Set(context.m_HeaderRect.x - 4.0f, context.m_HeaderRect.GetCenter().y);
        return true;
      }

      for (const ValueRowLayout& value : row.m_Rows)
      {
        if (sProperty == value.m_pProp->GetPropertyName())
        {
          out_bRowVisible = true;
          out_vPosition.Set(value.m_Rect.x - 8.0f, value.m_Rect.GetCenter().y);
          return true;
        }
      }

      // block closed, or the property is hidden right now: fold back onto the block row
      out_vPosition.Set(row.m_Rect.x - 4.0f, row.m_Rect.GetCenter().y);
      return true;
    }
  }

  return false;
}

void plParticleEffectGraphController::LayoutOperator(const plDocumentObject* pNode, NodeLayout& ref_layout, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const
{
  const plRTTI* pType = pNode->GetTypeAccessor().GetType();

  ref_layout.m_sTitle = plParticleGraphEditing::GetOperatorTitle(pNode);

  // the badge is coloured by what the node produces, so it matches its output pin and wires
  plStringView sValueProperty;
  ref_layout.m_BadgeColor = plParticleGraphEditing::GetValueKindColor(plParticleValueKind::FromNode(pType, sValueProperty));

  if (const plCategoryAttribute* pCategory = pType->GetAttributeByType<plCategoryAttribute>())
  {
    plStringBuilder sCategory = pCategory->GetCategory();
    sCategory.ToUpper();
    ref_layout.m_sSubtitle = sCategory;
  }

  // wide enough for the header, never narrower than a readable row
  const float fTitleWidth = plMath::Max(plEditorUi::MeasureText(ref_layout.m_sTitle, s_uiTitleSize), plEditorUi::MeasureText(ref_layout.m_sSubtitle, s_uiSmallSize) + 12.0f);
  float w = plMath::Max(s_fOperatorWidth, fTitleWidth + 40.0f);

  ref_layout.m_fHeaderHeight = 6.0f + s_fTitleLine + (ref_layout.m_sSubtitle.IsEmpty() ? 0.0f : 2.0f + s_fBadgeHeight) + 6.0f;
  float y = ref_layout.m_fHeaderHeight + 4.0f;

  // an Expression node shows its code in a box, one line per line of code; the node widens to fit it
  const bool bExpression = pType->IsDerivedFrom<plParticleExpressionNode>();
  if (bExpression)
  {
    ref_layout.m_sCode = pNode->GetTypeAccessor().GetValue("Expression").ConvertTo<plString>();

    const plStringBuilder sCode = ref_layout.m_sCode;
    plHybridArray<plStringView, 4> lines;
    sCode.Split(true, lines, "\n");

    float fWidest = 0.0f;
    for (plStringView sLine : lines)
      fWidest = plMath::Max(fWidest, plEditorUi::MeasureText(sLine, s_uiSmallSize));

    w = plMath::Clamp(fWidest + 2.0f * s_fPad + 16.0f, w, s_fMaxCodeWidth);

    const float fHeight = 8.0f + s_fCodeLine * plMath::Max(lines.GetCount(), 1u);
    ref_layout.m_CodeRect = plRectFloat(s_fPad, y, w - 2.0f * s_fPad, fHeight);
    y += fHeight + 4.0f;

    if (const plAbstractProperty* pCode = pType->FindPropertyByName("Expression"))
      plParticleGraphEditing::DescribeValue(pNode, pCode, ref_layout.m_CodeRow);
  }

  ref_layout.m_bHasPreview = plParticleGraphEditing::GetOperatorPreview(pNode, ref_layout.m_Preview);
  if (ref_layout.m_bHasPreview)
  {
    // asset thumbnails are square; the node grows to show them, up to a limit
    float fHeight = s_fPreviewBand;
    if (!ref_layout.m_Preview.m_bIsColor)
    {
      const plString sThumbnail = plParticleGraphEditing::GetThumbnailPath(ref_layout.m_Preview.m_Asset);
      if (!sThumbnail.IsEmpty() && plOSFile::ExistsFile(sThumbnail))
      {
        ref_layout.m_sThumbnail = sThumbnail;
        fHeight = plMath::Min(w - 2.0f * s_fPad, s_fMaxPreview);
      }
    }

    ref_layout.m_PreviewRect = plRectFloat(s_fPad, y, w - 2.0f * s_fPad, fHeight);
    y += fHeight + 2.0f;
  }
  else
  {
    plParticleGraphEditing::CollectOperatorRows<ValueRowLayout>(pNode, ref_layout.m_Rows);

    // the code has its box; the rows are the inputs
    if (bExpression)
    {
      for (plUInt32 i = ref_layout.m_Rows.GetCount(); i > 0; --i)
      {
        if (plStringUtils::IsEqual(ref_layout.m_Rows[i - 1].m_pProp->GetPropertyName(), "Expression"))
          ref_layout.m_Rows.RemoveAtAndCopy(i - 1);
      }
    }

    LayoutValueRows(ref_layout.m_Rows, s_fPad, w - 2.0f * s_fPad, s_fValueRow, 0.56f, y);
  }

  plParticleGraphCompiler compiler(m_pManager, FindEffectNode());
  ref_layout.m_Tier = compiler.GetTier(pNode);

  // the selected node shows its live values in the preview; a constant shows its value already
  if (const auto* pDocument = plDynamicCast<const plParticleEffectAssetDocument*>(m_pManager->GetDocument());
      pDocument != nullptr && pDocument->GetSpyNode() == pNode->GetGuid() && ref_layout.m_Tier != plParticleTier::Constant)
  {
    const plParticlePreviewStatusMsgToEditor& status = pDocument->GetPreviewStatus();
    const bool bCurrent = status.m_SpyNode == pNode->GetGuid();

    ref_layout.m_bSpy = true;
    ref_layout.m_uiSpyCount = bCurrent ? status.m_uiSpyCount : 0;
    ref_layout.m_fSpyMin = status.m_fSpyMin;
    ref_layout.m_fSpyAvg = status.m_fSpyAvg;
    ref_layout.m_fSpyMax = status.m_fSpyMax;
    ref_layout.m_SpyBins = status.m_SpyBins.GetArrayPtr();
    ref_layout.m_bSpyOnGPU = bCurrent && status.m_bSpyOnGPU;

    // a per-frame node has one value, so it gets no histogram
    const bool bHistogram = ref_layout.m_Tier == plParticleTier::Particle && ref_layout.m_uiSpyCount > 0;
    const float fHeight = bHistogram ? 70.0f : 38.0f;
    ref_layout.m_SpyRect = plRectFloat(s_fPad, y + 2.0f, w - 2.0f * s_fPad, fHeight);
    y += fHeight + 6.0f;
  }

  // a node that cannot produce a value says why, instead of its wires being dropped silently
  ref_layout.m_sIssue = compiler.GetIssue(pNode);

  // a blackboard entry the preview does not have reads as the node's default, and the node says so
  if (const auto* pDocument = plDynamicCast<const plParticleEffectAssetDocument*>(m_pManager->GetDocument()); pDocument != nullptr && ref_layout.m_sIssue.IsEmpty())
  {
    const plString sPath = plParticleEffectAssetDocument::GetBlackboardPath(pNode);
    if (!sPath.IsEmpty() && pDocument->GetPreviewStatus().m_MissingBlackboardEntries.Contains(sPath))
    {
      ref_layout.m_sIssue = "Not on the blackboard in the preview; reads Default.";
      ref_layout.m_bIssueIsNote = true;
    }
  }
  if (!ref_layout.m_sIssue.IsEmpty())
  {
    ref_layout.m_IssueRect = plRectFloat(s_fPad, y + 2.0f, w - 2.0f * s_fPad, 2.0f * s_fValueRow);
    y += 2.0f * s_fValueRow + 4.0f;
  }

  ref_layout.m_vSize.Set(w, y + 6.0f);

  // inputs sit on the row of the literal they override
  for (ValueRowLayout& row : ref_layout.m_Rows)
  {
    if (row.m_sPin.IsEmpty())
      continue;

    if (const plVisualGraphPin* pPin = m_pManager->GetInputPinByName(pNode, row.m_sPin))
    {
      row.m_bDriven = row.m_Kind != plParticleValueRow::Kind::PinOnly && m_pManager->HasConnections(*pPin);
      AddPin(out_pins, pPin, plVec2(0.0f, row.m_Rect.GetCenter().y));

      if (row.m_bDriven)
      {
        const plDocumentObject* pSource = compiler.GetInputSource(pNode, row.m_sPin);
        row.m_DrivenTier = compiler.GetTier(pSource);
        row.m_bDrivenUnresolved = !compiler.Fold(pSource).IsValid();
        row.m_sDrivenBy = compiler.Describe(pSource, 1);
      }
    }
  }

  // the output lines up with what the node produces: the preview, else the middle of the rows
  float fBodyCenter = ref_layout.m_fHeaderHeight * 0.5f;
  if (ref_layout.m_bHasPreview)
    fBodyCenter = ref_layout.m_PreviewRect.GetCenter().y;
  else if (!ref_layout.m_Rows.IsEmpty())
    fBodyCenter = (ref_layout.m_Rows[0].m_Rect.y + ref_layout.m_Rows.PeekBack().m_Rect.Bottom()) * 0.5f;

  for (const auto& pPin : m_pManager->GetOutputPins(pNode))
    AddPin(out_pins, pPin.Borrow(), plVec2(w, fBodyCenter));
}

//////////////////////////////////////////////////////////////////////////
// Elements

void plParticleEffectGraphController::BuildCustomNode(const plDocumentObject* pNode, plUiElement* pNodePanel, plStringView sNamePrefix) const
{
  // the nodes have no controls of their own: presses reach OnCustomNodePointer, which needs drags
  PL_IGNORE_UNUSED(sNamePrefix);

  const NodeLayout* pLayout = FindLayout(pNode);
  if (pLayout == nullptr)
    return;

  switch (pLayout->m_Type)
  {
    case NodeType::Effect:
      BuildEffect(*pLayout, pNodePanel);
      break;
    case NodeType::System:
      BuildSystem(pNode, *pLayout, pNodePanel);
      break;
    case NodeType::Operator:
      BuildOperator(*pLayout, pNodePanel);
      break;
    case NodeType::None:
      break;
  }
}

void plParticleEffectGraphController::BuildEffect(const NodeLayout& layout, plUiElement* pPanel) const
{
  const float w = layout.m_vSize.x;

  // the strip along the top edge marks where the systems arrive, in the colour of event reactions
  plUiPanel* pStrip = AddPanel(pPanel, plRectFloat(0, 0, w, s_fEffectStrip), plColorGammaUB(217, 96, 92));
  pStrip->m_vCornerRadius = plVec4(s_fCornerRadius, s_fCornerRadius, 0.0f, 0.0f);

  AddText(pPanel, plRectFloat(s_fPad, 7.0f, w - 2.0f * s_fPad, s_fTitleLine), layout.m_sTitle, s_uiTitleSize, plEditorUi::Foreground(), plUiTextHAlign::Center);
  AddText(pPanel, plRectFloat(s_fPad, 7.0f + s_fTitleLine, w - 2.0f * s_fPad, 18.0f), layout.m_sSubtitle, s_uiSmallSize, plEditorUi::Muted(), plUiTextHAlign::Center);
}

void plParticleEffectGraphController::BuildHeader(const NodeLayout& layout, plUiElement* pPanel) const
{
  const float w = layout.m_vSize.x;

  plUiPanel* pHeader = AddPanel(pPanel, plRectFloat(0, 0, w, layout.m_fHeaderHeight), plEditorUi::Raised());
  pHeader->m_vCornerRadius = plVec4(s_fCornerRadius, s_fCornerRadius, 0.0f, 0.0f);

  AddText(pPanel, plRectFloat(s_fPad, 6.0f, w - 2.0f * s_fPad, s_fTitleLine), layout.m_sTitle, s_uiTitleSize, plEditorUi::Foreground(), plUiTextHAlign::Center);

  if (layout.m_sSubtitle.IsEmpty())
    return;

  // a badge under the title: the category in its value colour, or the simulation target
  const bool bNeutral = layout.m_Type == NodeType::System;
  const plColorGammaUB badge = bNeutral ? plEditorUi::Muted() : layout.m_BadgeColor;
  const float fBadgeWidth = plMath::Min(plEditorUi::MeasureText(layout.m_sSubtitle, s_uiSmallSize) + 12.0f, w - 2.0f * s_fPad);
  const plRectFloat badgeRect((w - fBadgeWidth) * 0.5f, 6.0f + s_fTitleLine + 2.0f, fBadgeWidth, s_fBadgeHeight);

  AddPanel(pPanel, badgeRect, WithAlpha(badge, 45), 4.0f);
  AddText(pPanel, badgeRect, layout.m_sSubtitle, s_uiSmallSize, badge, plUiTextHAlign::Center);
}

void plParticleEffectGraphController::BuildSystem(const plDocumentObject* pNode, const NodeLayout& layout, plUiElement* pPanel) const
{
  BuildHeader(layout, pPanel);

  // the GPU verdict: what the runtime's lowering would say, before the effect is ever played
  if (layout.m_bTargetsGpu)
  {
    const bool bLowers = layout.m_sLoweringReason.IsEmpty();
    const plColorGammaUB tint = bLowers ? plEditorUi::Accent() : plEditorUi::Danger();

    plStringBuilder sText;
    if (bLowers)
      sText = "Lowered to GPU";
    else
      sText.SetFormat("Falls back to CPU: {0}", layout.m_sLoweringReason);

    AddPanel(pPanel, layout.m_VerdictRect, WithAlpha(tint, 38), 3.0f);
    AddText(pPanel, Inset(layout.m_VerdictRect, 6.0f, 6.0f), sText, s_uiSmallSize, tint);
  }

  BuildAttributes(layout, pPanel);

  for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
    BuildContext(pNode, layout, uiContext, pPanel);

  if (!layout.m_sEventNote.IsEmpty())
  {
    AddPanel(pPanel, layout.m_EventNoteRect, WithAlpha(s_EventColor, 18), 2.0f);
    AddPanel(pPanel, plRectFloat(layout.m_EventNoteRect.x, layout.m_EventNoteRect.y, 2.0f, layout.m_EventNoteRect.height), s_EventColor);
    AddText(pPanel, Inset(layout.m_EventNoteRect, 8.0f, 6.0f), layout.m_sEventNote, s_uiSmallSize, Mix(s_EventColor, plEditorUi::Foreground(), 0.55f));
  }
}

void plParticleEffectGraphController::BuildAttributes(const NodeLayout& layout, plUiElement* pPanel) const
{
  if (layout.m_Attributes.IsEmpty())
    return;

  // more than a system holds: the count turns into a warning
  const plUInt32 uiCount = layout.m_Attributes.GetCount();
  const bool bTooMany = uiCount > plParticleEffectAssetDocument::s_uiMaxCustomAttributes;

  plStringBuilder sTitle;
  sTitle.SetFormat("CUSTOM ATTRIBUTES ({}/{})", uiCount, plParticleEffectAssetDocument::s_uiMaxCustomAttributes);
  AddText(pPanel, layout.m_AttributesTitleRect, sTitle, s_uiChipSize, bTooMany ? plEditorUi::Danger() : plEditorUi::Dim());

  plUiButton* pAdd = AddHoverPlate(pPanel, layout.m_AddAttributeRect, plColorGammaUB(0, 0, 0, 0), 3.0f);
  pAdd->m_HoverColor = plEditorUi::Raised();
  pAdd->m_PressedColor = plEditorUi::Raised();
  AddText(pPanel, layout.m_AddAttributeRect, "+ Add", s_uiChipSize, plEditorUi::Dim(), plUiTextHAlign::Center);

  for (const AttributeLayout& attribute : layout.m_Attributes)
  {
    AddText(pPanel, Inset(attribute.m_Rect, 8.0f, 8.0f), attribute.m_sName, s_uiSmallSize, plEditorUi::Foreground());
    AddText(pPanel, Inset(attribute.m_Rect, 8.0f, 8.0f), attribute.m_sType, s_uiSmallSize, plEditorUi::Muted(), plUiTextHAlign::Right);
  }

  // a rule under the list, as under the header
  AddPanel(pPanel, plRectFloat(layout.m_AttributesRect.x, layout.m_AttributesRect.Bottom() - 1.0f, layout.m_AttributesRect.width, 1.0f), plEditorUi::Line());
}

void plParticleEffectGraphController::BuildContext(const plDocumentObject* pNode, const NodeLayout& layout, plUInt32 uiContext, plUiElement* pPanel) const
{
  const ContextLayout& context = layout.m_Contexts[uiContext];
  const plParticleContext::Enum contextEnum = (plParticleContext::Enum)uiContext;
  const plColorGammaUB accent = plParticleGraphEditing::GetContextColor(contextEnum);

  // the context box and its accent bar
  AddPanel(pPanel, context.m_Rect, plEditorUi::Field(), 4.0f);
  AddPanel(pPanel, plRectFloat(context.m_Rect.x, context.m_Rect.y + 2.0f, s_fAccentWidth, context.m_Rect.height - 4.0f), accent);

  // header: disclosure marker and label in the context colour; a folded context shows its block count
  plStringBuilder sLabel = context.m_bCollapsed ? plEditorUi::TreeCollapsed : plEditorUi::TreeExpanded;
  plStringBuilder sName = plParticleContextInfo::Get(contextEnum).m_szLabel;
  sName.ToUpper();
  sLabel.Append(sName);

  const plRectFloat labelRect = Inset(context.m_HeaderRect, s_fAccentWidth + 6.0f, 8.0f);
  AddText(pPanel, labelRect, sLabel, s_uiSmallSize, accent);

  if (context.m_bCollapsed)
  {
    plStringBuilder sCount;
    sCount.SetFormat("{0}", context.m_Blocks.GetCount());
    AddText(pPanel, labelRect, sCount, s_uiSmallSize, plEditorUi::Dim(), plUiTextHAlign::Right);
    return;
  }

  // the size of the context's per-particle programs, in the per-particle tier colour
  if (!context.m_sProgram.IsEmpty())
  {
    const plColorGammaUB tier = GetTierColor(plParticleTier::Particle);
    AddFrame(pPanel, context.m_ProgramRect, WithAlpha(tier, 128), 3.0f);
    AddText(pPanel, context.m_ProgramRect, context.m_sProgram, s_uiChipSize, tier, plUiTextHAlign::Center);
  }

  for (const BlockLayout& block : context.m_Blocks)
    BuildBlock(block, accent, pPanel);

  // drop indicator while a block of this context is dragged
  const bool bDropHere = m_Press.m_bDragging && m_Press.m_Target == Press::Target::Block && m_Press.m_pNode == pNode && m_Press.m_iContext == (plInt32)uiContext && m_iDropIndex >= 0;
  if (bDropHere)
  {
    float fY = context.m_AddRect.y - 2.0f;
    if (!context.m_Blocks.IsEmpty())
      fY = m_iDropIndex >= (plInt32)context.m_Blocks.GetCount() ? context.m_Blocks.PeekBack().m_Rect.Bottom() + 1.0f : context.m_Blocks[m_iDropIndex].m_Rect.y - 1.0f;

    AddPanel(pPanel, plRectFloat(context.m_Rect.x + s_fAccentWidth, fY - 1.0f, context.m_Rect.width - s_fAccentWidth, 2.0f), plEditorUi::Accent());
  }

  plUiButton* pAdd = AddHoverPlate(pPanel, context.m_AddRect, plColorGammaUB(0, 0, 0, 0), 3.0f);
  pAdd->m_HoverColor = plEditorUi::Raised();
  pAdd->m_PressedColor = plEditorUi::Raised();
  pAdd->m_fBorderWidth = 1.0f;
  pAdd->m_BorderColor = plEditorUi::Line();
  AddText(pPanel, context.m_AddRect, "+ Add block", s_uiSmallSize, plEditorUi::Dim(), plUiTextHAlign::Center);
}

void plParticleEffectGraphController::BuildBlock(const BlockLayout& block, plColorGammaUB accent, plUiElement* pPanel) const
{
  const plDocumentObject* pObject = m_pManager->GetObject(block.m_Block);
  const bool bSelected = pObject != nullptr && IsBlockSelected(pObject);
  const bool bDragged = m_Press.m_bDragging && m_Press.m_Target == Press::Target::Block && m_Press.m_Object == block.m_Block;

  plColorGammaUB fill = plEditorUi::Raised();
  if (block.m_bBlocksLowering)
    fill = Mix(fill, plEditorUi::Danger(), 0.25f);
  if (bSelected)
    fill = plEditorUi::Selection();

  plUiButton* pPlate = AddHoverPlate(pPanel, block.m_Rect, fill, 3.0f);
  if (bSelected || block.m_bBlocksLowering)
  {
    pPlate->m_fBorderWidth = 1.0f;
    pPlate->m_BorderColor = bSelected ? plEditorUi::Accent() : plEditorUi::Danger();
  }

  if (bDragged)
    pPlate->m_fOpacity = 0.4f;

  // disclosure marker in the context colour, then the name; the right side carries the summary
  AddText(pPanel, plRectFloat(block.m_Rect.x + 4.0f, block.m_Rect.y, 14.0f, block.m_Rect.height), block.m_bExpanded ? plEditorUi::TreeExpanded : plEditorUi::TreeCollapsed, s_uiSmallSize, accent);

  const plRectFloat nameRect = Inset(block.m_Rect, 18.0f, 26.0f);
  plColorGammaUB nameColor = plEditorUi::Foreground();
  if (!block.m_bEnabled)
    nameColor = plEditorUi::Dim();
  else if (block.m_bBlocksLowering)
    nameColor = Mix(plEditorUi::Danger(), plEditorUi::Foreground(), 0.4f);

  const float fNameWidth = plMath::Min(plEditorUi::MeasureText(block.m_sTitle, plEditorUi::FontSize) + 4.0f, nameRect.width);
  AddText(pPanel, plRectFloat(nameRect.x, nameRect.y, fNameWidth, nameRect.height), block.m_sTitle, plEditorUi::FontSize, nameColor);

  plRectFloat sideRect = Inset(nameRect, fNameWidth + 6.0f, 0.0f);

  // what a Set Attribute writes, right after its name: "Set Attribute · Color.A"
  if (!block.m_sDetail.IsEmpty())
  {
    plStringBuilder sDetail;
    sDetail.Set("\xC2\xB7 ", block.m_sDetail); // ·
    const float fDetailWidth = plMath::Min(plEditorUi::MeasureText(sDetail, s_uiSmallSize) + 4.0f, sideRect.width);
    AddText(pPanel, plRectFloat(sideRect.x, sideRect.y, fDetailWidth, sideRect.height), sDetail, s_uiSmallSize, plEditorUi::Muted());
    sideRect = Inset(sideRect, fDetailWidth + 4.0f, 0.0f);
  }

  if (block.m_bBlocksLowering)
  {
    AddText(pPanel, sideRect, "CPU only", s_uiSmallSize, plEditorUi::Danger(), plUiTextHAlign::Right);
  }
  else if (block.m_bConverted)
  {
    // made from a 1.3 block when the document opened; only marked in that session
    const float fTagWidth = plEditorUi::MeasureText("converted", s_uiChipSize) + 10.0f;
    const plRectFloat tagRect(sideRect.Right() - fTagWidth, sideRect.y + 4.0f, fTagWidth, sideRect.height - 8.0f);
    AddFrame(pPanel, tagRect, plEditorUi::Line(), 3.0f);
    AddText(pPanel, tagRect, "converted", s_uiChipSize, plEditorUi::Muted(), plUiTextHAlign::Center);
  }
  else if (!block.m_sEvent.IsEmpty())
  {
    // the event this block raises or spawns on, open or closed; the event wires end on this row
    AddText(pPanel, sideRect, block.m_sEvent, s_uiSmallSize, block.m_bEventUnmatched ? plEditorUi::Warning() : s_EventColor, plUiTextHAlign::Right);
  }
  else if (!block.m_bExpanded && !block.m_sSummary.IsEmpty())
  {
    AddText(pPanel, sideRect, block.m_sSummary, s_uiSmallSize, plEditorUi::Dim(), plUiTextHAlign::Right);
  }

  // enable toggle; a disabled block stays authored but is dropped when the descriptor is built
  AddFrame(pPanel, block.m_EnableRect, plEditorUi::Muted(), 2.0f, block.m_bEnabled ? accent : plColorGammaUB(0, 0, 0, 0));

  for (const ValueRowLayout& row : block.m_Rows)
    BuildValueRow(row, accent, pPanel);
}

void plParticleEffectGraphController::BuildValueRow(const ValueRowLayout& row, plColorGammaUB accent, plUiElement* pPanel) const
{
  // a wired row gives the label less room: what the wire carries is the longer text
  const float fLabelWidth = row.m_bDriven ? row.m_Rect.width * 0.38f : row.m_ValueRect.x - row.m_Rect.x - 6.0f;
  const plRectFloat labelRect(row.m_Rect.x, row.m_Rect.y, plMath::Max(fLabelWidth, 0.0f), row.m_Rect.height);
  plRectFloat drivenRect(row.m_Rect.x + row.m_Rect.width * 0.4f, row.m_Rect.y, row.m_Rect.width * 0.6f, row.m_Rect.height);
  AddText(pPanel, labelRect, row.m_sLabel, s_uiSmallSize, plEditorUi::Muted());

  if (row.m_Kind == plParticleValueRow::Kind::PinOnly)
    return;

  plRectFloat value = row.m_ValueRect;

  // the properties that take per-particle wires say so, wired or not, so a green wire's targets show
  if (row.m_bPerParticle)
  {
    const plColorGammaUB tier = GetTierColor(plParticleTier::Particle);
    const float fChipWidth = plEditorUi::MeasureText("per particle", s_uiChipSize) + 8.0f;
    const plRectFloat chipRect(row.m_Rect.Right() - fChipWidth, row.m_Rect.y + 3.0f, fChipWidth, row.m_Rect.height - 6.0f);

    AddFrame(pPanel, chipRect, WithAlpha(tier, 115), 3.0f);
    AddText(pPanel, chipRect, "per particle", s_uiChipSize, tier, plUiTextHAlign::Center);

    value.width = plMath::Max(value.width - fChipWidth - 4.0f, 0.0f);
    drivenRect.width = plMath::Max(drivenRect.width - fChipWidth - 4.0f, 0.0f);
  }

  const float fCenterY = value.GetCenter().y;

  // a wire supplies this one: show what it carries, in the colour of when it is computed; the typed
  // value is unused (the inspector still shows it)
  if (row.m_bDriven)
  {
    if (row.m_bDrivenUnresolved)
    {
      plStringBuilder sText;
      sText.SetFormat("{}: no value, using {}", row.m_sDrivenBy, row.m_sValue);
      AddText(pPanel, drivenRect, sText, s_uiSmallSize, plEditorUi::Danger(), plUiTextHAlign::Right);
      return;
    }

    plStringBuilder sText;
    sText.Set("\xE2\x86\x90 ", row.m_sDrivenBy.IsEmpty() ? plStringView("wired") : row.m_sDrivenBy.GetView()); // ←
    AddText(pPanel, drivenRect, sText, s_uiSmallSize, GetTierColor(row.m_DrivenTier), plUiTextHAlign::Right);
    return;
  }

  switch (row.m_Kind)
  {
    case plParticleValueRow::Kind::Bool:
      AddFrame(pPanel, plRectFloat(value.x, fCenterY - 5.0f, 10.0f, 10.0f), plEditorUi::Muted(), 2.0f, row.m_bBoolValue ? accent : plColorGammaUB(0, 0, 0, 0));
      break;

    case plParticleValueRow::Kind::Color:
    {
      plColorGammaUB swatch = row.m_Color;
      swatch.a = 255;
      AddFrame(pPanel, plRectFloat(value.Right() - 26.0f, fCenterY - 5.0f, 24.0f, 10.0f), plEditorUi::Muted(), 2.0f, swatch);
      break;
    }

    case plParticleValueRow::Kind::Enum:
      // the caret marks values that open a menu in place
      AddText(pPanel, Inset(value, 0.0f, 12.0f), row.m_sValue, s_uiSmallSize, plEditorUi::Foreground(), plUiTextHAlign::Right);
      AddText(pPanel, plRectFloat(value.Right() - 10.0f, value.y, 10.0f, value.height), "\xE2\x96\xBE", s_uiSmallSize, plEditorUi::Dim(), plUiTextHAlign::Center); // ▾
      break;

    case plParticleValueRow::Kind::Number:
    {
      // a faint underline marks the fields that scrub
      AddPanel(pPanel, plRectFloat(value.x + 2.0f, value.Bottom() - 1.0f, value.width - 3.0f, 1.0f), WithAlpha(plEditorUi::Dim(), 120));
      AddText(pPanel, value, row.m_sValue, s_uiSmallSize, plEditorUi::Foreground(), plUiTextHAlign::Right);

      if (row.m_bHasVariance)
      {
        const plRectFloat& deviation = row.m_ValueRect2;
        AddPanel(pPanel, plRectFloat(deviation.x + 2.0f, deviation.Bottom() - 1.0f, deviation.width - 3.0f, 1.0f), WithAlpha(plEditorUi::Dim(), 120));
        AddText(pPanel, deviation, row.m_sValue2, s_uiSmallSize, plEditorUi::Dim(), plUiTextHAlign::Right);
      }
      break;
    }

    case plParticleValueRow::Kind::Asset:
    {
      // the small square stands in for the asset browser button
      const bool bAssigned = row.m_sValue != "none";
      AddText(pPanel, Inset(value, 0.0f, 14.0f), row.m_sValue, s_uiSmallSize, bAssigned ? plEditorUi::Foreground() : plEditorUi::Dim(), plUiTextHAlign::Right);
      AddFrame(pPanel, plRectFloat(value.Right() - 9.0f, fCenterY - 4.5f, 9.0f, 9.0f), plEditorUi::Muted(), 0.0f);
      break;
    }

    default:
      AddText(pPanel, value, row.m_sValue, s_uiSmallSize, plEditorUi::Foreground(), plUiTextHAlign::Right);
      break;
  }
}

void plParticleEffectGraphController::BuildOperator(const NodeLayout& layout, plUiElement* pPanel) const
{
  BuildHeader(layout, pPanel);

  if (layout.m_bHasPreview)
    BuildPreview(layout, pPanel);

  // the code of an Expression node, as typed
  if (layout.m_CodeRect.width > 0.0f)
  {
    plUiButton* pBox = AddHoverPlate(pPanel, layout.m_CodeRect, plEditorUi::Field(), 3.0f);
    pBox->m_fBorderWidth = 1.0f;
    pBox->m_BorderColor = plEditorUi::Line();

    plUiText* pCode = AddText(pPanel, Inset(plRectFloat(layout.m_CodeRect.x, layout.m_CodeRect.y + 4.0f, layout.m_CodeRect.width, layout.m_CodeRect.height - 8.0f), 6.0f, 6.0f),
      layout.m_sCode.IsEmpty() ? plStringView("-") : layout.m_sCode.GetView(), s_uiSmallSize, plEditorUi::Foreground());
    pCode->m_VAlign = plUiTextVAlign::Top;
    pCode->m_bWrap = true;
  }

  for (const ValueRowLayout& row : layout.m_Rows)
    BuildValueRow(row, plEditorUi::Accent(), pPanel);

  if (layout.m_bSpy)
    BuildSpy(layout, pPanel);

  if (!layout.m_sIssue.IsEmpty())
  {
    // a note (a blackboard entry the preview lacks) is information, not a fault
    const plColorGammaUB color = layout.m_bIssueIsNote ? plEditorUi::Muted() : plEditorUi::Danger();
    AddPanel(pPanel, layout.m_IssueRect, WithAlpha(color, 38), 3.0f);

    plUiText* pText = AddText(pPanel, Inset(layout.m_IssueRect, 6.0f, 6.0f), layout.m_sIssue, s_uiSmallSize, color);
    pText->m_bWrap = true;
  }
}

void plParticleEffectGraphController::BuildSpy(const NodeLayout& layout, plUiElement* pPanel) const
{
  const plRectFloat& rect = layout.m_SpyRect;
  const plColorGammaUB tier = GetTierColor(layout.m_Tier);
  const bool bPerParticle = layout.m_Tier == plParticleTier::Particle;

  AddFrame(pPanel, rect, plEditorUi::Line(), 4.0f, plEditorUi::Field());

  // header: what the values come from, and their tier
  plStringBuilder sHeader;
  if (layout.m_uiSpyCount == 0)
    sHeader = "LIVE \xC2\xB7 NO PARTICLES"; // ·
  else if (bPerParticle)
    sHeader.SetFormat("LIVE \xC2\xB7 {} PARTICLES", layout.m_uiSpyCount);
  else
    sHeader = "LIVE";

  const plRectFloat headerRect(rect.x + 7.0f, rect.y + 3.0f, rect.width - 14.0f, 14.0f);
  AddText(pPanel, headerRect, sHeader, s_uiChipSize, plEditorUi::Dim());
  const char* szTier = bPerParticle ? (layout.m_bSpyOnGPU ? "per particle \xC2\xB7 GPU" : "per particle") : "per frame";
  AddText(pPanel, headerRect, szTier, s_uiChipSize, tier, plUiTextHAlign::Right);

  if (layout.m_uiSpyCount == 0)
  {
    AddText(pPanel, plRectFloat(rect.x + 7.0f, rect.y + 18.0f, rect.width - 14.0f, 16.0f), "Nothing alive in the preview.", s_uiSmallSize, plEditorUi::Dim());
    return;
  }

  auto format = [](float f) -> plString
  {
    plStringBuilder s;
    s.SetFormat("{}", plArgF(f, plMath::Abs(f) < 10.0f ? 2 : 1));
    return s;
  };

  if (!bPerParticle)
  {
    AddText(pPanel, plRectFloat(rect.x + 7.0f, rect.y + 18.0f, rect.width - 14.0f, 16.0f), format(layout.m_fSpyAvg), s_uiSmallSize, plEditorUi::Foreground());
    return;
  }

  // the histogram from min to max, scaled to its fullest bin
  const plRectFloat histRect(rect.x + 7.0f, rect.y + 19.0f, rect.width - 14.0f, 30.0f);
  plUInt32 uiPeak = 1;
  for (plUInt32 uiBin : layout.m_SpyBins)
    uiPeak = plMath::Max(uiPeak, uiBin);

  const plUInt32 uiBins = layout.m_SpyBins.GetCount();
  if (uiBins > 0)
  {
    const float fBinWidth = histRect.width / uiBins;
    for (plUInt32 i = 0; i < uiBins; ++i)
    {
      const float fHeight = plMath::Max(2.0f, histRect.height * layout.m_SpyBins[i] / uiPeak);
      AddPanel(pPanel, plRectFloat(histRect.x + i * fBinWidth, histRect.Bottom() - fHeight, plMath::Max(fBinWidth - 1.0f, 1.0f), fHeight), WithAlpha(tier, 190), 1.0f);
    }
  }

  // min, average, max under it
  const plRectFloat numsRect(rect.x + 7.0f, histRect.Bottom() + 3.0f, rect.width - 14.0f, 16.0f);
  plStringBuilder sMin, sAvg, sMax;
  sMin.Set("min ", format(layout.m_fSpyMin));
  sAvg.Set("avg ", format(layout.m_fSpyAvg));
  sMax.Set("max ", format(layout.m_fSpyMax));
  AddText(pPanel, numsRect, sMin, s_uiSmallSize, plEditorUi::Foreground());
  AddText(pPanel, numsRect, sAvg, s_uiSmallSize, plEditorUi::Foreground(), plUiTextHAlign::Center);
  AddText(pPanel, numsRect, sMax, s_uiSmallSize, plEditorUi::Foreground(), plUiTextHAlign::Right);
}

const plDocumentObject* plParticleEffectGraphController::FindEffectNode() const
{
  for (const plDocumentObject* pChild : m_pManager->GetRootObject()->GetChildren())
  {
    if (pChild->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleEffectNode>())
      return pChild;
  }

  return nullptr;
}

plColorGammaUB plParticleEffectGraphController::GetTierColor(plParticleTier tier)
{
  switch (tier)
  {
    case plParticleTier::Frame:
      return plColorGammaUB(145, 140, 210);
    case plParticleTier::Particle:
      return plColorGammaUB(170, 205, 120);
    default:
      return plColorGammaUB(145, 150, 160);
  }
}

void plParticleEffectGraphController::BuildPreview(const NodeLayout& layout, plUiElement* pPanel) const
{
  const plRectFloat& rect = layout.m_PreviewRect;

  if (layout.m_Preview.m_bIsColor)
  {
    plColorGammaUB color = layout.m_Preview.m_Color;
    color.a = 255;
    AddFrame(pPanel, rect, plEditorUi::Border(), 3.0f, color);
    return;
  }

  if (!layout.m_sThumbnail.IsEmpty())
  {
    // the asset's own thumbnail works for every asset kind without reading any of them
    plUiImage* pImage = PL_DEFAULT_NEW(plUiImage);
    Place(pImage, rect);
    pImage->m_BrushMode = plUiBrushMode::Image;
    pImage->m_hTexture = plResourceManager::LoadResource<plTexture2DResource>(layout.m_sThumbnail);
    pImage->m_Color = plColorGammaUB(255, 255, 255);
    pImage->m_vCornerRadius = plVec4(3.0f);
    pPanel->m_Children.PushBack(pImage);

    AddFrame(pPanel, rect, plEditorUi::Border(), 3.0f);
    return;
  }

  // no thumbnail yet (or no asset): the reference as text
  AddFrame(pPanel, rect, plEditorUi::Line(), 3.0f);
  AddText(pPanel, Inset(rect, 6.0f, 6.0f), plParticleGraphEditing::GetAssetDisplayName(layout.m_Preview.m_Original), s_uiSmallSize, plEditorUi::Dim(), plUiTextHAlign::Center);
}

bool plParticleEffectGraphController::IsBlockSelected(const plDocumentObject* pBlock) const
{
  return m_pManager->GetDocument()->GetSelectionManager()->GetSelection().IndexOf(pBlock) != plInvalidIndex;
}
