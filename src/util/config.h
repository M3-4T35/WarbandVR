#pragma once
// Réglages de WarbandVR : fichier warband_vr.ini à côté de la DLL (et de l'outil de
// configuration). La liste des réglages (valeurs par défaut, bornes, textes anglais et
// français) est définie une seule fois ici et partagée par la DLL et warband_vr_config.exe.
#include <string>

namespace config {

enum class Id {
    // Affichage
    RollMode,
    ScreenDistance,
    ScreenWidth,
    HudDistance,
    HudAngle,
    ResizeWindow,
    OpenXRRuntime,
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
    BowFullDraw,
    BowMaxPrecision,
    // Avancé
    AimGain,
    Count
};

enum class Kind { Float, Bool, Choice };
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
};

const Definition& Def(Id id);
constexpr int kCount = static_cast<int>(Id::Count);

// Valeurs courantes (lecture possible depuis n'importe quel thread)
float Get(Id id);
bool GetBool(Id id);
int GetInt(Id id);
void Set(Id id, float value); // borné à [min, max]
void ResetDefaults();

// Fichier : chargement (valeurs absentes = défaut), enregistrement complet
std::string DefaultPath(); // warband_vr.ini à côté du module courant
bool Load(const std::string& path);
bool Save(const std::string& path);

// Langue de l'outil de configuration (section [Tool] du fichier), anglais par défaut
Lang LoadLanguage(const std::string& path);
void SaveLanguage(const std::string& path, Lang lang);

// DLL : charge le fichier (le crée avec les valeurs par défaut s'il manque)
void Init();
// DLL : relit le fichier s'il a changé depuis la dernière lecture. true si rechargé.
bool ReloadIfChanged();

} // namespace config
