#pragma once
// Fenêtre du jeu : WSE2 tourne en fenêtré et rend à la taille de la zone cliente. Pour la VR
// on la redimensionne à la résolution recommandée par le casque (presque carrée), ce qui
// fait refaire au jeu son rendu à cette taille (Reset du device).
#include <windows.h>

namespace render {

// Fenêtre de rendu du jeu (hDeviceWindow), renseignée au premier Present
void SetGameWindow(HWND window);
HWND GameWindow();

// Redimensionne la zone cliente de la fenêtre du jeu (appelable depuis n'importe quel
// thread). Lève la limite de taille de Windows (taille de l'écran) si nécessaire.
void ResizeGameWindow(UINT clientWidth, UINT clientHeight);

// Met la fenêtre du jeu au premier plan : DirectInput ne donne la souris au jeu que s'il a
// le focus. Renvoie true si le jeu est au premier plan.
bool BringGameToFront();

// Déchargement de la DLL : rend à la fenêtre sa procédure d'origine
void RestoreGameWindow();

} // namespace render
