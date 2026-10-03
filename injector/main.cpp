// WarbandVR - injecteur CLI 64 bits
// Lance mb_warband_wse2_x64.exe depuis le dossier du jeu puis y injecte warband_vr.dll
// (CreateRemoteThread + LoadLibraryW). Ne modifie aucun fichier du jeu.
//
// Usage : warband_vr_injector [--game-dir <dossier>] [--dll <chemin>] [--early]
//                             [--steamvr | --runtime <json>]
//                             [--timeout <s>] [-- <arguments du jeu>]
//   --early   : injecte avant que le jeu ne démarre (processus suspendu).
//               Par défaut on attend que le renderer soit chargé, pour que la DLL
//               voie les modules graphiques.
//   --steamvr : utilise le runtime OpenXR de SteamVR pour ce lancement seulement
//               (Steam Frame, Index...), sans changer le runtime par défaut de Windows.
//   --runtime : idem avec le manifeste OpenXR (.json) d'un autre runtime.
//   --default-runtime : runtime OpenXR par défaut de Windows.
//   Sans ces options : réglage « Runtime VR » de warband_vr.ini (outil de configuration).
//   Arguments du jeu par défaut : --module Native --no-intro (comme Native_WSE2.bat)
#include "util/config.h"

#include <windows.h>
#include <psapi.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kDefaultGameDir[] =
    L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\MountBlade Warband";
constexpr wchar_t kExeName[] = L"mb_warband_wse2_x64.exe";
constexpr wchar_t kDllName[] = L"warband_vr.dll";
constexpr wchar_t kDefaultGameArgs[] = L"--module Native --no-intro";

// Modules dont la présence indique que le renderer est initialisé
constexpr const wchar_t* kGraphicsModules[] = {L"d3d9.dll", L"d3d11.dll", L"dxgi.dll",
                                               L"opengl32.dll"};

void PrintLastError(const wchar_t* what)
{
    DWORD err = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    fwprintf(stderr, L"[injector] ERREUR %s : %lu %s", what, err, msg ? msg : L"\n");
    if (msg)
        LocalFree(msg);
}

std::wstring DirOfSelf()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L'\\'));
}

std::wstring FullPath(const std::wstring& p)
{
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, buf, nullptr);
    return (n && n < MAX_PATH) ? std::wstring(buf) : p;
}

// Texte du fichier de réglages (page de code ANSI) vers UTF-16
std::wstring FromAnsi(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Renvoie le nom du premier module graphique chargé dans le processus, ou vide
std::wstring FindGraphicsModule(HANDLE process)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, mods, sizeof(mods), &needed, LIST_MODULES_64BIT))
        return {};
    DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), static_cast<DWORD>(std::size(mods)));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH]{};
        if (!GetModuleBaseNameW(process, mods[i], name, MAX_PATH))
            continue;
        for (const wchar_t* g : kGraphicsModules)
            if (_wcsicmp(name, g) == 0)
                return name;
    }
    return {};
}

bool IsModuleLoaded(HANDLE process, const std::wstring& fullPath)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, mods, sizeof(mods), &needed, LIST_MODULES_64BIT))
        return false;
    DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), static_cast<DWORD>(std::size(mods)));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleFileNameExW(process, mods[i], name, MAX_PATH) &&
            _wcsicmp(name, fullPath.c_str()) == 0)
            return true;
    }
    return false;
}

// Charge la DLL dans le processus cible via un thread distant sur LoadLibraryW
bool InjectDll(HANDLE process, const std::wstring& dllPath)
{
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        PrintLastError(L"VirtualAllocEx");
        return false;
    }

    bool ok = false;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        PrintLastError(L"WriteProcessMemory");
    } else {
        // kernel32 est chargé à la même adresse dans tous les processus d'une session
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (!thread) {
            PrintLastError(L"CreateRemoteThread");
        } else {
            if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) {
                fwprintf(stderr, L"[injector] ERREUR : LoadLibraryW distant n'a pas rendu la main en 30 s\n");
            } else {
                // Le code de sortie est le HMODULE tronqué à 32 bits : on vérifie par énumération
                DWORD exitCode = 0;
                GetExitCodeThread(thread, &exitCode);
                ok = exitCode != 0 || IsModuleLoaded(process, dllPath);
                if (!ok)
                    fwprintf(stderr, L"[injector] ERREUR : LoadLibraryW a échoué dans le jeu "
                                     L"(DLL introuvable, mauvaise architecture ou dépendance manquante)\n");
            }
            CloseHandle(thread);
        }
    }
    // En cas de timeout le thread distant peut encore lire la chaîne : on ne libère pas
    if (ok)
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

void Usage()
{
    fwprintf(stderr,
             L"Usage : warband_vr_injector [--game-dir <dossier>] [--dll <chemin>] [--early]\n"
             L"                            [--steamvr | --runtime <json> | --default-runtime]\n"
             L"                            [--timeout <s>] [-- <arguments du jeu>]\n"
             L"  Arguments du jeu par défaut : %s\n",
             kDefaultGameArgs);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // Accents : UTF-16 vers la console, UTF-8 quand la sortie est redirigée
    _setmode(_fileno(stdout), _isatty(_fileno(stdout)) ? _O_U16TEXT : _O_U8TEXT);
    _setmode(_fileno(stderr), _isatty(_fileno(stderr)) ? _O_U16TEXT : _O_U8TEXT);

    // Réglages du fichier warband_vr.ini (dossier du jeu, module, runtime), à côté de
    // l'injecteur ; la ligne de commande reste prioritaire
    config::Load(config::DefaultPath());
    std::wstring gameDir = kDefaultGameDir;
    {
        const std::string dir = config::GetText(config::Id::GameDir);
        if (!dir.empty())
            gameDir = FromAnsi(dir);
    }
    std::wstring dllPath = DirOfSelf() + L"\\" + kDllName;
    std::wstring gameArgs;
    bool early = false;
    bool customArgs = false;
    bool steamvr = false;
    bool runtimeChosen = false; // runtime imposé en ligne de commande
    std::wstring runtimeJson;
    DWORD timeoutSec = 120;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--game-dir" && i + 1 < argc) {
            gameDir = argv[++i];
        } else if (a == L"--dll" && i + 1 < argc) {
            dllPath = argv[++i];
        } else if (a == L"--timeout" && i + 1 < argc) {
            timeoutSec = static_cast<DWORD>(_wtoi(argv[++i]));
        } else if (a == L"--early") {
            early = true;
        } else if (a == L"--steamvr") {
            steamvr = runtimeChosen = true;
        } else if (a == L"--runtime" && i + 1 < argc) {
            runtimeJson = argv[++i];
            runtimeChosen = true;
        } else if (a == L"--default-runtime") {
            runtimeChosen = true;
        } else if (a == L"--") {
            customArgs = true;
            for (++i; i < argc; ++i) {
                if (!gameArgs.empty())
                    gameArgs += L' ';
                std::wstring arg = argv[i];
                // Sans guillemets : WSE2 découpe sa ligne de commande aux espaces et recolle les
                // mots d'une option (--module Viking Conquest) ; des guillemets resteraient dans
                // le nom du module, comme le fait aussi wse2_launcher.exe
                gameArgs += arg;
            }
        } else if (a == L"-h" || a == L"--help") {
            Usage();
            return 0;
        } else {
            fwprintf(stderr, L"[injector] Argument inconnu : %s\n", a.c_str());
            Usage();
            return 2;
        }
    }
    if (!customArgs) {
        const std::string module = config::GetText(config::Id::Module);
        if (module.empty())
            gameArgs = kDefaultGameArgs;
        else
            gameArgs = L"--module " + FromAnsi(module) + L" --no-intro"; // comme wse2_launcher.exe
    }

    dllPath = FullPath(dllPath);
    const std::wstring exePath = gameDir + L"\\" + kExeName;

    if (!FileExists(exePath)) {
        fwprintf(stderr, L"[injector] ERREUR : jeu introuvable : %s\n", exePath.c_str());
        return 1;
    }
    if (!FileExists(dllPath)) {
        fwprintf(stderr, L"[injector] ERREUR : DLL introuvable : %s\n", dllPath.c_str());
        return 1;
    }

    // Runtime OpenXR de ce lancement : variable XR_RUNTIME_JSON héritée par le jeu (le
    // chargeur OpenXR la préfère au runtime actif de Windows)
    // Sans option : réglage du fichier warband_vr.ini (à côté de l'injecteur et de la DLL)
    if (!runtimeChosen) {
        steamvr = config::GetInt(config::Id::OpenXRRuntime) == 1;
        if (steamvr)
            wprintf(L"[injector] Runtime VR : SteamVR (warband_vr.ini)\n");
    }
    if (steamvr && runtimeJson.empty()) {
        // SteamVR est installé à côté du jeu (steamapps\common\SteamVR)
        runtimeJson = FullPath(gameDir + L"\\..\\SteamVR\\steamxr_win64.json");
    }
    if (!runtimeJson.empty()) {
        runtimeJson = FullPath(runtimeJson);
        if (!FileExists(runtimeJson)) {
            fwprintf(stderr, L"[injector] ERREUR : runtime OpenXR introuvable : %s\n", runtimeJson.c_str());
            return 1;
        }
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", runtimeJson.c_str());
        wprintf(L"[injector] OpenXR    : %s\n", runtimeJson.c_str());
    } else {
        wprintf(L"[injector] OpenXR    : runtime actif de Windows\n");
    }

    wprintf(L"[injector] WarbandVR %hs\n", WARBANDVR_VERSION);
    wprintf(L"[injector] Jeu       : %s\n", exePath.c_str());
    wprintf(L"[injector] Arguments : %s\n", gameArgs.c_str());
    wprintf(L"[injector] DLL       : %s\n", dllPath.c_str());
    wprintf(L"[injector] Mode      : %s\n", early ? L"précoce (processus suspendu)" : L"après init du renderer");

    // CreateProcessW peut modifier la ligne de commande : il lui faut un tampon modifiable
    std::wstring cmdLine = L"\"" + exePath + L"\" " + gameArgs;
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exePath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                        early ? CREATE_SUSPENDED : 0, nullptr, gameDir.c_str(), &si, &pi)) {
        PrintLastError(L"CreateProcessW");
        return 1;
    }
    wprintf(L"[injector] Processus lancé, PID %lu\n", pi.dwProcessId);

    bool ok = false;
    if (early) {
        ok = InjectDll(pi.hProcess, dllPath);
        ResumeThread(pi.hThread);
    } else {
        // On attend qu'un module graphique apparaisse (ou que le jeu se ferme)
        const ULONGLONG deadline = GetTickCount64() + timeoutSec * 1000ULL;
        std::wstring gfx;
        while (GetTickCount64() < deadline) {
            if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(pi.hProcess, &code);
                fwprintf(stderr, L"[injector] ERREUR : le jeu s'est fermé avant l'injection (code %lu)\n", code);
                break;
            }
            gfx = FindGraphicsModule(pi.hProcess);
            if (!gfx.empty())
                break;
        }
        if (!gfx.empty()) {
            wprintf(L"[injector] Module graphique détecté : %s\n", gfx.c_str());
            // Laisse au jeu le temps de créer son device avant d'injecter
            WaitForInputIdle(pi.hProcess, 10000);
            ok = InjectDll(pi.hProcess, dllPath);
        } else if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
            fwprintf(stderr, L"[injector] Aucun module graphique après %lu s, injection quand même\n", timeoutSec);
            ok = InjectDll(pi.hProcess, dllPath);
        }
    }

    wprintf(ok ? L"[injector] Injection réussie. Log : %s\\warband_vr.log\n"
               : L"[injector] Injection échouée.\n",
            dllPath.substr(0, dllPath.find_last_of(L'\\')).c_str());

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok ? 0 : 1;
}
