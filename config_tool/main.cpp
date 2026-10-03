// WarbandVR - outil de configuration (warband_vr_config.exe)
// Édite warband_vr.ini (à côté de l'outil et de la DLL). La liste des réglages vient de
// src/util/config.* (partagée avec la DLL). Anglais par défaut, français au choix ; la langue
// est mémorisée dans le fichier ([Tool] language).
#define WARBANDVR_WIDEN2(x) L##x
#define WARBANDVR_WIDEN(x) WARBANDVR_WIDEN2(x)
#define WARBANDVR_WVERSION WARBANDVR_WIDEN(WARBANDVR_VERSION)
#include "util/config.h"

#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using config::Id;
using config::Kind;
using config::Lang;

// Identifiants des contrôles
constexpr int kIdLanguage = 100, kIdSave = 101, kIdDefaults = 102, kIdClose = 103;
constexpr int kIdSettingBase = 1000; // + index du réglage

struct Row {
    HWND label = nullptr;
    HWND control = nullptr;
};

HINSTANCE g_instance;
HWND g_window, g_panel, g_help, g_status, g_language, g_save, g_defaults, g_close;
HFONT g_font, g_bold;
std::vector<Row> g_rows(config::kCount);
std::vector<HWND> g_groups;
std::string g_path;
Lang g_lang = Lang::English;
int g_dpi = 96;
int g_scroll = 0, g_contentHeight = 0;

int S(int v)
{
    return MulDiv(v, g_dpi, 96);
}

std::wstring W(const char* s)
{
    std::wstring w;
    for (; s && *s; ++s)
        w += static_cast<wchar_t>(static_cast<unsigned char>(*s));
    return w;
}

std::wstring FromAnsi(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string ToAnsi(const std::wstring& w)
{
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ControlText(HWND c)
{
    const int len = GetWindowTextLengthW(c);
    std::wstring w(len, L'\0');
    if (len)
        GetWindowTextW(c, w.data(), len + 1);
    return w;
}

// --- Modules du Steam Workshop ------------------------------------------------------------
// WSE2 les lance avec --module "<titre Steam> (Steam Workshop)" (comme dans son propre menu)
// et retrouve leur dossier auprès de Steam. Leurs dossiers locaux ne portent que l'identifiant
// (steamapps\workshop\content\48700\<id>) : les titres sont demandés à l'API web publique
// de Steam (GetPublishedFileDetails, sans clé), une fois par lancement de l'outil.
constexpr wchar_t kWorkshopSuffix[] = L" (Steam Workshop)";
std::map<std::wstring, std::wstring> g_workshopTitles; // identifiant -> titre
bool g_workshopFetched = false;

std::wstring Utf8ToWide(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Chaîne JSON qui suit "key": dans text à partir de pos (échappements simples, \uXXXX)
bool JsonString(const std::string& text, const char* key, size_t& pos, std::string& out)
{
    const std::string k = std::string("\"") + key + "\"";
    size_t p = text.find(k, pos);
    if (p == std::string::npos)
        return false;
    p = text.find('"', text.find(':', p + k.size()));
    if (p == std::string::npos)
        return false;
    out.clear();
    for (++p; p < text.size() && text[p] != '"'; ++p) {
        char c = text[p];
        if (c == '\\' && p + 1 < text.size()) {
            c = text[++p];
            if (c == 'u' && p + 4 < text.size()) {
                const unsigned cp = std::stoul(text.substr(p + 1, 4), nullptr, 16);
                p += 4;
                // UTF-16 (BMP) -> UTF-8
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                continue;
            }
            c = c == 'n' ? '\n' : c == 't' ? '\t' : c;
        }
        out += c;
    }
    pos = p;
    return true;
}

std::string HttpPost(const wchar_t* host, const wchar_t* path, const std::string& body)
{
    std::string response;
    HINTERNET session = WinHttpOpen(L"WarbandVR", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return response;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 6000);
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    const wchar_t* headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    if (request &&
        WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), const_cast<char*>(body.data()),
                           static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || !read)
                break;
            response.append(chunk.data(), read);
        }
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

// Identifiants des objets du Workshop installés (dossiers à côté du dossier du jeu)
std::vector<std::wstring> WorkshopIds(const std::wstring& gameDir)
{
    std::vector<std::wstring> ids;
    const std::wstring pattern = gameDir + L"\\..\\..\\workshop\\content\\48700\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return ids;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] >= L'0' && fd.cFileName[0] <= L'9')
            ids.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ids;
}

void FetchWorkshopTitles(const std::vector<std::wstring>& ids)
{
    g_workshopFetched = true;
    if (ids.empty())
        return;
    std::string body = "itemcount=" + std::to_string(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        body += "&publishedfileids%5B" + std::to_string(i) + "%5D=" + ToAnsi(ids[i]);
    const std::string json =
        HttpPost(L"api.steampowered.com", L"/ISteamRemoteStorage/GetPublishedFileDetails/v1/", body);
    size_t pos = 0;
    std::string id, title;
    while (JsonString(json, "publishedfileid", pos, id)) {
        size_t next = pos;
        std::string nextId;
        const size_t nextItem = JsonString(json, "publishedfileid", next, nextId) ? next : std::string::npos;
        size_t t = pos;
        if (JsonString(json, "title", t, title) && t < nextItem && !title.empty())
            g_workshopTitles[Utf8ToWide(id)] = Utf8ToWide(title);
    }
}

// Liste du réglage Module : dossiers de <dossier du jeu>\\Modules, puis modules du Workshop
void FillModules()
{
    HWND combo = g_rows[static_cast<int>(Id::Module)].control;
    const std::wstring current = ControlText(combo);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const std::wstring dir = ControlText(g_rows[static_cast<int>(Id::GameDir)].control) + L"\\Modules\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(dir.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    const std::wstring gameDir = ControlText(g_rows[static_cast<int>(Id::GameDir)].control);
    const std::vector<std::wstring> ids = WorkshopIds(gameDir);
    if (!g_workshopFetched)
        FetchWorkshopTitles(ids);
    for (const std::wstring& id : ids) {
        auto it = g_workshopTitles.find(id);
        if (it != g_workshopTitles.end())
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((it->second + kWorkshopSuffix).c_str()));
    }
    SetWindowTextW(combo, current.c_str());
}

const wchar_t* T(const wchar_t* en, const wchar_t* fr)
{
    return g_lang == Lang::French ? fr : en;
}

std::vector<std::wstring> Split(const char* choices)
{
    std::vector<std::wstring> out(1);
    for (const char* c = choices; c && *c; ++c) {
        if (*c == '|')
            out.emplace_back();
        else
            out.back() += static_cast<wchar_t>(*c);
    }
    return out;
}

std::wstring LabelText(int i)
{
    const config::Definition& d = config::Def(static_cast<Id>(i));
    std::wstring s = W(d.label(g_lang));
    if (d.kind == Kind::Float) {
        wchar_t range[64];
        swprintf_s(range, L"  [%g - %g]", d.minValue, d.maxValue);
        s += range;
    }
    return s;
}

// Contrôles <- valeurs courantes
void RefreshControls()
{
    for (int i = 0; i < config::kCount; ++i) {
        const config::Definition& d = config::Def(static_cast<Id>(i));
        HWND c = g_rows[i].control;
        if (d.kind == Kind::Bool) {
            SendMessageW(c, BM_SETCHECK, config::GetBool(static_cast<Id>(i)) ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (d.kind == Kind::Choice) {
            SendMessageW(c, CB_SETCURSEL, config::GetInt(static_cast<Id>(i)), 0);
        } else if (d.kind == Kind::Text) {
            SetWindowTextW(c, FromAnsi(config::GetText(static_cast<Id>(i))).c_str());
        } else {
            wchar_t buf[32];
            swprintf_s(buf, L"%g", config::Get(static_cast<Id>(i)));
            SetWindowTextW(c, buf);
        }
    }
}

// Textes selon la langue
void RefreshTexts()
{
    SetWindowTextW(g_window, T(L"WarbandVR " WARBANDVR_WVERSION L" - Settings", L"WarbandVR " WARBANDVR_WVERSION L" - Reglages"));
    SetWindowTextW(g_save, T(L"Save", L"Enregistrer"));
    SetWindowTextW(g_defaults, T(L"Defaults", L"Valeurs par defaut"));
    SetWindowTextW(g_close, T(L"Close", L"Fermer"));
    int group = -1;
    const char* lastSection = nullptr;
    for (int i = 0; i < config::kCount; ++i) {
        const config::Definition& d = config::Def(static_cast<Id>(i));
        if (!lastSection || strcmp(lastSection, d.section) != 0) {
            SetWindowTextW(g_groups[++group], W(d.group(g_lang)).c_str());
            lastSection = d.section;
        }
        SetWindowTextW(g_rows[i].label, LabelText(i).c_str());
        if (d.kind == Kind::Choice) {
            HWND c = g_rows[i].control;
            const LRESULT sel = SendMessageW(c, CB_GETCURSEL, 0, 0);
            SendMessageW(c, CB_RESETCONTENT, 0, 0);
            for (const std::wstring& item : Split(d.choices(g_lang)))
                SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
            SendMessageW(c, CB_SETCURSEL, sel, 0);
        }
    }
    SetWindowTextW(g_help, T(L"Select a setting to see its description.",
                             L"Selectionnez un reglage pour voir sa description."));
    SetWindowTextW(g_status, W(g_path.c_str()).c_str());
}

// Valeurs courantes <- contrôles. false si une valeur numérique est invalide.
bool ReadControls()
{
    for (int i = 0; i < config::kCount; ++i) {
        const config::Definition& d = config::Def(static_cast<Id>(i));
        HWND c = g_rows[i].control;
        if (d.kind == Kind::Bool) {
            config::Set(static_cast<Id>(i), SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1.0f : 0.0f);
        } else if (d.kind == Kind::Choice) {
            config::Set(static_cast<Id>(i), static_cast<float>(SendMessageW(c, CB_GETCURSEL, 0, 0)));
        } else if (d.kind == Kind::Text) {
            config::SetText(static_cast<Id>(i), ToAnsi(ControlText(c)));
        } else {
            wchar_t buf[32]{};
            GetWindowTextW(c, buf, 32);
            for (wchar_t& ch : buf)
                if (ch == L',')
                    ch = L'.'; // virgule décimale acceptée
            wchar_t* end = nullptr;
            const float v = wcstof(buf, &end);
            if (end == buf) {
                SetFocus(c);
                MessageBoxW(g_window, (std::wstring(T(L"Invalid number: ", L"Nombre invalide : ")) + LabelText(i)).c_str(),
                            L"WarbandVR", MB_ICONWARNING);
                return false;
            }
            config::Set(static_cast<Id>(i), v); // borné à [min, max]
        }
    }
    return true;
}

void Layout()
{
    RECT rc;
    GetClientRect(g_window, &rc);
    const int w = rc.right, h = rc.bottom;
    const int top = S(44), bottom = S(64);
    MoveWindow(g_language, S(12), S(10), S(140), S(200), TRUE);
    MoveWindow(g_save, w - S(12 + 3 * 130 + 2 * 8), S(8), S(130), S(28), TRUE);
    MoveWindow(g_defaults, w - S(12 + 2 * 130 + 8), S(8), S(130), S(28), TRUE);
    MoveWindow(g_close, w - S(12 + 130), S(8), S(130), S(28), TRUE);
    MoveWindow(g_panel, 0, top, w, h - top - bottom, TRUE);
    MoveWindow(g_help, S(12), h - bottom + S(6), w - S(24), S(34), TRUE);
    MoveWindow(g_status, S(12), h - S(22), w - S(24), S(18), TRUE);

    // Contenu du panneau : groupes et lignes, décalés par le défilement
    RECT pr;
    GetClientRect(g_panel, &pr);
    const int labelW = (pr.right - S(40)) * 62 / 100, controlX = S(20) + labelW, controlW = pr.right - controlX - S(20);
    int y = S(8) - g_scroll, group = -1;
    const char* lastSection = nullptr;
    for (int i = 0; i < config::kCount; ++i) {
        const config::Definition& d = config::Def(static_cast<Id>(i));
        if (!lastSection || strcmp(lastSection, d.section) != 0) {
            y += group >= 0 ? S(10) : 0;
            MoveWindow(g_groups[++group], S(12), y, pr.right - S(24), S(24), TRUE);
            y += S(30);
            lastSection = d.section;
        }
        MoveWindow(g_rows[i].label, S(20), y + S(4), labelW, S(22), TRUE);
        const int ch = d.kind == Kind::Choice || i == static_cast<int>(Id::Module) ? S(200) : S(24);
        MoveWindow(g_rows[i].control, controlX, y, d.kind == Kind::Float ? S(90) : controlW, ch, TRUE);
        y += S(32);
    }
    g_contentHeight = y + g_scroll + S(8);

    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, g_contentHeight, static_cast<UINT>(pr.bottom),
                  g_scroll};
    SetScrollInfo(g_panel, SB_VERT, &si, TRUE);
}

void Scroll(int to)
{
    RECT pr;
    GetClientRect(g_panel, &pr);
    const int maxScroll = g_contentHeight > pr.bottom ? g_contentHeight - pr.bottom : 0;
    to = to < 0 ? 0 : to > maxScroll ? maxScroll : to;
    if (to == g_scroll)
        return;
    g_scroll = to;
    Layout();
}

HWND Create(const wchar_t* cls, const wchar_t* text, DWORD style, HWND parent, int id, DWORD exStyle = 0)
{
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    return h;
}

void ShowHelp(int index)
{
    const config::Definition& d = config::Def(static_cast<Id>(index));
    wchar_t def[64];
    std::wstring textDefault;
    if (d.kind == Kind::Text) {
        textDefault = std::wstring(T(L"  (default: ", L"  (defaut : ")) + W(d.defaultText) + L")";
        def[0] = 0;
    } else if (d.kind == Kind::Bool)
        swprintf_s(def, T(L"  (default: %s)", L"  (defaut : %s)"),
                   d.defaultValue != 0 ? T(L"yes", L"oui") : T(L"no", L"non"));
    else if (d.kind == Kind::Choice)
        swprintf_s(def, T(L"  (default: %s)", L"  (defaut : %s)"),
                   Split(d.choices(g_lang))[static_cast<int>(d.defaultValue)].c_str());
    else
        swprintf_s(def, T(L"  (default: %g)", L"  (defaut : %g)"), d.defaultValue);
    SetWindowTextW(g_help, (W(d.help(g_lang)) + def + textDefault).c_str());
}

// Panneau défilant : relaie les commandes des contrôles à la fenêtre principale
LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = g_scroll;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= S(32); break;
        case SB_LINEDOWN: pos += S(32); break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        }
        Scroll(pos);
        return 0;
    }
    case WM_MOUSEWHEEL:
        Scroll(g_scroll - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * S(64));
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= kIdSettingBase && id < kIdSettingBase + config::kCount &&
            (code == EN_SETFOCUS || code == CBN_SETFOCUS || code == BN_SETFOCUS || code == BN_CLICKED))
            ShowHelp(id - kIdSettingBase);
        // Dossier du jeu modifié : liste des modules mise à jour
        if (id == kIdSettingBase + static_cast<int>(Id::GameDir) && code == EN_KILLFOCUS)
            FillModules();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        // Titres de groupe en gras, sur le fond de la fenêtre
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_panel)
            Layout();
        return 0;
    case WM_MOUSEWHEEL:
        SendMessageW(g_panel, WM_MOUSEWHEEL, wp, lp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdLanguage:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                g_lang = SendMessageW(g_language, CB_GETCURSEL, 0, 0) == 1 ? Lang::French : Lang::English;
                config::SaveLanguage(g_path, g_lang);
                RefreshTexts();
                Layout();
            }
            return 0;
        case kIdSave:
            if (ReadControls()) {
                config::Save(g_path);
                config::SaveLanguage(g_path, g_lang);
                RefreshControls(); // valeurs bornées affichées
                SetWindowTextW(g_help, T(L"Saved. The game applies the new settings within a second.",
                                         L"Enregistre. Le jeu applique les nouveaux reglages en une seconde."));
            }
            return 0;
        case kIdDefaults:
            config::ResetDefaults();
            RefreshControls();
            FillModules();
            SetWindowTextW(g_help, T(L"Default values restored (click Save to write them).",
                                     L"Valeurs par defaut restaurees (cliquez sur Enregistrer pour les ecrire)."));
            return 0;
        case kIdClose:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g_path = config::DefaultPath();
    if (!config::Load(g_path))
        config::Save(g_path); // fichier absent : créé avec les valeurs par défaut
    g_lang = config::LoadLanguage(g_path);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"WarbandVRConfig";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = PanelProc;
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"WarbandVRConfigPanel";
    RegisterClassExW(&wc);

    g_window = CreateWindowExW(0, L"WarbandVRConfig", L"WarbandVR", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, 10, 10, nullptr, nullptr, instance, nullptr);
    g_dpi = static_cast<int>(GetDpiForWindow(g_window));
    SetWindowPos(g_window, nullptr, 0, 0, S(820), S(760), SWP_NOMOVE | SWP_NOZORDER);

    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_BOLD;
    bold.lfHeight = bold.lfHeight * 5 / 4;
    g_bold = CreateFontIndirectW(&bold);

    g_language = Create(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, g_window, kIdLanguage);
    SendMessageW(g_language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(g_language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Français"));
    SendMessageW(g_language, CB_SETCURSEL, g_lang == Lang::French ? 1 : 0, 0);
    g_save = Create(L"BUTTON", L"", BS_DEFPUSHBUTTON | WS_TABSTOP, g_window, kIdSave);
    g_defaults = Create(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, g_window, kIdDefaults);
    g_close = Create(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, g_window, kIdClose);
    g_panel = CreateWindowExW(WS_EX_CLIENTEDGE, L"WarbandVRConfigPanel", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                              WS_CLIPCHILDREN, 0, 0, 10, 10, g_window, nullptr, instance, nullptr);
    g_help = Create(L"STATIC", L"", SS_LEFT, g_window, 0);
    g_status = Create(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, g_window, 0);

    const char* lastSection = nullptr;
    for (int i = 0; i < config::kCount; ++i) {
        const config::Definition& d = config::Def(static_cast<Id>(i));
        if (!lastSection || strcmp(lastSection, d.section) != 0) {
            HWND g = Create(L"STATIC", L"", SS_LEFT, g_panel, 0);
            SendMessageW(g, WM_SETFONT, reinterpret_cast<WPARAM>(g_bold), FALSE);
            g_groups.push_back(g);
            lastSection = d.section;
        }
        g_rows[i].label = Create(L"STATIC", L"", SS_LEFT, g_panel, 0);
        const int id = kIdSettingBase + i;
        if (d.kind == Kind::Bool)
            g_rows[i].control = Create(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, g_panel, id);
        else if (d.kind == Kind::Choice)
            g_rows[i].control = Create(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, g_panel, id);
        else if (i == static_cast<int>(Id::Module)) // liste modifiable : modules installés
            g_rows[i].control = Create(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT | WS_TABSTOP | WS_VSCROLL,
                                       g_panel, id);
        else
            g_rows[i].control = Create(L"EDIT", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP, g_panel, id, WS_EX_CLIENTEDGE);
    }

    RefreshTexts();
    RefreshControls();
    FillModules();
    Layout();
    ShowWindow(g_window, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_window, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DeleteObject(g_font);
    DeleteObject(g_bold);
    return 0;
}
