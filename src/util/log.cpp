#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>

namespace vrlog {
namespace {
char g_dir[MAX_PATH]{};
char g_path[MAX_PATH]{};
std::mutex g_mutex;
// Gardé ouvert (lisible par les autres programmes pendant la partie) : ouvrir et fermer le fichier
// à chaque ligne coûte cher (antivirus, Wine). Vidé après chaque ligne : rien n'est perdu si le
// jeu plante.
FILE* g_file = nullptr;
} // namespace

const char* Dir()
{
    return g_dir;
}

void Init(HMODULE self)
{
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    if (char* slash = strrchr(g_dir, '\\'))
        snprintf(slash + 1, MAX_PATH - (slash + 1 - g_dir), "%s\\", kDirName);
    CreateDirectoryA(g_dir, nullptr); // échoue sans conséquence s'il existe déjà
    snprintf(g_path, MAX_PATH, "%swarband_vr.log", g_dir);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lock(g_mutex);
    if (g_file)
        fclose(g_file);
    g_file = _fsopen(g_path, "w", _SH_DENYNO);
    if (g_file) {
        fprintf(g_file, "=== WarbandVR - session du %04u-%02u-%02u %02u:%02u:%02u ===\n", t.wYear, t.wMonth,
                t.wDay, t.wHour, t.wMinute, t.wSecond);
        fflush(g_file);
    }
}

void Write(const char* tag, const char* fmt, ...)
{
    if (!g_path[0])
        return;
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lock(g_mutex);
    if (!g_file)
        g_file = _fsopen(g_path, "a", _SH_DENYNO); // ouverture ratée à Init : nouvel essai
    if (g_file) {
        fprintf(g_file, "%02u:%02u:%02u.%03u %-8s %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tag, msg);
        fflush(g_file);
    }
}

} // namespace vrlog
