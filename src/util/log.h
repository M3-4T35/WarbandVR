#pragma once
#include <windows.h>

namespace vrlog {

// Dossier des logs et captures, à côté de la DLL (créé au besoin)
constexpr char kDirName[] = "warbandVRlog";

// Ouvre warbandVRlog\warband_vr.log (vidé à chaque session)
void Init(HMODULE self);

// Dossier des logs, terminé par '\' (vide avant Init)
const char* Dir();

// Écrit une ligne : Write("[render]", "Present hooké à %p", addr)
void Write(const char* tag, const char* fmt, ...);

} // namespace vrlog

#define LOG_CORE(...)   ::vrlog::Write("[core]", __VA_ARGS__)
#define LOG_RENDER(...) ::vrlog::Write("[render]", __VA_ARGS__)
