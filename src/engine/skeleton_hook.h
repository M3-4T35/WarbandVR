#pragma once
#include "engine/rgl.h"

namespace engine {

// Squelettes animés : hooks de rglSkeleton::frameMove (repère monde de chaque squelette) et
// rglSkeleton::updateBonePositions (pose des os). Sert à faire suivre les bras du joueur par
// les manettes. Désactivé proprement si une signature manque.
bool InstallSkeletonHook();
void RemoveSkeletonHook();

// Centre du bouclier du joueur dans le monde (milieu de l'avant-bras gauche, placé par les
// bras VR). false si les bras VR ne s'appliquent pas depuis plus de 200 ms.
bool GetPlayerShieldCenter(rgl::Vector4& out);

} // namespace engine
