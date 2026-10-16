#pragma once
// Partage de l'image du jeu entre le device D3D9Ex du jeu et le device D3D11 d'OpenXR.
//
// Côté jeu (thread de rendu, dans le hook Present) : le back buffer est copié dans l'une
// de plusieurs textures partagées (D3D9Ex shared handles). Chaque image est rangée dans
// une catégorie : plate (hors mission), œil gauche ou œil droit (rendu alterné), ou
// interface (HUD rendu à part pendant la stéréo, avec transparence). Côté XR :
// on prend la dernière copie terminée d'une catégorie, on la lit, puis on la rend. La
// dernière image de chaque catégorie et celle en cours de lecture ne sont jamais réécrites.
#include <windows.h>
#include <cstdint>

struct IDirect3DDevice9;

namespace render {

constexpr int kSharedSlots = 11;

// Catégories d'image : eye = -1 (plate), 0 (gauche), 1 (droite), 2 (interface)
constexpr int kEyeFlat = -1;
constexpr int kEyeUi = 2;
constexpr int kCategories = 4;

// Vue avec laquelle une image a été rendue. valid = false hors mission : image plate.
struct RenderView {
    bool valid = false;
    int eye = kEyeFlat;
    uint64_t pair = 0;                    // paire stéréo (gauche + droite), attribuée par la caméra
    bool both = false;                    // vraie stéréo : les deux yeux rendus dans la même image du jeu
    float qx = 0, qy = 0, qz = 0, qw = 1; // orientation déclarée (espace LOCAL d'OpenXR)
    float px = 0, py = 0, pz = 0;         // position de l'œil (espace LOCAL)
    float angleLeft = 0, angleRight = 0, angleUp = 0, angleDown = 0; // radians
};

struct SharedFrame {
    HANDLE handle = nullptr; // handle partagé, à ouvrir avec ID3D11Device::OpenSharedResource
    int slot = -1;           // 0..kSharedSlots-1
    UINT width = 0;
    UINT height = 0;
    uint64_t generation = 0; // change quand les textures sont recréées (redimensionnement)
    uint64_t seq = 0;        // numéro de l'image (croissant)
    RenderView view;
};

// Thread de rendu du jeu : vue utilisée pour la scène de la frame en cours. Elle est
// attachée à l'image au prochain CaptureFrame puis oubliée.
void SetPendingView(const RenderView& view);

// Thread de rendu du jeu : copie le back buffer courant, et la texture d'interface si elle
// a été rendue à part (appelé avant le Present d'origine)
void CaptureFrame(IDirect3DDevice9* device);

// Thread de rendu du jeu, au début du rendu d'une scène principale (vue retrouvée). Vraie
// stéréo : à la seconde scène de l'image, le back buffer contient l'image finale (post-effets
// compris) de la première, copiée pour son œil. La seconde est copiée au Present.
void BeginMainScene(const RenderView& view);

// Thread XR : numéro de la dernière image terminée de chaque catégorie (0 = aucune),
// indexé par eye + 1
void LatestSeqs(uint64_t out[kCategories]);

// Thread XR : réserve la dernière image terminée de la catégorie eye si elle est plus
// récente que newerThan. Doit être suivi de ReleaseFrame(slot) une fois la lecture GPU terminée
// (plusieurs slots peuvent être en lecture en même temps : un par œil, l'interface).
// maxPair : n'accepter que les images des paires stéréo <= maxPair ; UINT64_MAX = pas de contrainte.
bool AcquireLatestFrame(int eye, uint64_t newerThan, SharedFrame& out, uint64_t maxPair = UINT64_MAX);

// Paire stéréo la plus récente dont l'œil gauche ET l'œil droit sont terminés ; 0 si aucune
uint64_t LatestCompletePair();

void ReleaseFrame(int slot);

// Thread du jeu, avant IDirect3DDevice9::Reset : libère les textures partagées (D3DPOOL_DEFAULT).
// Un device D3D9 classique (Proton) refuse Reset tant qu'elles existent ; D3D9Ex l'accepte.
// Elles sont recréées à l'image suivante.
void ReleaseForReset();

// Thread de rendu : true une fois après le début d'une capture de diagnostic (pour écrire dans
// le log les contextes rendus de cette image)
bool TakeCensusRequest();

// Debug (touche F11, ou automatique avec le réglage debug_capture) : enregistre en PNG à côté de
// la DLL l'œil gauche, l'œil droit et l'interface d'une même paire, avec leurs vues dans le log
void RequestDump();

} // namespace render
