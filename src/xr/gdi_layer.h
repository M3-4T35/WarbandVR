#pragma once
// Calque quad OpenXR dessiné avec GDI (clavier virtuel, menu radial) : une swapchain et une
// texture intermédiaire de taille fixe. On dessine dans un DC, puis Commit() copie l'image
// (alpha prémultiplié, calculé par l'appelant) dans une nouvelle image de la swapchain.
#include <d3d11.h>
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <vector>

namespace xr {

class GdiLayer {
public:
    void Init(XrSession session, ID3D11Device* device, ID3D11DeviceContext* context, int64_t format, int width,
              int height, const char* name);
    void Shutdown();

    // Prépare un DC de la taille du calque (créé au premier appel). nullptr si indisponible.
    HDC BeginDraw();
    // Pixels BGRA du DC (après BeginDraw), modifiables avant Commit (alpha)
    uint32_t* Pixels() { return bits_; }
    // Copie l'image dessinée dans la swapchain
    bool Commit();
    bool HasImage() const { return committed_; }

    // Calque quad (alpha prémultiplié) de la dernière image
    void Fill(XrSpace space, const XrPosef& pose, float width, XrCompositionLayerQuad& layer) const;

    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    bool Create();

    XrSession session_ = XR_NULL_HANDLE;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    int64_t format_ = 0;
    int width_ = 0, height_ = 0;
    const char* name_ = "";

    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    ID3D11Texture2D* staging_ = nullptr;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    uint32_t* bits_ = nullptr;
    bool failed_ = false;
    bool committed_ = false;
};

} // namespace xr
