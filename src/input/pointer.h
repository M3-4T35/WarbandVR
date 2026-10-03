#pragma once
// Pointeur VR pour les menus : la manette vise l'écran virtuel et pilote la souris du jeu.
//
// Position : GetCursorPos est détourné pour renvoyer le point visé (même hors du bureau,
// la fenêtre du jeu pouvant être plus grande que l'écran). Boutons et molette : injectés
// dans la souris DirectInput que le jeu lit (voir dinput_hook), le vrai curseur ne bouge pas.

namespace input {

bool InstallPointerHooks();
void RemovePointerHooks();

// Thread XR : point visé en coordonnées normalisées de la zone cliente (0..1), ou aucun
void SetPointerTarget(bool onScreen, float u, float v);

// Thread XR : bouton gauche / droit, molette (crans, positif = vers le haut)
void SetPointerButton(bool left, bool down);
void ScrollPointer(int notches);

} // namespace input
