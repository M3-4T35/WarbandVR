// Menu d'ordres du moteur. mbTacticalWindow::updateOrdersMenu est appelé par frameMove tant
// que le panneau d'ordres est ouvert : il remplit les 8 zones de texte du panneau
// (m_orderLabelWidgets) selon le niveau du menu (m_orderType). On garde la fenêtre et on
// relit ces champs à la demande, ce qui donne les libellés exacts du jeu, quel que soit le
// module ou la langue.
#include "engine/orders.h"
#include "util/log.h"
#include "util/pattern.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)

namespace engine {
namespace {

// Prologue de mbTacticalWindow::updateOrdersMenu (cadre de pile de 0x3348 octets)
constexpr char kUpdateOrdersMenuSig[] = "40 55 53 56 57 41 56 41 57 48 8D AC 24 B8 CD FF FF B8 48 33 00 00 E8";

// Champs relevés dans le PDB
constexpr int kOrderLabelWidgets = 0x1C0; // mbTacticalWindow::m_orderLabelWidgets[8]
constexpr int kOrderType = 0x488;         // mbTacticalWindow::m_orderType (< -1 : panneau masqué)
constexpr int kNumOrderLabels = 0x4AC;    // mbTacticalWindow::m_numOrderLabels
constexpr int kWidgetText = 0x1E0;        // rglWidget::m_text (rglString)
constexpr int kStringBuffer = 0x08;       // rglString::m_buffer
constexpr int kStringLength = 0x14;       // rglString::m_strLength
constexpr ULONGLONG kStaleMs = 300;       // plus d'appel depuis : panneau fermé

using UpdateOrdersMenuFn = void(__fastcall*)(void*);
UpdateOrdersMenuFn g_original = nullptr;
void* g_target = nullptr;

std::atomic<uint8_t*> g_window{nullptr};
std::atomic<ULONGLONG> g_lastUpdate{0};

void __fastcall HkUpdateOrdersMenu(void* window)
{
    g_original(window);
    g_window = static_cast<uint8_t*>(window);
    g_lastUpdate = GetTickCount64();
}

// Copie protégée (la fenêtre appartient au jeu, lue depuis un autre thread)
bool ReadMenu(uint8_t* w, int& type, int& count, char (*labels)[128])
{
    __try {
        type = *reinterpret_cast<volatile int*>(w + kOrderType);
        count = *reinterpret_cast<volatile int*>(w + kNumOrderLabels);
        if (count < 0 || count > 8)
            return false;
        for (int i = 0; i < count; ++i) {
            labels[i][0] = 0;
            auto* widget = *reinterpret_cast<uint8_t**>(w + kOrderLabelWidgets + i * 8);
            if (!widget)
                continue;
            const char* text = *reinterpret_cast<const char**>(widget + kWidgetText + kStringBuffer);
            int len = *reinterpret_cast<int*>(widget + kWidgetText + kStringLength);
            if (!text || len <= 0)
                continue;
            if (len > 127)
                len = 127;
            memcpy(labels[i], text, len);
            labels[i][len] = 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool InstallOrdersHook()
{
    void* target = pattern::Find("mbTacticalWindow::updateOrdersMenu", kUpdateOrdersMenuSig);
    if (!target) {
        LOG_ENGINE("menu radial des ordres : libelles du jeu indisponibles (signature manquante)");
        return false;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    st = MH_CreateHook(target, &HkUpdateOrdersMenu, reinterpret_cast<void**>(&g_original));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    if (st != MH_OK) {
        LOG_ENGINE("ERREUR : detour de updateOrdersMenu impossible (%d)", static_cast<int>(st));
        return false;
    }
    g_target = target;
    LOG_ENGINE("mbTacticalWindow::updateOrdersMenu detourne (libelles du menu d'ordres)");
    return true;
}

void RemoveOrdersHook()
{
    if (!g_target)
        return;
    MH_DisableHook(g_target);
    MH_RemoveHook(g_target);
    g_target = nullptr;
}

OrderMenu GetOrderMenu()
{
    OrderMenu m;
    uint8_t* w = g_window;
    if (!w || GetTickCount64() - g_lastUpdate > kStaleMs)
        return m;
    char labels[8][128];
    int type = -2, count = 0;
    if (!ReadMenu(w, type, count, labels) || type < -1)
        return m;
    m.open = count > 0;
    m.type = type;
    m.count = count;
    for (int i = 0; i < count; ++i)
        m.labels[i] = labels[i];
    return m;
}

} // namespace engine
