#include "util/hook.h"
#include "util/log.h"

#include <MinHook.h>

#include <cstring>
#include <mutex>

namespace hook {
namespace {

struct Patch {
    void** slot;   // entrée de table virtuelle ou d'imports
    void* original;
};
std::mutex g_mutex;
std::vector<Patch> g_patches;

bool WritePointer(void** slot, void* value)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

bool PatchSlot(void** slot, void* detour, void** original)
{
    std::lock_guard lock(g_mutex);
    for (const Patch& p : g_patches)
        if (p.slot == slot)
            return true; // déjà détournée (table partagée entre deux objets)
    void* previous = *slot;
    if (!WritePointer(slot, detour))
        return false;
    *original = previous;
    g_patches.push_back({slot, previous});
    return true;
}

// Module hybride ARM64EC : sa configuration de chargement pointe vers des métadonnées CHPE
bool IsHybridModule(const void* code)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(code), &module) ||
        !module)
        return false;
    auto* base = reinterpret_cast<const uint8_t*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    if (!dir.VirtualAddress || dir.Size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer) + 8)
        return false;
    auto* cfg = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY64*>(base + dir.VirtualAddress);
    return cfg->Size >= offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer) + 8 && cfg->CHPEMetadataPointer;
}

} // namespace

bool IsArm64EcCode(const void* code)
{
    using RtlIsEcCodeFn = BOOLEAN(NTAPI*)(ULONG64);
    static const RtlIsEcCodeFn isEc =
        reinterpret_cast<RtlIsEcCodeFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlIsEcCode"));
    if (isEc)
        return isEc(reinterpret_cast<ULONG64>(code)) != 0;
    return IsHybridModule(code);
}

bool HookMethod(void* object, int index, void* detour, void** original, const char* name, const char* prefix,
                std::vector<void*>& mhTargets)
{
    void** slot = &(*reinterpret_cast<void***>(object))[index];
    void* target = *slot;
    if (!target)
        return false;
    if (IsArm64EcCode(target)) {
        if (!PatchSlot(slot, detour, original)) {
            vrlog::Write(prefix, "  %-38s ECHEC table virtuelle (%lu)", name, GetLastError());
            return false;
        }
        vrlog::Write(prefix, "  %-38s %p detourne (table virtuelle, code ARM64EC)", name, target);
        return true;
    }
    for (void* t : mhTargets)
        if (t == target) {
            vrlog::Write(prefix, "  %-38s %p (deja detourne, partage)", name, target);
            return true;
        }
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st == MH_OK)
        st = MH_EnableHook(target);
    if (st != MH_OK) {
        vrlog::Write(prefix, "  %-38s %p ECHEC MinHook : %s", name, target, MH_StatusToString(st));
        MH_RemoveHook(target);
        return false;
    }
    mhTargets.push_back(target);
    vrlog::Write(prefix, "  %-38s %p detourne", name, target);
    return true;
}

bool HookImport(HMODULE module, const char* dll, const char* function, void* detour, void** original,
                const char* prefix)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0)
                continue;
            if (!PatchSlot(reinterpret_cast<void**>(&slots->u1.Function), detour, original)) {
                vrlog::Write(prefix, "  %s!%s : ECHEC table d'imports (%lu)", dll, function, GetLastError());
                return false;
            }
            vrlog::Write(prefix, "  %s!%s detourne (table d'imports du jeu)", dll, function);
            return true;
        }
    }
    vrlog::Write(prefix, "  %s!%s absent de la table d'imports", dll, function);
    return false;
}

void RemoveAll()
{
    std::lock_guard lock(g_mutex);
    for (const Patch& p : g_patches)
        WritePointer(p.slot, p.original);
    g_patches.clear();
}

} // namespace hook
