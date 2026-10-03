#include "util/config.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace config {
namespace {

constexpr Text kDisplay{"Display", "Affichage"};
constexpr Text kControls{"Controls", "Controles"};
constexpr Text kCombat{"Combat", "Combat"};
constexpr Text kAdvanced{"Advanced", "Avance"};
constexpr Text kNone{nullptr, nullptr};

// Valeurs par défaut = réglages validés au casque (Quest 2)
const Definition kDefs[kCount] = {
    // Affichage
    {"Display", "roll_mode", kDisplay, {"Roll mode (head tilt)", "Mode de roulis (tete inclinee)"}, Kind::Choice, 0,
     0, 2, {"Inverted compositor|Compositor|Game", "Compositeur inverse|Compositeur|Jeu"},
     {"How head tilt is rendered. Inverted compositor: chosen in headset testing.",
      "Comment l'inclinaison de la tete est rendue. Compositeur inverse : choisi au test casque."}},
    {"Display", "screen_distance", kDisplay, {"Virtual screen: distance (m)", "Ecran virtuel : distance (m)"},
     Kind::Float, 2.0f, 0.5f, 10.0f, kNone,
     {"Distance of the menu screen in front of you.", "Distance de l'ecran des menus devant vous."}},
    {"Display", "screen_width", kDisplay, {"Virtual screen: width (m)", "Ecran virtuel : largeur (m)"}, Kind::Float,
     2.4f, 0.5f, 10.0f, kNone, {"Width of the menu screen.", "Largeur de l'ecran des menus."}},
    {"Display", "hud_distance", kDisplay, {"HUD: distance (m)", "HUD : distance (m)"}, Kind::Float, 4.0f, 0.5f, 20.0f,
     kNone,
     {"Distance of the combat HUD. Farther: less double vision against the scene.",
      "Distance du HUD en combat. Loin : moins de dedoublement avec la scene."}},
    {"Display", "hud_angle", kDisplay, {"HUD: width (degrees)", "HUD : largeur (degres)"}, Kind::Float, 46.9f, 10.0f,
     120.0f, kNone,
     {"Apparent width of the HUD, independent of its distance.",
      "Largeur apparente du HUD, independante de sa distance."}},
    {"Display", "resize_window", kDisplay,
     {"Game window at headset resolution", "Fenetre du jeu a la resolution du casque"}, Kind::Bool, 1, 0, 1, kNone,
     {"When the VR session starts, the game window takes the resolution recommended by the headset.",
      "Au demarrage de la session VR, la fenetre prend la resolution recommandee par le casque."}},
    {"Display", "openxr_runtime", kDisplay, {"VR runtime (OpenXR)", "Runtime VR (OpenXR)"}, Kind::Choice, 0, 0, 1,
     {"Windows default|SteamVR", "Par defaut de Windows|SteamVR"},
     {"Runtime used by the launcher. SteamVR: Steam Frame, Index, Vive... Windows default: the one set in "
      "Windows (e.g. Meta for Quest Link). Applies at the next launch.",
      "Runtime utilise par le lanceur. SteamVR : Steam Frame, Index, Vive... Par defaut : celui de Windows "
      "(ex. Meta pour Quest Link). Pris en compte au prochain lancement."}},
    // Contrôles
    {"Controls", "turn_speed", kControls, {"Stick turning (degrees/s)", "Rotation au stick (degres/s)"}, Kind::Float,
     143.0f, 10.0f, 720.0f, kNone,
     {"Body turning speed, right stick fully pushed.", "Vitesse de rotation du corps, stick droit a fond."}},
    {"Controls", "turn_mode", kControls, {"Stick turning mode", "Mode de rotation au stick"}, Kind::Choice, 0, 0, 1,
     {"Smooth|Snap", "Continue|Par crans"},
     {"Smooth: the body turns while the stick is pushed. Snap: one step per push (more comfortable).",
      "Continue : le corps tourne tant que le stick est pousse. Par crans : un cran par poussee (plus confortable)."}},
    {"Controls", "snap_angle", kControls, {"Snap turning: step (degrees)", "Rotation par crans : cran (degres)"},
     Kind::Float, 30.0f, 10.0f, 90.0f, kNone,
     {"Angle of one step in snap turning mode.", "Angle d'un cran en rotation par crans."}},
    {"Controls", "turn_deadzone", kControls, {"Turning stick deadzone", "Zone morte du stick de rotation"},
     Kind::Float, 0.2f, 0.0f, 0.9f, kNone,
     {"Below this, the right stick does not turn.", "En deca, le stick droit ne fait pas tourner."}},
    {"Controls", "jump_threshold", kControls, {"Jump threshold (right stick up)", "Seuil du saut (stick droit haut)"},
     Kind::Float, 0.7f, 0.2f, 1.0f, kNone,
     {"Right stick pushed up beyond this: jump.", "Stick droit pousse vers le haut au-dela : saut."}},
    {"Controls", "aim_source", kControls, {"Crosshair source at start", "Source du reticule au demarrage"},
     Kind::Choice, 0, 0, 2, {"Gaze|Left hand|Right hand", "Regard|Main gauche|Main droite"},
     {"What the game's aim follows. Click the left stick to change in game.",
      "Ce que suit la visee du jeu. Clic du stick gauche pour changer en jeu."}},
    // Combat
    {"Combat", "gestures", kCombat, {"Gesture attacks", "Attaques au geste"}, Kind::Bool, 1, 0, 1, kNone,
     {"A quick right-hand gesture triggers a game attack in its direction.",
      "Un geste rapide de la main droite declenche une attaque du jeu dans sa direction."}},
    {"Combat", "gesture_speed", kCombat, {"Gesture: trigger speed (m/s)", "Geste : vitesse de declenchement (m/s)"},
     Kind::Float, 1.0f, 0.3f, 5.0f, kNone,
     {"Hand speed above which a gesture starts.", "Vitesse de la main au-dela de laquelle un geste commence."}},
    {"Combat", "gesture_window_ms", kCombat, {"Gesture: measuring time (ms)", "Geste : duree de mesure (ms)"},
     Kind::Float, 50.0f, 10.0f, 300.0f, kNone,
     {"Time during which the trajectory is measured to pick the direction.",
      "Temps pendant lequel la trajectoire est mesuree pour choisir la direction."}},
    {"Combat", "gesture_min_distance", kCombat, {"Gesture: minimal movement (m)", "Geste : deplacement minimal (m)"},
     Kind::Float, 0.05f, 0.0f, 0.5f, kNone,
     {"Minimal hand movement during the measurement.", "Deplacement minimal de la main pendant la mesure."}},
    {"Combat", "thrust_dominance", kCombat, {"Thrust: forward dominance", "Estoc : dominance du mouvement avant"},
     Kind::Float, 1.5f, 1.0f, 5.0f, kNone,
     {"A thrust needs the forward movement to dominate the other directions by this factor.",
      "Un estoc demande que l'avance domine d'autant les autres directions."}},
    {"Combat", "always_active_weapon", kCombat, {"Melee weapon always active", "Arme de melee toujours active"},
     Kind::Bool, 1, 0, 1, kNone,
     {"The blade hits as soon as it goes through an enemy fast enough (physical combat).",
      "La lame touche des qu'elle traverse un ennemi assez vite (combat physique)."}},
    {"Combat", "weapon_start_speed", kCombat, {"Active weapon: start speed (m/s)", "Arme active : vitesse de debut (m/s)"},
     Kind::Float, 1.5f, 0.3f, 6.0f, kNone,
     {"Hand speed above which the blade hurts.", "Vitesse de la main au-dela de laquelle la lame blesse."}},
    {"Combat", "weapon_end_speed", kCombat, {"Active weapon: end speed (m/s)", "Arme active : vitesse de fin (m/s)"},
     Kind::Float, 0.8f, 0.1f, 5.0f, kNone,
     {"Below this the movement is over: the next one can hit the same enemy again.",
      "En deca, le mouvement est fini : le suivant peut retoucher le meme ennemi."}},
    {"Combat", "power_per_speed", kCombat, {"Power per m/s of hand", "Puissance par m/s de main"}, Kind::Float, 1.0f,
     0.1f, 10.0f, kNone,
     {"Hit power from hand speed (damage is computed by the game).",
      "Puissance du coup selon la vitesse de la main (degats calcules par le jeu)."}},
    {"Combat", "max_power", kCombat, {"Maximum power", "Puissance maximale"}, Kind::Float, 4.0f, 1.0f, 20.0f, kNone,
     {"Upper limit of a hit's power.", "Plafond de la puissance d'un coup."}},
    {"Combat", "always_active_shield", kCombat, {"Shield always active", "Bouclier toujours actif"}, Kind::Bool, 1, 0,
     1, kNone,
     {"The shield blocks hits and missiles that touch it, without holding block.",
      "Le bouclier pare les coups et projectiles qui le touchent, sans tenir la defense."}},
    {"Combat", "shield_radius", kCombat, {"Shield: protection radius (m)", "Bouclier : rayon de protection (m)"},
     Kind::Float, 0.35f, 0.1f, 1.0f, kNone,
     {"Distance from the shield centre within which a hit is blocked.",
      "Distance au centre du bouclier en deca de laquelle un coup est pare."}},
    {"Combat", "bow_full_draw", kCombat, {"Bow: full draw (m between hands)", "Arc : allonge complete (m entre les mains)"},
     Kind::Float, 0.5f, 0.2f, 1.2f, kNone,
     {"Hands this far apart while drawing: best accuracy. Closer hands: less accurate.",
      "Mains ecartees de cette distance en bandant : meilleure precision. Plus proches : moins precis."}},
    {"Combat", "bow_max_precision", kCombat, {"Bow: maximum precision (%)", "Arc : precision maximale (%)"},
     Kind::Float, 110.0f, 10.0f, 150.0f, kNone,
     {"Precision reached at full draw. 100: the character's best accuracy; more: even tighter; less: some spread.",
      "Precision atteinte a pleine allonge. 100 : meilleure precision du personnage ; plus : encore plus serre ; moins : dispersion."}},
    // Avancé
    {"Advanced", "aim_gain", kAdvanced, {"Aim: servo gain", "Visee : gain de l'asservissement"}, Kind::Float, 0.5f,
     0.05f, 1.0f, kNone,
     {"Share of the aim error corrected each frame. Higher: snappier, may oscillate.",
      "Part de l'ecart de visee corrigee a chaque image. Plus haut : plus vif, risque d'oscillation."}},
};

std::atomic<float> g_values[kCount];
std::string g_path;
FILETIME g_lastWrite{};

float Clamp(Id id, float v)
{
    const Definition& d = Def(id);
    if (d.kind == Kind::Bool)
        return v != 0 ? 1.0f : 0.0f;
    if (d.kind == Kind::Choice)
        v = static_cast<float>(static_cast<int>(v + 0.5f));
    return v < d.minValue ? d.minValue : v > d.maxValue ? d.maxValue : v;
}

bool LastWrite(const std::string& path, FILETIME& out)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
        return false;
    out = data.ftLastWriteTime;
    return true;
}

struct Defaults {
    Defaults() { ResetDefaults(); }
} g_defaults;

} // namespace

const Definition& Def(Id id)
{
    return kDefs[static_cast<int>(id)];
}

float Get(Id id)
{
    return g_values[static_cast<int>(id)].load(std::memory_order_relaxed);
}

bool GetBool(Id id)
{
    return Get(id) != 0;
}

int GetInt(Id id)
{
    return static_cast<int>(Get(id) + 0.5f);
}

void Set(Id id, float value)
{
    g_values[static_cast<int>(id)] = Clamp(id, value);
}

void ResetDefaults()
{
    for (int i = 0; i < kCount; ++i)
        g_values[i] = kDefs[i].defaultValue;
}

std::string DefaultPath()
{
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&DefaultPath), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string s = path;
    return s.substr(0, s.find_last_of('\\') + 1) + "warband_vr.ini";
}

bool Load(const std::string& path)
{
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    for (int i = 0; i < kCount; ++i) {
        char buf[64]{};
        GetPrivateProfileStringA(kDefs[i].section, kDefs[i].key, "", buf, sizeof(buf), path.c_str());
        if (!buf[0]) {
            g_values[i] = kDefs[i].defaultValue;
            continue;
        }
        char* end = nullptr;
        const float v = strtof(buf, &end);
        g_values[i] = end != buf ? Clamp(static_cast<Id>(i), v) : kDefs[i].defaultValue;
    }
    return true;
}

bool Save(const std::string& path)
{
    const Lang lang = LoadLanguage(path); // conservée à la réécriture
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f)
        return false;
    fprintf(f, "; WarbandVR settings. Edit with warband_vr_config.exe or by hand.\n");
    fprintf(f, "; The game reloads this file as soon as it changes.\n");
    const char* section = nullptr;
    for (int i = 0; i < kCount; ++i) {
        const Definition& d = kDefs[i];
        if (!section || strcmp(section, d.section) != 0) {
            fprintf(f, "\n[%s]\n", d.section);
            section = d.section;
        }
        fprintf(f, "; %s", d.help.en);
        if (d.kind == Kind::Bool)
            fprintf(f, " (0 = no, 1 = yes)");
        else if (d.kind == Kind::Choice)
            fprintf(f, " (%s: 0, 1, 2...)", d.choices.en);
        else
            fprintf(f, " (%g to %g)", d.minValue, d.maxValue);
        fprintf(f, "\n%s=%g\n", d.key, g_values[i].load());
    }
    fprintf(f, "\n[Tool]\n; Configuration tool language (0 = English, 1 = French)\nlanguage=%d\n",
            lang == Lang::French ? 1 : 0);
    fclose(f);
    return true;
}

Lang LoadLanguage(const std::string& path)
{
    return GetPrivateProfileIntA("Tool", "language", 0, path.c_str()) == 1 ? Lang::French : Lang::English;
}

void SaveLanguage(const std::string& path, Lang lang)
{
    WritePrivateProfileStringA("Tool", "language", lang == Lang::French ? "1" : "0", path.c_str());
}

void Init()
{
    g_path = DefaultPath();
    if (!Load(g_path))
        Save(g_path); // premier lancement : fichier avec les valeurs par défaut
    LastWrite(g_path, g_lastWrite);
}

bool ReloadIfChanged()
{
    FILETIME t{};
    if (g_path.empty() || !LastWrite(g_path, t) || CompareFileTime(&t, &g_lastWrite) == 0)
        return false;
    g_lastWrite = t;
    return Load(g_path);
}

} // namespace config
