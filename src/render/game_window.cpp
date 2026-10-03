#include "render/game_window.h"
#include "util/log.h"

#include <algorithm>
#include <atomic>

namespace render {
namespace {

std::atomic<HWND> g_window{nullptr};
WNDPROC g_originalProc = nullptr;
std::atomic<LONG> g_maxWidth{0}; // taille de fenêtre maximale autorisée (0 = défaut Windows)
std::atomic<LONG> g_maxHeight{0};

// Sous-classement : par défaut Windows limite une fenêtre à la taille de l'écran
LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const LRESULT result = CallWindowProcW(g_originalProc, hwnd, msg, wp, lp);
    if (msg == WM_GETMINMAXINFO && g_maxWidth > 0) {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMaxTrackSize.x = std::max<LONG>(info->ptMaxTrackSize.x, g_maxWidth);
        info->ptMaxTrackSize.y = std::max<LONG>(info->ptMaxTrackSize.y, g_maxHeight);
        info->ptMaxSize.x = std::max<LONG>(info->ptMaxSize.x, g_maxWidth);
        info->ptMaxSize.y = std::max<LONG>(info->ptMaxSize.y, g_maxHeight);
    }
    return result;
}

} // namespace

void SetGameWindow(HWND window)
{
    if (window)
        g_window = window;
}

HWND GameWindow()
{
    return g_window;
}

void ResizeGameWindow(UINT clientWidth, UINT clientHeight)
{
    HWND hwnd = g_window;
    if (!hwnd || !IsWindow(hwnd)) {
        LOG_RENDER("redimensionnement impossible : fenetre du jeu inconnue");
        return;
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    if (static_cast<UINT>(client.right) == clientWidth && static_cast<UINT>(client.bottom) == clientHeight)
        return;
    if (IsZoomed(hwnd)) {
        LOG_RENDER("fenetre du jeu maximisee : pas de redimensionnement");
        return;
    }

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    RECT r{0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight)};
    AdjustWindowRectEx(&r, style, GetMenu(hwnd) != nullptr, exStyle);
    const LONG w = r.right - r.left, h = r.bottom - r.top;

    g_maxWidth = w;
    g_maxHeight = h;
    if (!g_originalProc) {
        g_originalProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc)));
    }
    LOG_RENDER("fenetre du jeu : %ldx%ld -> zone cliente %ux%u", client.right, client.bottom, clientWidth,
               clientHeight);
    SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    GetClientRect(hwnd, &client);
    LOG_RENDER("fenetre du jeu redimensionnee : zone cliente %ldx%ld", client.right, client.bottom);

    // Le jeu (framework DirectX CD3DApplication) ignore un WM_SIZE fenêtré et n'adapte son
    // rendu qu'à la fin d'un redimensionnement à la souris (WM_EXITSIZEMOVE) : on le simule.
    // Messages postés : le thread XR n'attend pas le Reset du device.
    PostMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    PostMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
}

bool BringGameToFront()
{
    HWND hwnd = g_window;
    if (!hwnd || !IsWindow(hwnd))
        return false;
    hwnd = GetAncestor(hwnd, GA_ROOT); // le premier plan concerne la fenêtre racine
    if (GetForegroundWindow() == hwnd)
        return true;
    // Windows refuse le premier plan à un processus qui n'a pas reçu la dernière entrée :
    // un appui simulé sur Alt lève ce verrou
    INPUT alt[2]{};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(hwnd);
    const bool ok = GetForegroundWindow() == hwnd;
    LOG_RENDER("mise au premier plan du jeu : %s", ok ? "ok" : "refusee par Windows");
    return ok;
}

void RestoreGameWindow()
{
    HWND hwnd = g_window;
    if (g_originalProc && hwnd && IsWindow(hwnd))
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalProc));
    g_originalProc = nullptr;
}

} // namespace render
