#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphCompiler.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphEditing.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/Set.h>
#include <EditorUi/Runtime.h>
#include <GuiFoundation/VisualGraph/VisualGraphController.h>

#include <functional>

struct plDocumentObjectStructureEvent;
struct plSelectionManagerEvent;
struct plParticleEffectAssetEvent;

/// \brief The particle effect graph in the engine graph view.
///
/// Systems, operators and the effect are custom nodes. A system is a vertical stack of its four
/// contexts (Spawn, Initialize, Update, Output); each context lists its blocks in run order. A click
/// on a block opens or closes it, its toggle enables or disables it, dragging it reorders it within
/// its context, and a right-click opens the block menu. An open block shows its values as rows: bools
/// and enums are set in place, colours and assets open their pickers, numbers scrub when dragged and
/// ask for a value when clicked. Operators show their values the same way, or the value itself for
/// colour, texture, gradient and curve nodes. Property pins sit on the row they drive.
///
/// Systems flow down into the effect, so wires are straight lines from a system's bottom edge to the
/// effect's top edge.
class plParticleEffectGraphController : public plVisualGraphController
{
public:
  plParticleEffectGraphController();
  ~plParticleEffectGraphController();

  void Init(const plVisualGraphObjectManager* pManager) override;

  void GetNodeLook(const plDocumentObject* pNode, plVisualGraphNodeLook& out_look) const override;
  void GetPinLook(const plVisualGraphPin& pin, plVisualGraphPinLook& out_look) const override;
  void GetConnectionLook(const plDocumentObject* pConnection, plVisualGraphConnectionLook& out_look) const override;
  void GetExtraWires(plDynamicArray<plVisualGraphExtraWire>& out_wires) const override;

  void ExtendNodeMenu(const plDocumentObject* pNode, plVisualGraphMenu& ref_menu) override;
  void ExtendBackgroundMenu(const plVec2& vGraphPosition, plVisualGraphMenu& ref_menu) override;
  bool OnKey(const plEditorUiKeyEvent& e) override;

  void LayoutCustomNode(const plDocumentObject* pNode, plVec2& out_vSize, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const override;
  void BuildCustomNode(const plDocumentObject* pNode, plUiElement* pNodePanel, plStringView sNamePrefix) const override;
  bool OnCustomNodePointer(const plDocumentObject* pNode, const plVec2& vLocal, const plEditorUiPointerEvent& e) override;

private:
  enum class NodeType : plUInt8
  {
    None,
    Effect,
    System,
    Operator,
  };

  /// One value row of an open block or an operator, in node space.
  struct ValueRowLayout : plParticleValueRow
  {
    plRectFloat m_Rect = plRectFloat(0, 0, 0, 0);       ///< label and value
    plRectFloat m_ValueRect = plRectFloat(0, 0, 0, 0);  ///< the primary value
    plRectFloat m_ValueRect2 = plRectFloat(0, 0, 0, 0); ///< the deviation of a variance pair
    bool m_bDriven = false;                             ///< a wire supplies the value
    plString m_sDrivenBy;                               ///< what the wire carries, such as "Intensity x 60"
    plParticleTier m_DrivenTier = plParticleTier::Constant;
    bool m_bDrivenUnresolved = false;                   ///< the wire has no value; the literal is used
    bool m_bPerParticle = false;                        ///< the property takes per-particle wires; marked on the row
  };

  /// Layouts hold guids, not objects: pointer input can arrive between an edit and the rebuild after it.
  struct BlockLayout
  {
    plUuid m_Block;
    plString m_sTitle;
    plString m_sDetail; ///< after the title, open or closed: what a Set Attribute writes ("Color.A")
    plString m_sSummary;
    plString m_sEvent;   ///< on the right of the title row, in the event colour: "raises Hit", "Hit · per event"
    plString m_sRaises;  ///< the event this block raises (an Events block)
    plString m_sListens; ///< the event this block spawns on (On Event)
    bool m_bEventUnmatched = false; ///< an On Event whose event nothing in the effect raises
    bool m_bConverted = false; ///< made from a 1.3 logic block when the document opened
    plRectFloat m_Rect = plRectFloat(0, 0, 0, 0); ///< the title row
    plRectFloat m_EnableRect = plRectFloat(0, 0, 0, 0);
    bool m_bExpanded = false;
    bool m_bEnabled = true;
    bool m_bBlocksLowering = false;
    plDynamicArray<ValueRowLayout> m_Rows;
  };

  struct ContextLayout
  {
    plRectFloat m_Rect = plRectFloat(0, 0, 0, 0); ///< header and body
    plRectFloat m_HeaderRect = plRectFloat(0, 0, 0, 0);
    plRectFloat m_AddRect = plRectFloat(0, 0, 0, 0);
    bool m_bCollapsed = false;
    plDynamicArray<BlockLayout> m_Blocks;
    plString m_sProgram; ///< "program: 12 ops" when the context's per-particle wires compile to programs
    plRectFloat m_ProgramRect = plRectFloat(0, 0, 0, 0);
  };

  /// One custom attribute the system's Set Attribute blocks write.
  struct AttributeLayout
  {
    plString m_sName;
    plString m_sType;
    plRectFloat m_Rect = plRectFloat(0, 0, 0, 0);
  };

  struct NodeLayout
  {
    NodeType m_Type = NodeType::None;
    plVec2 m_vSize = plVec2::MakeZero();
    float m_fHeaderHeight = 0.0f;
    plString m_sTitle;
    plString m_sSubtitle;
    plColorGammaUB m_BadgeColor = plColorGammaUB(170, 180, 196);

    // systems
    ContextLayout m_Contexts[plParticleContext::Count];
    bool m_bTargetsGpu = false;
    plString m_sLoweringReason;
    plRectFloat m_VerdictRect = plRectFloat(0, 0, 0, 0);
    plString m_sEventNote;  ///< a GPU system's events arrive late; said under the Events context
    plRectFloat m_EventNoteRect = plRectFloat(0, 0, 0, 0);
    plHybridArray<AttributeLayout, 8> m_Attributes;
    plRectFloat m_AttributesRect = plRectFloat(0, 0, 0, 0); ///< empty without custom attributes
    plRectFloat m_AttributesTitleRect = plRectFloat(0, 0, 0, 0);
    plRectFloat m_AddAttributeRect = plRectFloat(0, 0, 0, 0);

    // operators
    plDynamicArray<ValueRowLayout> m_Rows;
    plString m_sIssue; ///< why the node produces no value; shown under its header
    bool m_bIssueIsNote = false; ///< m_sIssue is information rather than a fault
    plRectFloat m_IssueRect = plRectFloat(0, 0, 0, 0);
    plString m_sCode; ///< an Expression node's code, shown in a box above its inputs
    plRectFloat m_CodeRect = plRectFloat(0, 0, 0, 0);
    plParticleValueRow m_CodeRow; ///< the row that edits the code when the box is clicked

    // the value spy of the selected node: its live values in the preview
    bool m_bSpy = false;
    plRectFloat m_SpyRect = plRectFloat(0, 0, 0, 0);
    plUInt32 m_uiSpyCount = 0;
    float m_fSpyMin = 0.0f;
    float m_fSpyAvg = 0.0f;
    float m_fSpyMax = 0.0f;
    plHybridArray<plUInt32, 16> m_SpyBins;
    bool m_bSpyOnGPU = false;
    plParticleTier m_Tier = plParticleTier::Constant;
    bool m_bHasPreview = false;
    plParticleOperatorPreview m_Preview;
    plString m_sThumbnail; ///< empty: the preview shows the asset's name instead
    plRectFloat m_PreviewRect = plRectFloat(0, 0, 0, 0);
  };

  /// What a press on a custom node hit; kept until its release, drop or cancel.
  struct Press
  {
    enum class Target : plUInt8
    {
      None,
      ContextHeader,
      AddRow,
      AddAttribute,
      Block,
      BlockEnable,
      Value,
      Preview,
    };

    Target m_Target = Target::None;
    plEditorUiPointerEvent::Button m_Button = plEditorUiPointerEvent::Button::None;
    const plDocumentObject* m_pNode = nullptr;
    plUuid m_Object; ///< the block, or the node itself for operator values
    plInt32 m_iContext = -1;
    plInt32 m_iIndex = -1;
    plParticleValueRow m_Row; ///< by value: every edit rebuilds the layouts
    bool m_bSecondary = false;
    plVec2 m_vLocal = plVec2::MakeZero();
    double m_fStartValue = 0.0;
    bool m_bDragging = false;
    bool m_bTransaction = false;
  };

  /// Per system: which contexts are folded and which blocks are open. Survives every rebuild.
  struct SystemState
  {
    bool m_bCollapsed[plParticleContext::Count] = {};
  };

  // layout (ParticleEffectGraphViewBuild.cpp)
  NodeType GetNodeType(const plDocumentObject* pNode) const;
  const NodeLayout* FindLayout(const plDocumentObject* pNode) const;
  void LayoutEffect(const plDocumentObject* pNode, NodeLayout& ref_layout, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const;
  void LayoutSystem(const plDocumentObject* pNode, NodeLayout& ref_layout, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const;
  void LayoutSystemContexts(const plDocumentObject* pNode, NodeLayout& ref_layout, float& ref_fY) const;
  void LayoutSystemPrograms(const plDocumentObject* pNode, NodeLayout& ref_layout, float& ref_fY) const;
  void LayoutSystemPins(const plDocumentObject* pNode, const NodeLayout& layout, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const;
  void LayoutBlockEvents(const plDocumentObject* pBlock, plArrayPtr<const plString> raised, bool bHasRaycast, BlockLayout& ref_block) const;
  bool FindPinAnchor(const NodeLayout& layout, const plUuid& block, plStringView sProperty, plVec2& out_vPosition, bool& out_bRowVisible) const;
  void LayoutOperator(const plDocumentObject* pNode, NodeLayout& ref_layout, plDynamicArray<plVisualGraphPinPlacement>& out_pins) const;
  void LayoutValueRows(plArrayPtr<ValueRowLayout> rows, float fLeft, float fWidth, float fRowHeight, float fValueShare, float& ref_fY) const;

  // build (ParticleEffectGraphViewBuild.cpp)
  void BuildEffect(const NodeLayout& layout, plUiElement* pPanel) const;
  void BuildSystem(const plDocumentObject* pNode, const NodeLayout& layout, plUiElement* pPanel) const;
  void BuildAttributes(const NodeLayout& layout, plUiElement* pPanel) const;
  void BuildContext(const plDocumentObject* pNode, const NodeLayout& layout, plUInt32 uiContext, plUiElement* pPanel) const;
  void BuildBlock(const BlockLayout& block, plColorGammaUB accent, plUiElement* pPanel) const;
  void BuildOperator(const NodeLayout& layout, plUiElement* pPanel) const;
  void BuildPreview(const NodeLayout& layout, plUiElement* pPanel) const;
  void BuildSpy(const NodeLayout& layout, plUiElement* pPanel) const;
  void BuildHeader(const NodeLayout& layout, plUiElement* pPanel) const;
  void BuildValueRow(const ValueRowLayout& row, plColorGammaUB accent, plUiElement* pPanel) const;

  // pointer (ParticleEffectGraphView.cpp)
  bool OnPress(const plDocumentObject* pNode, const NodeLayout& layout, const plVec2& vLocal, const plEditorUiPointerEvent& e);
  bool HitSystem(const NodeLayout& layout, const plVec2& vLocal, Press& ref_press) const;
  bool HitOperator(const plDocumentObject* pNode, const NodeLayout& layout, const plVec2& vLocal, Press& ref_press) const;
  void OnDragStart();
  void OnDragMove(const plVec2& vLocal);
  void OnDrop();
  void OnDragCancel();
  void OnClick(const plVec2& vLocal);
  void OnValueClick(const plVec2& vGraph);
  plInt32 GetDropIndex(const plVec2& vLocal) const;

  // menus
  void ShowEnumMenu(const plVec2& vGraph);
  void ShowAddMenu(const plDocumentObject* pSystem, plParticleContext::Enum context, const plVec2& vGraph);
  void ShowBlockMenu(const plDocumentObject* pSystem, const plVec2& vGraph);

  /// Runs a modal picker after the pointer event that asked for it has been handled.
  void RunDeferred(std::function<void()> picker);

  bool IsBlockSelected(const plDocumentObject* pBlock) const;

  /// The effect node, which holds the parameter defaults the operator graph reads.
  const plDocumentObject* FindEffectNode() const;

  /// The colour wires and output pins of a tier are drawn in.
  static plColorGammaUB GetTierColor(plParticleTier tier);

  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void SelectionEventHandler(const plSelectionManagerEvent& e);
  void EffectEventHandler(const plParticleEffectAssetEvent& e);

  class plParticleEffectAssetDocument* GetEffectDocument() const;
  plTime m_LastSpyRefresh;
  plDynamicArray<plString> m_MissingBlackboardEntries; ///< as last laid out

  mutable plHashTable<const plDocumentObject*, NodeLayout> m_Layouts;
  plHashTable<plUuid, SystemState> m_SystemStates;
  plSet<plUuid> m_ExpandedBlocks;

  Press m_Press;
  plInt32 m_iDropIndex = -1; ///< while a block is dragged: where it would land in its context

  // the targets of the open menu; its actions only capture an index
  plUuid m_MenuObject;
  plParticleValueRow m_MenuRow;
  plDynamicArray<plParticleBlockChoice> m_MenuBlockChoices;
  plUuid m_MenuSystem;
  plParticleContext::Enum m_MenuContext = plParticleContext::Spawn;
  plInt32 m_iMenuIndex = -1;

  plEditorUiTimerGroup m_Timers; ///< the deferred pickers
  plEventSubscriptionID m_ThumbnailSubscription = 0;
  bool m_bBlockSelected = false;
  bool m_bSubscribed = false;
};
