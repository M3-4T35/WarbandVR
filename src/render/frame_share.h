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

// Thread XR : numéro de la dernière image terminée de chaque catégorie (0 = aucune),
// indexé par eye + 1
void LatestSeqs(uint64_t out[kCategories]);

// Thread XR : réserve la dernière image terminée de la catégorie eye si elle est plus
// récente que newerThan. Doit être suivi de ReleaseFrame() une fois la lecture GPU terminée.
bool AcquireLatestFrame(int eye, uint64_t newerThan, SharedFrame& out);
void ReleaseFrame();

// Debug (touche F11) : à la prochaine frame de l'œil gauche, enregistre l'image de la scène
// et la texture d'interface en PNG à côté de la DLL
void RequestDump();

} // namespace render
