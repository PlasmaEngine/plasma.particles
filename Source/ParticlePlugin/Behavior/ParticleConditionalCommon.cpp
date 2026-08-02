#include <ParticlePlugin/ParticlePluginPCH.h>

#include <Foundation/DataProcessing/Stream/ProcessingStream.h>
#include <ParticlePlugin/Behavior/ParticleConditionalCommon.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleAttribute, 1)
  PL_ENUM_CONSTANT(plParticleAttribute::PositionX),
  PL_ENUM_CONSTANT(plParticleAttribute::PositionY),
  PL_ENUM_CONSTANT(plParticleAttribute::PositionZ),
  PL_ENUM_CONSTANT(plParticleAttribute::Speed),
  PL_ENUM_CONSTANT(plParticleAttribute::Size),
  PL_ENUM_CONSTANT(plParticleAttribute::LifeFraction),
  PL_ENUM_CONSTANT(plParticleAttribute::ColorR),
  PL_ENUM_CONSTANT(plParticleAttribute::ColorG),
  PL_ENUM_CONSTANT(plParticleAttribute::ColorB),
  PL_ENUM_CONSTANT(plParticleAttribute::ColorA),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plParticleConditionOp, 1)
  PL_ENUM_CONSTANT(plParticleConditionOp::Less),
  PL_ENUM_CONSTANT(plParticleConditionOp::LessEqual),
  PL_ENUM_CONSTANT(plParticleConditionOp::Greater),
  PL_ENUM_CONSTANT(plParticleConditionOp::GreaterEqual),
  PL_ENUM_CONSTANT(plParticleConditionOp::Equal),
  PL_ENUM_CONSTANT(plParticleConditionOp::NotEqual),
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

float plReadParticleAttribute(
  plParticleAttribute::Enum attr, plUInt32 uiIndex,
  const plProcessingStream* pPosition,
  const plProcessingStream* pVelocity,
  const plProcessingStream* pSize,
  const plProcessingStream* pColor,
  const plProcessingStream* pLifeTime)
{
  switch (attr)
  {
    case plParticleAttribute::PositionX:
      return pPosition ? static_cast<float>(pPosition->GetData<plSimdVec4f>()[uiIndex].GetComponent<0>()) : 0.0f;
    case plParticleAttribute::PositionY:
      return pPosition ? static_cast<float>(pPosition->GetData<plSimdVec4f>()[uiIndex].GetComponent<1>()) : 0.0f;
    case plParticleAttribute::PositionZ:
      return pPosition ? static_cast<float>(pPosition->GetData<plSimdVec4f>()[uiIndex].GetComponent<2>()) : 0.0f;
    case plParticleAttribute::Speed:
      return pVelocity ? static_cast<float>(pVelocity->GetData<plFloat16Vec4>()[uiIndex].w) : 0.0f;
    case plParticleAttribute::Size:
      return pSize ? static_cast<float>(pSize->GetData<plFloat16>()[uiIndex]) : 1.0f;
    case plParticleAttribute::LifeFraction:
    {
      if (pLifeTime)
      {
        const plVec2& lt = pLifeTime->GetData<plVec2>()[uiIndex];
        return 1.0f - lt.x * lt.y;
      }
      return 0.0f;
    }
    case plParticleAttribute::ColorR:
      return pColor ? static_cast<float>(pColor->GetData<plFloat16Vec4>()[uiIndex].x) : 1.0f;
    case plParticleAttribute::ColorG:
      return pColor ? static_cast<float>(pColor->GetData<plFloat16Vec4>()[uiIndex].y) : 1.0f;
    case plParticleAttribute::ColorB:
      return pColor ? static_cast<float>(pColor->GetData<plFloat16Vec4>()[uiIndex].z) : 1.0f;
    case plParticleAttribute::ColorA:
      return pColor ? static_cast<float>(pColor->GetData<plFloat16Vec4>()[uiIndex].w) : 1.0f;
    default:
      return 0.0f;
  }
}

void plWriteParticleAttribute(
  plParticleAttribute::Enum attr, plUInt32 uiIndex, float fValue,
  plProcessingStream* pPosition,
  plProcessingStream* pVelocity,
  plProcessingStream* pSize,
  plProcessingStream* pColor)
{
  switch (attr)
  {
    case plParticleAttribute::PositionX:
    {
      if (pPosition)
      {
        plSimdVec4f& pos = pPosition->GetWritableData<plSimdVec4f>()[uiIndex];
        pos.SetX(plSimdFloat(fValue));
      }
      break;
    }
    case plParticleAttribute::PositionY:
    {
      if (pPosition)
      {
        plSimdVec4f& pos = pPosition->GetWritableData<plSimdVec4f>()[uiIndex];
        pos.SetY(plSimdFloat(fValue));
      }
      break;
    }
    case plParticleAttribute::PositionZ:
    {
      if (pPosition)
      {
        plSimdVec4f& pos = pPosition->GetWritableData<plSimdVec4f>()[uiIndex];
        pos.SetZ(plSimdFloat(fValue));
      }
      break;
    }
    case plParticleAttribute::Speed:
    {
      if (pVelocity)
      {
        plFloat16Vec4& v = pVelocity->GetWritableData<plFloat16Vec4>()[uiIndex];
        v = plFloat16Vec4(plVec4(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z), fValue));
      }
      break;
    }
    case plParticleAttribute::Size:
    {
      if (pSize)
        pSize->GetWritableData<plFloat16>()[uiIndex] = fValue;
      break;
    }
    case plParticleAttribute::ColorR:
    {
      if (pColor)
      {
        plFloat16Vec4& c = pColor->GetWritableData<plFloat16Vec4>()[uiIndex];
        c = plFloat16Vec4(plVec4(fValue, static_cast<float>(c.y), static_cast<float>(c.z), static_cast<float>(c.w)));
      }
      break;
    }
    case plParticleAttribute::ColorG:
    {
      if (pColor)
      {
        plFloat16Vec4& c = pColor->GetWritableData<plFloat16Vec4>()[uiIndex];
        c = plFloat16Vec4(plVec4(static_cast<float>(c.x), fValue, static_cast<float>(c.z), static_cast<float>(c.w)));
      }
      break;
    }
    case plParticleAttribute::ColorB:
    {
      if (pColor)
      {
        plFloat16Vec4& c = pColor->GetWritableData<plFloat16Vec4>()[uiIndex];
        c = plFloat16Vec4(plVec4(static_cast<float>(c.x), static_cast<float>(c.y), fValue, static_cast<float>(c.w)));
      }
      break;
    }
    case plParticleAttribute::ColorA:
    {
      if (pColor)
      {
        plFloat16Vec4& c = pColor->GetWritableData<plFloat16Vec4>()[uiIndex];
        c = plFloat16Vec4(plVec4(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), fValue));
      }
      break;
    }
    case plParticleAttribute::LifeFraction:
      // LifeFraction is read-only (derived from remaining life)
      break;
    default:
      break;
  }
}

bool plEvaluateParticleCondition(plParticleConditionOp::Enum op, float fValue, float fThreshold)
{
  switch (op)
  {
    case plParticleConditionOp::Less:
      return fValue < fThreshold;
    case plParticleConditionOp::LessEqual:
      return fValue <= fThreshold;
    case plParticleConditionOp::Greater:
      return fValue > fThreshold;
    case plParticleConditionOp::GreaterEqual:
      return fValue >= fThreshold;
    case plParticleConditionOp::Equal:
      return plMath::IsEqual(fValue, fThreshold, 0.001f);
    case plParticleConditionOp::NotEqual:
      return !plMath::IsEqual(fValue, fThreshold, 0.001f);
    default:
      return false;
  }
}


PL_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_Behavior_ParticleConditionalCommon);
