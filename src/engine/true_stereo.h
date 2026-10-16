#pragma once
// Vraie stéréo : les deux yeux rendus à chaque image du jeu (réglage true_stereo), au lieu
// d'un œil par image (rendu alterné).
//
// La scène principale est remplie par rglScene::render dans le contexte du widget de scène
// (œil A). Pour l'œil B, on rappelle rglScene::render avec un second contexte qui nous
// appartient : pris une fois dans le pool du moteur (rglResourceManager::getRenderContext),
// jamais rendu, et reconfiguré à chaque image comme celui du widget (rglRenderContext::
// setSourceContext du moteur, puis les cibles HDR, la profondeur et les nœuds de post-effets
// qu'elle ne recopie pas). Chaque œil a ainsi son propre culling ; le renderer rend les deux
// contextes à la suite, chacun avec ses post-effets jusqu'au back buffer. La simulation, les
// ombres et l'interface ne sont faites qu'une fois.
#include "engine/rgl.h"

namespace engine::stereo {

// Signatures des fonctions du moteur utilisées. false : vraie stéréo indisponible (loggué),
// le rendu alterné reste utilisé.
bool Init();

// Réglage actif et fonctions du moteur trouvées
bool Enabled();

// Thread du jeu, dans rglScene::render de la scène principale, AVANT le remplissage du
// contexte du widget : second contexte reconfiguré comme main (caméra comprise, celle du jeu).
// nullptr si indisponible pour cette image (encore en file...).
rgl::RenderContext* PrepareSecond(rgl::RenderContext* main);

// Autour du second remplissage : l'adaptation des ombres factices des personnages de la
// scène ne doit pas voir deux rendus par image (sinon sa portée dérive).
struct SceneShadowState {
    int count = 0;
    float range = 0;
};
SceneShadowState SaveSceneShadows(void* scene);
void RestoreSceneShadows(void* scene, const SceneShadowState& afterFirst);

// Thread de rendu, avant le rendu de chaque scène d'œil en vraie stéréo : la réfraction de l'eau
// (screen_texture) est une copie de la scène prise au premier maillage d'eau, une seule fois par
// image du renderer. Sans cela, le second œil rendu montrait sous son eau la scène du premier.
void BeforeEyeScene(void* renderer);

} // namespace engine::stereo
