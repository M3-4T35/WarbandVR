#pragma once
// Diagnostic de performance (réglage debug_frame_profile) : temps passés par image sur le
// thread du jeu, le thread de rendu et le GPU (timestamps D3D9), résumés toutes les 10 s dans
// le log ([perf]). Sert à savoir ce que coûterait un vrai rendu stéréo (scène rendue deux fois
// par image) : quelle part de l'image est la scène, et qui limite (CPU du jeu, rendu, GPU).
// Inactif : chaque appel se résume à la lecture du réglage.

struct IDirect3DDevice9;

namespace render::profile {

enum class Ctx { Scene, Ui, Other };

// Thread du jeu : durée de rglScene::render pour la scène principale (remplissage, culling)
void SceneFill(double ms);

// Thread de rendu : autour de rglRenderer::renderContext
void BeginContext(Ctx kind);
void EndContext(Ctx kind);

// Thread de rendu : notre capture de l'image pour le casque (avant le Present d'origine)
void Capture(double ms);

// Thread de rendu : autour du Present d'origine
void BeforePresent(IDirect3DDevice9* device);
void AfterPresent();

// Avant IDirect3DDevice9::Reset : requêtes GPU libérées (recréées ensuite)
void ReleaseForReset();

// Horloge commune (millisecondes, QueryPerformanceCounter)
double NowMs();

} // namespace render::profile
