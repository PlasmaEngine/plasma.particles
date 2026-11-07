#pragma once

#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Color.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Strings/HashedString.h>
#include <Foundation/Types/ArrayPtr.h>
#include <ParticlePlugin/ParticlePluginDLL.h>

struct PL_PARTICLEPLUGIN_DLL plParticleEvent
{
  PL_DECLARE_POD_TYPE();

  plTempHashedString m_EventType;
  plVec3 m_vPosition;
  plVec3 m_vDirection;
  plVec3 m_vNormal;

  /// The particle that raised the event, for On Event's Inherit properties and the Event inputs.
  plVec3 m_vVelocity;
  plColor m_Color;
  float m_fSize;
};

using plParticleEventQueue = plArrayPtr<plParticleEvent>;

/// The events one occasion raises (a system's deaths, a Raycast's collisions). The descriptor keeps
/// several names in one string, separated by ';', as the editor's Events blocks give them.
using plParticleEventNames = plHybridArray<plTempHashedString, 1>;

PL_PARTICLEPLUGIN_DLL void plParticleSplitEventNames(plStringView sNames, plParticleEventNames& out_names);
