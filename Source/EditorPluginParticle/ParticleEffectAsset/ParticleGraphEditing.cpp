#include <EditorPluginParticle/EditorPluginParticlePCH.h>

#include <EditorFramework/Assets/EngineAssetPicker.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectAsset.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleEffectNodeManager.h>
#include <EditorPluginParticle/ParticleEffectAsset/ParticleGraphEditing.h>
#include <Foundation/Types/VarianceTypes.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/EngineUi/EngineColorDialog.h>
#include <GuiFoundation/EngineUi/EnginePrompt.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <ParticlePlugin/Behavior/ParticleBehavior_Attribute.h>
#include <ParticlePlugin/Initializer/ParticleInitializer_SetAttribute.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Selection/SelectionManager.h>

namespace
{
  /// Runs one command in its own transaction.
  void RunCommand(const plDocument* pDocument, const char* szName, plCommand& ref_cmd)
  {
    plCommandHistory* pHistory = pDocument->GetCommandHistory();
    pHistory->StartTransaction(szName);

    if (pHistory->AddCommand(ref_cmd).Failed())
      pHistory->CancelTransaction();
    else
      pHistory->FinishTransaction();
  }

  void DescribeNumber(const plVariant& value, plParticleValueRow& ref_row)
  {
    ref_row.m_Kind = plParticleValueRow::Kind::Number;

    if (value.IsA<plVarianceTypeFloat>())
    {
      ref_row.m_bHasVariance = true;
      ref_row.m_fNumber = value.Get<plVarianceTypeFloat>().m_Value;
      ref_row.m_fVariance = value.Get<plVarianceTypeFloat>().m_fVariance;
    }
    else if (value.IsA<plVarianceTypeTime>())
    {
      ref_row.m_bHasVariance = true;
      ref_row.m_fNumber = value.Get<plVarianceTypeTime>().m_Value.GetSeconds();
      ref_row.m_fVariance = value.Get<plVarianceTypeTime>().m_fVariance;
    }
    else if (value.IsA<plVarianceTypeAngle>())
    {
      ref_row.m_bHasVariance = true;
      ref_row.m_fNumber = value.Get<plVarianceTypeAngle>().m_Value.GetDegree();
      ref_row.m_fVariance = value.Get<plVarianceTypeAngle>().m_fVariance;
    }
    else if (value.IsA<plTime>())
    {
      ref_row.m_fNumber = value.Get<plTime>().GetSeconds();
    }
    else if (value.IsA<plAngle>())
    {
      ref_row.m_fNumber = value.Get<plAngle>().GetDegree();
    }
    else
    {
      ref_row.m_fNumber = value.ConvertTo<double>();
      ref_row.m_bIsInteger = !value.IsFloatingPoint();
    }

    plStringBuilder s;
    if (ref_row.m_bIsInteger)
      s.SetFormat("{0}", (plInt64)ref_row.m_fNumber);
    else
      s.SetFormat("{0}", plArgF(ref_row.m_fNumber, 2));

    ref_row.m_sValue = s;

    if (ref_row.m_bHasVariance)
    {
      s.SetFormat("+-{0}", plArgF(ref_row.m_fVariance, 2));
      ref_row.m_sValue2 = s;
    }
  }

  bool IsNumber(const plVariant& value)
  {
    return value.IsA<plVarianceTypeFloat>() || value.IsA<plVarianceTypeTime>() || value.IsA<plVarianceTypeAngle>() || value.IsA<plTime>() || value.IsA<plAngle>() || (value.IsNumber() && !value.IsA<bool>());
  }

  bool IsUnsignedInteger(const plVariant& value)
  {
    const plVariantType::Enum type = value.GetType();
    return type == plVariantType::UInt8 || type == plVariantType::UInt16 || type == plVariantType::UInt32 || type == plVariantType::UInt64;
  }

  /// The "Set Value" prompt for a number: integer rows take whole numbers only, other rows three decimals.
  bool AskNumber(plStringView sLabel, bool bInteger, double fValue, double fMin, double fMax, double& out_fValue)
  {
    const plStringView sTitle = "Set Value";

    bool bOk = false;
    if (bInteger)
    {
      const double fIntMin = static_cast<double>(plMath::MinValue<int>());
      const double fIntMax = static_cast<double>(plMath::MaxValue<int>());
      const int iMin = static_cast<int>(plMath::Clamp(fMin, fIntMin, fIntMax));
      const int iMax = static_cast<int>(plMath::Clamp(fMax, fIntMin, fIntMax));
      out_fValue = plEnginePrompt::GetInt(sTitle, sLabel, static_cast<int>(plMath::Clamp(fValue, fIntMin, fIntMax)), iMin, iMax, &bOk);
    }
    else
    {
      out_fValue = plEnginePrompt::GetDouble(sTitle, sLabel, fValue, fMin, fMax, 3, &bOk);
    }

    return bOk;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// Rows

void plParticleGraphEditing::DescribeValue(const plDocumentObject* pObject, const plAbstractProperty* pProp, plParticleValueRow& out_row)
{
  const plVariant value = pObject->GetTypeAccessor().GetValue(pProp->GetPropertyName());

  out_row.m_pProp = pProp;
  out_row.m_sLabel = plTranslate(pProp->GetPropertyName());
  out_row.m_Original = value;

  if (const plClampValueAttribute* pClamp = pProp->GetAttributeByType<plClampValueAttribute>())
  {
    if (pClamp->GetMinValue().CanConvertTo<double>() && pClamp->GetMaxValue().CanConvertTo<double>())
    {
      out_row.m_bHasClamp = true;
      out_row.m_fMin = pClamp->GetMinValue().ConvertTo<double>();
      out_row.m_fMax = pClamp->GetMaxValue().ConvertTo<double>();
    }
  }

  const plRTTI* pPropType = pProp->GetSpecificType();

  if (pPropType->IsDerivedFrom<plEnumBase>() || pPropType->IsDerivedFrom<plBitflagsBase>())
  {
    plStringBuilder sValue;
    plReflectionUtils::EnumerationToString(pPropType, value.ConvertTo<plInt64>(), sValue);
    out_row.m_Kind = pPropType->IsDerivedFrom<plEnumBase>() ? plParticleValueRow::Kind::Enum : plParticleValueRow::Kind::Text;
    out_row.m_sValue = plTranslate(sValue);
  }
  else if (value.IsA<bool>())
  {
    out_row.m_Kind = plParticleValueRow::Kind::Bool;
    out_row.m_bBoolValue = value.Get<bool>();
    out_row.m_sValue = out_row.m_bBoolValue ? "true" : "false";
  }
  else if (value.IsA<plColor>() || value.IsA<plColorGammaUB>())
  {
    out_row.m_Kind = plParticleValueRow::Kind::Color;
    out_row.m_Color = value.IsA<plColor>() ? plColorGammaUB(value.Get<plColor>()) : value.Get<plColorGammaUB>();
  }
  else if (IsNumber(value))
  {
    DescribeNumber(value, out_row);
  }
  else if (pProp->GetAttributeByType<plAssetBrowserAttribute>() != nullptr)
  {
    out_row.m_Kind = plParticleValueRow::Kind::Asset;
    out_row.m_sValue = value.ConvertTo<plString>().IsEmpty() ? plString("none") : plString("assigned");
  }
  else if (value.CanConvertTo<plString>())
  {
    plStringBuilder s = value.ConvertTo<plString>();
    s.ReplaceAll("\n", " ");
    if (s.GetCharacterCount() > 16)
    {
      s.Shrink(0, s.GetCharacterCount() - 14);
      s.Append("...");
    }

    out_row.m_sValue = s.IsEmpty() ? plString("-") : plString(s);
  }
  else
  {
    out_row.m_sValue = "...";
  }
}

void plParticleGraphEditing::GetBlockProperties(const plDocumentObject* pBlock, plDynamicArray<const plAbstractProperty*>& out_properties)
{
  out_properties.Clear();

  // the same dynamic visibility rules the properties dock uses, so a block does not show fields
  // that are irrelevant for its current mode
  plMap<plString, plPropertyUiState> uiStates;
  plPropertyMetaState::GetSingleton()->GetTypePropertiesState(pBlock, uiStates);

  plHybridArray<const plAbstractProperty*, 32> properties;
  pBlock->GetTypeAccessor().GetType()->GetAllProperties(properties);

  for (const plAbstractProperty* pProp : properties)
  {
    if (pProp->GetCategory() != plPropertyCategory::Member || pProp->GetFlags().IsSet(plPropertyFlags::ReadOnly))
      continue;

    if (pProp->GetAttributeByType<plHiddenAttribute>() != nullptr)
      continue;

    auto itState = uiStates.Find(pProp->GetPropertyName());
    if (itState.IsValid() && itState.Value().m_Visibility == plPropertyUiState::Invisible)
      continue;

    out_properties.PushBack(pProp);
  }
}

void plParticleGraphEditing::GetOperatorProperties(const plDocumentObject* pNode, plDynamicArray<const plAbstractProperty*>& out_properties)
{
  out_properties.Clear();

  plHybridArray<const plAbstractProperty*, 16> properties;
  pNode->GetTypeAccessor().GetType()->GetAllProperties(properties);

  for (const plAbstractProperty* pProp : properties)
  {
    if (pProp->GetCategory() != plPropertyCategory::Member || pProp->GetFlags().IsSet(plPropertyFlags::ReadOnly))
      continue;

    // the name is the node's header, not a row
    if (plStringUtils::IsEqual(pProp->GetPropertyName(), "Name"))
      continue;

    out_properties.PushBack(pProp);
  }
}

plStringView plParticleGraphEditing::GetOperatorPinForProperty(const plDocumentObject* pNode, plStringView sProperty)
{
  for (const auto& input : plParticleOperatorInputs::Get(pNode->GetTypeAccessor().GetType()))
  {
    if (input.m_szLiteral != nullptr && sProperty == input.m_szLiteral)
      return input.m_szPin;
  }

  return {};
}

bool plParticleGraphEditing::GetOperatorPreview(const plDocumentObject* pNode, plParticleOperatorPreview& out_preview)
{
  out_preview = plParticleOperatorPreview();

  const plRTTI* pType = pNode->GetTypeAccessor().GetType();

  plStringView sValueProperty;
  const plParticleValueKind::Enum kind = plParticleValueKind::FromNode(pType, sValueProperty);
  if (sValueProperty.IsEmpty())
    return false;

  if (kind != plParticleValueKind::Color && kind != plParticleValueKind::Texture && kind != plParticleValueKind::Gradient && kind != plParticleValueKind::Curve)
    return false;

  const plAbstractProperty* pProp = pType->FindPropertyByName(sValueProperty);
  if (pProp == nullptr)
    return false;

  const plVariant value = pNode->GetTypeAccessor().GetValue(sValueProperty);
  out_preview.m_pProp = pProp;
  out_preview.m_Original = value;

  if (kind == plParticleValueKind::Color)
  {
    out_preview.m_bIsColor = true;
    out_preview.m_Color = value.IsA<plColor>() ? plColorGammaUB(value.Get<plColor>()) : value.ConvertTo<plColorGammaUB>();
    return true;
  }

  out_preview.m_Asset = plConversionUtils::ConvertStringToUuid(value.ConvertTo<plString>());
  return true;
}

plString plParticleGraphEditing::GetThumbnailPath(const plUuid& asset)
{
  if (!asset.IsValid())
    return {};

  plAssetCurator::plLockedSubAsset pSubAsset = plAssetCurator::GetSingleton()->GetSubAsset(asset);
  if (!pSubAsset.isValid())
    return {};

  return pSubAsset->m_pAssetInfo->GetManager()->GenerateResourceThumbnailPath(pSubAsset->m_pAssetInfo->m_Path, pSubAsset->m_Data.m_sName);
}

plString plParticleGraphEditing::GetAssetDisplayName(const plVariant& reference)
{
  const plString sReference = reference.ConvertTo<plString>();
  if (sReference.IsEmpty())
    return "none";

  if (plConversionUtils::IsStringUuid(sReference))
  {
    if (auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sReference))
      return pSubAsset->GetName();
  }

  return plStringBuilder(sReference).GetFileName();
}

//////////////////////////////////////////////////////////////////////////
// Values

plVariant plParticleGraphEditing::MakeNumberVariant(const plParticleValueRow& row, double fPrimary, double fVariance)
{
  if (row.m_Original.IsA<plVarianceTypeFloat>())
  {
    plVarianceTypeFloat v;
    v.m_Value = (float)fPrimary;
    v.m_fVariance = (float)fVariance;
    return plVariant(v);
  }

  if (row.m_Original.IsA<plVarianceTypeTime>())
  {
    plVarianceTypeTime v;
    v.m_Value = plTime::MakeFromSeconds(fPrimary);
    v.m_fVariance = (float)fVariance;
    return plVariant(v);
  }

  if (row.m_Original.IsA<plVarianceTypeAngle>())
  {
    plVarianceTypeAngle v;
    v.m_Value = plAngle::MakeFromDegree((float)fPrimary);
    v.m_fVariance = (float)fVariance;
    return plVariant(v);
  }

  if (row.m_Original.IsA<plTime>())
    return plVariant(plTime::MakeFromSeconds(fPrimary));

  if (row.m_Original.IsA<plAngle>())
    return plVariant(plAngle::MakeFromDegree((float)fPrimary));

  // plain numbers keep their exact storage type, otherwise the command is rejected
  return plVariant(fPrimary).ConvertTo(row.m_Original.GetType());
}

double plParticleGraphEditing::ScrubNumber(const plParticleValueRow& row, double fStartValue, double fDeltaX, bool bSecondary)
{
  // the step scales with the magnitude, so a rate of 200 and a size of 0.5 both feel right
  double fStep = row.m_bIsInteger ? 0.25 : 0.01;
  if (!row.m_bIsInteger)
  {
    const double fMagnitude = plMath::Abs(fStartValue);
    if (fMagnitude > 100.0)
      fStep = 1.0;
    else if (fMagnitude > 10.0)
      fStep = 0.1;
  }

  double fNew = fStartValue + fDeltaX * fStep;

  if (row.m_bIsInteger)
    fNew = plMath::Round(fNew);

  if (row.m_bHasClamp)
    fNew = plMath::Clamp(fNew, row.m_fMin, row.m_fMax);
  else if (bSecondary)
    fNew = plMath::Max(fNew, 0.0); // a deviation is never negative

  return fNew;
}

void plParticleGraphEditing::SetValue(const plDocument* pDocument, const plUuid& object, const plAbstractProperty* pProp, const plVariant& value)
{
  if (!value.IsValid() || pProp == nullptr)
    return;

  plSetObjectPropertyCommand cmd;
  cmd.m_Object = object;
  cmd.m_sProperty = pProp->GetPropertyName();
  cmd.m_NewValue = value;
  RunCommand(pDocument, "Change Value", cmd);
}

void plParticleGraphEditing::SetNumber(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row, double fPrimary, double fVariance)
{
  SetValue(pDocument, object, row.m_pProp, MakeNumberVariant(row, fPrimary, fVariance));
}

void plParticleGraphEditing::GetEnumChoices(const plParticleValueRow& row, plDynamicArray<plReflectionUtils::EnumKeyValuePair>& out_entries)
{
  out_entries.Clear();
  if (row.m_pProp == nullptr)
    return;

  plReflectionUtils::GetEnumKeysAndValues(row.m_pProp->GetSpecificType(), out_entries, plReflectionUtils::EnumConversionMode::ValueNameOnly);

  for (auto& entry : out_entries)
  {
    // via a builder: a translated view is not guaranteed to be null terminated
    const plStringBuilder sLabel = plTranslate(entry.m_sKey);
    entry.m_sKey = sLabel;
  }
}

void plParticleGraphEditing::ToggleBool(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row)
{
  if (row.m_Original.IsA<bool>())
    SetValue(pDocument, object, row.m_pProp, plVariant(!row.m_Original.Get<bool>()));
}

//////////////////////////////////////////////////////////////////////////
// Dialogs

void plParticleGraphEditing::PickColor(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row)
{
  const bool bIsLinear = row.m_Original.IsA<plColor>();
  const bool bAlpha = row.m_pProp->GetAttributeByType<plExposeColorAlphaAttribute>() != nullptr;

  plColor picked = bIsLinear ? row.m_Original.Get<plColor>() : plColor(row.m_Color);
  if (plEngineColorDialog::PickColor(picked, bAlpha, bIsLinear, row.m_sLabel))
    SetValue(pDocument, object, row.m_pProp, bIsLinear ? plVariant(picked) : plVariant(plColorGammaUB(picked)));
}

void plParticleGraphEditing::PickAsset(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row)
{
  const plAssetBrowserAttribute* pAttr = row.m_pProp->GetAttributeByType<plAssetBrowserAttribute>();
  if (pAttr == nullptr)
    return;

  const plUuid current = plConversionUtils::ConvertStringToUuid(row.m_Original.ConvertTo<plString>());

  plEngineAssetPicker::Result picked;
  if (!plEngineAssetPicker::PickAsset(nullptr, current, pAttr->GetTypeFilter(), picked))
    return;

  plStringBuilder sResult;
  if (picked.m_Guid.IsValid())
    plConversionUtils::ToString(picked.m_Guid, sResult);
  else
    sResult = picked.m_sPathRelative;

  SetValue(pDocument, object, row.m_pProp, plVariant(plString(sResult)));
}

void plParticleGraphEditing::TypeNumber(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row, bool bSecondary)
{
  // an unsigned row starts at 0: a negative value would wrap around in the stored type
  const double fLowest = IsUnsignedInteger(row.m_Original) ? 0.0 : -1000000.0;

  const double fCurrent = bSecondary ? row.m_fVariance : row.m_fNumber;
  const double fMin = row.m_bHasClamp ? row.m_fMin : fLowest;
  const double fMax = row.m_bHasClamp ? row.m_fMax : 1000000.0;

  plStringBuilder sLabel = row.m_sLabel;
  if (bSecondary)
    sLabel.Append(" (deviation)");

  double fNew = fCurrent;
  if (!AskNumber(sLabel, row.m_bIsInteger, fCurrent, bSecondary ? 0.0 : fMin, fMax, fNew))
    return;

  SetNumber(pDocument, object, row, bSecondary ? row.m_fNumber : fNew, bSecondary ? fNew : row.m_fVariance);
}

void plParticleGraphEditing::TypeText(const plDocument* pDocument, const plUuid& object, const plParticleValueRow& row)
{
  // an empty text is a value too
  bool bOk = false;
  const plString sNew = plEnginePrompt::GetText("Set Value", row.m_sLabel, row.m_Original.ConvertTo<plString>(), &bOk, plEnginePrompt::AcceptAll());

  if (bOk)
    SetValue(pDocument, object, row.m_pProp, plVariant(sNew));
}

void plParticleGraphEditing::SaveSystemAsTemplate(const plParticleEffectAssetDocument* pDocument, const plDocumentObject* pSystem)
{
  const plString sSystemName = pSystem->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();

  bool bOk = false;
  const plString sTemplateName = plEnginePrompt::GetText("Save System as Template", "Template name:", sSystemName, &bOk);

  if (!bOk || sTemplateName.IsEmpty())
    return;

  const plStatus status = pDocument->SaveSystemAsTemplate(pSystem, sTemplateName);

  if (status.Failed())
    plEditorUiServices::MessageBoxStatus(status, "Saving the template failed.");
  else
    plEditorUiServices::ShowAllDocumentsTemporaryStatusBarMessage(plFmt("Saved system '{}' as template '{}'.", sSystemName, sTemplateName), plTime::MakeFromSeconds(5));
}

//////////////////////////////////////////////////////////////////////////
// Blocks

void plParticleGraphEditing::GetContextBlocks(const plDocumentObjectManager* pManager, const plDocumentObject* pSystem, plParticleContext::Enum context, plDynamicArray<const plDocumentObject*>& out_blocks)
{
  out_blocks.Clear();

  const char* szProperty = plParticleContextInfo::Get(context).m_szProperty;
  const auto& accessor = pSystem->GetTypeAccessor();
  const plInt32 iCount = accessor.GetCount(szProperty);

  for (plInt32 i = 0; i < iCount; ++i)
  {
    const plVariant value = accessor.GetValue(szProperty, i);
    if (!value.IsA<plUuid>())
      continue;

    if (const plDocumentObject* pBlock = pManager->GetObject(value.Get<plUuid>()))
      out_blocks.PushBack(pBlock);
  }
}

void plParticleGraphEditing::GetBlockChoices(plParticleContext::Enum context, plDynamicArray<plParticleBlockChoice>& out_choices)
{
  out_choices.Clear();

  plDynamicArray<const plRTTI*> types;
  plParticleEffectNodeManager::GetBlockTypes(context, types);

  for (const plRTTI* pType : types)
  {
    plParticleBlockChoice& choice = out_choices.ExpandAndGetRef();
    choice.m_sName = GetBlockDisplayName(pType);
    choice.m_pType = pType;
  }

  struct NameLess
  {
    PL_ALWAYS_INLINE bool Less(const plParticleBlockChoice& a, const plParticleBlockChoice& b) const { return a.m_sName.Compare(b.m_sName) < 0; }
  };

  out_choices.Sort(NameLess());
}

plString plParticleGraphEditing::GetBlockDisplayName(const plRTTI* pType)
{
  return plParticleEffectNodeManager::FriendlyTypeName(pType);
}

plString plParticleGraphEditing::GetBlockSummary(const plDocumentObject* pBlock)
{
  const plRTTI* pType = pBlock->GetTypeAccessor().GetType();

  const plTitleAttribute* pTitle = pType->GetAttributeByType<plTitleAttribute>();
  if (pTitle == nullptr)
    return plString();

  plStringBuilder sTitle = pTitle->GetTitle();

  plHybridArray<const plAbstractProperty*, 32> properties;
  pType->GetAllProperties(properties);

  plStringBuilder sValue, sToken;
  for (const plAbstractProperty* pProp : properties)
  {
    if (pProp->GetCategory() != plPropertyCategory::Member)
      continue;

    const plVariant value = pBlock->GetTypeAccessor().GetValue(pProp->GetPropertyName());

    if (pProp->GetSpecificType()->IsDerivedFrom<plEnumBase>() || pProp->GetSpecificType()->IsDerivedFrom<plBitflagsBase>())
    {
      plReflectionUtils::EnumerationToString(pProp->GetSpecificType(), value.ConvertTo<plInt64>(), sValue);
      sValue = plTranslate(sValue);
    }
    else if (value.CanConvertTo<plString>())
    {
      sValue = value.ConvertTo<plString>();
    }
    else
    {
      sValue.Clear();
    }

    sToken.Set("{", pProp->GetPropertyName(), "}");
    sTitle.ReplaceAll(sToken, sValue);

    sToken.Set("{?", pProp->GetPropertyName(), "}");
    sTitle.ReplaceAll(sToken, value == plVariant(0) ? plStringView() : plStringView(sValue));
  }

  sTitle.Trim("\' :");

  if (sTitle.GetCharacterCount() > 22)
  {
    sTitle.Shrink(0, sTitle.GetCharacterCount() - 20);
    sTitle.Append("...");
  }

  return sTitle;
}

plString plParticleGraphEditing::GetSinkTargetLabel(const plDocumentObject* pBlock)
{
  const plRTTI* pType = pBlock->GetTypeAccessor().GetType();
  if (!pType->IsDerivedFrom<plParticleInitializerFactory_SetAttribute>() && !pType->IsDerivedFrom<plParticleBehaviorFactory_Attribute>())
    return {};

  const auto& accessor = pBlock->GetTypeAccessor();
  const plInt64 iTarget = accessor.GetValue("Target").ConvertTo<plInt64>();
  const plInt64 iCustomType = accessor.GetValue("CustomType").ConvertTo<plInt64>();

  plStringBuilder sLabel;
  if (iTarget == plParticleSinkTarget::Custom)
  {
    sLabel.Set("Custom.", accessor.GetValue("CustomAttribute").ConvertTo<plString>());
  }
  else
  {
    plReflectionUtils::EnumerationToString(plGetStaticRTTI<plParticleSinkTarget>(), iTarget, sLabel, plReflectionUtils::EnumConversionMode::ValueNameOnly);
    sLabel = plParticleEffectNodeManager::SplitWords(sLabel);
  }

  plParticleAttributeSinkSettings settings;
  settings.m_Target = (plParticleSinkTarget::Enum)iTarget;
  settings.m_CustomType = (plParticleCustomAttributeType::Enum)iCustomType;
  settings.m_Component = (plParticleSinkComponent::Enum)accessor.GetValue("Component").ConvertTo<plInt64>();

  if (settings.GetWrittenComponent() >= 0)
    sLabel.Append(".", GetSinkComponentLabel(iTarget, iCustomType, settings.m_Component));

  return sLabel;
}

plString plParticleGraphEditing::GetSinkComponentLabel(plInt64 iTarget, plInt64 iCustomType, plInt64 iComponent)
{
  const bool bColor = iTarget == plParticleSinkTarget::Color || (iTarget == plParticleSinkTarget::Custom && iCustomType == plParticleCustomAttributeType::Color);

  static const char* s_szVector[] = {"All", "X", "Y", "Z", "W"};
  static const char* s_szColor[] = {"All", "R", "G", "B", "A"};

  if (iComponent < 0 || iComponent > 4)
    return "All";

  return bColor ? s_szColor[iComponent] : s_szVector[iComponent];
}

bool plParticleGraphEditing::IsPerParticleProperty(const plAbstractProperty* pProp)
{
  return pProp != nullptr && pProp->GetAttributeByType<plParticlePerParticleAttribute>() != nullptr;
}

bool plParticleGraphEditing::IsBlockEnabled(const plDocumentObject* pBlock)
{
  const plVariant enabled = pBlock->GetTypeAccessor().GetValue("Enabled");
  return !enabled.IsA<bool>() || enabled.Get<bool>();
}

bool plParticleGraphEditing::IsBlock(const plDocumentObject* pObject)
{
  return pObject != nullptr && pObject->GetParent() != nullptr && pObject->GetParent()->GetTypeAccessor().GetType()->IsDerivedFrom<plParticleSystemNode>();
}

void plParticleGraphEditing::AddBlock(const plDocument* pDocument, const plDocumentObject* pSystem, plParticleContext::Enum context, const plRTTI* pType)
{
  const char* szProperty = plParticleContextInfo::Get(context).m_szProperty;

  plAddObjectCommand cmd;
  cmd.m_pType = pType;
  cmd.m_Parent = pSystem->GetGuid();
  cmd.m_sParentProperty = szProperty;
  cmd.m_Index = pSystem->GetTypeAccessor().GetCount(szProperty);
  RunCommand(pDocument, "Add Block", cmd);
}

void plParticleGraphEditing::AddCustomAttribute(const plDocument* pDocument, const plDocumentObject* pSystem, plArrayPtr<const plString> taken)
{
  plStringBuilder sName;
  for (plUInt32 i = 1;; ++i)
  {
    sName.SetFormat("Attribute{}", i);
    if (!taken.Contains(sName))
      break;
  }

  const char* szProperty = plParticleContextInfo::Get(plParticleContext::Initialize).m_szProperty;

  plCommandHistory* pHistory = pDocument->GetCommandHistory();
  pHistory->StartTransaction("Add Custom Attribute");

  plAddObjectCommand add;
  add.m_pType = plGetStaticRTTI<plParticleInitializerFactory_SetAttribute>();
  add.m_Parent = pSystem->GetGuid();
  add.m_sParentProperty = szProperty;
  add.m_Index = pSystem->GetTypeAccessor().GetCount(szProperty);
  add.m_NewObjectGuid = plUuid::MakeUuid();

  plSetObjectPropertyCommand target;
  target.m_Object = add.m_NewObjectGuid;
  target.m_sProperty = "Target";
  target.m_NewValue = (plInt64)plParticleSinkTarget::Custom;

  plSetObjectPropertyCommand name;
  name.m_Object = add.m_NewObjectGuid;
  name.m_sProperty = "CustomAttribute";
  name.m_NewValue = plString(sName);

  if (pHistory->AddCommand(add).Failed() || pHistory->AddCommand(target).Failed() || pHistory->AddCommand(name).Failed())
    pHistory->CancelTransaction();
  else
    pHistory->FinishTransaction();
}

void plParticleGraphEditing::RemoveBlock(const plDocument* pDocument, const plDocumentObject* pBlock)
{
  plRemoveObjectCommand cmd;
  cmd.m_Object = pBlock->GetGuid();
  RunCommand(pDocument, "Remove Block", cmd);
}

void plParticleGraphEditing::ReorderBlock(const plDocument* pDocument, const plDocumentObject* pSystem, const plDocumentObject* pBlock, plParticleContext::Enum context, plInt32 iNewIndex)
{
  const plInt32 iOldIndex = pBlock->GetPropertyIndex().ConvertTo<plInt32>();

  // plMoveObjectCommand indexes into the list as it is before the move, so moving down by one
  // means targeting the slot after the next element
  if (iNewIndex == iOldIndex || iNewIndex == iOldIndex + 1)
    return;

  plMoveObjectCommand cmd;
  cmd.m_Object = pBlock->GetGuid();
  cmd.m_NewParent = pSystem->GetGuid();
  cmd.m_sParentProperty = plParticleContextInfo::Get(context).m_szProperty;
  cmd.m_Index = iNewIndex;
  RunCommand(pDocument, "Reorder Block", cmd);
}

void plParticleGraphEditing::ToggleBlockEnabled(const plDocument* pDocument, const plDocumentObject* pBlock)
{
  if (const plAbstractProperty* pEnabled = pBlock->GetTypeAccessor().GetType()->FindPropertyByName("Enabled"))
    SetValue(pDocument, pBlock->GetGuid(), pEnabled, !IsBlockEnabled(pBlock));
}

void plParticleGraphEditing::SelectObject(const plDocument* pDocument, const plDocumentObject* pObject)
{
  pDocument->GetSelectionManager()->SetSelection(pObject);
}

bool plParticleGraphEditing::RemoveSelectedBlock(const plDocument* pDocument)
{
  const auto& selection = pDocument->GetSelectionManager()->GetSelection();
  if (selection.GetCount() != 1 || !IsBlock(selection[0]))
    return false;

  RemoveBlock(pDocument, selection[0]);
  return true;
}

//////////////////////////////////////////////////////////////////////////
// Looks

plColorGammaUB plParticleGraphEditing::GetContextColor(plParticleContext::Enum context)
{
  switch (context)
  {
    case plParticleContext::Spawn:
      return plColorGammaUB(224, 145, 58);
    case plParticleContext::Initialize:
      return plColorGammaUB(103, 184, 89);
    case plParticleContext::Update:
      return plColorGammaUB(63, 169, 196);
    case plParticleContext::Events:
      return plColorGammaUB(224, 163, 92); // the event colour of the rows and wires
    default:
      return plColorGammaUB(168, 116, 201);
  }
}

plColorGammaUB plParticleGraphEditing::GetValueKindColor(plParticleValueKind::Enum kind)
{
  switch (kind)
  {
    case plParticleValueKind::Number:
      return plColorGammaUB(1, 163, 243);
    case plParticleValueKind::Bool:
      return plColorGammaUB(217, 96, 92);
    case plParticleValueKind::Color:
      return plColorGammaUB(224, 179, 58);
    case plParticleValueKind::Texture:
      return plColorGammaUB(168, 116, 201);
    case plParticleValueKind::Gradient:
      return plColorGammaUB(224, 145, 58);
    case plParticleValueKind::Curve:
      return plColorGammaUB(103, 184, 89);
    default:
      return plColorGammaUB(170, 180, 196);
  }
}

plString plParticleGraphEditing::GetOperatorTitle(const plDocumentObject* pNode)
{
  // the name is edited from the properties dock; it is not one of the values worth a row. Most operators have none,
  // and an invalid variant would read "<Invalid>".
  const plVariant name = pNode->GetTypeAccessor().GetValue("Name");
  if (name.IsValid() && !name.ConvertTo<plString>().IsEmpty())
    return name.ConvertTo<plString>();

  // a node that reads one of several inputs is titled after the one it reads, as its palette entry is ("Event Normal")
  const plRTTI* pType = pNode->GetTypeAccessor().GetType();
  if (const plTitleAttribute* pTitle = pType->GetAttributeByType<plTitleAttribute>(); pTitle != nullptr && plStringView(pTitle->GetTitle()) == "{Input}")
  {
    if (const plAbstractProperty* pInput = pType->FindPropertyByName("Input"); pInput != nullptr && pInput->GetSpecificType()->IsDerivedFrom<plEnumBase>())
    {
      plStringBuilder sValue;
      plReflectionUtils::EnumerationToString(pInput->GetSpecificType(), pNode->GetTypeAccessor().GetValue("Input").ConvertTo<plInt64>(), sValue);
      return plTranslate(sValue);
    }
  }

  return plParticleEffectNodeManager::FriendlyTypeName(pType);
}

plString plParticleGraphEditing::GetSystemTitle(const plDocumentObject* pSystem)
{
  const plString sName = pSystem->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
  return sName.IsEmpty() ? plString("System") : sName;
}

plString plParticleGraphEditing::GetSimulationTargetLabel(const plDocumentObject* pSystem)
{
  plStringBuilder sTarget;
  plReflectionUtils::EnumerationToString(plGetStaticRTTI<plParticleSimulationTarget>(), pSystem->GetTypeAccessor().GetValue("SimulationTarget").ConvertTo<plInt64>(), sTarget);
  return plTranslate(sTarget);
}

plString plParticleGraphEditing::GetEffectSubtitle(const plDocumentObject* pEffect)
{
  const plInt32 iReactions = pEffect->GetTypeAccessor().GetCount("EventReactions");
  if (iReactions <= 0)
    return "no event reactions";

  plStringBuilder sSubtitle;
  sSubtitle.SetFormat("{0} event reaction{1}", iReactions, iReactions == 1 ? "" : "s");
  return sSubtitle;
}
