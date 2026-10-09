#pragma once
#include <windows.h>

#include <cstdint>
#include <initializer_list>

namespace pattern {

// Cherche une signature du type "48 8B C4 ?? 89" dans les sections exécutables du
// module (l'exe du jeu par défaut). Renvoie nullptr si absente ou ambiguë (loggué).
void* Find(const char* name, const char* signature, HMODULE module = nullptr);

// Variable globale lue par une instruction RIP-relative : toutes les occurrences de la
// signature sont résolues (déplacement 32 bits à dispOffset, instruction de instrLength
// octets) et doivent désigner la même adresse. nullptr sinon (loggué).
void* FindGlobal(const char* name, const char* signature, int dispOffset, int instrLength, HMODULE module = nullptr);

// Plusieurs signatures possibles d'une même fonction (une par version connue de WSE2) : la
// première trouvée, et unique, l'emporte. nullptr si aucune (loggué une seule fois).
void* FindFirst(const char* name, std::initializer_list<const char*> signatures, HMODULE module = nullptr);

// Constante 32 bits lue dans le code (ex. taille d'une structure dans un imul) : toutes les
// occurrences de la signature doivent donner la même valeur, à immOffset. 0 sinon (loggué).
uint32_t FindImm32(const char* name, const char* signature, int immOffset, HMODULE module = nullptr);

} // namespace pattern
