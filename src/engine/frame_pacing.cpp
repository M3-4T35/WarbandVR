#include "engine/frame_pacing.h"
#include "util/log.h"
#include "util/pattern.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)

namespace engine {
namespace {

// CD3DApplication::SetMinFrameTime(this, double secondes) : test de bForceVSync puis du
// rafraîchissement de l'écran (+0x2B0)
constexpr char kSetMinFrameTimeSig[] = "80 3D ?? ?? ?? ?? 00 74 28 8B 81 B0 02 00 00 83 F8 1E 76 1D";
// lea rcx, [g_D3DApp] ; call ... ; lea rcx, [...] ; call ... ; cmp byte ptr [...]
constexpr char kD3DAppSig[] = "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 3D";

using SetMinFrameTimeFn = void(__fastcall*)(void* app, double seconds);
SetMinFrameTimeFn g_setMinFrameTime = nullptr;
void* g_app = nullptr;
size_t g_minFrameTimeOffset = 0; // CD3DApplication : temps minimal par image (double), lu dans le code
std::atomic<double> g_wantedFps{0};
bool g_enforce = false;

// Offset du champ écrit par SetMinFrameTime : movsd qword ptr [rcx + disp32], xmm1 (F2 0F 11 89)
size_t FindMinFrameTimeOffset(const uint8_t* fn)
{
    for (int i = 0; i < 0x40; ++i)
        if (fn[i] == 0xF2 && fn[i + 1] == 0x0F && fn[i + 2] == 0x11 && fn[i + 3] == 0x89) {
            int32_t disp = 0;
            memcpy(&disp, fn + i + 4, sizeof(disp));
            return disp > 0 && disp < 0x10000 ? static_cast<size_t>(disp) : 0;
        }
    return 0;
}

double CurrentMinFrameTime()
{
    double v = 0;
    memcpy(&v, static_cast<const char*>(g_app) + g_minFrameTimeOffset, sizeof(v));
    return v;
}

} // namespace

bool InitFramePacing()
{
    g_setMinFrameTime =
        reinterpret_cast<SetMinFrameTimeFn>(pattern::Find("CD3DApplication::SetMinFrameTime", kSetMinFrameTimeSig));
    g_app = pattern::FindGlobal("g_D3DApp", kD3DAppSig, 3, 7);
    if (!g_setMinFrameTime || !g_app) {
        LOG_ENGINE("cadence du jeu non reglable (signature manquante) : elle reste celle du jeu");
        g_setMinFrameTime = nullptr;
        return false;
    }
    g_minFrameTimeOffset = FindMinFrameTimeOffset(reinterpret_cast<const uint8_t*>(g_setMinFrameTime));
    if (g_minFrameTimeOffset)
        LOG_ENGINE("temps minimal par image du jeu : CD3DApplication+0x%zX", g_minFrameTimeOffset);
    else
        LOG_ENGINE("temps minimal par image du jeu introuvable : limite des options du jeu non surveillee");
    return true;
}

bool SetGameMaxFps(double fps)
{
    if (!g_setMinFrameTime || fps < 10)
        return false;
    // Pas de iMaxFrameRate : le jeu l'enregistre dans rgl_config.ini (il resterait hors VR)
    g_wantedFps = fps;
    g_enforce = g_minFrameTimeOffset != 0;
    g_setMinFrameTime(g_app, 1.0 / fps);
    LOG_ENGINE("cadence du jeu : %.1f images/s maximum", fps);
    return true;
}

void EnforceGameMaxFps()
{
    if (!g_enforce || !g_setMinFrameTime)
        return;
    const double fps = g_wantedFps;
    const double wanted = 1.0 / fps;
    const double current = CurrentMinFrameTime();
    // 0 : le jeu lève sa limite pendant les chargements (à chaque image) ; on le laisse faire
    if (std::fabs(current - wanted) < 1e-7 || current <= 0)
        return;
    // Options du jeu (limite d'images), redimensionnement : le jeu a remis sa propre limite
    g_setMinFrameTime(g_app, wanted);
    if (std::fabs(CurrentMinFrameTime() - wanted) >= 1e-7) {
        LOG_ENGINE("cadence imposee par le jeu (VSync forcee ?) : %.1f images/s au lieu de %.1f",
                   current > 0 ? 1.0 / current : 0.0, fps);
        g_enforce = false;
        return;
    }
    // Une ligne par valeur différente (options du jeu), pas à chaque redimensionnement identique
    static double lastLogged = -1;
    if (current != lastLogged) {
        LOG_ENGINE("limite d'images changee par le jeu (%.1f images/s) : remise a %.1f", 1.0 / current, fps);
        lastLogged = current;
    }
}

} // namespace engine
