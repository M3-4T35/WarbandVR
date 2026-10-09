#include "game_settings.h"

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace game {
namespace {

constexpr float kSame = -1e9f; // niveau qui ne change pas ce réglage
constexpr float kHdrHigh = -2e9f; // Haut / Ultra : 1 si bDisableHighHDR, sinon 2 (comme le jeu)

struct Entry {
    Setting s;
    float levels[4]; // Bas, Moyen, Haut, Ultra
};

// Libellés sans accents, comme ceux de src/util/config.cpp
const Entry kEntries[] = {
    {{"Graphics", "iPreset", {"Overall quality (game preset)", "Qualite globale (prereglage du jeu)"},
      {"Same as the presets of the game's video options: sets all the settings below.",
       "Comme les prereglages des options video du jeu : regle tous les reglages ci-dessous."},
      Kind::Choice, 0, 0, {"Custom|Low|Medium|High|Ultra", "Personnalise|Bas|Moyen|Haut|Ultra"}, nullptr},
     {kSame, kSame, kSame, kSame}},
    {{"Graphics", "iShaderQuality", {"Shader quality", "Qualite des shaders"},
      {"Lighting and material effects. Heavy for the graphics card.",
       "Effets d'eclairage et de matieres. Lourd pour la carte graphique."},
      Kind::Choice, 0, 0, {"Low|Medium|High|Ultra", "Bas|Moyen|Haut|Ultra"}, nullptr},
     {0, 1, 2, 3}},
    {{"Graphics", "iTextureQuality", {"Texture quality", "Qualite des textures"},
      {"Texture resolution. Uses video memory.", "Resolution des textures. Utilise la memoire video."},
      Kind::Choice, 0, 0, {"Low|Medium|High", "Bas|Moyen|Haut"}, "1|2|3"},
     {1, 2, 3, 3}},
    {{"Graphics", "iAnisotropicFiltering", {"Anisotropic filtering", "Filtrage anisotrope"},
      {"Sharpness of textures seen at a grazing angle (ground, walls).",
       "Nettete des textures vues en biais (sol, murs)."},
      Kind::Choice, 0, 0, {"Off|4x|8x|16x", "Non|4x|8x|16x"}, "0|4|8|16"},
     {0, 4, 8, 16}},
    {{"ShadowMap", "bShadowMap", {"Shadows", "Ombres"},
      {"Shadows of the scene (buildings, trees, terrain).", "Ombres de la scene (batiments, arbres, terrain)."},
      Kind::Bool, 0, 1, {nullptr, nullptr}, nullptr},
     {0, 1, 1, 1}},
    {{"ShadowMap", "iShadowMapTextureSize", {"Shadow resolution", "Resolution des ombres"},
      {"Sharper shadows, heavier for the graphics card.", "Ombres plus nettes, plus lourdes pour la carte graphique."},
      Kind::Choice, 0, 0, {"1024|2048|4096", "1024|2048|4096"}, "1024|2048|4096"},
     {1024, 2048, 4096, 4096}},
    {{"FakeCharacterShadows", "bFakeCharacterShadows", {"Character shadows", "Ombres des personnages"},
      {"Shadows under soldiers and horses.", "Ombres sous les soldats et les chevaux."},
      Kind::Bool, 0, 1, {nullptr, nullptr}, nullptr},
     {0, 1, 1, 1}},
    {{"FakeCharacterShadows", "fFakeCharacterShadowDetail", {"Character shadow detail", "Detail des ombres des personnages"},
      {"Distance up to which characters get a shadow.", "Distance jusqu'a laquelle les personnages ont une ombre."},
      Kind::Float, 0.1f, 1, {nullptr, nullptr}, nullptr},
     {0.1f, 0.7f, 1, 1}},
    {{"Flora", "iFloraShadows", {"Vegetation shadows", "Ombres de la vegetation"},
      {"Shadows of trees and bushes.", "Ombres des arbres et buissons."},
      Kind::Choice, 0, 0, {"Off|Low|High", "Non|Bas|Haut"}, nullptr},
     {0, 1, 2, 2}},
    {{"Flora", "fGrassDensity", {"Grass density", "Densite de l'herbe"},
      {"Amount of grass. Many small objects: costly in VR.", "Quantite d'herbe. Beaucoup de petits objets : couteux en VR."},
      Kind::Float, 0, 1, {nullptr, nullptr}, nullptr},
     {0, 0.5f, 1, 1}},
    {{"Flora", "iFloraLod", {"Vegetation detail", "Detail de la vegetation"},
      {"Level of detail of trees and plants.", "Niveau de detail des arbres et des plantes."},
      Kind::Choice, 0, 0, {"Low|Medium|High|Ultra", "Bas|Moyen|Haut|Ultra"}, "3|2|1|0"},
     {3, 2, 1, 0}},
    {{"Flora", "fFloraDegradeDistance", {"Vegetation distance", "Distance de la vegetation"},
      {"Distance up to which vegetation is drawn in full (meters).",
       "Distance jusqu'a laquelle la vegetation est dessinee entierement (metres)."},
      Kind::Float, 75, 150, {nullptr, nullptr}, nullptr},
     {75, 100, 125, 150}},
    {{"Scene", "fCharacterDetail", {"Character detail", "Detail des personnages"},
      {"Level of detail of soldiers with distance.", "Niveau de detail des soldats selon la distance."},
      Kind::Float, 0.1f, 1, {nullptr, nullptr}, nullptr},
     {0.1f, 0.7f, 1, 1}},
    {{"PostFx", "iHighDynamicRange", {"HDR", "HDR"},
      {"High dynamic range lighting and glow. In true stereo, computed for each eye.",
       "Eclairage a grande dynamique et halo. En vraie stereo, calcule pour chaque oeil."},
      Kind::Choice, 0, 0, {"Off|On|High", "Non|Oui|Haut"}, nullptr},
     {0, 1, kHdrHigh, kHdrHigh}},
    {{"PostFx", "bAutoExposure", {"Auto exposure", "Exposition automatique"},
      {"Brightness adapts to the scene (with HDR).", "La luminosite s'adapte a la scene (avec le HDR)."},
      Kind::Bool, 0, 1, {nullptr, nullptr}, nullptr},
     {0, 0, 1, 1}},
    {{"PostFx", "bDepthEffects", {"Depth effects", "Effets de profondeur"},
      {"Depth of field blur. Not recommended in VR.", "Flou de profondeur de champ. Deconseille en VR."},
      Kind::Bool, 0, 1, {nullptr, nullptr}, nullptr},
     {0, 0, 0, 1}},
    {{"PostFx", "iFxaa", {"Antialiasing (FXAA)", "Anticrenelage (FXAA)"},
      {"Smooths edges. Slightly blurs the image.", "Adoucit les contours. Floute un peu l'image."},
      Kind::Choice, 0, 0, {"Off|Low|High", "Non|Bas|Haut"}, nullptr},
     {0, 0, 1, 2}},
    {{"Reflections", "iReflectionsMode", {"Water reflections", "Reflets de l'eau"},
      {"Reflections on water. Each reflection is an extra render of the scene.",
       "Reflets sur l'eau. Chaque reflet est un rendu supplementaire de la scene."},
      Kind::Choice, 0, 0, {"Off|Low|High", "Non|Bas|Haut"}, nullptr},
     {0, 1, 2, 2}},
    {{"Reflections", "iReflectionsTextureSize", {"Reflection resolution", "Resolution des reflets"},
      {"Sharpness of water reflections.", "Nettete des reflets sur l'eau."},
      Kind::Choice, 0, 0, {"128|256|512", "128|256|512"}, "128|256|512"},
     {128, 128, 256, 512}},
    {{"ParticleSystems", "bParticleSystems", {"Particles", "Particules"},
      {"Smoke, dust, fire.", "Fumee, poussiere, feu."},
      Kind::Bool, 0, 1, {nullptr, nullptr}, nullptr},
     {0, 1, 1, 1}},
    {{"ParticleSystems", "fParticleSystemDegradeDistance", {"Particle distance", "Distance des particules"},
      {"Distance up to which particles are drawn (meters).", "Distance jusqu'a laquelle les particules sont dessinees (metres)."},
      Kind::Float, 10, 60, {nullptr, nullptr}, nullptr},
     {10, 15, 40, 60}},
    {{"Graphics", "bDynamicLighting", {"Dynamic lighting", "Eclairage dynamique"},
      {"Lights from torches and fires.", "Lumieres des torches et des feux."},
      Kind::Bool, 0, 1, {nullptr, nullptr}, nullptr},
     {0, 0, 1, 1}},
    {{"Scene", "bSunFlare", {"Sun flare", "Halo du soleil"},
      {"Lens flare when looking at the sun.", "Reflet d'objectif en regardant le soleil."},
      Kind::Bool, 0, 1, {nullptr, nullptr}, nullptr},
     {0, 0, 1, 1}},
    {{"Battle", "iMaxNumCorpses", {"Corpses", "Cadavres"},
      {"Maximum number of bodies left on the battlefield.", "Nombre maximal de corps laisses sur le champ de bataille."},
      Kind::Int, 0, 100, {nullptr, nullptr}, nullptr},
     {0, 20, 50, 100}},
    {{"Battle", "iMaxNumRagdolls", {"Ragdolls", "Ragdolls"},
      {"Bodies falling with physics. Uses the processor.", "Corps qui tombent avec la physique. Utilise le processeur."},
      Kind::Int, 0, 50, {nullptr, nullptr}, nullptr},
     {0, 5, 10, 50}},
    {{"Battle", "bBlood", {"Blood", "Sang"},
      {"Blood effects when hitting. Not changed by the overall quality.",
       "Effets de sang aux coups. Pas modifie par la qualite globale."},
      Kind::Bool, 0, 1, {nullptr, nullptr}, nullptr},
     {kSame, kSame, kSame, kSame}},
    {{"Battle", "iBloodStains", {"Blood stains", "Taches de sang"},
      {"Blood on the ground and on characters.", "Sang au sol et sur les personnages."},
      Kind::Choice, 0, 0, {"Off|Low|High", "Non|Bas|Haut"}, nullptr},
     {0, 1, 2, 2}},
    {{"Battle", "iWeaponTrails", {"Weapon trails", "Trainees des armes"},
      {"Trail behind swung weapons.", "Trainee derriere les armes en mouvement."},
      Kind::Choice, 0, 0, {"Off|Low|High", "Non|Bas|Haut"}, nullptr},
     {0, 0, 2, 2}},
    {{"Battle", "fBattleSize", {"Battle size (troops)", "Taille des batailles (troupes)"},
      {"Soldiers on the field at the same time. Main processor load in battle.",
       "Soldats presents en meme temps sur le terrain. Principale charge du processeur en bataille."},
      Kind::Float, 0, 1, {nullptr, nullptr}, nullptr},
     {kSame, kSame, kSame, kSame}},
};
constexpr int kCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

std::wstring g_path;
bool g_loaded = false;
float g_values[kCount];
float g_original[kCount];
bool g_disableHighHdr = false;
int g_battleMin = 30, g_battleMax = 150;

std::wstring Wide(const char* s)
{
    std::wstring w;
    for (; s && *s; ++s)
        w += static_cast<wchar_t>(static_cast<unsigned char>(*s));
    return w;
}

std::vector<float> ChoiceValues(int i)
{
    std::vector<float> out;
    const Setting& s = kEntries[i].s;
    if (s.values) {
        std::string cur;
        for (const char* c = s.values;; ++c) {
            if (*c == '|' || !*c) {
                out.push_back(std::strtof(cur.c_str(), nullptr));
                cur.clear();
                if (!*c)
                    break;
            } else {
                cur += *c;
            }
        }
    } else {
        int n = 1;
        for (const char* c = s.choices.en; c && *c; ++c)
            n += *c == '|';
        for (int k = 0; k < n; ++k)
            out.push_back(static_cast<float>(k));
    }
    return out;
}

bool ReadRaw(const std::wstring& file, const wchar_t* section, const wchar_t* key, float& out)
{
    wchar_t buf[64]{};
    GetPrivateProfileStringW(section, key, L"", buf, 64, file.c_str());
    if (!buf[0])
        return false;
    if (_wcsicmp(buf, L"true") == 0)
        out = 1;
    else if (_wcsicmp(buf, L"false") == 0)
        out = 0;
    else
        out = std::wcstof(buf, nullptr);
    return true;
}

std::wstring Format(int i, float v)
{
    wchar_t buf[64];
    switch (kEntries[i].s.kind) {
    case Kind::Bool:
        return v != 0 ? L"true" : L"false";
    case Kind::Float:
        swprintf_s(buf, L"%f", v);
        return buf;
    default:
        swprintf_s(buf, L"%d", static_cast<int>(std::lround(v)));
        return buf;
    }
}

float Clamp(int i, float v)
{
    const Setting& s = kEntries[i].s;
    if (s.kind == Kind::Bool)
        return v != 0 ? 1.0f : 0.0f;
    if (s.kind == Kind::Choice) {
        const std::vector<float> vals = ChoiceValues(i);
        float best = vals[0];
        for (float c : vals)
            if (std::fabs(c - v) < std::fabs(best - v))
                best = c;
        return best;
    }
    if (s.kind == Kind::Int)
        v = std::round(v);
    return v < s.minValue ? s.minValue : v > s.maxValue ? s.maxValue : v;
}

} // namespace

int Count()
{
    return kCount;
}

const Setting& Get(int i)
{
    return kEntries[i].s;
}

int PresetSetting()
{
    return 0;
}

int Find(const char* key)
{
    for (int i = 0; i < kCount; ++i)
        if (strcmp(kEntries[i].s.key, key) == 0)
            return i;
    return -1;
}

bool Load()
{
    wchar_t docs[MAX_PATH]{};
    g_loaded = false;
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs)))
        return false;
    g_path = std::wstring(docs) + L"\\Mount&Blade Warband WSE2\\rgl_config.ini";
    if (GetFileAttributesW(g_path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    for (int i = 0; i < kCount; ++i) {
        float v = kEntries[i].levels[0] > kSame / 2 ? kEntries[i].levels[0] : kEntries[i].s.minValue;
        ReadRaw(g_path, Wide(kEntries[i].s.section).c_str(), Wide(kEntries[i].s.key).c_str(), v);
        g_values[i] = g_original[i] = Clamp(i, v);
    }
    float f = 0;
    if (ReadRaw(g_path, L"PostFx", L"bDisableHighHDR", f))
        g_disableHighHdr = f != 0;
    if (ReadRaw(g_path, L"Battle", L"iBattleSizeMin", f))
        g_battleMin = static_cast<int>(f);
    if (ReadRaw(g_path, L"Battle", L"iBattleSizeMax", f))
        g_battleMax = static_cast<int>(f);
    // Le jeu écrit souvent iPreset=0 même après avoir appliqué un niveau : niveau reconnu si
    // toutes les valeurs y correspondent
    if (g_values[PresetSetting()] == 0) {
        for (int level = 1; level <= 4; ++level) {
            bool match = true;
            for (int i = 0; i < kCount && match; ++i) {
                float v = kEntries[i].levels[level - 1];
                if (v == kHdrHigh)
                    v = g_disableHighHdr ? 1.0f : 2.0f;
                if (v > kSame / 2 && i != PresetSetting())
                    match = std::fabs(Clamp(i, v) - g_values[i]) < 1e-3f;
            }
            if (match) {
                g_values[PresetSetting()] = g_original[PresetSetting()] = static_cast<float>(level);
                break;
            }
        }
    }
    g_loaded = true;
    return true;
}

bool Loaded()
{
    return g_loaded;
}

std::wstring Path()
{
    return g_path;
}

float Value(int i)
{
    return g_values[i];
}

void SetValue(int i, float v)
{
    g_values[i] = Clamp(i, v);
    if (i != PresetSetting())
        g_values[PresetSetting()] = 0; // réglé à la main : Personnalisé, comme dans le jeu
}

void ApplyLevel(int level)
{
    g_values[PresetSetting()] = static_cast<float>(level);
    if (level < 1 || level > 4)
        return;
    for (int i = 0; i < kCount; ++i) {
        float v = kEntries[i].levels[level - 1];
        if (v == kHdrHigh)
            v = g_disableHighHdr ? 1.0f : 2.0f;
        if (v > kSame / 2 && i != PresetSetting())
            g_values[i] = Clamp(i, v);
    }
}

int ChoiceIndex(int i)
{
    const std::vector<float> vals = ChoiceValues(i);
    for (size_t k = 0; k < vals.size(); ++k)
        if (std::fabs(vals[k] - g_values[i]) < 1e-3f)
            return static_cast<int>(k);
    return 0;
}

void SetChoiceIndex(int i, int index)
{
    const std::vector<float> vals = ChoiceValues(i);
    if (index < 0 || index >= static_cast<int>(vals.size()))
        return;
    if (i == PresetSetting())
        ApplyLevel(static_cast<int>(vals[index]));
    else
        SetValue(i, vals[index]);
}

int ChoiceCount(int i)
{
    return static_cast<int>(ChoiceValues(i).size());
}

int BattleTroops(float fraction)
{
    return static_cast<int>(std::lround(g_battleMin + fraction * (g_battleMax - g_battleMin)));
}

bool Save()
{
    if (!g_loaded)
        return true;
    bool ok = true;
    for (int i = 0; i < kCount; ++i) {
        if (g_values[i] == g_original[i])
            continue;
        ok &= WritePrivateProfileStringW(Wide(kEntries[i].s.section).c_str(), Wide(kEntries[i].s.key).c_str(),
                                         Format(i, g_values[i]).c_str(), g_path.c_str()) != 0;
        g_original[i] = g_values[i];
    }
    return ok;
}

std::string g_language, g_languageSaved;

std::wstring LanguageFile()
{
    wchar_t appData[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData)))
        return L"";
    return std::wstring(appData) + L"\\Mount&Blade Warband WSE2\\language.txt";
}

std::vector<std::string> Languages(const std::wstring& gameDir)
{
    std::vector<std::string> out{"en"};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((gameDir + L"\\languages\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
                std::string code;
                for (const wchar_t* c = fd.cFileName; *c; ++c)
                    code += static_cast<char>(*c < 128 ? *c : '?');
                if (code != "en")
                    out.push_back(code);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!g_language.empty() && std::find(out.begin(), out.end(), g_language) == out.end())
        out.push_back(g_language);
    return out;
}

std::string Language()
{
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        FILE* f = nullptr;
        if (_wfopen_s(&f, LanguageFile().c_str(), L"rb") == 0 && f) {
            char buf[16]{};
            fgets(buf, sizeof(buf), f);
            fclose(f);
            for (char c : std::string(buf))
                if (c > ' ')
                    g_language += c;
        }
        g_languageSaved = g_language;
    }
    return g_language;
}

void SetLanguage(const std::string& code)
{
    Language();
    g_language = code;
}

bool SaveLanguage()
{
    Language();
    if (g_language.empty() || g_language == g_languageSaved)
        return true;
    const std::wstring path = LanguageFile();
    CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return false;
    fwrite(g_language.data(), 1, g_language.size(), f); // comme le jeu : le code seul
    fclose(f);
    g_languageSaved = g_language;
    return true;
}

bool GameRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"mb_warband_wse2_x64.exe") == 0 || _wcsicmp(pe.szExeFile, L"mb_warband.exe") == 0;
    CloseHandle(snap);
    return found;
}

void WriteToPreset(const std::wstring& presetPath)
{
    if (!g_loaded)
        return;
    for (int i = 0; i < kCount; ++i) {
        const std::wstring key = Wide(kEntries[i].s.section) + L"." + Wide(kEntries[i].s.key);
        WritePrivateProfileStringW(L"Warband", key.c_str(), Format(i, g_values[i]).c_str(), presetPath.c_str());
    }
}

bool ReadFromPreset(const std::wstring& presetPath)
{
    wchar_t probe[8]{};
    if (!g_loaded || !GetPrivateProfileSectionW(L"Warband", probe, 8, presetPath.c_str()))
        return false;
    for (int i = 0; i < kCount; ++i) {
        const std::wstring key = Wide(kEntries[i].s.section) + L"." + Wide(kEntries[i].s.key);
        float v = 0;
        if (ReadRaw(presetPath, L"Warband", key.c_str(), v))
            g_values[i] = Clamp(i, v);
    }
    return true;
}

} // namespace game
