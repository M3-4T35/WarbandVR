#pragma once

namespace engine {

// Combat physique : mbMission::checkAttackCollision teste le balayage de l'arme d'un agent
// contre les autres (pose de l'arme = os de l'arme, donc nos bras VR pour le joueur).
// Arme toujours active : checkAttackCollision appelé à chaque frame quand la lame du joueur
// va vite (mêlée uniquement, arme lue dans son agent).
bool InstallCombatHook();
void RemoveCombatHook();

// Thread de jeu, à chaque frame : lit l'arme tenue par le joueur (type itp_type_*) et en
// déduit les modes de combat (arc...). Sans effet hors mission.
void UpdatePlayerWeapon();

// Les mains du joueur gardent l'orientation de l'animation du jeu : arme à distance (arc,
// arbalète, armes à feu, de jet) en visée, tir ou rechargement, et 1 s après. Tout thread.
bool PlayerAimingRangedWeapon();

// Le joueur tient une arme à distance tenue à deux mains (arc, arbalète, pistolet, mousquet) :
// ses bras gardent alors l'animation du jeu. Lisible depuis n'importe quel thread.
bool PlayerHoldsRangedWeapon();

} // namespace engine
