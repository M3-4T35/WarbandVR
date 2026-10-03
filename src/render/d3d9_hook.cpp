// Hook du renderer Direct3D 9 de WSE2.
//
// Aucune adresse codée en dur : on crée un device D3D9 factice (et un device D3D9Ex)
// sur une fenêtre cachée, on lit leurs vtables pour obtenir les adresses de Present/Reset
// dans d3d9.dll, puis on les détourne avec MinHook. Les index de vtable sont fixés par
// l'ABI COM de d3d9.h, ils ne bougent pas avec les mises à jour de WSE2.
#include "render/d3d9_hook.h"
#include "render/frame_share.h"
#include "render/game_window.h"
#include "render/ui_redirect.h"
#include "util/log.h"

#include <d3d9.h>
#include <MinHook.h>

#include <atomic>
#include <vector>

namespace render {
namespace {

// Index dans les vtables (ordre des méthodes dans d3d9.h)
constexpr int kDeviceReset = 16;
constexpr int kDevicePresent = 17;
constexpr int kDeviceExPresentEx = 121;
constexpr int kDeviceExResetEx = 132;
constexpr int kSwapChainPresent = 3;
constexpr int kDeviceSetRenderTarget = 37;
constexpr int kDeviceShowCursor = 12;

// Deux variantes possibles : 0 = device D3D9, 1 = device D3D9Ex. Leurs méthodes peuvent
// pointer vers des fonctions différentes, chacune a donc sa trampoline.
constexpr int kVariants = 2;
const char* const kVariantName[kVariants] = {"D3D9", "D3D9Ex"};

using DevicePresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                                     const RGNDATA*);
using DeviceResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using ShowCursorFn = BOOL(STDMETHODCALLTYPE*)(IDirect3DDevice9*, BOOL);
using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                        const RGNDATA*, DWORD);
using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND,
                                                 const RGNDATA*, DWORD);
using ResetExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

DevicePresentFn g_devicePresent[kVariants]{};
DeviceResetFn g_deviceReset[kVariants]{};
SetRenderTargetFn g_setRenderTarget[kVariants]{};
ShowCursorFn g_showCursor[kVariants]{};
std::atomic<int> g_gameCursor{-1};
SwapChainPresentFn g_swapChainPresent[kVariants]{};
PresentExFn g_presentEx = nullptr;
ResetExFn g_resetEx = nullptr;

std::vector<void*> g_targets; // fonctions effectivement détournées
bool g_mhInitialized = false;

// --- Compteur de frames -------------------------------------------------------------

constexpr ULONGLONG kStatsIntervalMs = 10000;

std::atomic<unsigned long long> g_frames{0};
std::atomic<IDirect3DDevice9*> g_lastDevice{nullptr};
unsigned long long g_framesAtLastStats = 0;
ULONGLONG g_lastStatsTick = 0;

// Present du device peut appeler en interne le Present de la swap chain : on ne compte
// que l'appel le plus externe.
thread_local int t_presentDepth = 0;

struct PresentScope {
    bool outermost;
    PresentScope() : outermost(t_presentDepth++ == 0) {}
    ~PresentScope() { --t_presentDepth; }
};

void LogDeviceInfo(IDirect3DDevice9* device)
{
    IDirect3DDevice9Ex* ex = nullptr;
    const bool isEx = SUCCEEDED(device->QueryInterface(IID_IDirect3DDevice9Ex, reinterpret_cast<void**>(&ex)));
    if (ex)
        ex->Release();
    LOG_RENDER("device du jeu : %p (%s)", device, isEx ? "D3D9Ex" : "D3D9");

    IDirect3DSwapChain9* sc = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
        if (SUCCEEDED(sc->GetPresentParameters(&pp))) {
            SetGameWindow(pp.hDeviceWindow);
            LOG_RENDER("  back buffer : %ux%u, format %d, %s, %u buffer(s), multisample %d, fenetre %p",
                       pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat,
                       pp.Windowed ? "fenetre" : "plein ecran", pp.BackBufferCount, pp.MultiSampleType,
                       pp.hDeviceWindow);
        }
        sc->Release();
    }
}

void OnPresent(IDirect3DDevice9* device, const char* via)
{
    // Le back buffer contient encore l'image finie : on la copie pour le casque (avec la
    // texture d'interface si elle a été rendue à part)
    SetGameDevice(device);
    CaptureFrame(device);
    EndFrame();

    const unsigned long long n = ++g_frames;
    const ULONGLONG now = GetTickCount64();

    if (device && g_lastDevice.exchange(device) != device) {
        LOG_RENDER("premier Present sur ce device (via %s, frame %llu)", via, n);
        LogDeviceInfo(device);
    }
    if (n == 1) {
        g_lastStatsTick = now;
        g_framesAtLastStats = 1;
        return;
    }
    if (now - g_lastStatsTick >= kStatsIntervalMs) {
        const double fps = (n - g_framesAtLastStats) * 1000.0 / static_cast<double>(now - g_lastStatsTick);
        LOG_RENDER("frames : %llu au total, %.1f fps", n, fps);
        g_lastStatsTick = now;
        g_framesAtLastStats = n;
    }
}

// --- Détours ------------------------------------------------------------------------

template <int V>
HRESULT STDMETHODCALLTYPE HkDevicePresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst,
                                          HWND wnd, const RGNDATA* dirty)
{
    PresentScope scope;
    if (scope.outermost)
        OnPresent(self, "IDirect3DDevice9::Present");
    return g_devicePresent[V](self, src, dst, wnd, dirty);
}

template <int V>
HRESULT STDMETHODCALLTYPE HkSwapChainPresent(IDirect3DSwapChain9* self, const RECT* src, const RECT* dst,
                                             HWND wnd, const RGNDATA* dirty, DWORD flags)
{
    PresentScope scope;
    if (scope.outermost) {
        IDirect3DDevice9* device = nullptr;
        self->GetDevice(&device);
        OnPresent(device, "IDirect3DSwapChain9::Present");
        if (device)
            device->Release();
    }
    return g_swapChainPresent[V](self, src, dst, wnd, dirty, flags);
}

HRESULT STDMETHODCALLTYPE HkPresentEx(IDirect3DDevice9Ex* self, const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty, DWORD flags)
{
    PresentScope scope;
    if (scope.outermost)
        OnPresent(self, "IDirect3DDevice9Ex::PresentEx");
    return g_presentEx(self, src, dst, wnd, dirty, flags);
}

template <int V>
HRESULT STDMETHODCALLTYPE HkSetRenderTarget(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface)
{
    return g_setRenderTarget[V](self, index, FilterRenderTarget(index, surface));
}

template <int V>
BOOL STDMETHODCALLTYPE HkShowCursor(IDirect3DDevice9* self, BOOL show)
{
    const int state = show ? 1 : 0;
    if (g_gameCursor.exchange(state) != state)
        LOG_RENDER("curseur du jeu : %s", show ? "affiche" : "masque");
    return g_showCursor[V](self, show);
}

template <int V>
HRESULT STDMETHODCALLTYPE HkDeviceReset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp)
{
    LOG_RENDER("Reset (%s) : %ux%u, %s", kVariantName[V], pp ? pp->BackBufferWidth : 0,
               pp ? pp->BackBufferHeight : 0, pp && pp->Windowed ? "fenetre" : "plein ecran");
    const HRESULT hr = g_deviceReset[V](self, pp);
    LOG_RENDER("Reset -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE HkResetEx(IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode)
{
    LOG_RENDER("ResetEx : %ux%u", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
    const HRESULT hr = g_resetEx(self, pp, mode);
    LOG_RENDER("ResetEx -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

// --- Installation -------------------------------------------------------------------

void* VtableEntry(void* object, int index)
{
    return (*reinterpret_cast<void***>(object))[index];
}

bool AlreadyHooked(void* target)
{
    for (void* t : g_targets)
        if (t == target)
            return true;
    return false;
}

// Détourne target sauf s'il l'est déjà (fonction partagée entre D3D9 et D3D9Ex)
template <typename Fn>
bool Hook(void* target, void* detour, Fn* original, const char* name)
{
    if (!target)
        return false;
    if (AlreadyHooked(target)) {
        LOG_RENDER("  %-32s %p (deja detourne, partage)", name, target);
        return true;
    }
    MH_STATUS st = MH_CreateHook(target, detour, reinterpret_cast<void**>(original));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    if (st != MH_OK) {
        LOG_RENDER("  %-32s %p ECHEC MinHook : %s", name, target, MH_StatusToString(st));
        MH_RemoveHook(target);
        return false;
    }
    g_targets.push_back(target);
    LOG_RENDER("  %-32s %p detourne", name, target);
    return true;
}

HWND CreateDummyWindow()
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WarbandVR_DummyD3D";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

D3DPRESENT_PARAMETERS DummyParams(HWND wnd)
{
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 16;
    pp.BackBufferHeight = 16;
    pp.hDeviceWindow = wnd;
    return pp;
}

constexpr DWORD kDummyFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT;

// Hooke les méthodes d'un device factice. V = variante (D3D9 / D3D9Ex)
template <int V>
int HookDevice(IDirect3DDevice9* device)
{
    int ok = 0;
    ok += Hook(VtableEntry(device, kDevicePresent), &HkDevicePresent<V>, &g_devicePresent[V],
               V ? "IDirect3DDevice9Ex::Present" : "IDirect3DDevice9::Present");
    ok += Hook(VtableEntry(device, kDeviceReset), &HkDeviceReset<V>, &g_deviceReset[V],
               V ? "IDirect3DDevice9Ex::Reset" : "IDirect3DDevice9::Reset");
    ok += Hook(VtableEntry(device, kDeviceSetRenderTarget), &HkSetRenderTarget<V>, &g_setRenderTarget[V],
               V ? "IDirect3DDevice9Ex::SetRenderTarget" : "IDirect3DDevice9::SetRenderTarget");
    ok += Hook(VtableEntry(device, kDeviceShowCursor), &HkShowCursor<V>, &g_showCursor[V],
               V ? "IDirect3DDevice9Ex::ShowCursor" : "IDirect3DDevice9::ShowCursor");

    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
        ok += Hook(VtableEntry(sc, kSwapChainPresent), &HkSwapChainPresent<V>, &g_swapChainPresent[V],
                   V ? "IDirect3DSwapChain9(Ex)::Present" : "IDirect3DSwapChain9::Present");
        sc->Release();
    }
    return ok;
}

} // namespace

bool InstallD3D9Hooks()
{
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) {
        LOG_RENDER("d3d9.dll pas encore chargee par le jeu, chargement anticipe");
        d3d9 = LoadLibraryW(L"d3d9.dll");
    }
    if (!d3d9) {
        LOG_RENDER("ERREUR : impossible de charger d3d9.dll, hooks desactives");
        return false;
    }
    char d3d9Path[MAX_PATH]{};
    GetModuleFileNameA(d3d9, d3d9Path, MAX_PATH);
    LOG_RENDER("d3d9.dll : %s", d3d9Path);

    using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
    using Create9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    auto create9 = reinterpret_cast<Create9Fn>(GetProcAddress(d3d9, "Direct3DCreate9"));
    auto create9Ex = reinterpret_cast<Create9ExFn>(GetProcAddress(d3d9, "Direct3DCreate9Ex"));
    if (!create9) {
        LOG_RENDER("ERREUR : Direct3DCreate9 introuvable dans d3d9.dll, hooks desactives");
        return false;
    }

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_RENDER("ERREUR : MH_Initialize : %s, hooks desactives", MH_StatusToString(st));
        return false;
    }
    g_mhInitialized = true;

    HWND wnd = CreateDummyWindow();
    if (!wnd) {
        LOG_RENDER("ERREUR : fenetre factice non creee (%lu), hooks desactives", GetLastError());
        return false;
    }

    int hooked = 0;

    // Device D3D9 classique. HAL d'abord, NULLREF si le GPU est déjà pris en exclusif.
    if (IDirect3D9* d3d = create9(D3D_SDK_VERSION)) {
        D3DPRESENT_PARAMETERS pp = DummyParams(wnd);
        IDirect3DDevice9* dev = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, kDummyFlags, &pp, &dev);
        if (FAILED(hr)) {
            LOG_RENDER("device factice HAL : echec 0x%08lX, essai NULLREF", static_cast<unsigned long>(hr));
            pp = DummyParams(wnd);
            hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, wnd, kDummyFlags, &pp, &dev);
        }
        if (SUCCEEDED(hr) && dev) {
            LOG_RENDER("device factice D3D9 cree, installation des hooks :");
            hooked += HookDevice<0>(dev);
            dev->Release();
        } else {
            LOG_RENDER("ERREUR : device factice D3D9 non cree (0x%08lX)", static_cast<unsigned long>(hr));
        }
        d3d->Release();
    }

    // Device D3D9Ex, au cas où WSE2 l'utilise
    IDirect3D9Ex* d3dEx = nullptr;
    if (create9Ex && SUCCEEDED(create9Ex(D3D_SDK_VERSION, &d3dEx)) && d3dEx) {
        D3DPRESENT_PARAMETERS pp = DummyParams(wnd);
        IDirect3DDevice9Ex* dev = nullptr;
        HRESULT hr = d3dEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, kDummyFlags, &pp, nullptr, &dev);
        if (SUCCEEDED(hr) && dev) {
            LOG_RENDER("device factice D3D9Ex cree, installation des hooks :");
            hooked += HookDevice<1>(dev);
            hooked += Hook(VtableEntry(dev, kDeviceExPresentEx), &HkPresentEx, &g_presentEx,
                           "IDirect3DDevice9Ex::PresentEx");
            hooked += Hook(VtableEntry(dev, kDeviceExResetEx), &HkResetEx, &g_resetEx,
                           "IDirect3DDevice9Ex::ResetEx");
            dev->Release();
        } else {
            LOG_RENDER("device factice D3D9Ex non cree (0x%08lX), hooks Ex ignores", static_cast<unsigned long>(hr));
        }
        d3dEx->Release();
    } else {
        LOG_RENDER("Direct3DCreate9Ex indisponible, hooks Ex ignores");
    }

    DestroyWindow(wnd);

    if (g_targets.empty()) {
        LOG_RENDER("ERREUR : aucun hook installe (%d tentatives reussies), rendu non intercepte", hooked);
        return false;
    }
    LOG_RENDER("%zu fonction(s) detournee(s), en attente du premier Present du jeu", g_targets.size());
    return true;
}

int GameCursorState()
{
    return g_gameCursor;
}

void RemoveD3D9Hooks()
{
    if (!g_mhInitialized)
        return;
    for (void* t : g_targets) {
        MH_DisableHook(t);
        MH_RemoveHook(t);
    }
    g_targets.clear();
    MH_Uninitialize();
    g_mhInitialized = false;
    LOG_RENDER("hooks retires (%llu frames vues)", g_frames.load());
}

} // namespace render
