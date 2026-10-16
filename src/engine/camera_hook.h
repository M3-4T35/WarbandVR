#pragma once
#include "engine/rgl.h"

namespace engine {

// Suivi de la tête : hooks de rglScene::render et rglRenderer::renderContext (trouvés par
// signature). Désactivé proprement si une signature manque.
bool InstallCameraHook();
void RemoveCameraHook();

// Dernière caméra de la scène principale telle que calculée par le jeu (avant la VR).
// false si aucune scène n'a encore été rendue.
bool GetGameCamera(rgl::Transform& out);

// Vecteur « arrière » horizontal du repère de la vue VR dans le monde (cap du corps, piloté
// au stick ; la caméra du jeu, elle, suit la visée de la main gauche). false hors stéréo.
bool GetBodyBack(rgl::Vector4& back);

// Thread de rendu, au Present : fin de l'image pour le recensement des contextes rendus
// (diagnostic). Écrit la liste dans le log si elle a changé, ou toujours avec force.
void EndContextFrame(bool force);

} // namespace engine
