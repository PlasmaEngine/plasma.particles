#pragma once

#include <Foundation/Basics.h>
#include <GuiFoundation/VisualGraph/Scene.moc.h>

class plQtParticleEffectGraphScene : public plQtVisualGraphScene
{
  Q_OBJECT

public:
  plQtParticleEffectGraphScene(QObject* pParent = nullptr);
  ~plQtParticleEffectGraphScene();
};