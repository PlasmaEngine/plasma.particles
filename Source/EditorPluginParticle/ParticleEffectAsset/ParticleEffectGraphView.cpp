#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectGraphView.h>
#include <EditorUi/Surface.h>
#include <GuiFoundation/UIServices/ImageCache.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Selection/SelectionManager.h>

plParticleEffectGraphController::plParticleEffectGraphController()
{
  // systems stack vertically and feed straight down into the effect, so a routed wire only adds noise
  SetWireStyle(plVisualGraphWireStyle::StraightLine);
}

plParticleEffectGraphController::~plParticleEffectGraphController()
{
  if (m_ThumbnailSubscription != 0 && plImageCache::GetSingleton() != nullptr)
    plImageCache::GetSingleton()->m_ImageInvalidated.RemoveEventHandler(m_ThumbnailSubscription);

  if (m_bSubscribed)
  {
    auto* pManager = const_cast<plVisualGraphObjectManager*>(m_pManager);
    pManager->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plParticleEffectGraphController::StructureEventHandler, this));
    m_pManager->GetDocument()->GetSelectionManager()->m_Events.RemoveEventHandler(plMakeDelegate(&plParticleEffectGraphController::SelectionEventHandler, this));

    if (auto* pDocument = GetEffectDocument())
      pDocument->m_Events.RemoveEventHandler(plMakeDelegate(&plParticleEffectGraphController::EffectEventHandler, this));
  }
}

plParticleEffectAssetDocument* plParticleEffectGraphController::GetEffectDocument() const
{
  return const_cast<plParticleEffectAssetDocument*>(plDynamicCast<const plParticleEffectAssetDocument*>(m_pManager->GetDocument()));
}

void plParticleEffectGraphController::Init(const plVisualGraphObjectManager* pManager)
{
  plVisualGraphController::Init(pManager);

  // Blocks are not graph nodes, so the view does not rebuild when one is added, removed or moved, and
  // block rows show the selection. Structure events are only published on the mutable manager.
  auto* pMutableManager = const_cast<plVisualGraphObjectManager*>(pManager);
  pMutableManager->m_StructureEvents.AddEventHandler(plMakeDelegate(&plParticleEffectGraphController::StructureEventHandler, this));
  pManager->GetDocument()->GetSelectionManager()->m_Events.AddEventHandler(plMakeDelegate(&plParticleEffectGraphController::SelectionEventHandler, this));

  if (auto* pDocument = GetEffectDocument())
    pDocument->m_Events.AddEventHandler(plMakeDelegate(&plParticleEffectGraphController::EffectEventHandler, this));

  m_bSubscribed = true;

  // A regenerated thumbnail replaces the texture the preview shows (the property grid reloads the
  // resource); rebuilding picks up thumbnails that did not exist before.
  if (plImageCache::GetSingleton() != nullptr && m_ThumbnailSubscription == 0)
  {
    m_ThumbnailSubscription = plImageCache::GetSingleton()->m_ImageInvalidated.AddEventHandler([this](const plImageCacheEvent&)
      { InvalidateLook(); });
  }
}

//////////////////////////////////////////////////////////////////////////
// Look

void plParticleEffectGraphController::GetNodeLook(const plDocumentObject* pNode, plVisualGraphNodeLook& out_look) const
{
  plVisualGraphController::GetNodeLook(pNode, out_look);

  if (GetNodeType(pNode) == NodeType::None)
    return;

  out_look.m_Kind = plVisualGraphNodeLook::Kind::Custom;

  // the GPU verdict in full, which the node's verdict line may cut short
  if (const NodeLayout* pLayout = FindLayout(pNode); pLayout != nullptr && !pLayout->m_sLoweringReason.IsEmpty())
  {
    plStringBuilder sTooltip;
    sTooltip.SetFormat("Falls back to CPU: {0}", pLayout->m_sLoweringReason);
    out_look.m_sTooltip = sTooltip;
  }
}

void plParticleEffectGraphController::GetPinLook(const plVisualGraphPin& pin, plVisualGraphPinLook& out_look) const
{
  plVisualGraphController::GetPinLook(pin, out_look);

  if (GetNodeType(pin.GetParent()) == NodeType::None)
    return;

  // custom nodes show what a pin is next to it; the pin itself is a dot with a tooltip
  out_look.m_sLabel.Clear();

  plStringBuilder sKey;
  plStringBuilder sProperty;
  if (plParticlePropertyPin::Parse(pin.GetName(), sKey, sProperty))
  {
    plStringBuilder sTooltip = plTranslate(sProperty);
    if (const plDocumentObject* pBlock = plParticlePropertyPin::FindBlock(pin.GetParent(), pin.GetName(), sProperty))
      sTooltip.PrependFormat("{0}: ", plParticleGraphEditing::GetBlockDisplayName(pBlock->GetTypeAccessor().GetType()));

    out_look.m_sTooltip = sTooltip;
  }
  else
  {
    out_look.m_sTooltip = plTranslate(pin.GetName());
  }

  // an operator's output shows when its value is computed, like the wires leaving it
  if (pin.GetType() == plVisualGraphPin::Type::Output && GetNodeType(pin.GetParent()) == NodeType::Operator)
  {
    plParticleGraphCompiler compiler(m_pManager, FindEffectNode());
    out_look.m_Color = GetTierColor(compiler.GetTier(pin.GetParent()));
  }
}

void plParticleEffectGraphController::GetConnectionLook(const plDocumentObject* pConnection, plVisualGraphConnectionLook& out_look) const
{
  plVisualGraphController::GetConnectionLook(pConnection, out_look);

  const plVisualGraphConnection& connection = m_pManager->GetConnection(pConnection);
  const auto& sourcePin = static_cast<const plParticleGraphPin&>(connection.GetSourcePin());

  if (sourcePin.m_Category != plParticleGraphPinCategory::Value)
    return;

  // value wires are coloured by when they are computed: constant, per frame or per particle
  plParticleGraphCompiler compiler(m_pManager, FindEffectNode());
  const plParticleTier tier = compiler.GetTier(sourcePin.GetParent());

  out_look.m_SourceColor = GetTierColor(tier);
  out_look.m_TargetColor = out_look.m_SourceColor;

  switch (tier)
  {
    case plParticleTier::Frame:
      out_look.m_sTooltip = "Per frame: computed once per simulation step";
      break;
    case plParticleTier::Particle:
      out_look.m_sTooltip = "Per particle: computed for every particle";
      break;
    default:
      out_look.m_sTooltip = "Constant: folded when the asset is built";
      break;
  }
}

//////////////////////////////////////////////////////////////////////////
// Menus and keys

void plParticleEffectGraphController::ExtendNodeMenu(const plDocumentObject* pNode, plVisualGraphMenu& ref_menu)
{
  if (GetNodeType(pNode) != NodeType::System)
    return;

  m_MenuSystem = pNode->GetGuid();
  ref_menu.AddSeparator();

  // chaining: a second system that spawns where this one's particles die
  ref_menu.AddAction("Spawn a System on Its Deaths", [this]()
    {
      auto* pDocument = GetEffectDocument();
      const plDocumentObject* pSystem = m_pManager->GetObject(m_MenuSystem);
      if (pDocument != nullptr && pSystem != nullptr)
        pDocument->AddSystemOnDeaths(pSystem).LogFailure();
    });

  ref_menu.AddAction("Save as Template...", [this]()
    {
      RunDeferred([this]()
        {
          const plDocumentObject* pSystem = m_pManager->GetObject(m_MenuSystem);
          const auto* pDocument = plDynamicCast<const plParticleEffectAssetDocument*>(m_pManager->GetDocument());
          if (pSystem != nullptr && pDocument != nullptr)
            plParticleGraphEditing::SaveSystemAsTemplate(pDocument, pSystem);
        });
    });
}

bool plParticleEffectGraphController::OnKey(const plEditorUiKeyEvent& e)
{
  // Delete on a selected block removes that block, not the system it belongs to
  if (e.m_Key == plEditorUiKeyEvent::Key::Delete)
    return plParticleGraphEditing::RemoveSelectedBlock(m_pManager->GetDocument());

  return false;
}

void plParticleEffectGraphController::ShowEnumMenu(const plVec2& vGraph)
{
  plDynamicArray<plReflectionUtils::EnumKeyValuePair> entries;
  plParticleGraphEditing::GetEnumChoices(m_MenuRow, entries);

  if (entries.IsEmpty() || !m_ShowMenu.IsValid())
    return;

  const plInt64 iCurrent = m_MenuRow.m_Original.ConvertTo<plInt64>();

  // a Set Attribute's components read as its target names them: R, G, B, A for a colour
  const plDocumentObject* pMenuObject = m_pManager->GetObject(m_MenuObject);
  if (pMenuObject != nullptr && m_MenuRow.m_pProp != nullptr && plStringUtils::IsEqual(m_MenuRow.m_pProp->GetPropertyName(), "Component") &&
      !plParticleGraphEditing::GetSinkTargetLabel(pMenuObject).IsEmpty())
  {
    const auto& accessor = pMenuObject->GetTypeAccessor();
    for (auto& entry : entries)
      entry.m_sKey = plParticleGraphEditing::GetSinkComponentLabel(accessor.GetValue("Target").ConvertTo<plInt64>(), accessor.GetValue("CustomType").ConvertTo<plInt64>(), entry.m_iValue);
  }

  plVisualGraphMenu menu;
  for (const auto& entry : entries)
  {
    const plInt64 iValue = entry.m_iValue;
    plEditorUiMenuItem& item = menu.AddAction(entry.m_sKey, [this, iValue]()
      {
        if (m_pManager->GetObject(m_MenuObject) != nullptr)
          plParticleGraphEditing::SetValue(m_pManager->GetDocument(), m_MenuObject, m_MenuRow.m_pProp, plVariant(iValue));
      });

    item.m_bCheckable = true;
    item.m_bChecked = iValue == iCurrent;
  }

  m_ShowMenu(vGraph, menu);
}

void plParticleEffectGraphController::ShowAddMenu(const plDocumentObject* pSystem, plParticleContext::Enum context, const plVec2& vGraph)
{
  if (!m_ShowMenu.IsValid())
    return;

  m_MenuSystem = pSystem->GetGuid();
  m_MenuContext = context;
  plParticleGraphEditing::GetBlockChoices(context, m_MenuBlockChoices);

  if (m_MenuBlockChoices.IsEmpty())
    return;

  plVisualGraphMenu menu;
  menu.m_Menu.AddHeader(plParticleContextInfo::Get(context).m_szLabel);

  for (plUInt32 i = 0; i < m_MenuBlockChoices.GetCount(); ++i)
  {
    menu.AddAction(m_MenuBlockChoices[i].m_sName, [this, i]()
      {
        if (const plDocumentObject* pTarget = m_pManager->GetObject(m_MenuSystem); pTarget != nullptr && i < m_MenuBlockChoices.GetCount())
          plParticleGraphEditing::AddBlock(m_pManager->GetDocument(), pTarget, m_MenuContext, m_MenuBlockChoices[i].m_pType);
      });
  }

  m_ShowMenu(vGraph, menu);
}

void plParticleEffectGraphController::ShowBlockMenu(const plDocumentObject* pSystem, const plVec2& vGraph)
{
  const plDocumentObject* pBlock = m_pManager->GetObject(m_Press.m_Object);
  const NodeLayout* pLayout = FindLayout(pSystem);
  if (pBlock == nullptr || pLayout == nullptr || m_Press.m_iContext < 0 || !m_ShowMenu.IsValid())
    return;

  m_MenuSystem = pSystem->GetGuid();
  m_MenuObject = m_Press.m_Object;
  m_MenuContext = (plParticleContext::Enum)m_Press.m_iContext;
  m_iMenuIndex = m_Press.m_iIndex;

  const plInt32 iCount = (plInt32)pLayout->m_Contexts[m_Press.m_iContext].m_Blocks.GetCount();
  const plString sTitle = plParticleGraphEditing::GetBlockDisplayName(pBlock->GetTypeAccessor().GetType());

  // the actions look the objects up again: the document may have changed while the menu was open
  auto run = [this](plUInt32 uiAction)
  {
    const plDocumentObject* pTargetSystem = m_pManager->GetObject(m_MenuSystem);
    const plDocumentObject* pTargetBlock = m_pManager->GetObject(m_MenuObject);
    if (pTargetSystem == nullptr || pTargetBlock == nullptr)
      return;

    const plDocument* pDocument = m_pManager->GetDocument();
    switch (uiAction)
    {
      case 0:
        plParticleGraphEditing::ToggleBlockEnabled(pDocument, pTargetBlock);
        break;
      case 1:
        plParticleGraphEditing::ReorderBlock(pDocument, pTargetSystem, pTargetBlock, m_MenuContext, m_iMenuIndex - 1);
        break;
      case 2:
        plParticleGraphEditing::ReorderBlock(pDocument, pTargetSystem, pTargetBlock, m_MenuContext, m_iMenuIndex + 2); // the move indexes the list before the removal
        break;
      case 3:
        plParticleGraphEditing::RemoveBlock(pDocument, pTargetBlock);
        break;
    }
  };

  plStringBuilder sRemove;
  sRemove.SetFormat("Remove '{0}'", sTitle);

  plVisualGraphMenu menu;
  menu.AddAction(plParticleGraphEditing::IsBlockEnabled(pBlock) ? "Disable" : "Enable", [run]()
    { run(0); });
  menu.AddSeparator();
  menu.AddAction("Move Up", [run]()
    { run(1); }, m_iMenuIndex > 0);
  menu.AddAction("Move Down", [run]()
    { run(2); }, m_iMenuIndex + 1 < iCount);
  menu.AddSeparator();
  menu.AddAction(sRemove, [run]()
    { run(3); }).m_bDanger = true;

  m_ShowMenu(vGraph, menu);
}

void plParticleEffectGraphController::RunDeferred(std::function<void()> picker)
{
  // Modal pickers run after the pointer event that asked for them: a nested event loop inside the
  // view's pointer handling would rebuild the view under it.
  m_Timers.Start(plTime::MakeZero(), std::move(picker));
}

//////////////////////////////////////////////////////////////////////////
// Pointer

bool plParticleEffectGraphController::OnCustomNodePointer(const plDocumentObject* pNode, const plVec2& vLocal, const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  if (e.m_Type == Type::Press)
  {
    const NodeLayout* pLayout = FindLayout(pNode);
    return pLayout != nullptr && OnPress(pNode, *pLayout, vLocal, e);
  }

  if (m_Press.m_Target == Press::Target::None || m_Press.m_pNode != pNode)
    return false;

  switch (e.m_Type)
  {
    case Type::DragStart:
      if (e.m_Button == plEditorUiPointerEvent::Button::Left)
      {
        OnDragStart();
        OnDragMove(vLocal);
      }
      break;

    case Type::DragMove:
      OnDragMove(vLocal);
      break;

    case Type::Drop:
      OnDrop();
      m_Press = Press();
      break;

    case Type::DragCancel:
      OnDragCancel();
      m_Press = Press();
      break;

    case Type::Release:
      OnClick(vLocal);
      m_Press = Press();
      break;

    default:
      break;
  }

  return true;
}

bool plParticleEffectGraphController::OnPress(const plDocumentObject* pNode, const NodeLayout& layout, const plVec2& vLocal, const plEditorUiPointerEvent& e)
{
  m_Press = Press();

  if (e.m_Button != plEditorUiPointerEvent::Button::Left && e.m_Button != plEditorUiPointerEvent::Button::Right)
    return false;

  Press press;
  bool bHit = false;
  if (layout.m_Type == NodeType::System)
    bHit = HitSystem(layout, vLocal, press);
  else if (layout.m_Type == NodeType::Operator)
    bHit = HitOperator(pNode, layout, vLocal, press);

  if (!bHit)
    return false;

  // a right-click only means something on a block: elsewhere it opens the node menu
  if (e.m_Button == plEditorUiPointerEvent::Button::Right && press.m_Target != Press::Target::Block && press.m_Target != Press::Target::BlockEnable)
    return false;

  if (e.m_Button == plEditorUiPointerEvent::Button::Right)
    press.m_Target = Press::Target::Block;

  press.m_Button = e.m_Button;
  press.m_pNode = pNode;
  press.m_vLocal = vLocal;
  m_Press = press;

  // a block (or one of its values) routes the properties dock at it
  if (layout.m_Type == NodeType::System && press.m_Object.IsValid())
  {
    if (const plDocumentObject* pBlock = m_pManager->GetObject(press.m_Object))
      plParticleGraphEditing::SelectObject(m_pManager->GetDocument(), pBlock);
  }

  return true;
}

bool plParticleEffectGraphController::HitSystem(const NodeLayout& layout, const plVec2& vLocal, Press& ref_press) const
{
  if (!layout.m_Attributes.IsEmpty() && layout.m_AddAttributeRect.Contains(vLocal))
  {
    ref_press.m_Target = Press::Target::AddAttribute;
    return true;
  }

  for (plUInt32 uiContext = 0; uiContext < plParticleContext::Count; ++uiContext)
  {
    const ContextLayout& context = layout.m_Contexts[uiContext];
    ref_press.m_iContext = (plInt32)uiContext;

    if (context.m_HeaderRect.Contains(vLocal))
    {
      ref_press.m_Target = Press::Target::ContextHeader;
      return true;
    }

    if (context.m_bCollapsed)
      continue;

    if (context.m_AddRect.Contains(vLocal))
    {
      ref_press.m_Target = Press::Target::AddRow;
      return true;
    }

    for (plUInt32 uiBlock = 0; uiBlock < context.m_Blocks.GetCount(); ++uiBlock)
    {
      const BlockLayout& block = context.m_Blocks[uiBlock];
      ref_press.m_Object = block.m_Block;
      ref_press.m_iIndex = (plInt32)uiBlock;

      if (block.m_Rect.Contains(vLocal))
      {
        const plRectFloat enable(block.m_EnableRect.x - 3.0f, block.m_EnableRect.y - 3.0f, block.m_EnableRect.width + 6.0f, block.m_EnableRect.height + 6.0f);
        ref_press.m_Target = enable.Contains(vLocal) ? Press::Target::BlockEnable : Press::Target::Block;
        return true;
      }

      for (const ValueRowLayout& row : block.m_Rows)
      {
        if (!row.m_Rect.Contains(vLocal))
          continue;

        ref_press.m_Target = Press::Target::Value;
        ref_press.m_Row = row;
        ref_press.m_bSecondary = row.m_bHasVariance && row.m_ValueRect2.Contains(vLocal);
        ref_press.m_fStartValue = ref_press.m_bSecondary ? row.m_fVariance : row.m_fNumber;
        return true;
      }
    }
  }

  ref_press = Press();
  return false;
}

bool plParticleEffectGraphController::HitOperator(const plDocumentObject* pNode, const NodeLayout& layout, const plVec2& vLocal, Press& ref_press) const
{
  ref_press.m_Object = pNode->GetGuid();

  if (layout.m_bHasPreview && layout.m_PreviewRect.Contains(vLocal))
  {
    ref_press.m_Target = Press::Target::Preview;
    ref_press.m_Row.m_pProp = layout.m_Preview.m_pProp;
    ref_press.m_Row.m_Original = layout.m_Preview.m_Original;
    ref_press.m_Row.m_Color = layout.m_Preview.m_Color;
    return true;
  }

  // an Expression node's code box edits the code
  if (layout.m_CodeRect.width > 0.0f && layout.m_CodeRect.Contains(vLocal))
  {
    ref_press.m_Target = Press::Target::Value;
    ref_press.m_Row = layout.m_CodeRow;
    return true;
  }

  for (const ValueRowLayout& row : layout.m_Rows)
  {
    // pin-only and wired rows have nothing to edit; the press moves the node
    if (row.m_Kind == plParticleValueRow::Kind::PinOnly || row.m_bDriven || !row.m_Rect.Contains(vLocal))
      continue;

    ref_press.m_Target = Press::Target::Value;
    ref_press.m_Row = row;
    ref_press.m_fStartValue = row.m_fNumber;
    return true;
  }

  return false;
}

void plParticleEffectGraphController::OnDragStart()
{
  if (m_Press.m_Button != plEditorUiPointerEvent::Button::Left)
    return;

  if (m_Press.m_Target == Press::Target::Value && m_Press.m_Row.m_Kind == plParticleValueRow::Kind::Number)
  {
    // one transaction for the whole gesture, so a scrub is a single undo step
    m_pManager->GetDocument()->GetCommandHistory()->StartTransaction("Scrub Value");
    m_Press.m_bTransaction = true;
    m_Press.m_bDragging = true;
  }
  else if (m_Press.m_Target == Press::Target::Block)
  {
    m_Press.m_bDragging = true;
    m_iDropIndex = -1;
  }
}

void plParticleEffectGraphController::OnDragMove(const plVec2& vLocal)
{
  if (!m_Press.m_bDragging)
    return;

  if (m_Press.m_Target == Press::Target::Value)
  {
    if (m_pManager->GetObject(m_Press.m_Object) == nullptr)
      return;

    const plParticleValueRow& row = m_Press.m_Row;
    const double fNew = plParticleGraphEditing::ScrubNumber(row, m_Press.m_fStartValue, vLocal.x - m_Press.m_vLocal.x, m_Press.m_bSecondary);
    plParticleGraphEditing::SetNumber(m_pManager->GetDocument(), m_Press.m_Object, row, m_Press.m_bSecondary ? row.m_fNumber : fNew, m_Press.m_bSecondary ? fNew : row.m_fVariance);
    return;
  }

  const plInt32 iDropIndex = GetDropIndex(vLocal);
  if (iDropIndex != m_iDropIndex)
  {
    m_iDropIndex = iDropIndex;
    InvalidateLook();
  }
}

void plParticleEffectGraphController::OnDrop()
{
  if (m_Press.m_bTransaction)
  {
    m_pManager->GetDocument()->GetCommandHistory()->FinishTransaction();
    return;
  }

  if (m_Press.m_Target != Press::Target::Block || !m_Press.m_bDragging)
    return;

  const plDocumentObject* pBlock = m_pManager->GetObject(m_Press.m_Object);
  if (pBlock != nullptr && m_iDropIndex >= 0)
    plParticleGraphEditing::ReorderBlock(m_pManager->GetDocument(), m_Press.m_pNode, pBlock, (plParticleContext::Enum)m_Press.m_iContext, m_iDropIndex);

  m_iDropIndex = -1;
  InvalidateLook();
}

void plParticleEffectGraphController::OnDragCancel()
{
  // a cancelled scrub restores the value it started from
  if (m_Press.m_bTransaction)
    m_pManager->GetDocument()->GetCommandHistory()->CancelTransaction();

  if (m_Press.m_Target == Press::Target::Block && m_Press.m_bDragging)
  {
    m_iDropIndex = -1;
    InvalidateLook();
  }
}

plInt32 plParticleEffectGraphController::GetDropIndex(const plVec2& vLocal) const
{
  const NodeLayout* pLayout = FindLayout(m_Press.m_pNode);
  if (pLayout == nullptr || m_Press.m_iContext < 0)
    return -1;

  const ContextLayout& context = pLayout->m_Contexts[m_Press.m_iContext];
  for (plUInt32 i = 0; i < context.m_Blocks.GetCount(); ++i)
  {
    if (vLocal.y < context.m_Blocks[i].m_Rect.GetCenter().y)
      return (plInt32)i;
  }

  return (plInt32)context.m_Blocks.GetCount();
}

void plParticleEffectGraphController::OnClick(const plVec2& vLocal)
{
  const plVec2 vGraph = m_pManager->GetNodePos(m_Press.m_pNode) + vLocal;
  const plDocumentObject* pSystem = m_Press.m_pNode;

  if (m_Press.m_Button == plEditorUiPointerEvent::Button::Right)
  {
    ShowBlockMenu(pSystem, vGraph);
    return;
  }

  switch (m_Press.m_Target)
  {
    case Press::Target::ContextHeader:
    {
      bool& bCollapsed = m_SystemStates[pSystem->GetGuid()].m_bCollapsed[m_Press.m_iContext];
      bCollapsed = !bCollapsed;
      InvalidateLook();
      break;
    }

    case Press::Target::AddRow:
      ShowAddMenu(pSystem, (plParticleContext::Enum)m_Press.m_iContext, vGraph);
      break;

    case Press::Target::AddAttribute:
    {
      plHybridArray<plString, 8> taken;
      if (const NodeLayout* pLayout = FindLayout(pSystem))
      {
        for (const AttributeLayout& attribute : pLayout->m_Attributes)
          taken.PushBack(attribute.m_sName);
      }

      plParticleGraphEditing::AddCustomAttribute(m_pManager->GetDocument(), pSystem, taken);
      break;
    }

    case Press::Target::BlockEnable:
      if (const plDocumentObject* pBlock = m_pManager->GetObject(m_Press.m_Object))
        plParticleGraphEditing::ToggleBlockEnabled(m_pManager->GetDocument(), pBlock);
      break;

    case Press::Target::Block:
      // the whole row is the disclosure header
      if (!m_ExpandedBlocks.Remove(m_Press.m_Object))
        m_ExpandedBlocks.Insert(m_Press.m_Object);

      InvalidateLook();
      break;

    case Press::Target::Value:
    case Press::Target::Preview:
      OnValueClick(vGraph);
      break;

    case Press::Target::None:
      break;
  }
}

void plParticleEffectGraphController::OnValueClick(const plVec2& vGraph)
{
  if (m_pManager->GetObject(m_Press.m_Object) == nullptr)
    return;

  const plUuid object = m_Press.m_Object;
  const plParticleValueRow row = m_Press.m_Row;
  const bool bSecondary = m_Press.m_bSecondary;

  // the pickers check again that the object still exists once they run
  auto exists = [this, object]()
  { return m_pManager->GetObject(object) != nullptr; };

  if (m_Press.m_Target == Press::Target::Preview)
  {
    const bool bColor = row.m_Original.IsA<plColor>() || row.m_Original.IsA<plColorGammaUB>();
    RunDeferred([this, exists, object, row, bColor]()
      {
        if (!exists())
          return;

        if (bColor)
          plParticleGraphEditing::PickColor(m_pManager->GetDocument(), object, row);
        else
          plParticleGraphEditing::PickAsset(m_pManager->GetDocument(), object, row);
      });
    return;
  }

  switch (row.m_Kind)
  {
    case plParticleValueRow::Kind::Bool:
      plParticleGraphEditing::ToggleBool(m_pManager->GetDocument(), object, row);
      break;

    case plParticleValueRow::Kind::Enum:
      m_MenuObject = object;
      m_MenuRow = row;
      ShowEnumMenu(vGraph);
      break;

    case plParticleValueRow::Kind::Color:
      RunDeferred([this, exists, object, row]()
        {
          if (exists())
            plParticleGraphEditing::PickColor(m_pManager->GetDocument(), object, row);
        });
      break;

    case plParticleValueRow::Kind::Asset:
      RunDeferred([this, exists, object, row]()
        {
          if (exists())
            plParticleGraphEditing::PickAsset(m_pManager->GetDocument(), object, row);
        });
      break;

    case plParticleValueRow::Kind::Number:
      // a click that never turned into a scrub asks for the value
      RunDeferred([this, exists, object, row, bSecondary]()
        {
          if (exists())
            plParticleGraphEditing::TypeNumber(m_pManager->GetDocument(), object, row, bSecondary);
        });
      break;

    case plParticleValueRow::Kind::Text:
      RunDeferred([this, exists, object, row]()
        {
          if (exists())
            plParticleGraphEditing::TypeText(m_pManager->GetDocument(), object, row);
        });
      break;

    case plParticleValueRow::Kind::PinOnly:
      break;
  }
}

//////////////////////////////////////////////////////////////////////////
// Document events

void plParticleEffectGraphController::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  switch (e.m_EventType)
  {
    case plDocumentObjectStructureEvent::Type::AfterObjectAdded:
    case plDocumentObjectStructureEvent::Type::AfterObjectRemoved:
    case plDocumentObjectStructureEvent::Type::AfterObjectMoved2:
      if (plParticleGraphEditing::IsBlock(e.m_pObject) || (e.m_pPreviousParent != nullptr && GetNodeType(e.m_pPreviousParent) == NodeType::System) || (e.m_pNewParent != nullptr && GetNodeType(e.m_pNewParent) == NodeType::System))
        InvalidateLook();
      break;

    default:
      break;
  }
}

void plParticleEffectGraphController::SelectionEventHandler(const plSelectionManagerEvent& e)
{
  PL_IGNORE_UNUSED(e);

  // block rows show the selection; nodes are outlined by the view itself
  bool bBlockSelected = false;
  for (const plDocumentObject* pObject : m_pManager->GetDocument()->GetSelectionManager()->GetSelection())
    bBlockSelected |= plParticleGraphEditing::IsBlock(pObject);

  if (bBlockSelected || m_bBlockSelected)
    InvalidateLook();

  m_bBlockSelected = bBlockSelected;

  // the value spy follows the one selected operator
  const auto& selection = m_pManager->GetDocument()->GetSelectionManager()->GetSelection();
  const plDocumentObject* pSpied = nullptr;
  if (selection.GetCount() == 1 && GetNodeType(selection[0]) == NodeType::Operator)
    pSpied = selection[0];

  if (auto* pDocument = GetEffectDocument(); pDocument != nullptr && pDocument->GetSpyNode() != (pSpied != nullptr ? pSpied->GetGuid() : plUuid()))
  {
    pDocument->SetSpyNode(pSpied);
    InvalidateLook();
  }
}

void plParticleEffectGraphController::EffectEventHandler(const plParticleEffectAssetEvent& e)
{
  if (e.m_Type != plParticleEffectAssetEvent::PreviewStatusChanged)
    return;

  // the Blackboard nodes say which entries the preview lacks; that changes rarely
  const auto& status = e.m_pDocument->GetPreviewStatus();
  if (status.m_MissingBlackboardEntries != m_MissingBlackboardEntries)
  {
    m_MissingBlackboardEntries = status.m_MissingBlackboardEntries;
    InvalidateLook();
  }

  // the spy panel follows about four times a second; the graph is laid out again for it
  if (!status.m_SpyNode.IsValid() || status.m_SpyNode != e.m_pDocument->GetSpyNode())
    return;

  const plTime tNow = plTime::Now();
  if (tNow - m_LastSpyRefresh < plTime::MakeFromMilliseconds(250))
    return;

  m_LastSpyRefresh = tNow;
  InvalidateLook();
}

void plParticleEffectGraphController::ExtendBackgroundMenu(const plVec2& vGraphPosition, plVisualGraphMenu& ref_menu)
{
  // a template is placed right of the systems, wherever the menu was opened
  PL_IGNORE_UNUSED(vGraphPosition);

  auto* pDocument = GetEffectDocument();
  if (pDocument == nullptr)
    return;

  plDynamicArray<plParticleEffectAssetDocument::SystemTemplate> templates;
  plParticleEffectAssetDocument::GetAvailableSystemTemplates(templates);

  ref_menu.AddSeparator();
  plEditorUiMenuItem& submenu = ref_menu.m_Menu.AddSubmenu("Add system from template");

  // shipped templates first, then the project's own, each with what it holds as its tooltip
  bool bProjectHeader = false;
  bool bShippedHeader = false;
  for (const auto& tmpl : templates)
  {
    if (!tmpl.m_bProjectLocal && !bShippedHeader)
    {
      plEditorUiMenuItem& header = submenu.m_Children.ExpandAndGetRef();
      header.m_Type = plEditorUiMenuItem::Type::Header;
      header.m_sLabel = "Shipped with packages";
      bShippedHeader = true;
    }

    if (tmpl.m_bProjectLocal && !bProjectHeader)
    {
      if (bShippedHeader)
        submenu.m_Children.ExpandAndGetRef().m_Type = plEditorUiMenuItem::Type::Separator;

      plEditorUiMenuItem& header = submenu.m_Children.ExpandAndGetRef();
      header.m_Type = plEditorUiMenuItem::Type::Header;
      header.m_sLabel = "This project";
      bProjectHeader = true;
    }

    const plString sPath = tmpl.m_sFilePath;
    ref_menu.m_Actions.PushBack([pDocument, sPath]()
      {
        const plStatus status = pDocument->AddSystemFromTemplate(sPath);
        if (status.Failed())
          plLog::Error("Adding the system from the template failed: {}", status.m_sMessage);
      });

    plEditorUiMenuItem& item = submenu.m_Children.ExpandAndGetRef();
    item.m_sLabel = tmpl.m_sName;
    item.m_uiId = ref_menu.m_Actions.GetCount();
    item.m_sTooltip = plParticleEffectAssetDocument::DescribeSystemTemplate(tmpl.m_sFilePath);
  }

  if (templates.IsEmpty())
  {
    plEditorUiMenuItem& item = submenu.m_Children.ExpandAndGetRef();
    item.m_sLabel = "No templates found";
    item.m_bEnabled = false;
  }
}
