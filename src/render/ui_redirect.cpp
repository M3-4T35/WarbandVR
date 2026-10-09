#include "render/ui_redirect.h"
#include "util/log.h"

#include <d3d9.h>

#include <atomic>

namespace render {
namespace {

std::atomic<IDirect3DDevice9*> g_gameDevice{nullptr};

IDirect3DTexture9* g_uiTexture = nullptr;
IDirect3DSurface9* g_uiSurface = nullptr;
IDirect3DDevice9* g_uiOwner = nullptr;
UINT g_uiWidth = 0;
UINT g_uiHeight = 0;

IDirect3DSurface9* g_savedTarget = nullptr; // cible d'origine pendant la redirection
bool g_active = false;
bool g_stereoFrame = false;
bool g_uiRendered = false;
bool g_failed = false;
bool g_logged = false;

void ReleaseUiTarget()
{
    if (g_uiSurface)
        g_uiSurface->Release();
    if (g_uiTexture)
        g_uiTexture->Release();
    g_uiSurface = nullptr;
    g_uiTexture = nullptr;
    g_uiOwner = nullptr;
    g_uiWidth = g_uiHeight = 0;
}

// Texture d'interface de la taille de la cible d'origine
bool EnsureUiTarget(IDirect3DDevice9* device, IDirect3DSurface9* like)
{
    D3DSURFACE_DESC desc{};
    like->GetDesc(&desc);
    if (g_uiSurface && device == g_uiOwner && desc.Width == g_uiWidth && desc.Height == g_uiHeight)
        return true;
    ReleaseUiTarget();
    HRESULT hr = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                       D3DPOOL_DEFAULT, &g_uiTexture, nullptr);
    if (FAILED(hr)) {
        LOG_RENDER("ERREUR : texture d'interface %ux%u non creee : 0x%08lX", desc.Width, desc.Height,
                   static_cast<unsigned long>(hr));
        return false;
    }
    g_uiTexture->GetSurfaceLevel(0, &g_uiSurface);
    g_uiOwner = device;
    g_uiWidth = desc.Width;
    g_uiHeight = desc.Height;
    LOG_RENDER("texture d'interface %ux%u creee", desc.Width, desc.Height);
    return true;
}

} // namespace

void ReleaseUiForReset()
{
    if (g_uiTexture)
        LOG_RENDER("Reset : texture d'interface liberee");
    ReleaseUiTarget();
}

void SetGameDevice(IDirect3DDevice9* device)
{
    g_gameDevice = device;
}

IDirect3DDevice9* GameDevice()
{
    return g_gameDevice;
}

void MarkStereoFrame()
{
    g_stereoFrame = true;
}

bool BeginUiRedirect()
{
    IDirect3DDevice9* device = g_gameDevice;
    if (!g_stereoFrame || g_failed || g_active || !device)
        return false;

    IDirect3DSurface9* current = nullptr;
    if (FAILED(device->GetRenderTarget(0, &current)) || !current)
        return false;
    if (!EnsureUiTarget(device, current)) {
        current->Release();
        g_failed = true;
        LOG_RENDER("redirection de l'interface desactivee");
        return false;
    }
    g_savedTarget = current; // garde la référence jusqu'à EndUiRedirect
    device->SetRenderTarget(0, g_uiSurface);
    // Plusieurs contextes d'interface par frame : on ne vide qu'au premier
    if (!g_uiRendered)
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
    g_active = true;
    if (!g_logged) {
        LOG_RENDER("interface redirigee vers la texture d'interface (thread %lu)", GetCurrentThreadId());
        g_logged = true;
    }
    return true;
}

void EndUiRedirect()
{
    if (!g_active)
        return;
    g_active = false;
    if (IDirect3DDevice9* device = g_gameDevice)
        device->SetRenderTarget(0, g_savedTarget);
    g_savedTarget->Release();
    g_savedTarget = nullptr;
    g_uiRendered = true;
}

IDirect3DSurface9* FilterRenderTarget(DWORD index, IDirect3DSurface9* surface)
{
    if (g_active && index == 0 && surface == g_savedTarget)
        return g_uiSurface;
    return surface;
}

IDirect3DSurface9* UiSurfaceForFrame()
{
    return g_uiRendered ? g_uiSurface : nullptr;
}

void EndFrame()
{
    g_stereoFrame = false;
    g_uiRendered = false;
}

} // namespace render
