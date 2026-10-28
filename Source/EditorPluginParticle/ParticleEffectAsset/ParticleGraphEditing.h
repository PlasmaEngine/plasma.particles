#pragma once

#include <EditorPluginParticle/EditorPluginParticleDLL.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodes.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Strings/TranslationLookup.h>

class plDocument;
class plDocumentObject;
class plDocumentObjectManager;
class plParticleEffectAssetDocument;

/// One editable value of a block or an operator node, as the particle graph view shows it.
///
/// Bools, enums, colours and assets are edited with one click; numbers scrub when dragged and open a
/// type-in dialog when clicked. A variance pair (value and deviation) scrubs each half on its own.
struct plParticleValueRow
{
  enum class Kind : plUInt8
  {
    Text,
    Bool,
    Enum,
    Color,
    Number, ///< float / int / time / angle / variance
    Asset,
    PinOnly, ///< an operator input with no literal behind it, such as a Branch condition
  };

  const plAbstractProperty* m_pProp = nullptr;
  plString m_sLabel;
  plString m_sValue;  ///< primary field, already formatted
  plString m_sValue2; ///< variance field
  plString m_sPin;    ///< operator rows: the input pin sitting on this row, if any
  plVariant m_Original;
  Kind m_Kind = Kind::Text;
  bool m_bBoolValue = false;
  plColorGammaUB m_Color;

  double m_fNumber = 0.0;
  double m_fVariance = 0.0;
  bool m_bHasVariance = false;
  bool m_bIsInteger = false;
  bool m_bHasClamp = false;
  double m_fMin = 0.0;
  double m_fMax = 0.0;
};

/// What an operator node shows instead of rows: its colour, or the thumbnail of its asset.
struct plParticleOperatorPreview
{
  const plAbstractProperty* m_pProp = nullptr;
  plVariant m_Original;
  bool m_bIsColor = false;
  plColorGammaUB m_Color;
  plUuid m_Asset; ///< invalid when no asset is assigned
};

/// One entry of a context's "Add block" menu.
struct plParticleBlockChoice
{
  plString m_sName;
  const plRTTI* m_pType = nullptr;
};

/// Document logic of the engine graph controller: what the rows of a block or operator show, and the edits clicking, scrubbing and the block menus make. Every edit is
/// one undoable transaction.
namespace plParticleGraphEditing
{
  // Rows

  /// Fills a row from one property's current value.
  void DescribeValue(const plDocumentObject* pObject, const plAbstractProperty* pProp, plParticleValueRow& out_row);

  /// The properties an open block shows, honouring the property grid's visibility rules.
  void GetBlockProperties(const plDocumentObject* pBlock, plDynamicArray<const plAbstractProperty*>& out_properties);

  /// The properties an operator node shows as rows: every editable member except its name.
  void GetOperatorProperties(const plDocumentObject* pNode, plDynamicArray<const plAbstractProperty*>& out_properties);

  /// The input pin that overrides an operator property, or empty.
  plStringView GetOperatorPinForProperty(const plDocumentObject* pNode, plStringView sProperty);

  /// The rows of an open block. ROW derives from plParticleValueRow.
  template <typename ROW, typename CONTAINER>
  void CollectBlockRows(const plDocumentObject* pBlock, CONTAINER& out_rows)
  {
    plHybridArray<const plAbstractProperty*, 32> properties;
    GetBlockProperties(pBlock, properties);

    for (const plAbstractProperty* pProp : properties)
    {
      ROW& row = out_rows.ExpandAndGetRef();
      DescribeValue(pBlock, pProp, row);
    }
  }

  /// The rows of an operator node, ending with a row for each input pin that has no literal.
  template <typename ROW, typename CONTAINER>
  void CollectOperatorRows(const plDocumentObject* pNode, CONTAINER& out_rows)
  {
    plHybridArray<const plAbstractProperty*, 16> properties;
    GetOperatorProperties(pNode, properties);

    for (const plAbstractProperty* pProp : properties)
    {
      ROW& row = out_rows.ExpandAndGetRef();
      DescribeValue(pNode, pProp, row);
      row.m_sPin = GetOperatorPinForProperty(pNode, pProp->GetPropertyName());
    }

    for (const auto& input : plParticleOperatorInputs::Get(pNode->GetTypeAccessor().GetType()))
    {
      if (input.m_szLiteral != nullptr)
        continue;

      ROW& row = out_rows.ExpandAndGetRef();
      row.m_Kind = plParticleValueRow::Kind::PinOnly;
      row.m_sLabel = plTranslate(input.m_szPin);
      row.m_sPin = input.m_szPin;
    }
  }

  /// Texture, colour, gradient and curve nodes show their value instead of rows; false for the others.
  bool GetOperatorPreview(const plDocumentObject* pNode, plParticleOperatorPreview& out_preview);

  /// The thumbnail file of an asset, empty when the asset is unknown.
  plString GetThumbnailPath(const plUuid& asset);

  /// The display name of an assigned asset reference: the asset's name, or the raw path.
  plString GetAssetDisplayName(const plVariant& reference);

  // Values

  /// Rebuilds a number property's value from a primary value and a deviation, keeping its type.
  plVariant MakeNumberVariant(const plParticleValueRow& row, double fPrimary, double fVariance);

  /// The value a scrub of fDeltaX units from fStartValue gives; the step grows with the magnitude.
  double ScrubNumber(const plParticleValueRow& row, double fStartValue, double fDeltaX, bool bSecondary);

  /// Sets one property in a "Change Value" transaction.
  void SetValue(const plDocument* pDocument, const plUuid& object, const plAbstractProperty* pProp, const plVariant& value);

  /// Sets a number row from a primary value and a deviation.
  void SetNumber(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row, double fPrimary, double fVariance);

  /// The entries of an enum row, with translated names.
  void GetEnumChoices(const plParticleValueRow& row, plDynamicArray<plReflectionUtils::EnumKeyValuePair>& out_entries);

  /// One click on a bool row flips it.
  void ToggleBool(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row);

  // Dialogs (modal: the engine colour dialog, the engine asset picker, engine prompts for numbers, texts and template
  // names; each applies its result as one transaction)

  void PickColor(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row);
  void PickAsset(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row);
  void TypeNumber(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row, bool bSecondary);
  void TypeText(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row);

  /// Asks for a template name and saves the system as a template, reporting the result.
  void SaveSystemAsTemplate(const plParticleEffectAssetDocument* pDocument, const plDocumentObject* pSystem);

  // Blocks

  /// The blocks of one context, in the order the runtime runs them.
  void GetContextBlocks(const plDocumentObjectManager* pManager, const plDocumentObject* pSystem, plParticleContext::Enum context, plDynamicArray<const plDocumentObject*>& out_blocks);

  /// The block types a context offers, sorted by display name.
  void GetBlockChoices(plParticleContext::Enum context, plDynamicArray<plParticleBlockChoice>& out_choices);

  plString GetBlockDisplayName(const plRTTI* pType);

  /// One-line summary of a closed block from its plTitleAttribute; empty without one.
  plString GetBlockSummary(const plDocumentObject* pBlock);

  /// What a Set Attribute block writes, such as "Color.A", "Size" or "Custom.Heat"; empty for other blocks.
  plString GetSinkTargetLabel(const plDocumentObject* pBlock);

  /// A Set Attribute component as the target names it: R, G, B, A for a colour, X, Y, Z for vectors.
  plString GetSinkComponentLabel(plInt64 iTarget, plInt64 iCustomType, plInt64 iComponent);

  /// True for block properties that take per-particle wires (plParticlePerParticleAttribute).
  bool IsPerParticleProperty(const plAbstractProperty* pProp);

  bool IsBlockEnabled(const plDocumentObject* pBlock);

  /// True for objects that live in a system's context arrays.
  bool IsBlock(const plDocumentObject* pObject);

  /// Appends a block of the given type to a context.
  void AddBlock(const plDocument* pDocument, const plDocumentObject* pSystem, plParticleContext::Enum context, const plRTTI* pType);

  /// Adds a custom attribute to a system: a Set Attribute block in Initialize that writes a new
  /// float attribute, named "Attribute1", "Attribute2" and so on, skipping the names in taken.
  void AddCustomAttribute(const plDocument* pDocument, const plDocumentObject* pSystem, plArrayPtr<const plString> taken);

  void RemoveBlock(const plDocument* pDocument, const plDocumentObject* pBlock);

  /// Moves a block to iNewIndex, an index into the list as it is before the move.
  void ReorderBlock(const plDocument* pDocument, const plDocumentObject* pSystem, const plDocumentObject* pBlock, plParticleContext::Enum context, plInt32 iNewIndex);

  void ToggleBlockEnabled(const plDocument* pDocument, const plDocumentObject* pBlock);

  void SelectObject(const plDocument* pDocument, const plDocumentObject* pObject);

  /// Removes the selected block, if exactly one block is selected; true when it did.
  bool RemoveSelectedBlock(const plDocument* pDocument);

  // Looks

  /// The category colour of a context: its accent bar, header and toggles.
  plColorGammaUB GetContextColor(plParticleContext::Enum context);

  /// The badge colour of an operator node, by the kind of value it produces.
  plColorGammaUB GetValueKindColor(plParticleValueKind::Enum kind);

  /// An operator's name, else what kind of node it is.
  plString GetOperatorTitle(const plDocumentObject* pNode);

  /// A system's name, else "System".
  plString GetSystemTitle(const plDocumentObject* pSystem);

  /// The translated simulation target of a system.
  plString GetSimulationTargetLabel(const plDocumentObject* pSystem);

  /// "2 event reactions" and the like.
  plString GetEffectSubtitle(const plDocumentObject* pEffect);
} // namespace plParticleGraphEditing
