#include "util/pattern.h"
#include "util/log.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace pattern {
namespace {

// -1 = joker
std::vector<int> Parse(const char* sig)
{
    std::vector<int> bytes;
    for (const char* p = sig; *p;) {
        if (*p == ' ') {
            ++p;
        } else if (*p == '?') {
            bytes.push_back(-1);
            while (*p == '?')
                ++p;
        } else {
            char* end = nullptr;
            bytes.push_back(static_cast<int>(strtoul(p, &end, 16)));
            p = end;
        }
    }
    return bytes;
}

} // namespace

namespace {

// Toutes les occurrences de la signature dans les sections exécutables
std::vector<uint8_t*> FindAll(const char* signature, HMODULE module)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const std::vector<int> sig = Parse(signature);
    std::vector<uint8_t*> hits;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t* start = base + sec->VirtualAddress;
        const size_t size = sec->Misc.VirtualSize;
        if (size < sig.size())
            continue;
        for (size_t off = 0; off <= size - sig.size(); ++off) {
            size_t k = 0;
            while (k < sig.size() && (sig[k] < 0 || start[off + k] == sig[k]))
                ++k;
            if (k == sig.size())
                hits.push_back(start + off);
        }
    }
    return hits;
}

} // namespace

void* FindGlobal(const char* name, const char* signature, int dispOffset, int instrLength, HMODULE module)
{
    if (!module)
        module = GetModuleHandleW(nullptr);
    const auto base = reinterpret_cast<uintptr_t>(module);
    const std::vector<uint8_t*> hits = FindAll(signature, module);
    uintptr_t target = 0;
    for (uint8_t* h : hits) {
        int32_t disp = 0;
        memcpy(&disp, h + dispOffset, sizeof(disp));
        const uintptr_t t = reinterpret_cast<uintptr_t>(h) + instrLength + disp;
        if (target && t != target) {
            LOG_CORE("ERREUR : variable %s : occurrences contradictoires", name);
            return nullptr;
        }
        target = t;
    }
    if (!target) {
        LOG_CORE("ERREUR : variable %s introuvable", name);
        return nullptr;
    }
    LOG_CORE("variable %s trouvee : exe+0x%llX (%zu occurrences concordantes)", name,
             static_cast<unsigned long long>(target - base), hits.size());
    return reinterpret_cast<void*>(target);
}

void* Find(const char* name, const char* signature, HMODULE module)
{
    if (!module)
        module = GetModuleHandleW(nullptr);
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const std::vector<int> sig = Parse(signature);

    uint8_t* found = nullptr;
    int hits = 0;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t* start = base + sec->VirtualAddress;
        const size_t size = sec->Misc.VirtualSize;
        if (size < sig.size())
            continue;
        for (size_t off = 0; off <= size - sig.size(); ++off) {
            size_t k = 0;
            while (k < sig.size() && (sig[k] < 0 || start[off + k] == sig[k]))
                ++k;
            if (k == sig.size()) {
                if (++hits == 1)
                    found = start + off;
            }
        }
    }

    if (hits == 1) {
        LOG_CORE("signature %s trouvee : exe+0x%llX", name, static_cast<unsigned long long>(found - base));
        return found;
    }
    LOG_CORE("ERREUR : signature %s %s (%d occurrences)", name, hits ? "ambigue" : "introuvable", hits);
    return nullptr;
}

} // namespace pattern
