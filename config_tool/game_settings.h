#pragma once
// Réglages graphiques de Warband (WSE2) : rgl_config.ini dans Documents\Mount&Blade Warband WSE2.
// Fichier du joueur, que le jeu lit au démarrage et réécrit en quittant : l'outil n'y écrit que
// les clés qu'il gère et qui ont changé. Valeurs des niveaux Bas / Moyen / Haut / Ultra relevées
// dans mbOptionsWindowHelper::applyPreset (préréglage graphique des options du jeu).
#include "util/config.h"

#include <string>
#include <vector>

namespace game {

enum class Kind { Bool, Choice, Float, Int };

struct Setting {
    const char* section;   // section de rgl_config.ini
    const char* key;
    config::Text label;
    config::Text help;
    Kind kind;
    float minValue, maxValue; // Float / Int
    config::Text choices;     // Choice : libellés séparés par '|'
    const char* values;       // Choice : valeurs écrites, séparées par '|' (nullptr : 0, 1, 2...)
};

int Count();
const Setting& Get(int i);
int PresetSetting(); // index du préréglage graphique global (iPreset)
int Find(const char* key); // index du réglage de cette clé, -1 si absent

// Fichier du joueur. false s'il n'existe pas (jeu jamais lancé) : onglet indisponible.
bool Load();
bool Loaded();
std::wstring Path();

float Value(int i);
// Valeur d'un réglage ; un réglage individuel modifié passe le préréglage global à Personnalisé
void SetValue(int i, float v);
// Préréglage global : 1 Bas, 2 Moyen, 3 Haut, 4 Ultra (0 Personnalisé : rien ne change)
void ApplyLevel(int level);

// Choice : index de la valeur courante dans la liste
int ChoiceIndex(int i);
void SetChoiceIndex(int i, int index);
int ChoiceCount(int i);

// Taille des batailles : nombre de troupes affiché pour la valeur 0..1 du jeu
int BattleTroops(float fraction);

// Écrit les clés modifiées depuis Load. false en cas d'erreur d'écriture.
bool Save();
// Jeu en cours : il réécrira le fichier en quittant
bool GameRunning();

// Langue du jeu : %APPDATA%\Mount&Blade Warband WSE2\language.txt (code : fr, en, de...), lue par
// mbLanguageManager::readLanguageCode au démarrage. Langues : dossiers languages\ du jeu, plus en.
std::vector<std::string> Languages(const std::wstring& gameDir);
std::string Language(); // langue écrite dans le fichier, vide s'il n'existe pas
void SetLanguage(const std::string& code);
bool SaveLanguage(); // écrit le fichier si la langue a changé

// Préréglages de l'outil : section [Warband] (clés « Section.clé »)
void WriteToPreset(const std::wstring& presetPath);
bool ReadFromPreset(const std::wstring& presetPath); // false : pas de section, rien de changé

} // namespace game
