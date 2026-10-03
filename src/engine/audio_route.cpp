#include "engine/audio_route.h"
#include "util/log.h"

#include <windows.h>
#include <MinHook.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <atomic>
#include <mutex>
#include <string>

#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)

namespace engine {
namespace {

// DLL FMOD Ex possibles du jeu (WSE2 64 bits)
const wchar_t* const kFmodModules[] = {L"fmodex_wse2.dll", L"fmodex64.dll"};
constexpr char kUpdateExport[] = "?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ";

// API C de FMOD Ex : FMOD_SYSTEM* est le même objet que FMOD::System*
using GetNumDriversFn = int(__stdcall*)(void*, int*);
using GetDriverInfoWFn = int(__stdcall*)(void*, int, short*, int, void*);
using GetDriverFn = int(__stdcall*)(void*, int*);
using SetDriverFn = int(__stdcall*)(void*, int);
using UpdateFn = int(__fastcall*)(void*);

GetNumDriversFn g_getNumDrivers = nullptr;
GetDriverInfoWFn g_getDriverInfoW = nullptr;
GetDriverFn g_getDriver = nullptr;
SetDriverFn g_setDriver = nullptr;
UpdateFn g_update = nullptr;
void* g_updateTarget = nullptr;

std::mutex g_mutex;
std::wstring g_wanted;  // nom de la sortie voulue
std::wstring g_applied; // nom pour lequel la bascule a été tentée
std::atomic<bool> g_changed{false};

bool SameDevice(const std::wstring& fmodName, const std::wstring& wanted)
{
    // FMOD peut tronquer les noms (WinMM : 31 caractères) : comparaison sur la partie commune
    if (fmodName.empty() || wanted.empty())
        return false;
    const size_t n = std::min(fmodName.size(), wanted.size());
    return n >= 8 && _wcsnicmp(fmodName.c_str(), wanted.c_str(), n) == 0;
}

// Thread du jeu, avant System::update : bascule sur la sortie voulue si nécessaire
void ApplyDevice(void* system)
{
    if (!g_changed.exchange(false))
        return;
    std::wstring wanted;
    {
        std::lock_guard lock(g_mutex);
        if (g_wanted == g_applied)
            return;
        wanted = g_applied = g_wanted;
    }
    int count = 0, current = -1;
    if (g_getNumDrivers(system, &count) != 0)
        return;
    g_getDriver(system, &current);
    int match = -1;
    for (int i = 0; i < count; ++i) {
        short name[256]{};
        g_getDriverInfoW(system, i, name, 255, nullptr);
        const std::wstring w(reinterpret_cast<wchar_t*>(name));
        LOG_ENGINE("son : sortie FMOD %d%s : %ls", i, i == current ? " (actuelle)" : "", w.c_str());
        if (match < 0 && SameDevice(w, wanted))
            match = i;
    }
    if (match < 0) {
        LOG_ENGINE("son : sortie du casque \"%ls\" absente de FMOD, son inchange", wanted.c_str());
        return;
    }
    if (match == current) {
        LOG_ENGINE("son : deja sur la sortie du casque (%d)", match);
        return;
    }
    const int r = g_setDriver(system, match);
    LOG_ENGINE("son : bascule sur la sortie %d (%ls) : %s (code %d)", match, wanted.c_str(), r == 0 ? "ok" : "ECHEC",
               r);
}

int __fastcall HkUpdate(void* system)
{
    ApplyDevice(system);
    return g_update(system);
}

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), défini ici pour l'édition de liens
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring FriendlyName(IMMDevice* device)
{
    std::wstring name;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR)
            name = v.pwszVal;
        PropVariantClear(&v);
        props->Release();
    }
    return name;
}

} // namespace

bool InstallAudioRoute()
{
    if (g_updateTarget)
        return true;
    HMODULE fmod = nullptr;
    const wchar_t* fmodName = nullptr;
    for (const wchar_t* m : kFmodModules)
        if ((fmod = GetModuleHandleW(m)) != nullptr) {
            fmodName = m;
            break;
        }
    if (!fmod)
        return false;
    void* update = reinterpret_cast<void*>(GetProcAddress(fmod, kUpdateExport));
    g_getNumDrivers = reinterpret_cast<GetNumDriversFn>(GetProcAddress(fmod, "FMOD_System_GetNumDrivers"));
    g_getDriverInfoW = reinterpret_cast<GetDriverInfoWFn>(GetProcAddress(fmod, "FMOD_System_GetDriverInfoW"));
    g_getDriver = reinterpret_cast<GetDriverFn>(GetProcAddress(fmod, "FMOD_System_GetDriver"));
    g_setDriver = reinterpret_cast<SetDriverFn>(GetProcAddress(fmod, "FMOD_System_SetDriver"));
    if (!update || !g_getNumDrivers || !g_getDriverInfoW || !g_getDriver || !g_setDriver) {
        LOG_ENGINE("son : fonctions FMOD introuvables dans %ls, son du casque desactive", fmodName);
        g_updateTarget = reinterpret_cast<void*>(1); // ne pas réessayer
        return true;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return true;
    st = MH_CreateHook(update, &HkUpdate, reinterpret_cast<void**>(&g_update));
    if (st == MH_OK)
        st = MH_EnableHook(update);
    if (st != MH_OK) {
        LOG_ENGINE("son : detour de FMOD::System::update impossible (%d)", static_cast<int>(st));
        g_updateTarget = reinterpret_cast<void*>(1);
        return true;
    }
    g_updateTarget = update;
    LOG_ENGINE("son : FMOD::System::update detourne (%ls)", fmodName);
    return true;
}

void RemoveAudioRoute()
{
    if (!g_updateTarget || g_updateTarget == reinterpret_cast<void*>(1))
        return;
    MH_DisableHook(g_updateTarget);
    MH_RemoveHook(g_updateTarget);
    g_updateTarget = nullptr;
}

void SetHeadsetAudioDevice(const wchar_t* endpointId)
{
    static bool comReady = false;
    if (!comReady)
        comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return;
    IMMDevice* device = nullptr;
    if (endpointId && *endpointId)
        enumerator->GetDevice(endpointId, &device);
    if (!device)
        enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    std::wstring name;
    if (device) {
        name = FriendlyName(device);
        device->Release();
    }
    enumerator->Release();
    if (name.empty())
        return;
    std::lock_guard lock(g_mutex);
    if (name != g_wanted) {
        LOG_ENGINE("son : sortie du casque = %ls (%s)", name.c_str(),
                   endpointId && *endpointId ? "donnee par le runtime" : "sortie Windows par defaut");
        g_wanted = name;
        g_changed = true;
    }
}

} // namespace engine
