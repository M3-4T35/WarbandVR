#pragma once
// Clavier virtuel des menus : affiché sous l'écran virtuel quand un champ de texte du jeu a
// le focus, piloté au pointeur des manettes. Les touches suivent la disposition du clavier
// Windows du jeu (AZERTY, QWERTY...) ; les caractères sont envoyés à la fenêtre du jeu en
// WM_CHAR, que le moteur lit pour ses zones de texte.
#include "xr/gdi_layer.h"

#include <string>
#include <vector>

namespace xr {

class VirtualKeyboard {
public:
    void Init(XrSession session, ID3D11Device* device, ID3D11DeviceContext* context, int64_t format);
    void Shutdown();

    // Taille en mètres pour une largeur donnée (le rapport largeur / hauteur est fixe)
    static float HeightFor(float width);

    // Par frame. hover : le pointeur est sur le clavier, en (u, v) normalisés (0..1, v vers
    // le bas) ; click : nouvel appui de la gâchette à cet endroit.
    void Update(bool hover, float u, float v, bool click);

    // Remplit le calque du clavier (centre et largeur en mètres dans space). false si indisponible.
    bool Layer(XrSpace space, const XrVector3f& center, float width, XrCompositionLayerQuad& layer);

private:
    enum class Kind { Char, Backspace, Shift, Enter, Space };
    struct Key {
        float x, y, w, h; // en unités de touche
        Kind kind;
        unsigned scancode;
        wchar_t normal, shifted;
    };
    void BuildLayout();
    int KeyAt(float u, float v) const;
    void Press(const Key& k);
    bool Redraw();

    XrSession session_ = XR_NULL_HANDLE;
    GdiLayer layer_;
    bool drawn_ = false;

    std::vector<Key> keys_;
    void* layoutId_ = nullptr; // disposition Windows utilisée pour les libellés
    int hover_ = -1;
    int flash_ = -1;           // touche appuyée, surlignée brièvement
    unsigned long long flashUntil_ = 0;
    bool shift_ = false;
    int drawnHover_ = -2, drawnFlash_ = -2;
    bool drawnShift_ = false;
    int typed_ = 0;
};

} // namespace xr
