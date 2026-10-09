#pragma once
// Détours compatibles avec Windows sur ARM et Proton sur ARM (Steam Frame) : le jeu x64 y
// tourne émulé, mais les DLL du système, de Wine et DXVK sont compilées en ARM64EC (code ARM
// natif appelé par le code x64). MinHook ne sait réécrire que du code x64 : pour ces
// fonctions, on détourne le pointeur qui y mène (table virtuelle d'un objet COM, table
// d'imports du jeu) au lieu de leur code.
#include <windows.h>

#include <vector>

namespace hook {

// true si l'adresse est du code ARM64EC natif (non réécrivable par MinHook)
bool IsArm64EcCode(const void* code);

// Méthode COM n° index de l'objet : MinHook si son code est x64 (cible ajoutée à mhTargets,
// à retirer par l'appelant), sinon remplacement de l'entrée de la table virtuelle (restaurée
// par RemoveAll). Une méthode ou une entrée déjà détournée n'est pas reprise (*original
// inchangé). prefix : préfixe du log ("[render]"...). true si détournée ou déjà détournée.
bool HookMethod(void* object, int index, void* detour, void** original, const char* name, const char* prefix,
                std::vector<void*>& mhTargets);

// Fonction importée par module (table d'imports) : true si détournée. Restaurée par RemoveAll.
bool HookImport(HMODULE module, const char* dll, const char* function, void* detour, void** original,
                const char* prefix);

// Restaure les tables virtuelles et d'imports modifiées
void RemoveAll();

} // namespace hook
