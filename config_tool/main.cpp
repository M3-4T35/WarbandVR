// WarbandVR - outil de configuration et lanceur (warband_vr_config.exe)
// Édite warband_vr.ini (à côté de l'outil et de la DLL) et les réglages graphiques de Warband
// (rgl_config.ini, game_settings.*). La liste des réglages VR vient de src/util/config.*
// (partagée avec la DLL). Anglais par défaut, français au choix ([Tool] language).
//
// Interface entièrement dessinée dans la fenêtre (GDI, double tampon) : aucune liste déroulante
// ni case native, dont les fenêtres surgissantes ne s'affichent pas toujours sous Wine/Proton
// (Steam Frame dans le casque). Gros textes, zoom réglable ([Tool] ui_zoom), listes ouvertes
// dans la fenêtre même. Seuls restent natifs le champ texte du dossier du jeu et les boîtes
// Enregistrer/Ouvrir de Windows.
//
// Préréglages : intégrés (PCVR, Steam Frame autonome) et de l'utilisateur
// (warband_vr_presets\*.ini, exportables et importables), réglages Warband compris.
// --launcher : lanceur ouvert par warband_vr_injector avant le jeu. Jouer enregistre et rend 0,
// Quitter (ou la fermeture) rend 1 : le jeu n'est pas lancé.
#define WARBANDVR_WIDEN2(x) L##x
#define WARBANDVR_WIDEN(x) WARBANDVR_WIDEN2(x)
#define WARBANDVR_WVERSION WARBANDVR_WIDEN(WARBANDVR_VERSION)
#include "game_settings.h"
#include "util/config.h"

#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using config::Id;
using config::Kind;
using config::Lang;

constexpr UINT kMsgWorkshop = WM_APP + 1; // titres du Workshop reçus (thread)
constexpr UINT_PTR kTimerStatus = 1;
constexpr int kIdEdit = 200;

// --- État ---------------------------------------------------------------------------------
HINSTANCE g_instance;
HWND g_window, g_edit;
std::string g_path;
Lang g_lang = Lang::English;
bool g_launcher = false;
int g_exitCode = 0;
int g_dpi = 96;
float g_zoom = 1.0f;
HFONT g_fontBody, g_fontBold, g_fontSmall, g_fontTitle, g_fontButton;
HBRUSH g_editBrush;

enum Tab { kTabLaunch, kTabDisplay, kTabControls, kTabCombat, kTabGame, kTabAdvanced, kTabCount };
int g_tab = kTabLaunch;
int g_scroll = 0, g_contentHeight = 0;
RECT g_content{};

std::vector<std::wstring> g_modules;
std::wstring g_help, g_status;
int g_hoverRow = -1;

// Préréglages de la liste : intégrés puis fichiers de l'utilisateur
struct Preset {
    int builtin = -1;   // index du préréglage intégré, -1 : fichier
    std::wstring name;  // nom affiché (fichier : sans .ini)
    std::wstring path;  // fichier
};
std::vector<Preset> g_presets;
std::wstring g_presetName; // dernier préréglage appliqué
bool g_presetDirty = false;

// Ligne de réglage : réglage VR (config) ou de Warband (game)
struct Row {
    bool game;
    int index;
};
std::vector<Row> g_rows; // lignes de l'onglet courant

// Liste ouverte dans la fenêtre (remplace les listes déroulantes)
enum OverlayKind { kOvPreset, kOvChoice };
struct Overlay {
    bool open = false;
    OverlayKind kind = kOvPreset;
    Row row{};
    std::wstring title;
    std::vector<std::wstring> items;
    int selected = -1;
    int scroll = 0;
    RECT panel{}, list{};
    int itemHeight = 0;
} g_overlay;

// Zones cliquables, reconstruites à chaque dessin ; la dernière ajoutée est au-dessus
enum ZoneType {
    kZNone,
    kZPlay, kZQuit, kZLanguage, kZZoomOut, kZZoomIn,
    kZPresetBox, kZPresetSave, kZPresetExport, kZPresetImport, kZPresetDelete,
    kZTab, kZRow, kZToggle, kZPrev, kZNext, kZValue, kZMinus, kZPlus, kZSlider,
    kZScrollTrack, kZOverlayBg, kZOverlayItem, kZOverlayClose,
};
struct Zone {
    RECT r;
    ZoneType type;
    int a; // onglet, ligne, élément
};
std::vector<Zone> g_zones;
Zone g_down{{}, kZNone, -1};
Zone g_hover{{}, kZNone, -1};

enum DragKind { kDragNone, kDragSlider, kDragContent, kDragThumb, kDragOverlay };
DragKind g_drag = kDragNone;
int g_dragRow = -1;
RECT g_dragRect{};
POINT g_dragStart{};
int g_dragStartScroll = 0;
bool g_dragMoved = false;

// --- Couleurs (thème sombre, lisible dans un casque) ---------------------------------------
constexpr COLORREF kBg = RGB(24, 26, 31), kPanel = RGB(33, 36, 43), kSidebar = RGB(20, 22, 26),
                   kHover = RGB(46, 51, 61), kBorder = RGB(62, 68, 80), kText = RGB(236, 238, 242),
                   kDim = RGB(160, 167, 178), kAccent = RGB(76, 139, 245), kPlay = RGB(46, 160, 67),
                   kWarn = RGB(230, 170, 60), kOff = RGB(80, 86, 98);

int S(float v)
{
    return static_cast<int>(std::lround(v * g_zoom * g_dpi / 96.0f));
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
            out.back() += static_cast<wchar_t>(static_cast<unsigned char>(*c));
    }
    return out;
}

void SetStatus(const std::wstring& text)
{
    g_status = text;
    SetTimer(g_window, kTimerStatus, 8000, nullptr);
    InvalidateRect(g_window, nullptr, FALSE);
}

// --- Modules du Steam Workshop ------------------------------------------------------------
// WSE2 les lance avec --module "<titre Steam> (Steam Workshop)" (comme dans son propre menu)
// et retrouve leur dossier auprès de Steam. Leurs dossiers locaux ne portent que l'identifiant
// (steamapps\workshop\content\48700\<id>) : les titres sont demandés à l'API web publique
// de Steam (GetPublishedFileDetails, sans clé), une fois par lancement de l'outil.
constexpr wchar_t kWorkshopSuffix[] = L" (Steam Workshop)";
std::map<std::wstring, std::wstring> g_workshopTitles; // identifiant -> titre
std::atomic<bool> g_workshopFetched{false}; // titres reçus (thread)

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
    if (ids.empty()) {
        g_workshopFetched = true;
        return;
    }
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
    g_workshopFetched = true;
}

// Liste des modules : dossiers de <dossier du jeu>\Modules, puis modules du Workshop
void FillModules()
{
    g_modules.clear();
    const std::wstring gameDir = FromAnsi(config::GetText(Id::GameDir));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((gameDir + L"\\Modules\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                g_modules.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (g_workshopFetched) {
        for (const std::wstring& id : WorkshopIds(gameDir)) {
            auto it = g_workshopTitles.find(id);
            if (it != g_workshopTitles.end())
                g_modules.push_back(it->second + kWorkshopSuffix);
        }
    }
    std::sort(g_modules.begin(), g_modules.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    // Module choisi absent de la liste (dossier du jeu changé...) : gardé en tête
    const std::wstring current = FromAnsi(config::GetText(Id::Module));
    if (!current.empty() && std::find(g_modules.begin(), g_modules.end(), current) == g_modules.end())
        g_modules.insert(g_modules.begin(), current);
}

// Outil posé dans le dossier du jeu (cas de Proton : le jeu n'y est pas dans le dossier Steam de
// Windows) : ce dossier est le dossier du jeu, comme pour warband_vr_injector
void DetectGameDir()
{
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    if (GetFileAttributesW((dir + L"\\mb_warband_wse2_x64.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        config::SetText(Id::GameDir, ToAnsi(dir));
}

// Titres du Workshop demandés à Steam sans bloquer la fenêtre ; liste mise à jour à la réponse
void StartWorkshopFetch()
{
    const std::wstring gameDir = FromAnsi(config::GetText(Id::GameDir));
    std::thread([gameDir] {
        FetchWorkshopTitles(WorkshopIds(gameDir));
        PostMessageW(g_window, kMsgWorkshop, 0, 0);
    }).detach();
}

// --- Lignes de réglages -------------------------------------------------------------------
enum class RowKind { Bool, Choice, Float, Int, Text, Module, Language };

// Ligne hors des réglages VR : langue du jeu (language.txt), onglet Lancement
constexpr int kRowGameLanguage = -2;

bool IsLanguage(const Row& r)
{
    return !r.game && r.index == kRowGameLanguage;
}

std::vector<std::string> g_languages; // codes des langues du jeu

std::wstring LanguageName(const std::string& code)
{
    static const std::map<std::string, const wchar_t*> kNames = {
        {"en", L"English"}, {"fr", L"Francais"}, {"de", L"Deutsch"}, {"es", L"Espanol"},
        {"pl", L"Polski"}, {"ru", L"Russkiy"}, {"cz", L"Cestina"}, {"hu", L"Magyar"},
        {"tr", L"Turkce"}, {"cns", L"Chinese (simplified)"}, {"cnt", L"Chinese (traditional)"}};
    const auto it = kNames.find(code);
    return (it != kNames.end() ? std::wstring(it->second) + L" (" : std::wstring(L"(")) + FromAnsi(code) + L")";
}

const char* TabSection(int tab)
{
    switch (tab) {
    case kTabLaunch: return "Launch";
    case kTabDisplay: return "Display";
    case kTabControls: return "Controls";
    case kTabCombat: return "Combat";
    case kTabAdvanced: return "Advanced";
    default: return "";
    }
}

const wchar_t* TabName(int tab)
{
    switch (tab) {
    case kTabLaunch: return T(L"Launch", L"Lancement");
    case kTabDisplay: return T(L"Display", L"Affichage");
    case kTabControls: return T(L"Controls", L"Controles");
    case kTabCombat: return T(L"Combat", L"Combat");
    case kTabGame: return T(L"Game graphics", L"Graphismes");
    default: return T(L"Advanced", L"Avance");
    }
}

void BuildRows()
{
    g_rows.clear();
    if (g_tab == kTabGame) {
        if (game::Loaded())
            for (int i = 0; i < game::Count(); ++i)
                g_rows.push_back({true, i});
        return;
    }
    for (int i = 0; i < config::kCount; ++i) {
        if (strcmp(config::Def(static_cast<Id>(i)).section, TabSection(g_tab)) == 0)
            g_rows.push_back({false, i});
        if (i == static_cast<int>(Id::Module) && g_tab == kTabLaunch)
            g_rows.push_back({false, kRowGameLanguage});
    }
}

RowKind KindOf(const Row& r)
{
    if (r.game) {
        switch (game::Get(r.index).kind) {
        case game::Kind::Bool: return RowKind::Bool;
        case game::Kind::Choice: return RowKind::Choice;
        case game::Kind::Int: return RowKind::Int;
        default: return RowKind::Float;
        }
    }
    if (IsLanguage(r))
        return RowKind::Language;
    if (r.index == static_cast<int>(Id::Module))
        return RowKind::Module;
    switch (config::Def(static_cast<Id>(r.index)).kind) {
    case Kind::Bool: return RowKind::Bool;
    case Kind::Choice: return RowKind::Choice;
    case Kind::Text: return RowKind::Text;
    default: return RowKind::Float;
    }
}

std::wstring Label(const Row& r)
{
    if (IsLanguage(r))
        return T(L"Game language", L"Langue du jeu");
    return r.game ? W(game::Get(r.index).label(g_lang)) : W(config::Def(static_cast<Id>(r.index)).label(g_lang));
}

float Value(const Row& r)
{
    if (IsLanguage(r))
        return 0;
    return r.game ? game::Value(r.index) : config::Get(static_cast<Id>(r.index));
}

float MinOf(const Row& r)
{
    if (IsLanguage(r))
        return 0;
    return r.game ? game::Get(r.index).minValue : config::Def(static_cast<Id>(r.index)).minValue;
}

float MaxOf(const Row& r)
{
    if (IsLanguage(r))
        return 1;
    return r.game ? game::Get(r.index).maxValue : config::Def(static_cast<Id>(r.index)).maxValue;
}

std::vector<std::wstring> Choices(const Row& r)
{
    if (KindOf(r) == RowKind::Module)
        return g_modules;
    if (IsLanguage(r)) {
        std::vector<std::wstring> names;
        for (const std::string& code : g_languages)
            names.push_back(LanguageName(code));
        return names;
    }
    return Split(r.game ? game::Get(r.index).choices(g_lang) : config::Def(static_cast<Id>(r.index)).choices(g_lang));
}

int ChoiceIndex(const Row& r)
{
    if (KindOf(r) == RowKind::Module) {
        const std::wstring cur = FromAnsi(config::GetText(Id::Module));
        auto it = std::find(g_modules.begin(), g_modules.end(), cur);
        return it == g_modules.end() ? -1 : static_cast<int>(it - g_modules.begin());
    }
    if (IsLanguage(r)) {
        const std::string cur = game::Language().empty() ? "en" : game::Language();
        auto it = std::find(g_languages.begin(), g_languages.end(), cur);
        return it == g_languages.end() ? -1 : static_cast<int>(it - g_languages.begin());
    }
    return r.game ? game::ChoiceIndex(r.index) : config::GetInt(static_cast<Id>(r.index));
}

// Réglage modifié : le préréglage courant ne correspond plus (sauf réglages du lancement)
void Changed(const Row& r)
{
    if (r.game || (r.index >= 0 && config::InPreset(static_cast<Id>(r.index))))
        g_presetDirty = true;
    InvalidateRect(g_window, nullptr, FALSE);
}

void SetChoice(const Row& r, int k)
{
    const int n = static_cast<int>(Choices(r).size());
    if (n == 0)
        return;
    k = (k % n + n) % n;
    if (KindOf(r) == RowKind::Module)
        config::SetText(Id::Module, ToAnsi(g_modules[k]));
    else if (IsLanguage(r))
        game::SetLanguage(g_languages[k]);
    else if (r.game)
        game::SetChoiceIndex(r.index, k);
    else
        config::Set(static_cast<Id>(r.index), static_cast<float>(k));
    Changed(r);
}

void SetValue(const Row& r, float v)
{
    if (r.game)
        game::SetValue(r.index, v);
    else
        config::Set(static_cast<Id>(r.index), v);
    Changed(r);
}

// Pas « rond » proche de x (1, 2 ou 5 × 10^n)
float NiceStep(float x)
{
    if (x <= 0)
        return 1;
    const float p = std::pow(10.0f, std::floor(std::log10(x)));
    const float m = x / p;
    return (m < 1.5f ? 1 : m < 3.5f ? 2 : m < 7.5f ? 5 : 10) * p;
}

float Step(const Row& r)
{
    const float s = NiceStep((MaxOf(r) - MinOf(r)) / 20);
    return KindOf(r) == RowKind::Int ? std::max(1.0f, s) : s;
}

float FineStep(const Row& r)
{
    const float s = NiceStep((MaxOf(r) - MinOf(r)) / 200);
    return KindOf(r) == RowKind::Int ? 1.0f : s;
}

float Snap(const Row& r, float v)
{
    const float f = FineStep(r);
    v = std::round(v / f) * f;
    return std::clamp(v, MinOf(r), MaxOf(r));
}

std::wstring FormatValue(const Row& r)
{
    const float v = Value(r);
    wchar_t buf[64];
    if (r.game && strcmp(game::Get(r.index).key, "fBattleSize") == 0) {
        swprintf_s(buf, L"%d", game::BattleTroops(v));
        return buf;
    }
    const float f = FineStep(r);
    const int decimals = std::max(0, static_cast<int>(-std::floor(std::log10(f) + 1e-4f)));
    swprintf_s(buf, L"%.*f", decimals, v);
    return buf;
}

std::wstring HelpOf(const Row& r)
{
    if (IsLanguage(r))
        return T(L"Language of the game's texts (menus, dialogs), for every module that provides it. Applied at the next launch.",
                 L"Langue des textes du jeu (menus, dialogues), pour chaque module qui la propose. Appliquee au prochain lancement.");
    if (r.game)
        return W(game::Get(r.index).help(g_lang));
    const config::Definition& d = config::Def(static_cast<Id>(r.index));
    std::wstring def;
    wchar_t buf[96];
    if (d.kind == Kind::Text)
        def = std::wstring(T(L"default: ", L"defaut : ")) + W(d.defaultText);
    else if (d.kind == Kind::Bool)
        def = std::wstring(T(L"default: ", L"defaut : ")) + (d.defaultValue != 0 ? T(L"yes", L"oui") : T(L"no", L"non"));
    else if (d.kind == Kind::Choice)
        def = std::wstring(T(L"default: ", L"defaut : ")) + Split(d.choices(g_lang))[static_cast<int>(d.defaultValue)];
    else {
        swprintf_s(buf, T(L"default: %g, from %g to %g", L"defaut : %g, de %g a %g"), d.defaultValue, d.minValue,
                   d.maxValue);
        def = buf;
    }
    return W(d.help(g_lang)) + L"  (" + def + L")";
}

// --- Préréglages ---------------------------------------------------------------------------
// Graphismes de Warband des préréglages intégrés : niveau du jeu puis écarts
struct GameOverride {
    const char* key;
    float value;
};
void ApplyBuiltinGame(int builtin)
{
    if (builtin == 0) {
        // PCVR : Ultra (PC puissant), sans le flou de profondeur de champ (fait pour un écran plat :
        // dans le casque, il floute ce qu'on regarde)
        game::ApplyLevel(4);
        if (const int i = game::Find("bDepthEffects"); i >= 0)
            game::SetValue(i, 0);
    } else if (builtin == 1) {
        // Steam Frame autonome : Bas (validé au casque, 72 paires/s), mais sang au maximum
        game::ApplyLevel(1);
        for (const GameOverride& o : {GameOverride{"bBlood", 1}, GameOverride{"iBloodStains", 2}})
            if (const int i = game::Find(o.key); i >= 0)
                game::SetValue(i, o.value);
    }
}

std::wstring PresetDirW()
{
    return FromAnsi(config::PresetDir(g_path));
}

void FillPresets()
{
    g_presets.clear();
    for (int i = 0; i < config::BuiltinPresetCount(); ++i)
        g_presets.push_back({i, W(config::BuiltinPresetName(i)(g_lang)), L""});
    std::vector<Preset> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((PresetDirW() + L"\\*.ini").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                const std::wstring name = fd.cFileName;
                files.push_back({-1, name.substr(0, name.size() - 4), PresetDirW() + L"\\" + name});
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end(),
              [](const Preset& a, const Preset& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    g_presets.insert(g_presets.end(), files.begin(), files.end());
}

// Préréglage courant, si c'est un fichier de l'utilisateur
const Preset* CurrentUserPreset()
{
    for (const Preset& p : g_presets)
        if (p.builtin < 0 && _wcsicmp(p.name.c_str(), g_presetName.c_str()) == 0)
            return &p;
    return nullptr;
}

bool IsCurrent(const Preset& p)
{
    if (p.builtin >= 0)
        return g_presetName == W(config::BuiltinPresetName(p.builtin).en) ||
               g_presetName == W(config::BuiltinPresetName(p.builtin).fr);
    return _wcsicmp(p.name.c_str(), g_presetName.c_str()) == 0;
}

std::wstring PresetDisplayName()
{
    for (int i = 0; i < config::BuiltinPresetCount(); ++i)
        if (g_presetName == W(config::BuiltinPresetName(i).en) || g_presetName == W(config::BuiltinPresetName(i).fr))
            return W(config::BuiltinPresetName(i)(g_lang));
    return g_presetName.empty() ? std::wstring(T(L"(none)", L"(aucun)")) : g_presetName;
}

void ApplyPreset(const Preset& p)
{
    if (p.builtin >= 0) {
        config::ApplyBuiltinPreset(p.builtin);
        ApplyBuiltinGame(p.builtin);
        g_presetName = W(config::BuiltinPresetName(p.builtin).en);
    } else {
        if (!config::LoadPreset(ToAnsi(p.path))) {
            SetStatus(std::wstring(T(L"Cannot read ", L"Lecture impossible : ")) + p.path);
            return;
        }
        game::ReadFromPreset(p.path);
        g_presetName = p.name;
    }
    g_presetDirty = false;
    SetStatus(std::wstring(T(L"Preset applied: ", L"Prereglage applique : ")) + PresetDisplayName() +
              (g_launcher ? T(L". Click PLAY to start with it.", L". Cliquez sur JOUER pour lancer avec.")
                          : T(L". Click Save to write it.", L". Cliquez sur Enregistrer pour l'ecrire.")));
}

std::wstring AskSavePath(bool inPresetDir)
{
    wchar_t file[MAX_PATH] = L"";
    if (CurrentUserPreset())
        wcsncpy_s(file, g_presetName.c_str(), _TRUNCATE);
    const std::wstring dir = PresetDirW();
    CreateDirectoryW(dir.c_str(), nullptr);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_window;
    ofn.lpstrFilter = L"WarbandVR preset (*.ini)\0*.ini\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = inPresetDir ? dir.c_str() : nullptr;
    ofn.lpstrDefExt = L"ini";
    ofn.lpstrTitle = inPresetDir ? T(L"Save preset", L"Enregistrer le prereglage")
                                 : T(L"Export preset", L"Exporter le prereglage");
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) ? file : L"";
}

void SavePresetAs(bool inPresetDir)
{
    const std::wstring path = AskSavePath(inPresetDir);
    if (path.empty())
        return;
    if (!config::SavePreset(ToAnsi(path))) {
        SetStatus(std::wstring(T(L"Cannot write ", L"Ecriture impossible : ")) + path);
        return;
    }
    game::WriteToPreset(path);
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    name = name.substr(0, name.find_last_of(L'.'));
    // Enregistré dans le dossier des préréglages : il devient le préréglage courant
    if (_wcsnicmp(path.c_str(), PresetDirW().c_str(), PresetDirW().size()) == 0) {
        g_presetName = name;
        g_presetDirty = false;
    }
    FillPresets();
    SetStatus(std::wstring(T(L"Preset written: ", L"Prereglage ecrit : ")) + path);
}

void ImportPreset()
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_window;
    ofn.lpstrFilter = L"WarbandVR preset (*.ini)\0*.ini\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = T(L"Import preset", L"Importer un prereglage");
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return;
    const std::wstring src = file;
    const std::wstring name = src.substr(src.find_last_of(L"\\/") + 1);
    const std::wstring dst = PresetDirW() + L"\\" + name;
    CreateDirectoryW(PresetDirW().c_str(), nullptr);
    if (_wcsicmp(src.c_str(), dst.c_str()) != 0) {
        if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES &&
            MessageBoxW(g_window,
                        (std::wstring(T(L"Replace the existing preset ", L"Remplacer le prereglage existant ")) + name +
                         L" ?").c_str(),
                        L"WarbandVR", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            SetStatus(std::wstring(T(L"Cannot copy to ", L"Copie impossible vers ")) + dst);
            return;
        }
    }
    FillPresets();
    const std::wstring stem = name.substr(0, name.find_last_of(L'.'));
    for (const Preset& p : g_presets)
        if (p.builtin < 0 && _wcsicmp(p.name.c_str(), stem.c_str()) == 0) {
            ApplyPreset(p);
            break;
        }
}

void DeletePreset()
{
    const Preset* p = CurrentUserPreset();
    if (!p)
        return;
    if (MessageBoxW(g_window, (std::wstring(T(L"Delete the preset ", L"Supprimer le prereglage ")) + p->name + L" ?").c_str(),
                    L"WarbandVR", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    DeleteFileW(p->path.c_str());
    g_presetName.clear();
    FillPresets();
    SetStatus(T(L"Preset deleted.", L"Prereglage supprime."));
}

// Enregistre réglages VR, langue, zoom, préréglage courant et réglages Warband modifiés
bool SaveAll()
{
    config::Save(g_path);
    config::SaveLanguage(g_path, g_lang);
    config::SavePresetName(g_path, ToAnsi(g_presetName));
    wchar_t zoom[16];
    swprintf_s(zoom, L"%g", g_zoom);
    WritePrivateProfileStringW(L"Tool", L"ui_zoom", zoom, FromAnsi(g_path).c_str());
    if (!game::SaveLanguage()) {
        SetStatus(T(L"Cannot write the game language (language.txt).", L"Ecriture impossible de la langue du jeu (language.txt)."));
        return false;
    }
    if (!game::Save()) {
        SetStatus(std::wstring(T(L"Cannot write the Warband settings: ", L"Ecriture impossible des reglages Warband : ")) +
                  game::Path());
        return false;
    }
    return true;
}

// --- Polices -------------------------------------------------------------------------------
HFONT MakeFont(int px, int weight)
{
    return CreateFontW(-S(static_cast<float>(px)), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void CreateFonts()
{
    for (HFONT f : {g_fontBody, g_fontBold, g_fontSmall, g_fontTitle, g_fontButton})
        if (f)
            DeleteObject(f);
    g_fontBody = MakeFont(20, FW_NORMAL);
    g_fontBold = MakeFont(20, FW_SEMIBOLD);
    g_fontSmall = MakeFont(16, FW_NORMAL);
    g_fontTitle = MakeFont(26, FW_BOLD);
    g_fontButton = MakeFont(19, FW_SEMIBOLD);
    if (g_edit)
        SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);
}

// --- Dessin --------------------------------------------------------------------------------
void Fill(HDC dc, const RECT& r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void Round(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void Text(HDC dc, RECT r, const std::wstring& s, HFONT font, COLORREF color, UINT flags)
{
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags | DT_NOPREFIX);
}

// Triangle plein dans r : dir -1 gauche, 1 droite, 2 bas (pas de caractère : polices de Wine)
void Triangle(HDC dc, const RECT& r, int dir, COLORREF c)
{
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, s = S(7);
    POINT pts[3];
    if (dir == -1)
        pts[0] = {cx - s, cy}, pts[1] = {cx + s / 2 + 1, cy - s}, pts[2] = {cx + s / 2 + 1, cy + s};
    else if (dir == 1)
        pts[0] = {cx + s, cy}, pts[1] = {cx - s / 2 - 1, cy - s}, pts[2] = {cx - s / 2 - 1, cy + s};
    else
        pts[0] = {cx, cy + s * 2 / 3}, pts[1] = {cx - s, cy - s / 2}, pts[2] = {cx + s, cy - s / 2};
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, pts, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

// Signe − / + dessiné (barres), comme les triangles
void Sign(HDC dc, const RECT& r, bool plus, COLORREF c)
{
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, l = S(9), t = std::max(2, S(3)) / 2;
    Fill(dc, RECT{cx - l, cy - t, cx + l, cy + t + 1}, c);
    if (plus)
        Fill(dc, RECT{cx - t, cy - l, cx + t + 1, cy + l}, c);
}

bool Hovered(ZoneType t, int a = -1)
{
    return g_hover.type == t && (a < 0 || g_hover.a == a);
}

void AddZone(RECT r, ZoneType t, int a = -1, const RECT* clip = nullptr)
{
    if (clip) {
        RECT c;
        if (!IntersectRect(&c, &r, clip))
            return;
        r = c;
    }
    g_zones.push_back({r, t, a});
}

enum ButtonStyle { kNormal, kPrimary, kGo, kDisabled };

COLORREF Lighter(COLORREF c)
{
    return RGB(std::min(255, GetRValue(c) + 25), std::min(255, GetGValue(c) + 25), std::min(255, GetBValue(c) + 25));
}

void Button(HDC dc, const RECT& r, const std::wstring& label, ZoneType t, ButtonStyle style = kNormal)
{
    const bool hover = Hovered(t) && style != kDisabled;
    COLORREF fill = style == kPrimary ? kAccent : style == kGo ? kPlay : kPanel;
    if (hover)
        fill = style == kNormal ? kHover : Lighter(fill);
    Round(dc, r, S(10), fill, style == kNormal || style == kDisabled ? kBorder : fill);
    Text(dc, r, label, g_fontButton, style == kDisabled ? kOff : kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (style != kDisabled)
        AddZone(r, t);
}

RECT g_editRect{}; // position voulue du champ texte (dossier du jeu), vide : masqué

void DrawRow(HDC dc, int i, RECT rr)
{
    const Row& row = g_rows[i];
    const RowKind kind = KindOf(row);
    const bool hover = g_hoverRow == i;
    Round(dc, rr, S(12), hover ? kHover : kPanel, hover ? kBorder : kPanel);
    AddZone(rr, kZRow, i, &g_content);

    const int pad = S(16), btn = S(46), h = S(44);
    const int width = rr.right - rr.left;
    const int ctrlW = std::clamp(width * 48 / 100, S(300), S(560));
    RECT ctrl{rr.right - pad - ctrlW, (rr.top + rr.bottom - h) / 2, rr.right - pad, (rr.top + rr.bottom + h) / 2};
    // Libellé centré verticalement, sur deux lignes s'il le faut
    {
        RECT label{rr.left + pad, rr.top + S(4), ctrl.left - pad, rr.bottom - S(4)};
        const std::wstring l = Label(row);
        RECT probe = label;
        SelectObject(dc, g_fontBold);
        DrawTextW(dc, l.c_str(), static_cast<int>(l.size()), &probe, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        const int th = std::min(probe.bottom - probe.top, label.bottom - label.top);
        label.top += (label.bottom - label.top - th) / 2;
        Text(dc, label, l, g_fontBold, kText, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

    switch (kind) {
    case RowKind::Bool: {
        const bool on = Value(row) != 0;
        RECT sw{ctrl.right - S(92), ctrl.top + S(4), ctrl.right, ctrl.bottom - S(4)};
        Round(dc, sw, sw.bottom - sw.top, on ? kAccent : kOff, on ? kAccent : kOff);
        const int d = sw.bottom - sw.top - S(8);
        RECT knob{on ? sw.right - S(4) - d : sw.left + S(4), sw.top + S(4), 0, 0};
        knob.right = knob.left + d;
        knob.bottom = knob.top + d;
        Round(dc, knob, d, kText, kText);
        RECT txt{ctrl.left, ctrl.top, sw.left - S(14), ctrl.bottom};
        Text(dc, txt, on ? T(L"Yes", L"Oui") : T(L"No", L"Non"), g_fontBody, on ? kText : kDim,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        AddZone(RECT{sw.left - S(80), ctrl.top, ctrl.right, ctrl.bottom}, kZToggle, i, &g_content);
        break;
    }
    case RowKind::Choice:
    case RowKind::Module:
    case RowKind::Language: {
        RECT prev{ctrl.left, ctrl.top, ctrl.left + btn, ctrl.bottom};
        RECT next{ctrl.right - btn, ctrl.top, ctrl.right, ctrl.bottom};
        RECT value{prev.right + S(6), ctrl.top, next.left - S(6), ctrl.bottom};
        Round(dc, prev, S(10), Hovered(kZPrev, i) ? kHover : kBg, kBorder);
        Round(dc, next, S(10), Hovered(kZNext, i) ? kHover : kBg, kBorder);
        Triangle(dc, prev, -1, kText);
        Triangle(dc, next, 1, kText);
        Round(dc, value, S(10), Hovered(kZValue, i) ? kHover : kBg, kBorder);
        const std::vector<std::wstring> items = Choices(row);
        const int k = ChoiceIndex(row);
        std::wstring shown = k >= 0 && k < static_cast<int>(items.size()) ? items[k] : std::wstring();
        if (kind == RowKind::Module && k < 0)
            shown = FromAnsi(config::GetText(Id::Module));
        Text(dc, RECT{value.left + S(12), value.top, value.right - S(30), value.bottom}, shown, g_fontBody, kText,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Triangle(dc, RECT{value.right - S(30), value.top, value.right - S(6), value.bottom}, 2, kDim);
        AddZone(prev, kZPrev, i, &g_content);
        AddZone(next, kZNext, i, &g_content);
        AddZone(value, kZValue, i, &g_content);
        break;
    }
    case RowKind::Float:
    case RowKind::Int: {
        const int valueW = S(96);
        RECT minus{ctrl.left, ctrl.top, ctrl.left + btn, ctrl.bottom};
        RECT valueBox{ctrl.right - valueW, ctrl.top, ctrl.right, ctrl.bottom};
        RECT plus{valueBox.left - S(8) - btn, ctrl.top, valueBox.left - S(8), ctrl.bottom};
        RECT slider{minus.right + S(14), ctrl.top, plus.left - S(14), ctrl.bottom};
        Round(dc, minus, S(10), Hovered(kZMinus, i) ? kHover : kBg, kBorder);
        Round(dc, plus, S(10), Hovered(kZPlus, i) ? kHover : kBg, kBorder);
        Sign(dc, minus, false, kText);
        Sign(dc, plus, true, kText);
        const float t = (Value(row) - MinOf(row)) / std::max(1e-6f, MaxOf(row) - MinOf(row));
        const int cy = (slider.top + slider.bottom) / 2, knob = S(26);
        Round(dc, RECT{slider.left, cy - S(4), slider.right, cy + S(4)}, S(8), kOff, kOff);
        const int kx = slider.left + knob / 2 + static_cast<int>(t * (slider.right - slider.left - knob));
        Round(dc, RECT{slider.left, cy - S(4), kx, cy + S(4)}, S(8), kAccent, kAccent);
        Round(dc, RECT{kx - knob / 2, cy - knob / 2, kx + knob / 2, cy + knob / 2}, knob,
              Hovered(kZSlider, i) ? kText : kDim, kAccent);
        Round(dc, valueBox, S(10), kBg, kBorder);
        Text(dc, valueBox, FormatValue(row), g_fontBody, kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddZone(minus, kZMinus, i, &g_content);
        AddZone(plus, kZPlus, i, &g_content);
        AddZone(RECT{slider.left - S(8), ctrl.top, slider.right + S(8), ctrl.bottom}, kZSlider, i, &g_content);
        break;
    }
    case RowKind::Text: {
        Round(dc, ctrl, S(10), kBg, kBorder);
        // Champ natif posé dessus s'il est entièrement visible
        RECT inner{ctrl.left + S(10), ctrl.top + S(9), ctrl.right - S(10), ctrl.bottom - S(6)};
        if (inner.top >= g_content.top && inner.bottom <= g_content.bottom)
            g_editRect = inner;
        break;
    }
    }
}

void Paint(HDC dc, const RECT& client)
{
    g_zones.clear();
    g_editRect = {};
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, client, kBg);
    const int w = client.right, h = client.bottom, pad = S(16);

    // En-tête
    const int headerH = S(68);
    Fill(dc, RECT{0, 0, w, headerH}, kSidebar);
    int x = w - pad;
    const int bh = S(46), by = (headerH - bh) / 2;
    auto placeRight = [&](int bw) {
        RECT r{x - bw, by, x, by + bh};
        x -= bw + S(10);
        return r;
    };
    if (g_launcher) {
        Button(dc, placeRight(S(170)), T(L"PLAY", L"JOUER"), kZPlay, kGo);
        Button(dc, placeRight(S(130)), T(L"Quit", L"Quitter"), kZQuit);
    } else {
        Button(dc, placeRight(S(170)), T(L"Save", L"Enregistrer"), kZPlay, kPrimary);
        Button(dc, placeRight(S(130)), T(L"Close", L"Fermer"), kZQuit);
    }
    x -= S(16);
    Button(dc, placeRight(S(56)), L"+", kZZoomIn);
    Button(dc, placeRight(S(56)), L"-", kZZoomOut);
    Button(dc, placeRight(S(120)), g_lang == Lang::French ? L"English" : L"Francais", kZLanguage);
    Text(dc, RECT{pad, 0, x, headerH},
         std::wstring(L"WarbandVR ") + WARBANDVR_WVERSION + L"  -  " +
             (g_launcher ? T(L"Launch", L"Lancement") : T(L"Settings", L"Reglages")),
         g_fontTitle, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Ligne des préréglages
    const int presetTop = headerH + S(10), presetH = S(52);
    RECT pl{pad, presetTop, pad + S(140), presetTop + presetH};
    Text(dc, pl, T(L"Preset", L"Prereglage"), g_fontBold, kDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT box{pl.right, presetTop + S(4), pl.right + S(380), presetTop + presetH - S(4)};
    Round(dc, box, S(10), Hovered(kZPresetBox) ? kHover : kPanel, kBorder);
    std::wstring presetText = PresetDisplayName();
    if (g_presetDirty)
        presetText += T(L"  (modified)", L"  (modifie)");
    Text(dc, RECT{box.left + S(14), box.top, box.right - S(34), box.bottom}, presetText, g_fontBody, kText,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Triangle(dc, RECT{box.right - S(34), box.top, box.right - S(8), box.bottom}, 2, kDim);
    AddZone(box, kZPresetBox);
    x = box.right + S(12);
    auto placeLeft = [&](int bw) {
        RECT r{x, box.top, x + bw, box.bottom};
        x += bw + S(10);
        return r;
    };
    Button(dc, placeLeft(S(150)), T(L"Save as...", L"Enregistrer..."), kZPresetSave);
    Button(dc, placeLeft(S(130)), T(L"Export...", L"Exporter..."), kZPresetExport);
    Button(dc, placeLeft(S(130)), T(L"Import...", L"Importer..."), kZPresetImport);
    Button(dc, placeLeft(S(130)), T(L"Delete", L"Supprimer"), kZPresetDelete, CurrentUserPreset() ? kNormal : kDisabled);

    // Pied : aide de la ligne survolée, message
    const int footerH = S(104);
    const int bodyTop = presetTop + presetH + S(10), bodyBottom = h - footerH;
    Fill(dc, RECT{0, bodyBottom, w, h}, kSidebar);
    std::wstring help = g_help;
    if (help.empty())
        help = g_launcher ? T(L"Choose the module and the settings, then click PLAY. Point at a setting to see what it does.",
                              L"Choisissez le module et les reglages, puis cliquez sur JOUER. Pointez un reglage pour voir son role.")
                          : T(L"Point at a setting to see what it does. Save writes the settings (the game applies the VR ones at once).",
                              L"Pointez un reglage pour voir son role. Enregistrer ecrit les reglages (le jeu applique ceux de la VR aussitot).");
    Text(dc, RECT{pad, bodyBottom + S(8), w - pad, bodyBottom + S(66)}, help, g_fontBody, kDim, DT_LEFT | DT_WORDBREAK);
    Text(dc, RECT{pad, h - S(34), w - pad, h - S(6)}, g_status.empty() ? FromAnsi(g_path) : g_status, g_fontSmall,
         g_status.empty() ? kOff : kWarn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);

    // Onglets
    const int sideW = S(250);
    Fill(dc, RECT{0, bodyTop, sideW, bodyBottom}, kSidebar);
    int ty = bodyTop + S(6);
    for (int t = 0; t < kTabCount; ++t) {
        RECT tr{S(8), ty, sideW - S(8), ty + S(54)};
        const bool sel = t == g_tab;
        if (sel || Hovered(kZTab, t))
            Round(dc, tr, S(10), sel ? kPanel : kHover, sel ? kPanel : kHover);
        if (sel)
            Fill(dc, RECT{tr.left, tr.top + S(10), tr.left + S(5), tr.bottom - S(10)}, kAccent);
        Text(dc, RECT{tr.left + S(18), tr.top, tr.right - S(8), tr.bottom}, TabName(t), sel ? g_fontBold : g_fontBody,
             sel ? kText : kDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddZone(tr, kZTab, t);
        ty += S(60);
    }

    // Contenu défilant
    g_content = {sideW + pad, bodyTop, w - pad, bodyBottom};
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, g_content.left, g_content.top, g_content.right, g_content.bottom);
    int y = g_content.top + S(4) - g_scroll;
    const int rowH = S(74), scrollW = S(14);
    const int rowRight = g_content.right - scrollW - S(10);
    if (g_tab == kTabGame) {
        std::wstring note;
        COLORREF color = kDim;
        if (!game::Loaded()) {
            note = std::wstring(T(L"Warband settings file not found (start the game once): ",
                                  L"Fichier des reglages de Warband introuvable (lancez le jeu une fois) : ")) +
                   game::Path();
            color = kWarn;
        } else if (game::GameRunning()) {
            note = T(L"The game is running: it rewrites these settings when it quits. Close it before saving; they apply at the next launch.",
                     L"Le jeu est ouvert : il reecrit ces reglages en quittant. Fermez-le avant d'enregistrer ; ils s'appliquent au prochain lancement.");
            color = kWarn;
        } else {
            note = T(L"Game graphics settings (rgl_config.ini), applied at the next launch. In VR each image is drawn twice: lower settings give a smoother image.",
                     L"Reglages graphiques du jeu (rgl_config.ini), appliques au prochain lancement. En VR chaque image est dessinee deux fois : des reglages plus bas donnent une image plus fluide.");
        }
        RECT nr{g_content.left, y, rowRight, y + S(82)};
        Round(dc, nr, S(12), kSidebar, color == kWarn ? kWarn : kBorder);
        Text(dc, RECT{nr.left + S(16), nr.top + S(10), nr.right - S(16), nr.bottom - S(8)}, note, g_fontBody, color,
             DT_LEFT | DT_WORDBREAK);
        y += S(94);
    }
    for (int i = 0; i < static_cast<int>(g_rows.size()); ++i) {
        RECT rr{g_content.left, y, rowRight, y + rowH};
        if (rr.bottom > g_content.top && rr.top < g_content.bottom)
            DrawRow(dc, i, rr);
        y += rowH + S(8);
    }
    g_contentHeight = y + g_scroll - g_content.top + S(8);
    RestoreDC(dc, saved);

    // Barre de défilement
    const int visible = g_content.bottom - g_content.top;
    if (g_contentHeight > visible) {
        RECT track{g_content.right - scrollW, g_content.top, g_content.right, g_content.bottom};
        Round(dc, track, scrollW, kSidebar, kSidebar);
        const int thumbH = std::max(S(40), visible * visible / g_contentHeight);
        const int thumbY = track.top + (visible - thumbH) * g_scroll / std::max(1, g_contentHeight - visible);
        Round(dc, RECT{track.left, thumbY, track.right, thumbY + thumbH}, scrollW, Hovered(kZScrollTrack) ? kDim : kOff,
              kOff);
        AddZone(track, kZScrollTrack);
    }

    // Liste ouverte : par-dessus tout
    if (g_overlay.open) {
        Overlay& o = g_overlay;
        AddZone(client, kZOverlayBg);
        o.itemHeight = S(54);
        const int titleH = S(60);
        const int pw = std::min(S(620), w - 2 * pad);
        const int listH = std::max(o.itemHeight, std::min(static_cast<int>(o.items.size()) * o.itemHeight,
                                                          h - S(160) - titleH));
        const int ph = titleH + listH + S(16);
        o.panel = {(w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2};
        Round(dc, RECT{o.panel.left - S(3), o.panel.top - S(3), o.panel.right + S(3), o.panel.bottom + S(3)}, S(16), kBg,
              kAccent);
        Round(dc, o.panel, S(14), kPanel, kPanel);
        AddZone(o.panel, kZNone);
        Text(dc, RECT{o.panel.left + S(18), o.panel.top, o.panel.right - S(70), o.panel.top + titleH}, o.title, g_fontBold,
             kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Button(dc, RECT{o.panel.right - S(62), o.panel.top + S(8), o.panel.right - S(10), o.panel.top + titleH - S(8)},
               L"X", kZOverlayClose);
        o.list = {o.panel.left + S(8), o.panel.top + titleH, o.panel.right - S(8), o.panel.top + titleH + listH};
        const int maxScroll = std::max(0, static_cast<int>(o.items.size()) * o.itemHeight - listH);
        o.scroll = std::clamp(o.scroll, 0, maxScroll);
        const int s2 = SaveDC(dc);
        IntersectClipRect(dc, o.list.left, o.list.top, o.list.right, o.list.bottom);
        for (int k = 0; k < static_cast<int>(o.items.size()); ++k) {
            RECT ir{o.list.left, o.list.top + k * o.itemHeight - o.scroll, o.list.right - S(10),
                    o.list.top + (k + 1) * o.itemHeight - o.scroll};
            if (ir.bottom < o.list.top || ir.top > o.list.bottom)
                continue;
            const bool sel = k == o.selected;
            if (sel || Hovered(kZOverlayItem, k))
                Round(dc, RECT{ir.left, ir.top + S(3), ir.right, ir.bottom - S(3)}, S(10), sel ? kAccent : kHover,
                      sel ? kAccent : kHover);
            Text(dc, RECT{ir.left + S(16), ir.top, ir.right - S(8), ir.bottom}, o.items[k], g_fontBody, kText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            AddZone(ir, kZOverlayItem, k, &o.list);
        }
        RestoreDC(dc, s2);
        if (o.items.empty())
            Text(dc, o.list, T(L"(empty)", L"(vide)"), g_fontBody, kDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (maxScroll > 0) { // indicateur de défilement
            const int th = std::max(S(30), listH * listH / (listH + maxScroll));
            const int tyy = o.list.top + (listH - th) * o.scroll / maxScroll;
            Round(dc, RECT{o.list.right - S(8), tyy, o.list.right - S(2), tyy + th}, S(6), kDim, kDim);
        }
    }
}

// Défilement du contenu, borné
void ScrollTo(int v)
{
    const int visible = g_content.bottom - g_content.top;
    g_scroll = std::clamp(v, 0, std::max(0, g_contentHeight - visible));
    InvalidateRect(g_window, nullptr, FALSE);
}

void SelectTab(int t)
{
    g_tab = t;
    g_scroll = 0;
    g_hoverRow = -1;
    g_help.clear();
    BuildRows();
    InvalidateRect(g_window, nullptr, FALSE);
}

const Zone* HitTest(POINT p)
{
    for (auto it = g_zones.rbegin(); it != g_zones.rend(); ++it)
        if (PtInRect(&it->r, p))
            return &*it;
    return nullptr;
}

void OpenOverlay(OverlayKind kind, const Row& row)
{
    Overlay& o = g_overlay;
    o = Overlay{};
    o.open = true;
    o.kind = kind;
    o.row = row;
    if (kind == kOvPreset) {
        FillPresets();
        o.title = T(L"Choose a preset (applied at once)", L"Choisir un prereglage (applique aussitot)");
        for (const Preset& p : g_presets) {
            o.items.push_back(p.builtin >= 0 ? p.name : p.name + T(L"  (yours)", L"  (perso)"));
            if (IsCurrent(p))
                o.selected = static_cast<int>(o.items.size()) - 1;
        }
    } else {
        o.title = Label(row);
        o.items = Choices(row);
        o.selected = ChoiceIndex(row);
    }
    o.scroll = std::max(0, o.selected - 3) * S(54); // élément choisi visible
    InvalidateRect(g_window, nullptr, FALSE);
}

void ChooseOverlayItem(int k)
{
    Overlay& o = g_overlay;
    o.open = false;
    if (k >= 0 && k < static_cast<int>(o.items.size())) {
        if (o.kind == kOvPreset)
            ApplyPreset(g_presets[k]);
        else
            SetChoice(o.row, k);
    }
    InvalidateRect(g_window, nullptr, FALSE);
}

void SetSliderFromX(int x)
{
    if (g_dragRow < 0 || g_dragRow >= static_cast<int>(g_rows.size()))
        return;
    const Row& row = g_rows[g_dragRow];
    const int knob = S(26);
    const RECT& r = g_dragRect; // zone du curseur, élargie de S(8) de chaque côté
    const float t = std::clamp(static_cast<float>(x - r.left - S(8) - knob / 2) /
                                   static_cast<float>(std::max(1, static_cast<int>(r.right - r.left) - S(16) - knob)),
                               0.0f, 1.0f);
    SetValue(row, Snap(row, MinOf(row) + t * (MaxOf(row) - MinOf(row))));
}

void Click(const Zone& z)
{
    const Row* row = z.a >= 0 && z.a < static_cast<int>(g_rows.size()) ? &g_rows[z.a] : nullptr;
    switch (z.type) {
    case kZPlay:
        if (SaveAll()) {
            if (g_launcher) {
                g_exitCode = 0;
                DestroyWindow(g_window);
                return;
            }
            SetStatus(game::GameRunning()
                          ? T(L"Saved. The game is running: it will overwrite the Warband graphics settings when it quits.",
                              L"Enregistre. Le jeu est ouvert : il ecrasera les reglages graphiques Warband en quittant.")
                          : T(L"Saved. The game applies the VR settings within a second.",
                              L"Enregistre. Le jeu applique les reglages VR en une seconde."));
        }
        break;
    case kZQuit:
        DestroyWindow(g_window);
        break;
    case kZLanguage:
        g_lang = g_lang == Lang::French ? Lang::English : Lang::French;
        config::SaveLanguage(g_path, g_lang);
        g_help.clear();
        g_hoverRow = -1;
        FillPresets();
        InvalidateRect(g_window, nullptr, FALSE);
        break;
    case kZZoomOut:
    case kZZoomIn:
        g_zoom = std::clamp(g_zoom + (z.type == kZZoomIn ? 0.125f : -0.125f), 0.75f, 2.5f);
        CreateFonts();
        ScrollTo(g_scroll);
        break;
    case kZPresetBox:
        OpenOverlay(kOvPreset, {});
        break;
    case kZPresetSave:
        SavePresetAs(true);
        break;
    case kZPresetExport:
        SavePresetAs(false);
        break;
    case kZPresetImport:
        ImportPreset();
        break;
    case kZPresetDelete:
        DeletePreset();
        break;
    case kZTab:
        SelectTab(z.a);
        break;
    case kZToggle:
        if (row)
            SetValue(*row, Value(*row) != 0 ? 0.0f : 1.0f);
        break;
    case kZPrev:
    case kZNext:
        if (row) {
            const int k = ChoiceIndex(*row);
            SetChoice(*row, (k < 0 ? 0 : k) + (z.type == kZNext ? 1 : -1));
        }
        break;
    case kZValue:
        if (row)
            OpenOverlay(kOvChoice, *row);
        break;
    case kZMinus:
    case kZPlus:
        if (row)
            SetValue(*row, Snap(*row, Value(*row) + (z.type == kZPlus ? Step(*row) : -Step(*row))));
        break;
    case kZOverlayItem:
        ChooseOverlayItem(z.a);
        break;
    case kZOverlayBg:
    case kZOverlayClose:
        g_overlay.open = false;
        InvalidateRect(g_window, nullptr, FALSE);
        break;
    default:
        break;
    }
}

void OnMouseMove(POINT p)
{
    if (g_drag == kDragSlider) {
        SetSliderFromX(p.x);
        return;
    }
    if (g_drag == kDragContent || g_drag == kDragOverlay) {
        const int dy = p.y - g_dragStart.y;
        if (std::abs(dy) > S(8))
            g_dragMoved = true;
        if (g_dragMoved) {
            if (g_drag == kDragContent) {
                ScrollTo(g_dragStartScroll - dy);
            } else {
                g_overlay.scroll = g_dragStartScroll - dy;
                InvalidateRect(g_window, nullptr, FALSE);
            }
        }
        return;
    }
    if (g_drag == kDragThumb) {
        const int visible = g_content.bottom - g_content.top;
        ScrollTo(g_dragStartScroll + (p.y - g_dragStart.y) * g_contentHeight / std::max(1, visible));
        return;
    }
    const Zone* z = HitTest(p);
    const Zone hover = z ? *z : Zone{{}, kZNone, -1};
    int row = -1;
    if (!g_overlay.open && z && z->type >= kZRow && z->type <= kZSlider)
        row = z->a;
    if (hover.type != g_hover.type || hover.a != g_hover.a || row != g_hoverRow) {
        g_hover = hover;
        if (row != g_hoverRow) {
            g_hoverRow = row;
            if (row >= 0 && row < static_cast<int>(g_rows.size()))
                g_help = HelpOf(g_rows[row]);
        }
        InvalidateRect(g_window, nullptr, FALSE);
    }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g_window, 0};
    TrackMouseEvent(&tme);
}

void OnMouseDown(POINT p)
{
    SetFocus(g_window);
    SetCapture(g_window);
    const Zone* z = HitTest(p);
    g_down = z ? *z : Zone{{}, kZNone, -1};
    g_dragStart = p;
    g_dragMoved = false;
    if (g_overlay.open) {
        if (PtInRect(&g_overlay.list, p)) {
            g_drag = kDragOverlay; // glisser la liste (manette, écran tactile)
            g_dragStartScroll = g_overlay.scroll;
        }
        return;
    }
    if (z && z->type == kZSlider) {
        g_drag = kDragSlider;
        g_dragRow = z->a;
        g_dragRect = z->r;
        SetSliderFromX(p.x);
    } else if (z && z->type == kZScrollTrack) {
        g_drag = kDragThumb;
        // Clic hors du curseur : saut à cet endroit
        const int visible = g_content.bottom - g_content.top;
        const int thumbH = std::max(S(40), visible * visible / std::max(1, g_contentHeight));
        const int thumbY = g_content.top + (visible - thumbH) * g_scroll / std::max(1, g_contentHeight - visible);
        if (p.y < thumbY || p.y > thumbY + thumbH)
            ScrollTo((p.y - g_content.top - thumbH / 2) * g_contentHeight / std::max(1, visible));
        g_dragStartScroll = g_scroll;
    } else if (PtInRect(&g_content, p) && (!z || z->type == kZRow)) {
        g_drag = kDragContent; // glisser le contenu (manette, écran tactile)
        g_dragStartScroll = g_scroll;
    }
}

void OnMouseUp(POINT p)
{
    ReleaseCapture();
    const DragKind drag = g_drag;
    g_drag = kDragNone;
    if (drag == kDragSlider || drag == kDragThumb || g_dragMoved)
        return;
    const Zone* z = HitTest(p);
    if (z && z->type == g_down.type && z->a == g_down.a)
        Click(*z);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        // Champ texte natif : là où le dessin l'a placé, masqué sinon (ou sous une liste ouverte)
        const bool show = !IsRectEmpty(&g_editRect) && !g_overlay.open;
        RECT cur{};
        GetWindowRect(g_edit, &cur);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&cur), 2);
        if (show != (IsWindowVisible(g_edit) != FALSE) || (show && !EqualRect(&cur, &g_editRect)))
            SetWindowPos(g_edit, HWND_TOP, g_editRect.left, g_editRect.top, g_editRect.right - g_editRect.left,
                         g_editRect.bottom - g_editRect.top, (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW) | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        ScrollTo(g_scroll);
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove({static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        return 0;
    case WM_MOUSELEAVE:
        if (g_hover.type != kZNone) {
            g_hover = Zone{{}, kZNone, -1};
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        OnMouseDown({static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        return 0;
    case WM_LBUTTONUP:
        OnMouseUp({static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp) * S(80) / WHEEL_DELTA;
        if (g_overlay.open) {
            g_overlay.scroll -= delta;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            ScrollTo(g_scroll - delta);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_overlay.open) {
            g_overlay.open = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_RETURN && g_launcher && !g_overlay.open) {
            Click(Zone{{}, kZPlay, -1});
        } else if (wp == VK_PRIOR || wp == VK_NEXT) {
            ScrollTo(g_scroll + (wp == VK_NEXT ? 1 : -1) * (g_content.bottom - g_content.top) * 3 / 4);
        }
        return 0;
    case WM_COMMAND:
        // Windows envoie EN_CHANGE dès la création du champ, avant que g_edit soit affecté : le
        // lire alors effacerait le dossier du jeu (liste des modules vide)
        if (LOWORD(wp) == kIdEdit && HIWORD(wp) == EN_CHANGE && g_edit) {
            const HWND edit = reinterpret_cast<HWND>(lp);
            const int len = GetWindowTextLengthW(edit);
            std::wstring text(len, L'\0');
            if (len)
                GetWindowTextW(edit, text.data(), len + 1);
            config::SetText(Id::GameDir, ToAnsi(text)); // réglage du lancement : hors préréglage
        } else if (LOWORD(wp) == kIdEdit && HIWORD(wp) == EN_KILLFOCUS) {
            FillModules(); // dossier du jeu changé : modules mis à jour
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText);
        SetBkColor(dc, kBg);
        return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_TIMER:
        if (wp == kTimerStatus) {
            KillTimer(hwnd, kTimerStatus);
            g_status.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case kMsgWorkshop:
        FillModules();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        CreateFonts();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int show)
{
    g_instance = instance;
    g_launcher = cmdLine && wcsstr(cmdLine, L"--launcher");
    g_exitCode = g_launcher ? 1 : 0; // lanceur fermé sans Jouer : pas de partie
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g_path = config::DefaultPath();
    if (!config::Load(g_path))
        config::Save(g_path); // fichier absent : créé avec les valeurs par défaut
    g_lang = config::LoadLanguage(g_path);
    g_presetName = FromAnsi(config::LoadPresetName(g_path));
    {
        wchar_t zoom[16]{};
        GetPrivateProfileStringW(L"Tool", L"ui_zoom", L"1", zoom, 16, FromAnsi(g_path).c_str());
        g_zoom = std::clamp(wcstof(zoom, nullptr), 0.75f, 2.5f);
    }
    DetectGameDir();
    game::Load();
    g_languages = game::Languages(FromAnsi(config::GetText(Id::GameDir)));
    FillPresets();
    FillModules();
    BuildRows();
    g_editBrush = CreateSolidBrush(kBg);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"WarbandVRConfig";
    RegisterClassExW(&wc);

    g_window = CreateWindowExW(0, L"WarbandVRConfig", g_launcher ? L"WarbandVR - Launch" : L"WarbandVR - Settings",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 10, 10, nullptr,
                               nullptr, instance, nullptr);
    g_dpi = static_cast<int>(GetDpiForWindow(g_window));
    CreateFonts();
    g_edit = CreateWindowExW(0, L"EDIT", FromAnsi(config::GetText(Id::GameDir)).c_str(),
                             WS_CHILD | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 10, 10, g_window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEdit)), instance, nullptr);
    SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);

    // Taille : 1240 x 900 (mise à l'échelle), bornée à l'écran ; écran plus petit : plein écran
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int ww = S(1240), wh = S(900);
    const int aw = work.right - work.left, ah = work.bottom - work.top;
    const bool fits = ww <= aw && wh <= ah;
    SetWindowPos(g_window, nullptr, work.left + std::max(0, (aw - ww) / 2), work.top + std::max(0, (ah - wh) / 2),
                 std::min(ww, aw), std::min(wh, ah), SWP_NOZORDER);
    ShowWindow(g_window, fits ? show : SW_SHOWMAXIMIZED);
    UpdateWindow(g_window);
    if (g_launcher)
        SetForegroundWindow(g_window);
    StartWorkshopFetch();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    for (HFONT f : {g_fontBody, g_fontBold, g_fontSmall, g_fontTitle, g_fontButton})
        DeleteObject(f);
    DeleteObject(g_editBrush);
    return g_exitCode;
}
