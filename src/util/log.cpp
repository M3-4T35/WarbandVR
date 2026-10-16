#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace vrlog {
namespace {
char g_dir[MAX_PATH]{};
char g_path[MAX_PATH]{};
std::mutex g_mutex;
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
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "w") == 0 && f) {
        fprintf(f, "=== WarbandVR - session du %04u-%02u-%02u %02u:%02u:%02u ===\n", t.wYear, t.wMonth,
                t.wDay, t.wHour, t.wMinute, t.wSecond);
        fclose(f);
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
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "a") == 0 && f) {
        fprintf(f, "%02u:%02u:%02u.%03u %-8s %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tag, msg);
        fclose(f);
    }
}

} // namespace vrlog
