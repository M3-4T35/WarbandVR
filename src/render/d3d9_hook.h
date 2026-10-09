#pragma once

namespace render {

// Installe les hooks Present/Reset de Direct3D 9. Renvoie false (et loggue la raison)
// si l'installation échoue : le jeu continue alors sans VR.
bool InstallD3D9Hooks();

// Retire proprement les hooks (déchargement de la DLL)
void RemoveD3D9Hooks();

// Curseur matériel demandé par le jeu (IDirect3DDevice9::ShowCursor) : 1 affiché (menu),
// 0 masqué (combat), -1 si le jeu ne l'a encore jamais piloté
int GameCursorState();

// Images présentées par le jeu depuis le démarrage (tout thread)
unsigned long long PresentCount();

} // namespace render
