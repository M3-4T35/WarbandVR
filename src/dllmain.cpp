// WarbandVR - point d'entrée de la DLL injectée dans mb_warband_wse2_x64.exe
#include <windows.h>

#include "engine/camera_hook.h"
#include "engine/combat_hook.h"
#include "engine/text_input.h"
#include "engine/orders.h"
#include "engine/audio_route.h"
#include "engine/frame_pacing.h"
#include "engine/skeleton_hook.h"
#include "input/pointer.h"
#include "render/d3d9_hook.h"
#include "render/game_window.h"
#include "util/config.h"
#include "util/hook.h"
#include "util/log.h"
#include "xr/xr_session.h"

static DWORD WINAPI InitThread(LPVOID)
{
    HMODULE exe = GetModuleHandleA(nullptr);
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(exe, exePath, MAX_PATH);

    LOG_CORE("DLL injectee (WarbandVR %s)", WARBANDVR_VERSION);
    LOG_CORE("  processus   : %s", exePath);
    LOG_CORE("  base exe    : %p", exe);
    LOG_CORE("  architecture: %u bits", static_cast<unsigned>(sizeof(void*) * 8));
    LOG_CORE("  d3d9.dll    : %s", GetModuleHandleA("d3d9.dll") ? "chargee" : "absente");
    LOG_CORE("  opengl32.dll: %s", GetModuleHandleA("opengl32.dll") ? "chargee" : "absente");

    // Réglages (warband_vr.ini, créé avec les valeurs par défaut s'il manque)
    config::Init();
    LOG_CORE("reglages : %s", config::DefaultPath().c_str());

    if (!render::InstallD3D9Hooks()) {
        LOG_CORE("hook du renderer indisponible : le jeu continue sans VR");
        return 0;
    }
    input::InstallPointerHooks();
    xr::Start();
    engine::InstallCameraHook();
    engine::InstallSkeletonHook();
    engine::InstallCombatHook();
    engine::InitTextInput();
    engine::InstallOrdersHook();
    engine::InitFramePacing();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        vrlog::Init(hModule);
        // Jamais de travail lourd dans DllMain (loader lock) : on délègue à un thread
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr))
            CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // FreeLibrary explicite : on retire les hooks. À la fermeture du processus
        // (reserved != nullptr) les autres threads sont déjà morts, on ne touche à rien.
        xr::RequestStop();
        engine::RemoveOrdersHook();
        engine::RemoveAudioRoute();
        engine::RemoveCombatHook();
        engine::RemoveSkeletonHook();
        engine::RemoveCameraHook();
        input::RemovePointerHooks();
        render::RemoveD3D9Hooks();
        hook::RemoveAll(); // tables virtuelles et d'imports (code ARM64EC)
        render::RestoreGameWindow();
    }
    return TRUE;
}
