#pragma once
// Redirection de l'interface 2D (HUD) vers une texture séparée, transparente.
//
// Pendant une frame stéréo, le contexte d'interface du moteur est rendu dans notre texture
// au lieu du back buffer : la scène reste seule dans le back buffer (une image par œil) et
// l'interface est affichée au casque dans un calque à part. Tout se passe sur le thread de
// rendu du jeu.
#include <windows.h>

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace render {

// Thread du jeu, avant Reset : libère la texture d'interface (D3DPOOL_DEFAULT), recréée ensuite
void ReleaseUiForReset();

// Device D3D9 du jeu (dernier vu dans Present), nullptr avant la première image
void SetGameDevice(IDirect3DDevice9* device);
IDirect3DDevice9* GameDevice();

// La scène principale de la frame en cours est rendue en stéréo : l'interface qui suit
// doit être redirigée. Remis à zéro à chaque Present.
void MarkStereoFrame();

// Autour du rendu du contexte d'interface. Begin renvoie false si la redirection n'a pas lieu.
bool BeginUiRedirect();
void EndUiRedirect();

// Hook de SetRenderTarget : pendant la redirection, une demande de retour à la cible
// d'origine est remplacée par notre texture
IDirect3DSurface9* FilterRenderTarget(DWORD index, IDirect3DSurface9* surface);

// Present : surface d'interface rendue pendant cette frame (nullptr sinon), et fin de frame
IDirect3DSurface9* UiSurfaceForFrame();
void EndFrame();

} // namespace render
