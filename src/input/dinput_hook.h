#pragma once
// Injection d'événements souris et clavier dans DirectInput : le jeu lit ses entrées via
// IDirectInputDevice8 (GetDeviceData tamponné / GetDeviceState). Les fonctions sont trouvées sur un
// périphérique souris factice (pas d'adresse en dur) puis détournées ; les événements du
// pointeur VR sont ajoutés à ceux de la vraie souris.
#include <cstdint>

namespace input {

bool InstallDInputHooks();
void RemoveDInputHooks();

// Souris. Bouton : 0 = gauche, 1 = droit. Molette : crans, positif = vers le haut.
// Mouvement : relatif, en unités de souris (comme une vraie souris).
void InjectMouseButton(int button, bool down);
void InjectMouseWheel(int notches);
void InjectMouseMove(int dx, int dy);

// Vraie souris (pas nos injections), relevée à la lecture du jeu. Mouvement lu par état
// (combat) : à consommer par le rendu de la scène. Mouvement lu en événements (interface) :
// à consommer par le curseur des menus. Activité : compteur croissant (mouvement + clics),
// pour savoir quel périphérique le joueur utilise.
void TakeRealMouseMotion(int& dx, int& dy);
void TakeRealCursorMotion(int& dx, int& dy);
bool RealMouseButtonHeld();
unsigned long long RealMouseActivity();

// Clavier : code DIK_* (dinput.h), appuyé / relâché
void InjectKey(int dik, bool down);

// Relâche tout ce qui est tenu (changement de mode, perte de la session)
void ReleaseAllInjected();

} // namespace input
