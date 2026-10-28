#include "input/dinput_hook.h"
#include "util/hook.h"
#include "util/log.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <MinHook.h>

#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#define LOG_INPUT(...) ::vrlog::Write("[input]", __VA_ARGS__)

namespace input {
namespace {

// Index dans la vtable d'IDirectInputDevice8 (ordre de dinput.h)
constexpr int kGetDeviceState = 9;
constexpr int kGetDeviceData = 10;

// Variantes : A/W et souris/clavier. Leurs méthodes peuvent pointer vers des fonctions
// différentes (le clavier a sa propre implémentation dans dinput8.dll) : une trampoline
// par variante. 0 = A souris, 1 = W souris, 2 = A clavier, 3 = W clavier.
constexpr int kVariants = 4;
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD cbData, LPVOID data);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod,
                                                    LPDWORD inOut, DWORD flags);
GetDeviceStateFn g_getDeviceState[kVariants]{};
GetDeviceDataFn g_getDeviceData[kVariants]{};
std::vector<void*> g_targets;

struct Event {
    DWORD ofs;
    DWORD data;
};
enum class Device { Unknown, Mouse, Keyboard, Other };

std::mutex g_mutex;
std::deque<Event> g_mouseEvents;    // en attente de lecture par le jeu
std::deque<Event> g_keyEvents;
bool g_buttons[2]{};                // état courant des boutons injectés
bool g_keys[256]{};                 // touches injectées tenues (codes DIK)
// Le jeu lit la souris des deux façons (état pour le combat, événements tamponnés pour
// l'interface), comme une vraie souris qui alimente les deux : mouvement et molette sont
// donc injectés dans chacune, indépendamment
LONG g_stateDx = 0, g_stateDy = 0, g_stateDz = 0; // non encore lus par GetDeviceState
// Vraie souris : mouvement non encore consommé (état, événements), boutons, activité
LONG g_realDx = 0, g_realDy = 0, g_realCursorDx = 0, g_realCursorDy = 0;
bool g_realButtons = false;
unsigned long long g_realActivity = 0;
std::unordered_map<void*, Device> g_devices;
DWORD g_sequence = 0x40000000;      // numéros de séquence propres, loin de ceux du système
bool g_loggedMouse = false;
bool g_loggedKeyboard = false;
constexpr size_t kMaxQueued = 256;  // le jeu ne lit pas (hors focus) : on ne s'accumule pas

// Type de périphérique, mis en cache (appelé sous g_mutex)
Device DeviceType(void* self)
{
    auto it = g_devices.find(self);
    if (it != g_devices.end())
        return it->second;
    DIDEVCAPS caps{sizeof(caps)};
    auto* dev = static_cast<IDirectInputDevice8W*>(self);
    Device type = Device::Other;
    if (SUCCEEDED(dev->GetCapabilities(&caps))) {
        if (GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_MOUSE)
            type = Device::Mouse;
        else if (GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_KEYBOARD)
            type = Device::Keyboard;
    }
    g_devices[self] = type;
    if (type == Device::Mouse)
        LOG_INPUT("souris DirectInput du jeu : %p", self);
    else if (type == Device::Keyboard)
        LOG_INPUT("clavier DirectInput du jeu : %p", self);
    return type;
}

// Ajoute des événements après ceux du vrai périphérique, dans la place restante
void Append(std::deque<Event>& events, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inOut, DWORD capacity)
{
    while (!events.empty() && *inOut < capacity) {
        auto* d = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(rgdod) + *inOut * cbObjectData);
        memset(d, 0, cbObjectData);
        d->dwOfs = events.front().ofs;
        d->dwData = events.front().data;
        d->dwTimeStamp = GetTickCount();
        d->dwSequence = ++g_sequence;
        events.pop_front();
        ++*inOut;
    }
}

template <int V>
HRESULT STDMETHODCALLTYPE HkGetDeviceData(void* self, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inOut,
                                          DWORD flags)
{
    const DWORD capacity = inOut ? *inOut : 0;
    const HRESULT hr = g_getDeviceData[V](self, cbObjectData, rgdod, inOut, flags);
    if (FAILED(hr) || !rgdod || !inOut || (flags & DIGDD_PEEK) || cbObjectData < sizeof(DIDEVICEOBJECTDATA_DX3))
        return hr;

    std::lock_guard lock(g_mutex);
    switch (DeviceType(self)) {
    case Device::Mouse:
        // Événements de la vraie souris (avant les nôtres) : curseur des menus en mode souris
        for (DWORD i = 0; i < *inOut; ++i) {
            const auto* d = reinterpret_cast<const DIDEVICEOBJECTDATA*>(reinterpret_cast<const BYTE*>(rgdod) +
                                                                        i * cbObjectData);
            const LONG v = static_cast<LONG>(d->dwData);
            if (d->dwOfs == DIMOFS_X)
                g_realCursorDx += v, g_realActivity += v < 0 ? -v : v;
            else if (d->dwOfs == DIMOFS_Y)
                g_realCursorDy += v, g_realActivity += v < 0 ? -v : v;
            else if ((d->dwOfs == DIMOFS_BUTTON0 || d->dwOfs == DIMOFS_BUTTON1) && (d->dwData & 0x80))
                g_realActivity += 100;
        }
        if (!g_mouseEvents.empty() && !g_loggedMouse) {
            LOG_INPUT("evenements injectes dans la souris DirectInput");
            g_loggedMouse = true;
        }
        Append(g_mouseEvents, cbObjectData, rgdod, inOut, capacity);
        break;
    case Device::Keyboard:
        if (!g_keyEvents.empty() && !g_loggedKeyboard) {
            LOG_INPUT("evenements injectes dans le clavier DirectInput (lecture tamponnee)");
            g_loggedKeyboard = true;
        }
        Append(g_keyEvents, cbObjectData, rgdod, inOut, capacity);
        break;
    default:
        break;
    }
    return hr;
}

template <int V>
HRESULT STDMETHODCALLTYPE HkGetDeviceState(void* self, DWORD cbData, LPVOID data)
{
    const HRESULT hr = g_getDeviceState[V](self, cbData, data);
    if (FAILED(hr) || !data)
        return hr;
    std::lock_guard lock(g_mutex);
    const Device type = DeviceType(self);
    if (type == Device::Mouse && cbData >= sizeof(DIMOUSESTATE)) {
        auto* state = static_cast<DIMOUSESTATE*>(data);
        g_realDx += state->lX;
        g_realDy += state->lY;
        g_realActivity += (state->lX < 0 ? -state->lX : state->lX) + (state->lY < 0 ? -state->lY : state->lY);
        const bool buttons = (state->rgbButtons[0] | state->rgbButtons[1]) & 0x80;
        if (buttons && !g_realButtons)
            g_realActivity += 100;
        g_realButtons = buttons;
        for (int b = 0; b < 2; ++b)
            if (g_buttons[b])
                state->rgbButtons[b] |= 0x80;
        state->lX += g_stateDx;
        state->lY += g_stateDy;
        state->lZ += g_stateDz;
        g_stateDx = g_stateDy = g_stateDz = 0;
    } else if (type == Device::Keyboard && cbData >= 256) {
        auto* keys = static_cast<BYTE*>(data);
        bool any = false;
        for (int k = 0; k < 256; ++k)
            if (g_keys[k]) {
                keys[k] |= 0x80;
                any = true;
            }
        if (any && !g_loggedKeyboard) {
            LOG_INPUT("touches injectees dans le clavier DirectInput (lecture par etat)");
            g_loggedKeyboard = true;
        }
    }
    return hr;
}

bool Hook(void* object, int index, void* detour, void** original, const char* name)
{
    return hook::HookMethod(object, index, detour, original, name, "[input]", g_targets);
}

// Détourne GetDeviceData / GetDeviceState d'un périphérique factice (souris ou clavier)
template <int V, typename Dev>
bool HookDevice(Dev* dev, const char* label)
{
    char nameData[64], nameState[64];
    snprintf(nameData, sizeof(nameData), "%s::GetDeviceData", label);
    snprintf(nameState, sizeof(nameState), "%s::GetDeviceState", label);
    bool ok = Hook(dev, kGetDeviceData, reinterpret_cast<void*>(&HkGetDeviceData<V>),
                   reinterpret_cast<void**>(&g_getDeviceData[V]), nameData);
    ok &= Hook(dev, kGetDeviceState, reinterpret_cast<void*>(&HkGetDeviceState<V>),
               reinterpret_cast<void**>(&g_getDeviceState[V]), nameState);
    return ok;
}

template <int VMouse, int VKeyboard, typename DI, typename Dev>
bool HookVariant(void* create, REFIID iid, const char* label)
{
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    DI* di = nullptr;
    if (FAILED(reinterpret_cast<CreateFn>(create)(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid,
                                                  reinterpret_cast<void**>(&di), nullptr)) ||
        !di) {
        LOG_INPUT("DirectInput8 %s indisponible", label);
        return false;
    }
    bool ok = false;
    char name[64];
    Dev* dev = nullptr;
    if (SUCCEEDED(di->CreateDevice(GUID_SysMouse, &dev, nullptr)) && dev) {
        LOG_INPUT("souris factice DirectInput %s creee, installation des hooks :", label);
        snprintf(name, sizeof(name), "souris %s", label);
        ok |= HookDevice<VMouse>(dev, name);
        dev->Release();
    }
    dev = nullptr;
    if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
        LOG_INPUT("clavier factice DirectInput %s cree, installation des hooks :", label);
        snprintf(name, sizeof(name), "clavier %s", label);
        ok |= HookDevice<VKeyboard>(dev, name);
        dev->Release();
    }
    di->Release();
    return ok;
}

} // namespace

bool InstallDInputHooks()
{
    HMODULE dinput = GetModuleHandleW(L"dinput8.dll");
    if (!dinput)
        dinput = LoadLibraryW(L"dinput8.dll");
    void* create = dinput ? reinterpret_cast<void*>(GetProcAddress(dinput, "DirectInput8Create")) : nullptr;
    if (!create) {
        LOG_INPUT("ERREUR : DirectInput8Create introuvable, clics du pointeur desactives");
        return false;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    const bool a = HookVariant<0, 2, IDirectInput8A, IDirectInputDevice8A>(create, IID_IDirectInput8A, "A");
    const bool w = HookVariant<1, 3, IDirectInput8W, IDirectInputDevice8W>(create, IID_IDirectInput8W, "W");
    if (!a && !w) {
        LOG_INPUT("ERREUR : aucun hook DirectInput, clics du pointeur desactives");
        return false;
    }
    return true;
}

void RemoveDInputHooks()
{
    for (void* t : g_targets) {
        MH_DisableHook(t);
        MH_RemoveHook(t);
    }
    g_targets.clear();
}

void InjectMouseButton(int button, bool down)
{
    if (button < 0 || button > 1)
        return;
    std::lock_guard lock(g_mutex);
    if (g_buttons[button] == down)
        return;
    g_buttons[button] = down;
    if (g_mouseEvents.size() < kMaxQueued)
        g_mouseEvents.push_back({static_cast<DWORD>(DIMOFS_BUTTON0 + button), down ? 0x80u : 0u});
}

void InjectMouseWheel(int notches)
{
    if (notches == 0)
        return;
    std::lock_guard lock(g_mutex);
    g_stateDz += notches * WHEEL_DELTA;
    if (g_mouseEvents.size() < kMaxQueued)
        g_mouseEvents.push_back({static_cast<DWORD>(DIMOFS_Z), static_cast<DWORD>(notches * WHEEL_DELTA)});
}

void InjectMouseMove(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;
    std::lock_guard lock(g_mutex);
    g_stateDx += dx;
    g_stateDy += dy;
    if (g_mouseEvents.size() + 2 > kMaxQueued)
        return;
    if (dx)
        g_mouseEvents.push_back({static_cast<DWORD>(DIMOFS_X), static_cast<DWORD>(dx)});
    if (dy)
        g_mouseEvents.push_back({static_cast<DWORD>(DIMOFS_Y), static_cast<DWORD>(dy)});
}

void TakeRealMouseMotion(int& dx, int& dy)
{
    std::lock_guard lock(g_mutex);
    dx = g_realDx, dy = g_realDy;
    g_realDx = g_realDy = 0;
}

void TakeRealCursorMotion(int& dx, int& dy)
{
    std::lock_guard lock(g_mutex);
    dx = g_realCursorDx, dy = g_realCursorDy;
    g_realCursorDx = g_realCursorDy = 0;
}

bool RealMouseButtonHeld()
{
    std::lock_guard lock(g_mutex);
    return g_realButtons;
}

unsigned long long RealMouseActivity()
{
    std::lock_guard lock(g_mutex);
    return g_realActivity;
}

void InjectKey(int dik, bool down)
{
    if (dik <= 0 || dik > 255)
        return;
    std::lock_guard lock(g_mutex);
    if (g_keys[dik] == down)
        return;
    g_keys[dik] = down;
    if (g_keyEvents.size() < kMaxQueued)
        g_keyEvents.push_back({static_cast<DWORD>(dik), down ? 0x80u : 0u});
}

void ReleaseAllInjected()
{
    for (int b = 0; b < 2; ++b)
        InjectMouseButton(b, false);
    for (int k = 1; k < 256; ++k)
        InjectKey(k, false);
}

} // namespace input
