#include "input/pointer.h"
#include "input/dinput_hook.h"
#include "render/game_window.h"
#include "util/hook.h"
#include "util/log.h"
#include "xr/xr_session.h"

#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <mutex>

#define LOG_INPUT(...) ::vrlog::Write("[input]", __VA_ARGS__)

namespace input {
namespace {

using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
GetCursorPosFn g_getCursorPos = nullptr;
void* g_getCursorPosTarget = nullptr;

std::mutex g_mutex;
bool g_onScreen = false;
POINT g_client{}; // point visé, coordonnées clientes de la fenêtre du jeu
bool g_loggedFirst = false;
int g_loggedReads = 0;
int g_loggedClicks = 0;
// Mode souris : curseur à nous, déplacé par la vraie souris (le vrai curseur Windows ne peut pas
// atteindre la partie de la fenêtre qui dépasse de l'écran)
float g_mouseX = -1, g_mouseY = -1; // coordonnées clientes, -1 = pas encore placé
bool g_mouseShown = false;
bool g_loggedMouseCursor = false;

// Fenêtre du jeu de la taille du casque (~2600 pixels de haut) : un mouvement de souris y couvre
// la même part de l'écran que sur un bureau en 1080p
void UpdateMouseCursor(HWND hwnd)
{
    int dx = 0, dy = 0;
    TakeRealCursorMotion(dx, dy);
    RECT client{};
    if (!hwnd || !GetClientRect(hwnd, &client) || client.bottom <= 0)
        return;
    if (g_mouseX < 0)
        g_mouseX = client.right / 2.0f, g_mouseY = client.bottom / 2.0f;
    const float scale = client.bottom / 1080.0f;
    g_mouseX = std::clamp(g_mouseX + dx * scale, 0.0f, client.right - 1.0f);
    g_mouseY = std::clamp(g_mouseY + dy * scale, 0.0f, client.bottom - 1.0f);
}

BOOL WINAPI HkGetCursorPos(LPPOINT pt)
{
    if (pt) {
        HWND hwnd = render::GameWindow();
        std::lock_guard lock(g_mutex);
        if (g_onScreen && hwnd) {
            POINT p = g_client;
            ClientToScreen(hwnd, &p);
            *pt = p;
            if (g_loggedReads < 3) {
                LOG_INPUT("position lue par le jeu : client %ld,%ld -> ecran %ld,%ld (thread %lu)", g_client.x,
                          g_client.y, p.x, p.y, GetCurrentThreadId());
                ++g_loggedReads;
            }
            return TRUE;
        }
        if (xr::IsMouseMode() && hwnd) {
            UpdateMouseCursor(hwnd);
            POINT p{static_cast<LONG>(g_mouseX), static_cast<LONG>(g_mouseY)};
            ClientToScreen(hwnd, &p);
            *pt = p;
            g_mouseShown = true;
            if (!g_loggedMouseCursor) {
                LOG_INPUT("mode souris : curseur des menus deplace par la vraie souris");
                g_loggedMouseCursor = true;
            }
            return TRUE;
        }
        g_mouseShown = false;
    }
    return g_getCursorPos(pt);
}

} // namespace

bool InstallPointerHooks()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    g_getCursorPosTarget = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos")) : nullptr;
    if (!g_getCursorPosTarget) {
        LOG_INPUT("ERREUR : GetCursorPos introuvable, pointeur desactive");
        return false;
    }
    // Windows / Proton sur ARM : user32 est en ARM64EC, MinHook ne peut pas réécrire son code ;
    // on détourne l'appel du jeu dans sa table d'imports
    if (hook::IsArm64EcCode(g_getCursorPosTarget)) {
        g_getCursorPosTarget = nullptr;
        if (!hook::HookImport(GetModuleHandleW(nullptr), "USER32.dll", "GetCursorPos",
                              reinterpret_cast<void*>(&HkGetCursorPos), reinterpret_cast<void**>(&g_getCursorPos),
                              "[input]")) {
            LOG_INPUT("ERREUR : GetCursorPos non detourne, pointeur desactive");
            return false;
        }
        LOG_INPUT("GetCursorPos detourne (pointeur VR)");
        InstallDInputHooks();
        return true;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_INPUT("ERREUR : MH_Initialize : %s", MH_StatusToString(st));
        return false;
    }
    st = MH_CreateHook(g_getCursorPosTarget, &HkGetCursorPos, reinterpret_cast<void**>(&g_getCursorPos));
    if (st == MH_OK)
        st = MH_EnableHook(g_getCursorPosTarget);
    if (st != MH_OK) {
        LOG_INPUT("ERREUR : hook GetCursorPos : %s", MH_StatusToString(st));
        MH_RemoveHook(g_getCursorPosTarget);
        g_getCursorPosTarget = nullptr;
        return false;
    }
    LOG_INPUT("GetCursorPos detourne (pointeur VR)");
    InstallDInputHooks();
    return true;
}

void RemovePointerHooks()
{
    RemoveDInputHooks();
    if (!g_getCursorPosTarget)
        return;
    MH_DisableHook(g_getCursorPosTarget);
    MH_RemoveHook(g_getCursorPosTarget);
    g_getCursorPosTarget = nullptr;
}

void SetPointerTarget(bool onScreen, float u, float v)
{
    HWND hwnd = render::GameWindow();
    RECT client{};
    if (hwnd)
        GetClientRect(hwnd, &client);
    std::lock_guard lock(g_mutex);
    g_onScreen = onScreen && hwnd && client.right > 0;
    if (g_onScreen) {
        g_client.x = std::clamp(static_cast<LONG>(u * client.right), 0L, client.right - 1);
        g_client.y = std::clamp(static_cast<LONG>(v * client.bottom), 0L, client.bottom - 1);
        if (!g_loggedFirst) {
            LOG_INPUT("pointeur sur l'ecran virtuel : %ld,%ld (zone cliente %ldx%ld)", g_client.x, g_client.y,
                      client.right, client.bottom);
            g_loggedFirst = true;
        }
    }
}

void SetPointerButton(bool left, bool down)
{
    POINT client{};
    {
        std::lock_guard lock(g_mutex);
        if (!g_onScreen && down)
            return;
        client = g_client;
    }
    // Sans le focus, DirectInput ne donne pas la souris au jeu : le clic serait perdu
    if (down)
        render::BringGameToFront();
    InjectMouseButton(left ? 0 : 1, down);
    if (g_loggedClicks < 10) {
        LOG_INPUT("clic %s %s en %ld,%ld", left ? "gauche" : "droit", down ? "appuye" : "relache", client.x,
                  client.y);
        ++g_loggedClicks;
    }
}

bool GetMouseCursor(float& u, float& v)
{
    HWND hwnd = render::GameWindow();
    RECT client{};
    if (!hwnd || !GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0)
        return false;
    std::lock_guard lock(g_mutex);
    if (!g_mouseShown || g_mouseX < 0)
        return false;
    u = g_mouseX / client.right;
    v = g_mouseY / client.bottom;
    return true;
}

void ScrollPointer(int notches)
{
    {
        std::lock_guard lock(g_mutex);
        if (!g_onScreen || notches == 0)
            return;
    }
    InjectMouseWheel(notches);
}

} // namespace input
