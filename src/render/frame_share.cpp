#include "render/frame_share.h"
#include "render/ui_redirect.h"
#include "util/log.h"

#include <d3d9.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace render {
namespace {

struct Slot {
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DQuery9* query = nullptr; // signale la fin de la copie côté GPU
    HANDLE shared = nullptr;
    bool pending = false;             // copie soumise, pas encore terminée
    uint64_t seq = 0;                 // numéro de la copie (pour trouver la plus récente)
    RenderView view;
};

std::mutex g_mutex;
Slot g_slots[kSharedSlots];
IDirect3DDevice9* g_owner = nullptr;
UINT g_width = 0;
UINT g_height = 0;
uint64_t g_generation = 0;
uint64_t g_seq = 0;
int g_latest[kCategories] = {-1, -1, -1, -1}; // dernière copie terminée par catégorie (eye + 1)
int g_reading = -1;                        // slot en cours de lecture par le thread XR
bool g_disabled = false;
RenderView g_pendingView;
std::atomic<bool> g_dumpRequested{false};
int g_dumpCount = 0;

// Enregistre une surface (cible de rendu) en PNG via D3DX, déjà chargé par le jeu
void DumpSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const char* name)
{
    using SaveFn = HRESULT(WINAPI*)(LPCSTR, int, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);
    constexpr int kPng = 3; // D3DXIFF_PNG
    HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll");
    auto save = d3dx ? reinterpret_cast<SaveFn>(GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA")) : nullptr;
    if (!save) {
        LOG_RENDER("capture PNG impossible : D3DXSaveSurfaceToFileA introuvable");
        return;
    }
    // Chemin : dossier de la DLL
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&DumpSurface), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\'))
        snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "capture_%d_%s.png", g_dumpCount, name);

    // Une cible de rendu doit d'abord être copiée en mémoire système
    D3DSURFACE_DESC desc{};
    surface->GetDesc(&desc);
    IDirect3DSurface9* sys = nullptr;
    HRESULT hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys,
                                                    nullptr);
    if (SUCCEEDED(hr))
        hr = device->GetRenderTargetData(surface, sys);
    if (SUCCEEDED(hr))
        hr = save(path, kPng, sys, nullptr, nullptr);
    if (sys)
        sys->Release();
    LOG_RENDER("capture %s : %s (0x%08lX)", name, SUCCEEDED(hr) ? path : "ECHEC", static_cast<unsigned long>(hr));
}

void DestroySlots()
{
    for (Slot& s : g_slots) {
        if (s.query)
            s.query->Release();
        if (s.surface)
            s.surface->Release();
        if (s.texture)
            s.texture->Release();
        s = Slot{};
    }
    for (int& l : g_latest)
        l = -1;
}

int Category(const RenderView& v)
{
    return v.eye >= 0 && v.eye <= kEyeUi ? v.eye + 1 : 0;
}

bool IsLatest(int slot)
{
    for (int l : g_latest)
        if (l == slot)
            return true;
    return false;
}

bool CreateSlots(IDirect3DDevice9* device, UINT w, UINT h)
{
    for (int i = 0; i < kSharedSlots; ++i) {
        Slot& s = g_slots[i];
        // A8R8G8B8 partagé = DXGI_FORMAT_B8G8R8A8_UNORM côté D3D11
        HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                           &s.texture, &s.shared);
        if (FAILED(hr) || !s.shared) {
            LOG_RENDER("ERREUR : texture partagee %d (%ux%u) non creee : 0x%08lX", i, w, h,
                       static_cast<unsigned long>(hr));
            return false;
        }
        s.texture->GetSurfaceLevel(0, &s.surface);
        hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &s.query);
        if (FAILED(hr)) {
            LOG_RENDER("ERREUR : query EVENT non creee : 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
    }
    return true;
}

// Copie source dans un slot libre : ni le plus récent d'une catégorie, ni en lecture, ni
// en cours de copie. Sans slot libre, l'image est perdue (le XR garde la précédente).
void CopyToFreeSlot(IDirect3DDevice9* device, IDirect3DSurface9* source, const RenderView& view)
{
    int target = -1;
    for (int i = 0; i < kSharedSlots && target == -1; ++i)
        if (!IsLatest(i) && i != g_reading && !g_slots[i].pending)
            target = i;
    if (target == -1)
        return;
    Slot& s = g_slots[target];
    if (SUCCEEDED(device->StretchRect(source, nullptr, s.surface, nullptr, D3DTEXF_NONE))) {
        s.query->Issue(D3DISSUE_END);
        s.pending = true;
        s.seq = ++g_seq;
        s.view = view;
    }
}

} // namespace

void CaptureFrame(IDirect3DDevice9* device)
{
    if (!device)
        return;
    std::lock_guard lock(g_mutex);
    if (g_disabled)
        return;

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
        return;
    D3DSURFACE_DESC desc{};
    backBuffer->GetDesc(&desc);

    // (Re)création si nouveau device ou nouvelle taille. On attend que le thread XR ait
    // fini de lire pour ne pas détruire une texture qu'il utilise.
    if (device != g_owner || desc.Width != g_width || desc.Height != g_height) {
        if (g_reading != -1) {
            backBuffer->Release();
            return;
        }
        DestroySlots();
        g_owner = device;
        g_width = desc.Width;
        g_height = desc.Height;
        ++g_generation;
        if (!CreateSlots(device, desc.Width, desc.Height)) {
            DestroySlots();
            g_disabled = true;
            LOG_RENDER("capture d'image desactivee");
            backBuffer->Release();
            return;
        }
        LOG_RENDER("capture : %d textures partagees %ux%u creees (generation %llu)", kSharedSlots, desc.Width,
                   desc.Height, g_generation);
    }

    // Copies terminées côté GPU -> la plus récente de sa catégorie devient disponible
    for (int i = 0; i < kSharedSlots; ++i) {
        Slot& s = g_slots[i];
        if (s.pending && s.query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK) {
            s.pending = false;
            int& latest = g_latest[Category(s.view)];
            if (latest == -1 || s.seq > g_slots[latest].seq)
                latest = i;
        }
    }

    CopyToFreeSlot(device, backBuffer, g_pendingView);
    // Interface : seulement celle des frames de l'œil gauche. Les éléments placés d'après la
    // caméra (noms au-dessus des alliés) changeraient sinon de position d'une frame à
    // l'autre et apparaîtraient en double.
    IDirect3DSurface9* ui = UiSurfaceForFrame();
    if (ui && g_pendingView.eye == 0 && g_dumpRequested.exchange(false)) {
        DumpSurface(device, backBuffer, "scene_oeil_gauche");
        DumpSurface(device, ui, "interface");
        ++g_dumpCount;
    }
    if (ui && g_pendingView.eye == 0) {
        RenderView uiView;
        uiView.eye = kEyeUi;
        CopyToFreeSlot(device, ui, uiView);
    }
    g_pendingView = RenderView{};
    backBuffer->Release();
}

void SetPendingView(const RenderView& view)
{
    std::lock_guard lock(g_mutex);
    g_pendingView = view;
}

void LatestSeqs(uint64_t out[kCategories])
{
    std::lock_guard lock(g_mutex);
    for (int c = 0; c < kCategories; ++c)
        out[c] = g_latest[c] == -1 ? 0 : g_slots[g_latest[c]].seq;
}

bool AcquireLatestFrame(int eye, uint64_t newerThan, SharedFrame& out)
{
    std::lock_guard lock(g_mutex);
    const int slot = g_latest[eye + 1];
    if (slot == -1 || g_slots[slot].seq <= newerThan)
        return false;
    g_reading = slot;
    out.handle = g_slots[slot].shared;
    out.slot = slot;
    out.width = g_width;
    out.height = g_height;
    out.generation = g_generation;
    out.seq = g_slots[slot].seq;
    out.view = g_slots[slot].view;
    return true;
}

void RequestDump()
{
    g_dumpRequested = true;
}

void ReleaseFrame()
{
    std::lock_guard lock(g_mutex);
    g_reading = -1;
}

} // namespace render
