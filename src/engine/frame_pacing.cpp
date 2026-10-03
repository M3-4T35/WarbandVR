#include "engine/frame_pacing.h"
#include "util/log.h"
#include "util/pattern.h"

#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)

namespace engine {
namespace {

// CD3DApplication::SetMinFrameTime(this, double secondes) : test de bForceVSync puis du
// rafraîchissement de l'écran (+0x2B0)
constexpr char kSetMinFrameTimeSig[] = "80 3D ?? ?? ?? ?? 00 74 28 8B 81 B0 02 00 00 83 F8 1E 76 1D";
// lea rcx, [g_D3DApp] ; call ... ; lea rcx, [...] ; call ... ; cmp byte ptr [...]
constexpr char kD3DAppSig[] = "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 3D";

// movd xmm0, [rglConfig::General::iMaxFrameRate] ; movsd xmm1, [1.0] ; mov ... (Initialize,
// UpdateBackBuffer : le jeu y recalcule sa cadence, par exemple après un redimensionnement)
constexpr char kMaxFrameRateSig[] = "66 0F 6E 05 ?? ?? ?? ?? F2 0F 10 0D ?? ?? ?? ?? 48 8B";

using SetMinFrameTimeFn = void(__fastcall*)(void* app, double seconds);
SetMinFrameTimeFn g_setMinFrameTime = nullptr;
void* g_app = nullptr;
int* g_maxFrameRate = nullptr;

} // namespace

bool InitFramePacing()
{
    g_setMinFrameTime =
        reinterpret_cast<SetMinFrameTimeFn>(pattern::Find("CD3DApplication::SetMinFrameTime", kSetMinFrameTimeSig));
    g_app = pattern::FindGlobal("g_D3DApp", kD3DAppSig, 3, 7);
    g_maxFrameRate = static_cast<int*>(pattern::FindGlobal("iMaxFrameRate", kMaxFrameRateSig, 4, 8));
    if (!g_setMinFrameTime || !g_app) {
        LOG_ENGINE("cadence du jeu non reglable (signature manquante) : elle reste celle du jeu");
        g_setMinFrameTime = nullptr;
        return false;
    }
    return true;
}

bool SetGameMaxFps(double fps)
{
    if (!g_setMinFrameTime || fps < 10)
        return false;
    // La variable du réglage aussi : le jeu la relit quand il recrée son back buffer
    if (g_maxFrameRate)
        *g_maxFrameRate = static_cast<int>(fps + 0.5);
    g_setMinFrameTime(g_app, 1.0 / fps);
    LOG_ENGINE("cadence du jeu : %.1f images/s maximum", fps);
    return true;
}

} // namespace engine
