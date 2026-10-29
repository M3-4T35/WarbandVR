#pragma once
// Réglages de WarbandVR : fichier warband_vr.ini à côté de la DLL (et de l'outil de
// configuration). La liste des réglages (valeurs par défaut, bornes, textes anglais et
// français) est définie une seule fois ici et partagée par la DLL et warband_vr_config.exe.
#include <string>

namespace config {

// Même ordre que la table kDefs de config.cpp (les valeurs sont rangées par position)
enum class Id {
    // Lancement (lu par l'injecteur)
    GameDir,
    Module,
    ShowLauncher,
    OpenXRRuntime,
    // Affichage
    RollMode,
    ScreenDistance,
    ScreenWidth,
    HudDistance,
    HudAngle,
    ResizeWindow,
    RenderScale,
    GameFpsLimit,
    ImmersiveMap,
    MapDistance,
    MapUiScale,
    // Contrôles
    TurnSpeed,
    TurnMode,
    SnapAngle,
    TurnDeadzone,
    JumpThreshold,
    AimSource,
    // Combat
    Gestures,
    SwingSpeed,
    SwingWindowMs,
    SwingMinDistance,
    ThrustDominance,
    AlwaysActive,
    ActiveStartSpeed,
    ActiveEndSpeed,
    PowerPerSpeed,
    MaxPower,
    ShieldAlwaysActive,
    ShieldRadius,
    TorsoForward,
    BowFullDraw,
    BowMaxPrecision,
    // Avancé
    AimGain,
    EyeOrderSwap,
    DebugLeftImageBothEyes,
    DebugFrameProfile,
    TrueStereo,
    ScreenInHandsLayer,
    DebugCapture,
    Count
};

enum class Kind { Float, Bool, Choice, Text };
enum class Lang { English, French };

struct Text {
    const char* en;
    const char* fr;
    const char* operator()(Lang l) const { return l == Lang::French ? fr : en; }
};

struct Definition {
    const char* section; // section du fichier .ini (anglais)
    const char* key;     // clé du fichier .ini (anglais)
    Text group;          // groupe dans l'outil
    Text label;          // libellé dans l'outil
    Kind kind;
    float defaultValue;
    float minValue, maxValue;
    Text choices;        // Choice : libellés séparés par '|'
    Text help;           // explication courte
    const char* defaultText = nullptr; // Text : valeur par défaut
};

const Definition& Def(Id id);
constexpr int kCount = static_cast<int>(Id::Count);

// Valeurs courantes (lecture possible depuis n'importe quel thread)
float Get(Id id);
bool GetBool(Id id);
int GetInt(Id id);
void Set(Id id, float value); // borné à [min, max]
// Réglages texte (Kind::Text)
std::string GetText(Id id);
void SetText(Id id, const std::string& value);
void ResetDefaults();

// Fichier : chargement (valeurs absentes = défaut), enregistrement complet
std::string DefaultPath(); // warband_vr.ini à côté du module courant
bool Load(const std::string& path);
bool Save(const std::string& path);

// --- Préréglages --------------------------------------------------------------------------
// Un préréglage fixe tous les réglages de jeu, pas ceux du lancement (dossier du jeu, module,
// lanceur, runtime VR) : propres à la machine, ou choisis à chaque partie.
bool InPreset(Id id);
// Préréglages intégrés : 0 = PCVR (valeurs par défaut), 1 = Steam Frame autonome. Choisis par
// l'utilisateur, jamais appliqués d'office selon le casque détecté.
int BuiltinPresetCount();
Text BuiltinPresetName(int index);
void ApplyBuiltinPreset(int index);
// Préréglages de l'utilisateur : fichiers .ini ([Preset] puis les sections des réglages),
// rangés dans warband_vr_presets à côté de warband_vr.ini, exportables n'importe où.
std::string PresetDir(const std::string& settingsPath);
bool SavePreset(const std::string& path);
bool LoadPreset(const std::string& path); // réglage absent du fichier : valeur par défaut
// Dernier préréglage appliqué ([Tool] preset), affiché par l'outil
std::string LoadPresetName(const std::string& path);
void SavePresetName(const std::string& path, const std::string& name);

// Langue de l'outil de configuration (section [Tool] du fichier), anglais par défaut
Lang LoadLanguage(const std::string& path);
void SaveLanguage(const std::string& path, Lang lang);

// DLL : charge le fichier (le crée avec les valeurs par défaut s'il manque)
void Init();
// DLL : relit le fichier s'il a changé depuis la dernière lecture. true si rechargé.
bool ReloadIfChanged();

} // namespace config
