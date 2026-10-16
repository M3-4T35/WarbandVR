#include "render/frame_share.h"
#include "render/ui_redirect.h"
#include "util/config.h"
#include "util/log.h"

#include <d3d9.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace render {
namespace {

struct Slot {
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DQuery9* query = nullptr; // signale la fin de la copie côté GPU
    HANDLE shared = nullptr;
    bool pending = false;             // copie soumise, pas encore terminée
    bool reading = false;             // en cours de lecture par le thread XR (copie GPU D3D11)
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
bool g_disabled = false;
RenderView g_pendingView;
int g_frameMainScenes = 0;   // scènes principales rendues depuis le dernier Present
RenderView g_firstMainView;  // vue de la première
bool g_loggedFirstEye = false;
std::atomic<bool> g_dumpRequested{false};
int g_dumpCount = 0;
// Capture de diagnostic : les deux yeux d'une même paire (et l'interface), qui arrivent à des
// moments différents (rendu alterné : deux Present ; vraie stéréo : BeginMainScene puis Present)
uint64_t g_dumpPair = 0;      // paire en cours de capture, 0 : aucune
int g_dumpDone = 0;           // 1 : œil gauche, 2 : œil droit, 4 : interface
ULONGLONG g_lastAutoDump = 0; // captures automatiques (réglage debug_capture)
int g_autoDumps = 0;
constexpr int kMaxAutoDumps = 10;
constexpr ULONGLONG kAutoDumpPeriodMs = 15000;
bool g_censusRequested = false; // contextes de l'image à écrire dans le log (TakeCensusRequest)

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

bool AnyReading()
{
    for (const Slot& s : g_slots)
        if (s.reading)
            return true;
    return false;
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
    for (int i = 0; i < kSharedSlots; ++i)
        if (!IsLatest(i) && !g_slots[i].reading && !g_slots[i].pending &&
            (target == -1 || g_slots[i].seq < g_slots[target].seq))
            target = i; // le plus ancien : la paire précédente reste disponible pour le casque
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

// Capture de diagnostic d'une image d'œil, si elle appartient à la paire en cours de capture
// (la première image d'œil après la demande fixe la paire). g_mutex tenu.
void DumpEye(IDirect3DDevice9* device, IDirect3DSurface9* surface, const RenderView& v)
{
    if (!v.valid || v.eye < 0 || v.eye > 1)
        return;
    // Paire suivante, entière : l'œil rendu en premier peut déjà être passé (vraie stéréo : l'œil
    // droit est copié avant le Present de l'œil gauche)
    if (g_dumpRequested && !g_dumpPair) {
        g_dumpRequested = false;
        g_dumpPair = v.pair + 1;
        g_dumpDone = 0;
        LOG_RENDER("capture %d : paire %llu (%s)", g_dumpCount, static_cast<unsigned long long>(g_dumpPair),
                   v.both ? "vraie stereo" : "rendu alterne");
        return;
    }
    if (!g_dumpPair)
        return;
    if (v.pair > g_dumpPair + 4) { // paire jamais complétée (image perdue) : abandon
        LOG_RENDER("capture %d : paire %llu incomplete (yeux 0x%X), abandonnee", g_dumpCount,
                   static_cast<unsigned long long>(g_dumpPair), g_dumpDone);
        g_dumpPair = 0;
        ++g_dumpCount;
        return;
    }
    const int bit = v.eye ? 2 : 1;
    if (v.pair != g_dumpPair || (g_dumpDone & bit))
        return;
    g_censusRequested = true; // contextes de cette image dans le log
    DumpSurface(device, surface, v.eye ? "oeil_droit" : "oeil_gauche");
    constexpr float kDeg = 57.29578f;
    LOG_RENDER("  oeil %s : position (%.3f %.3f %.3f), orientation (%.4f %.4f %.4f %.4f), champ g/d/h/b "
               "%.1f/%.1f/%.1f/%.1f deg",
               v.eye ? "droit" : "gauche", v.px, v.py, v.pz, v.qx, v.qy, v.qz, v.qw, v.angleLeft * kDeg,
               v.angleRight * kDeg, v.angleUp * kDeg, v.angleDown * kDeg);
    g_dumpDone |= bit;
    if ((g_dumpDone & 3) == 3) {
        g_dumpPair = 0;
        ++g_dumpCount;
    }
}

// Copies terminées côté GPU -> la plus récente de sa catégorie devient disponible
void UpdateCompleted()
{
    for (int i = 0; i < kSharedSlots; ++i) {
        Slot& s = g_slots[i];
        if (s.pending && s.query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK) {
            s.pending = false;
            int& latest = g_latest[Category(s.view)];
            if (latest == -1 || s.seq > g_slots[latest].seq)
                latest = i;
        }
    }
}

} // namespace

void BeginMainScene(const RenderView& view)
{
    std::lock_guard lock(g_mutex);
    if (view.both && g_frameMainScenes == 1 && g_firstMainView.both && g_firstMainView.pair == view.pair &&
        g_owner && !g_disabled && g_slots[0].texture) {
        IDirect3DSurface9* backBuffer = nullptr;
        if (SUCCEEDED(g_owner->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer) {
            D3DSURFACE_DESC desc{};
            backBuffer->GetDesc(&desc);
            if (desc.Width == g_width && desc.Height == g_height) {
                UpdateCompleted();
                CopyToFreeSlot(g_owner, backBuffer, g_firstMainView);
                DumpEye(g_owner, backBuffer, g_firstMainView);
                if (!g_loggedFirstEye) {
                    LOG_RENDER("vraie stereo : image de l'oeil %s copiee avant le rendu de l'oeil %s (paire %llu)",
                               g_firstMainView.eye ? "droit" : "gauche", view.eye ? "droit" : "gauche",
                               static_cast<unsigned long long>(view.pair));
                    g_loggedFirstEye = true;
                }
            }
            backBuffer->Release();
        }
    }
    g_firstMainView = view;
    ++g_frameMainScenes;
}

void CaptureFrame(IDirect3DDevice9* device)
{
    if (!device)
        return;
    std::lock_guard lock(g_mutex);
    g_frameMainScenes = 0; // image suivante
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
        if (AnyReading()) {
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

    UpdateCompleted();
    CopyToFreeSlot(device, backBuffer, g_pendingView);
    // Captures de diagnostic : automatiques en stéréo (réglage debug_capture), ou F11
    const ULONGLONG now = GetTickCount64();
    if (g_pendingView.valid && config::GetBool(config::Id::DebugCapture) && g_autoDumps < kMaxAutoDumps &&
        !g_dumpRequested && !g_dumpPair) {
        if (!g_lastAutoDump)
            g_lastAutoDump = now - kAutoDumpPeriodMs + 5000; // première capture 5 s après la stéréo
        if (now - g_lastAutoDump >= kAutoDumpPeriodMs) {
            g_lastAutoDump = now;
            ++g_autoDumps;
            g_dumpRequested = true;
        }
    }
    if (!g_pendingView.valid && g_dumpRequested.exchange(false)) { // écran plat (menus)
        DumpSurface(device, backBuffer, "ecran");
        ++g_dumpCount;
    }
    DumpEye(device, backBuffer, g_pendingView);
    // Interface : rendu alterné, seulement celle des frames de l'œil gauche. Les éléments placés
    // d'après la caméra (noms au-dessus des alliés) changeraient sinon de position d'une frame
    // à l'autre et apparaîtraient en double. Vraie stéréo : une seule interface par image.
    IDirect3DSurface9* ui = UiSurfaceForFrame();
    const bool uiFrame = g_pendingView.eye == 0 || g_pendingView.both;
    if (ui && uiFrame && g_dumpPair == g_pendingView.pair && g_dumpPair && !(g_dumpDone & 4)) {
        DumpSurface(device, ui, "interface");
        g_dumpDone |= 4;
    }
    if (ui && uiFrame) {
        RenderView uiView;
        uiView.eye = kEyeUi;
        CopyToFreeSlot(device, ui, uiView);
    }
    g_pendingView = RenderView{};
    backBuffer->Release();
}

void ReleaseForReset()
{
    // Le thread XR peut être en train de lire un slot : on attend (au plus 1 s) qu'il ait fini
    const ULONGLONG deadline = GetTickCount64() + 1000;
    for (;;) {
        {
            std::lock_guard lock(g_mutex);
            if (!AnyReading() || GetTickCount64() > deadline) {
                if (g_owner)
                    LOG_RENDER("Reset : textures partagees liberees%s", AnyReading() ? " (lecture XR en cours)" : "");
                DestroySlots();
                g_owner = nullptr;
                g_width = g_height = 0;
                return;
            }
        }
        Sleep(1);
    }
}

uint64_t LatestCompletePair()
{
    std::lock_guard lock(g_mutex);
    uint64_t best = 0;
    for (const Slot& a : g_slots) {
        // Œil gauche terminé ; un œil droit terminé de la même paire ?
        if (!a.texture || a.pending || !a.seq || Category(a.view) != 1 || a.view.pair <= best)
            continue;
        for (const Slot& b : g_slots)
            if (b.texture && !b.pending && b.seq && Category(b.view) == 2 && b.view.pair == a.view.pair) {
                best = a.view.pair;
                break;
            }
    }
    return best;
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

bool AcquireLatestFrame(int eye, uint64_t newerThan, SharedFrame& out, uint64_t maxPair)
{
    std::lock_guard lock(g_mutex);
    // Image terminée la plus récente de cet œil, dans une paire complète (<= maxPair)
    int slot = -1;
    for (int i = 0; i < kSharedSlots; ++i) {
        const Slot& s = g_slots[i];
        if (s.texture && !s.pending && s.seq && Category(s.view) == eye + 1 && s.view.pair <= maxPair &&
            (slot == -1 || s.seq > g_slots[slot].seq))
            slot = i;
    }
    if (slot == -1 || g_slots[slot].seq <= newerThan)
        return false;
    g_slots[slot].reading = true;
    out.handle = g_slots[slot].shared;
    out.slot = slot;
    out.width = g_width;
    out.height = g_height;
    out.generation = g_generation;
    out.seq = g_slots[slot].seq;
    out.view = g_slots[slot].view;
    return true;
}

bool TakeCensusRequest()
{
    std::lock_guard lock(g_mutex);
    return std::exchange(g_censusRequested, false);
}

void RequestDump()
{
    g_dumpRequested = true;
}

void ReleaseFrame(int slot)
{
    std::lock_guard lock(g_mutex);
    if (slot >= 0 && slot < kSharedSlots)
        g_slots[slot].reading = false;
}

} // namespace render
