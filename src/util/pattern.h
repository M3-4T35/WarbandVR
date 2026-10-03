#pragma once
#include <windows.h>

namespace pattern {

// Cherche une signature du type "48 8B C4 ?? 89" dans les sections exécutables du
// module (l'exe du jeu par défaut). Renvoie nullptr si absente ou ambiguë (loggué).
void* Find(const char* name, const char* signature, HMODULE module = nullptr);

// Variable globale lue par une instruction RIP-relative : toutes les occurrences de la
// signature sont résolues (déplacement 32 bits à dispOffset, instruction de instrLength
// octets) et doivent désigner la même adresse. nullptr sinon (loggué).
void* FindGlobal(const char* name, const char* signature, int dispOffset, int instrLength, HMODULE module = nullptr);

} // namespace pattern
