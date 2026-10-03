#pragma once
// Manettes en 3D pour le mode écran virtuel : une manette stylisée par main et le rayon de
// la main active, rendus par nous (D3D11) dans un calque de projection stéréo transparent.
// Générique : pas de modèle propre à un constructeur, fonctionne avec toutes les manettes.
#include <d3d11.h>
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>

namespace xr {

class XrControls;

class HandRenderer {
public:
    // size : côté en pixels des images par œil. false : manettes non affichées.
    bool Init(XrSession session, ID3D11Device* device, ID3D11DeviceContext* context, int64_t format, uint32_t size);
    void Shutdown();

    // Rend les manettes pour les deux yeux (vues localisées dans l'espace du calque) et
    // remplit le calque. false s'il n'y a rien à afficher.
    bool Render(const XrView eyes[2], const XrControls& pointer, XrSpace space, XrCompositionLayerProjection& layer,
                XrCompositionLayerProjectionView views[2]);

private:
    struct Eye {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<ID3D11RenderTargetView*> rtvs;
        ID3D11Texture2D* depth = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
    };
    bool CreateShaders();
    void DrawBox(const float world[16], const float viewProj[16], const float rotation[16], const float color[4]);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    int64_t format_ = 0;
    uint32_t size_ = 0;
    Eye eyes_[2];
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* vertices_ = nullptr;
    ID3D11Buffer* indices_ = nullptr;
    ID3D11Buffer* constants_ = nullptr;
    ID3D11DepthStencilState* depthState_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    bool ready_ = false;
    bool loggedRender_ = false;
    bool loggedError_ = false;
};

} // namespace xr
