#pragma once
// Menu radial des ordres : disque semi-transparent lié à la tête, une part par entrée
// (entrée 0 en haut, sens horaire), l'entrée visée surlignée. Dessiné avec GDI.
#include "xr/gdi_layer.h"
#include "xr/xr_controls.h"

#include <string>

namespace xr {

class RadialMenu {
public:
    void Init(XrSession session, ID3D11Device* device, ID3D11DeviceContext* context, int64_t format);
    void Shutdown();
    // Calque du menu dans space (espace VIEW : devant les yeux). false si indisponible.
    bool Layer(XrSpace space, const XrControls::Radial& radial, XrCompositionLayerQuad& layer);
    // Bandeau de consigne sous le regard (espace VIEW). false si indisponible.
    bool HintLayer(XrSpace space, const wchar_t* text, XrCompositionLayerQuad& layer);

private:
    bool Redraw(const XrControls::Radial& radial);

    GdiLayer layer_;
    GdiLayer hint_;
    std::wstring hintText_;
    bool drawn_ = false;
    std::wstring drawnKey_; // contenu de la dernière image (entrées + entrée visée)
};

} // namespace xr
