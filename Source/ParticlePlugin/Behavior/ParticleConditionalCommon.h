#pragma once

#include <Foundation/Math/Float16.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/SimdMath/SimdVec4f.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

class plProcessingStream;

/// Particle attribute that can be read or written by conditional behaviors.
///
/// Used by IF, Switch, Remap, and other logic nodes to select which
/// particle property to evaluate or modify.
struct PL_PARTICLEPLUGIN_DLL plParticleAttribute
{
  using StorageType = plUInt8;

  enum Enum
  {
    PositionX,
    PositionY,
    PositionZ,
    Speed,
    Size,
    LifeFraction,
    ColorR,
    ColorG,
    ColorB,
    ColorA,

    Default = Size
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleAttribute);

/// Comparison operator for conditional behaviors.
struct PL_PARTICLEPLUGIN_DLL plParticleConditionOp
{
  using StorageType = plUInt8;

  enum Enum
  {
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,    ///< Within epsilon (0.001)
    NotEqual, ///< Outside epsilon (0.001)

    Default = Less
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PARTICLEPLUGIN_DLL, plParticleConditionOp);

/// Reads a particle attribute value at the given index.
/// Returns 0 if the required stream is null.
PL_PARTICLEPLUGIN_DLL float plReadParticleAttribute(
  plParticleAttribute::Enum attr, plUInt32 uiIndex,
  const plProcessingStream* pPosition,
  const plProcessingStream* pVelocity,
  const plProcessingStream* pSize,
  const plProcessingStream* pColor,
  const plProcessingStream* pLifeTime);

/// Writes a float value to a particle attribute at the given index.
PL_PARTICLEPLUGIN_DLL void plWriteParticleAttribute(
  plParticleAttribute::Enum attr, plUInt32 uiIndex, float fValue,
  plProcessingStream* pPosition,
  plProcessingStream* pVelocity,
  plProcessingStream* pSize,
  plProcessingStream* pColor);

/// Evaluates a comparison between fValue and fThreshold.
PL_PARTICLEPLUGIN_DLL bool plEvaluateParticleCondition(
  plParticleConditionOp::Enum op, float fValue, float fThreshold);
