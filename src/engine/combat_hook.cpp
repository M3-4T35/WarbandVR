// Combat physique.
//
// mbAgent::updateMeleeAttackAction appelle, pendant la phase active d'un coup,
// mbMission::checkAttackCollision(agentNo, item, missile, blow, attack, out) : l'arme est
// balayée entre sa pose précédente et sa pose actuelle (mbAttack) contre les autres agents ;
// en cas de contact, le mbBlow est rempli (agent touché, os, dégâts). On loggue ici ce que
// le jeu y met pour les coups du joueur. checkAttackCollision applique lui-même le coup
// (mbMission::deliverBlow) quand il touche.
//
// Arme toujours active (active par défaut, mêlée uniquement ; F10 pour basculer) : hors des attaques du jeu, quand la
// lame du joueur est en mouvement rapide, on prépare son mbAttack comme le jeu en début de
// coup et on appelle checkAttackCollision à chaque frame : le jeu balaie la vraie lame (os
// de l'arme = bras VR) et applique ses propres dégâts, qui dépendent de la vitesse de l'arme.
#include "engine/combat_hook.h"
#include "engine/camera_hook.h"
#include "engine/skeleton_hook.h"
#include "engine/rgl.h"
#include "util/config.h"
#include "util/log.h"
#include "util/pattern.h"
#include "xr/xr_session.h"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)

namespace engine {
namespace {

std::atomic<bool> g_rangedAiming{false}; // arme à distance en main, en visée ou en tir
std::atomic<bool> g_meleeHeld{false};    // arme de mêlée en main droite
std::atomic<bool> g_twoHandedHeld{false}; // arme à deux mains ou d'hast en main droite
constexpr int kAgentInaccuracy = 0x9140;    // mbAgent::m_inaccuracy (taille du réticule, dispersion)

// Tir à l'arc VR : la précision dépend de l'écart des mains, pas du temps de visée. Bornes
// tirées des valeurs du jeu lui-même (meilleure et pire imprécision vues en bandant), donc
// des compétences du personnage, sans échelle codée en dur.
using UpdateAccuracyFn = void(__fastcall*)(uint8_t*);
UpdateAccuracyFn g_updateAccuracy = nullptr;
void* g_updateAccuracyTarget = nullptr;
constexpr float kBowMinDraw = 0.1f; // mains plus proches : pire précision
float g_bowBest = -1, g_bowWorst = -1;
bool g_bowDrawing = false;
ULONGLONG g_bowLogMs = 0;
int g_bowLogs = 0;

uint8_t* PlayerAgent();

void __fastcall HkUpdateAccuracy(uint8_t* agent)
{
    g_updateAccuracy(agent);
    if (!xr::IsBowMode() || !xr::IsDrawing() || agent != PlayerAgent()) {
        g_bowDrawing = false;
        return;
    }
    xr::HandPose hands[2];
    xr::GetHands(hands);
    if (!hands[0].valid || !hands[1].valid)
        return;
    float& inaccuracy = *reinterpret_cast<float*>(agent + kAgentInaccuracy);
    const float game = inaccuracy;
    if (g_bowBest < 0 || game < g_bowBest)
        g_bowBest = game;
    if (game > g_bowWorst)
        g_bowWorst = game;
    const float dx = hands[0].px - hands[1].px, dy = hands[0].py - hands[1].py, dz = hands[0].pz - hands[1].pz;
    const float draw = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float full = std::max(config::Get(config::Id::BowFullDraw), kBowMinDraw + 0.05f);
    const float t = std::clamp((draw - kBowMinDraw) / (full - kBowMinDraw), 0.0f, 1.0f);
    // Au-delà de 100 % : plus serré que la meilleure précision du jeu (jamais négatif)
    const float cap = std::clamp(config::Get(config::Id::BowMaxPrecision) / 100.0f, 0.0f, 1.5f);
    inaccuracy = std::max(0.0f, g_bowBest + (g_bowWorst - g_bowBest) * (1 - t * cap));

    const ULONGLONG now = GetTickCount64();
    if (!g_bowDrawing || (g_bowLogs < 60 && now - g_bowLogMs >= 500)) {
        LOG_ENGINE("arc : mains a %.2f m (%.0f %%), imprecision jeu %.4f -> %.4f (bornes %.4f..%.4f)", draw, t * 100,
                   game, inaccuracy, g_bowBest, g_bowWorst);
        g_bowLogMs = now;
        ++g_bowLogs;
    }
    g_bowDrawing = true;
}
int g_loggedPhase = -1;
int g_phaseLogs = 0;
constexpr ULONGLONG kGripAfterShotMs = 1000; // prise de l'animation gardée après le tir
ULONGLONG g_gripUntilMs = 0;
// Arbalète / arme à feu / arme de jet en main : visée sur la main droite, source précédente à restaurer
bool g_gunAim = false;
xr::AimSource g_savedAimSource = xr::AimSource::Head;

// mbMission::updateCamera commence par charger g_mission : mov rbx, [rip+disp32] à +0x17
constexpr char kUpdateCameraSig[] =
    "48 8B C4 48 89 78 18 55 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 89 58 08 48 8B 1D ?? ?? ?? ?? "
    "48 89 70 10 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8";
constexpr int kMissionLoadOffset = 0x17;
constexpr char kUpdateMeleeSig[] =
    "48 8B C4 55 53 41 54 41 ?? 41 ?? 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 0F 29 ?? ?? "
    "44 0F 29 40 98 44 0F 29 48 88 44 0F 29 90 78 FF FF FF 44 0F 29 98 68 FF FF FF";
// Chargement de g_itemKinds (tableau des sortes d'objets) : mov rax, [rip+disp32]
constexpr char kItemKindsLoadSig[] =
    "48 8B 05 ?? ?? ?? ?? 48 8B CA 48 69 C9 50 07 00 00 8B 8C 01 C0 06 00 00 48 0F BA E1 1D";
constexpr char kCheckDefendSig[] = "48 8B C4 44 89 40 18 89 50 10 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 "
                                   "?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 C7 45 10 FE FF FF FF 48 89 58 20";
// mbAgent::updateAccuracy : imprécision de visée de l'agent (m_inaccuracy), lissée à chaque frame
constexpr char kUpdateAccuracySig[] =
    "4C 8B DC 55 53 49 8D 6B ?? 48 81 EC ?? ?? 00 00 48 63 91 98 0A 00 00";
constexpr char kCheckAttackSig[] = "4C 89 4C 24 20 4C 89 44 24 18 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 "
                                   "41 56 41 57 48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 48 C7 85";

// Champs relevés dans le PDB
constexpr int kMissionPlayerAgentNo = 0xC0; // mbMission::m_playerAgentNo
constexpr int kAgentNo = 0x0C;              // mbAgent : numéro de l'agent
constexpr int kAgentAttack = 0x1B8;         // mbAgent : mbAttack
constexpr int kAgentAttackPhase = 0x90F4;   // mbAgent : phase d'attaque (2 = coup en cours)
constexpr float kSentinel = -20000000.0f;   // pose d'arme « aucune » (début de balayage)
constexpr int kMissionAgentBlocks = 0x28;   // mbMission::m_agents (+0x08) : table des blocs de 16 agents
uint32_t g_agentSize = 0;                   // taille de mbAgent (0x9630 en juillet 2026, 0x9640 en septembre)
// mov rax, [r9+28h] (blocs de 16 agents de la mission) ; mov ecx, r8d ; and ecx, 0Fh ;
// imul rcx, rcx, taille : la taille de mbAgent est lue dans le code, elle change avec WSE2
constexpr char kAgentSizeSig[] = "49 8B 41 28 41 8B C8 83 E1 0F 48 69 C9 ?? ?? ?? ??";
constexpr int kAgentValid = 0x08;
constexpr int kAgentItems = 0xA3C;          // mbAgent::m_items[10] (mbItem : sorte, drapeaux)
constexpr int kAgentWielded = 0xA98;        // mbAgent::m_wieldedItems[2] (main droite, gauche)
constexpr int kAgentAltUsages = 0xAAC;      // mbAgent::m_itemAlternativeUsages
constexpr int kItemKindSize = 0x750;
constexpr int kItemKindProperties = 0x6C0;  // mbItemKind::m_properties (octet bas = type itp_type_*,
                                            // comme mbItemKind::getType)
constexpr uint64_t kPropNextItemAsMelee = 1ull << 29; // usage alternatif = sorte suivante

// Types d'objets (itp_type_* du module system)
constexpr int kTypeOneHanded = 2, kTypeTwoHanded = 3, kTypePolearm = 4;
constexpr int kTypeBow = 8, kTypeCrossbow = 9, kTypeThrown = 10, kTypePistol = 16, kTypeMusket = 17;

bool IsMelee(int type)
{
    return type == kTypeOneHanded || type == kTypeTwoHanded || type == kTypePolearm;
}
struct Attack {                             // mbAttack
    int32_t type;                           // +0x00
    bool hitAgent, hitHorse, hitMissionObject; // +0x04
    float power;                            // +0x08
    float holdTime;                         // +0x0C
    bool crushthrough, blocked;             // +0x10
    uint8_t pad[2];
    rgl::Transform prevSword;               // +0x14
    rgl::Transform curSword;                // +0x54
    uint8_t pad2[0x98 - 0x94];
    uint32_t* hitAgentMaskBegin;            // +0x98 rglVector<unsigned int>
    uint32_t* hitAgentMaskEnd;              // +0xA0
};
static_assert(offsetof(Attack, hitAgentMaskBegin) == 0x98);
static_assert(offsetof(Attack, prevSword) == 0x14);
static_assert(offsetof(Attack, curSword) == 0x54);
struct Blow {                               // mbBlow (début)
    int32_t type;                           // +0x00
    uint32_t flags;                         // +0x04
    int32_t agentNo;                        // +0x08
    rgl::Vector4 position;                  // +0x0C
    rgl::Vector4 rotation;                  // +0x1C
    rgl::Vector4 direction;                 // +0x2C
    int32_t boneNo;                         // +0x3C
    bool rearHorse;                         // +0x40
    float rawDamage;                        // +0x44
    float inflictedDamage;                  // +0x48
    int32_t soakedDamage;                   // +0x4C
    int32_t damageType;                     // +0x50
};
static_assert(offsetof(Blow, boneNo) == 0x3C);
static_assert(offsetof(Blow, damageType) == 0x50);

using UpdateMeleeFn = uintptr_t(__fastcall*)(uint8_t* agent, float dt);
UpdateMeleeFn g_updateMelee = nullptr;
void* g_updateMeleeTarget = nullptr;
using CheckDefendFn = int(__fastcall*)(void* self, int agentA, int agentB, void* missile, rgl::Vector4* v1,
                                       rgl::Vector4* v2, int mode, float* out);
CheckDefendFn g_checkDefend = nullptr;
void* g_checkDefendTarget = nullptr;
int g_loggedDefends = 0;

// Bouclier toujours actif : un coup ou un projectile qui passe à moins de ShieldRadius() du
// centre du bouclier du joueur est paré (code 5 de checkDefendCollision, comme une parade
// au bouclier du jeu), sans tenir la défense
constexpr int kDefendNone = 0, kDefendShield = 5;
constexpr int kTypeShield = 7;
float ShieldRadius() { return config::Get(config::Id::ShieldRadius); }
int g_loggedShieldBlocks = 0;

float DistancePointSegment(const rgl::Vector4& p, const rgl::Vector4& a, const rgl::Vector4& b)
{
    const float abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
    const float apx = p.x - a.x, apy = p.y - a.y, apz = p.z - a.z;
    const float len2 = abx * abx + aby * aby + abz * abz;
    float t = len2 > 1e-8f ? (apx * abx + apy * aby + apz * abz) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float dx = apx - t * abx, dy = apy - t * aby, dz = apz - t * abz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
using CheckAttackFn = int(__fastcall*)(void* self, int agentNo, int32_t* item, void* missile, Blow* blow,
                                       Attack* attack, float* out);
CheckAttackFn g_checkAttack = nullptr;
void* g_checkAttackTarget = nullptr;
uint8_t** g_mission = nullptr;   // adresse de la variable globale g_mission
uint8_t** g_itemKinds = nullptr; // adresse de la variable globale g_itemKinds
uint8_t** g_game = nullptr;      // adresse de la variable globale g_game
constexpr int kGameCameraMode = 0x13DF0; // mbGame::m_cameraMode (0 : 3e personne, 1 : 1re personne)
// mov rax, [g_game] ; cmp dword ptr [rax+13DF0h], 0
constexpr char kGameCameraModeSig[] = "48 8B 05 ?? ?? ?? ?? 83 B8 F0 3D 01 00 00";
const uint8_t* g_viewAgent = nullptr; // agent du joueur pour lequel la vue a été vérifiée
// Vue 1re personne par défaut : R est une bascule, et le jeu peut l'ignorer ou remettre la 3e
// personne pendant l'apparition. On vérifie donc la vue pendant quelques secondes et on renvoie R
// tant qu'elle est en 3e personne (sauf si le joueur a changé de vue lui-même).
constexpr ULONGLONG kViewCheckMs = 5000;   // durée de la surveillance après l'apparition
constexpr ULONGLONG kViewRetryMs = 800;    // délai avant de renvoyer R (le temps que le jeu bascule)
constexpr int kViewMaxRequests = 4;
ULONGLONG g_viewSinceMs = 0, g_viewRequestMs = 0;
int g_viewRequests = 0;
int g_logged = 0;
int g_loggedStarts = 0;

// Arme toujours active
float ActiveStartSpeed() { return config::Get(config::Id::ActiveStartSpeed); } // m/s de la main : la lame frappe
float ActiveEndSpeed() { return config::Get(config::Id::ActiveEndSpeed); } // en deçà : fin du mouvement
// Puissance du coup (mbAttack::m_power, 1 par défaut) selon la vitesse de la main : hors des
// animations d'attaque du jeu, les dégâts bruts sont très faibles (~10 contre ~200)
float PowerPerSpeed() { return config::Get(config::Id::PowerPerSpeed); } // puissance par m/s de main
float MaxPower() { return config::Get(config::Id::MaxPower); }
bool g_f10Down = false;
unsigned long long g_playerMeleeCalls = 0, g_lastMeleeLog = 0;
bool g_swinging = false;
bool g_swingHit = false; // le mouvement en cours a déjà touché : plus de balayage jusqu'au suivant

// Type d'attaque (mbAttack::m_type) : 0 estoc, 1 taille par la droite, 2 par la gauche, 3 par le
// haut. mbMission::calculateDamage prend les dégâts d'estoc (perçants) pour le type 0 : il doit
// suivre le vrai mouvement de la lame, sinon une taille est comptée comme un estoc (dégâts
// presque nuls quand la lame avance de travers).
constexpr int kAttackThrust = 0, kAttackRight = 1, kAttackLeft = 2, kAttackOverhead = 3;
constexpr float kBladeProbe = 0.5f; // point de la lame suivi (m depuis la poignée)

// mbMission::getSweetSpotFactor : les dégâts sont multipliés par une courbe de la progression de
// l'animation d'attaque (canal du haut du corps, mbAgentActionChannel::m_progress), nulle hors
// de la fenêtre utile du coup. Hors attaque du jeu, la progression est celle de la pose de repos :
// estocs et coups à cheval tombaient au plancher (3,6 de dégâts bruts). Pendant notre balayage,
// on la place au milieu du plateau (facteur 1) : estoc [0,40 ; 0,60], par le haut [0,35 ; 1],
// tailles [0,45 ; 0,70] à cheval et [0,20 ; 0,80] à pied.
// cmp byte ptr [rglConfig::Battle::bAdjustedSweetSpot], 0 ; movss xmm2, [r9 + m_progress]
constexpr char kSweetSpotProgressSig[] = "80 3D ?? ?? ?? ?? 00 F3 41 0F 10 91 ?? ?? ?? ??";
constexpr float kSweetSpotProgress = 0.5f;
uint32_t g_attackProgressOffset = 0; // mbAgent : progression du canal d'animation des attaques

int AttackTypeFromMotion(const rgl::Transform& prev, const rgl::Transform& cur, int fallback)
{
    auto point = [](const rgl::Transform& t) {
        return rgl::Vector4{t.o.x + t.f.x * kBladeProbe, t.o.y + t.f.y * kBladeProbe, t.o.z + t.f.z * kBladeProbe, 0};
    };
    const rgl::Vector4 a = point(prev), b = point(cur);
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float flen = std::sqrt(cur.f.x * cur.f.x + cur.f.y * cur.f.y + cur.f.z * cur.f.z);
    if (len < 0.005f || flen < 1e-4f)
        return fallback;
    // La lame avance dans son axe : estoc
    if ((dx * cur.f.x + dy * cur.f.y + dz * cur.f.z) / (len * flen) > 0.7f)
        return kAttackThrust;
    // Elle descend : coup par le haut
    if (dz / len < -0.6f)
        return kAttackOverhead;
    // Sinon taille : vers la gauche du joueur = coup venant de la droite
    rgl::Vector4 back;
    if (!GetBodyBack(back))
        return fallback == kAttackThrust ? kAttackRight : fallback;
    const float rightX = -back.y, rightY = back.x; // avant = -arrière, droite = avant x haut
    return dx * rightX + dy * rightY < 0 ? kAttackRight : kAttackLeft;
}
// Arme tenue par le joueur, lue à chaque frame dans son agent
struct Weapon {
    int32_t item[2]{-1, 0}; // mbItem passé au jeu : sorte (usage alternatif compris), drapeaux
    int type = 0;
};
Weapon g_weapon;
int g_loggedWeaponKind = -2;
int g_loggedActive = 0;

int PlayerAgentNo()
{
    const uint8_t* mission = g_mission ? *g_mission : nullptr;
    return mission ? *reinterpret_cast<const int32_t*>(mission + kMissionPlayerAgentNo) : -1;
}

int __fastcall HkCheckAttack(void* self, int agentNo, int32_t* item, void* missile, Blow* blow, Attack* attack,
                             float* out)
{
    // Pose précédente sentinelle (-2e7) : premier appel d'un coup
    const bool firstOfAttack = attack && attack->prevSword.o.x < -1e6f;
    const int result = g_checkAttack(self, agentNo, item, missile, blow, attack, out);
    if (missile || agentNo != PlayerAgentNo() || !attack)
        return result;
    if (firstOfAttack && g_loggedStarts < 20) {
        ++g_loggedStarts;
        LOG_ENGINE("debut de coup du joueur : arme %d, attaque type %d, puissance %.2f, maintien %.2f",
                   item ? item[0] : -1, attack->type, attack->power, attack->holdTime);
    }
    if (result != 0 && g_logged < 40) {
        ++g_logged;
        const rgl::Transform& c = attack->curSword;
        const rgl::Transform& p = attack->prevSword;
        LOG_ENGINE("impact du joueur : resultat %d, attaque type %d puissance %.2f, bloque %d, sortie %.2f", result,
                   attack->type, attack->power, attack->blocked, out ? *out : 0.0f);
        LOG_ENGINE("  arme avant o=(%.2f %.2f %.2f) ; apres o=(%.2f %.2f %.2f) f=(%.2f %.2f %.2f)", p.o.x, p.o.y,
                   p.o.z, c.o.x, c.o.y, c.o.z, c.f.x, c.f.y, c.f.z);
        if (blow)
            LOG_ENGINE("  impact : agent %d os %d, degats bruts %.1f infliges %.1f, type %d, flags 0x%X", blow->agentNo,
                       blow->boneNo, blow->rawDamage, blow->inflictedDamage, blow->damageType, blow->flags);
    }
    return result;
}

// Agent du joueur dans la mission (blocs de 16 agents), nullptr hors mission
uint8_t* PlayerAgent()
{
    uint8_t* mission = g_mission ? *g_mission : nullptr;
    if (!mission)
        return nullptr;
    const int no = *reinterpret_cast<int32_t*>(mission + kMissionPlayerAgentNo);
    auto** blocks = *reinterpret_cast<uint8_t***>(mission + kMissionAgentBlocks);
    if (no < 0 || !blocks || !blocks[no >> 4])
        return nullptr;
    if (!g_agentSize)
        return nullptr;
    uint8_t* agent = blocks[no >> 4] + (no & 15) * g_agentSize;
    if (!*reinterpret_cast<int32_t*>(agent + kAgentValid) || *reinterpret_cast<int32_t*>(agent + kAgentNo) != no)
        return nullptr;
    return agent;
}

// Arme en main droite de l'agent, comme la construit mbAgent::updateMeleeAttackAction
Weapon ReadWeapon(const uint8_t* agent)
{
    Weapon w;
    uint8_t* kinds = g_itemKinds ? *g_itemKinds : nullptr;
    const int slot = *reinterpret_cast<const int32_t*>(agent + kAgentWielded);
    if (!kinds || slot < 0 || slot >= 10)
        return w;
    const auto* entry = reinterpret_cast<const int32_t*>(agent + kAgentItems + slot * 8);
    int kind = entry[0];
    if (kind < 0)
        return w;
    const uint64_t props = *reinterpret_cast<const uint64_t*>(kinds + kind * kItemKindSize + kItemKindProperties);
    if (*reinterpret_cast<const int32_t*>(agent + kAgentAltUsages + slot * 4) && (props & kPropNextItemAsMelee))
        ++kind;
    w.item[0] = kind;
    w.item[1] = entry[1];
    w.type = *(kinds + kind * kItemKindSize + kItemKindProperties); // octet bas des propriétés
    return w;
}

bool PlayerHasShield();

// Défense : agentA attaque agentB (v1-v2 : arme de l'attaquant, ou trajet d'un projectile)
int __fastcall HkCheckDefend(void* self, int agentA, int agentB, void* missile, rgl::Vector4* v1, rgl::Vector4* v2,
                             int mode, float* out)
{
    int result = g_checkDefend(self, agentA, agentB, missile, v1, v2, mode, out);
    const int player = PlayerAgentNo();
    rgl::Vector4 center;
    if (agentB == player && result == kDefendNone && v1 && v2 && config::GetBool(config::Id::ShieldAlwaysActive) &&
        PlayerHasShield() &&
        GetPlayerShieldCenter(center)) {
        const float d = DistancePointSegment(center, *v1, *v2);
        if (d < ShieldRadius()) {
            result = kDefendShield;
            if (g_loggedShieldBlocks < 40) {
                ++g_loggedShieldBlocks;
                LOG_ENGINE("bouclier toujours actif : %s de l'agent %d pare (%.2f m du centre)",
                           missile ? "projectile" : "coup", agentA, d);
            }
        }
    }
    if ((agentA == player || agentB == player) && g_loggedDefends < 60) {
        ++g_loggedDefends;
        LOG_ENGINE("defense : agents %d -> %d (joueur %d), projectile %p, mode %d, resultat %d, sortie %.2f, "
                   "v1 (%.2f %.2f %.2f) v2 (%.2f %.2f %.2f)",
                   agentA, agentB, player, missile, mode, result, out ? *out : 0.0f, v1 ? v1->x : 0, v1 ? v1->y : 0,
                   v1 ? v1->z : 0, v2 ? v2->x : 0, v2 ? v2->y : 0, v2 ? v2->z : 0);
    }
    return result;
}

// Bouclier en main gauche (emplacement tenu n° 2 de l'agent)
bool PlayerHasShield()
{
    const uint8_t* agent = PlayerAgent();
    uint8_t* kinds = g_itemKinds ? *g_itemKinds : nullptr;
    if (!agent || !kinds)
        return false;
    const int slot = *reinterpret_cast<const int32_t*>(agent + kAgentWielded + 4);
    if (slot < 0 || slot >= 10)
        return false;
    const int kind = *reinterpret_cast<const int32_t*>(agent + kAgentItems + slot * 8);
    return kind >= 0 && *(kinds + kind * kItemKindSize + kItemKindProperties) == kTypeShield;
}

// Début d'un balayage, comme le jeu en début de coup (mbAttack::clear + poses sentinelles)
void StartSweep(Attack* a)
{
    a->type = kAttackRight;
    a->power = 1.0f;
    a->hitAgent = a->hitHorse = a->hitMissionObject = false;
    a->crushthrough = a->blocked = false;
    for (uint32_t* m = a->hitAgentMaskBegin; m && m < a->hitAgentMaskEnd; ++m)
        *m = 0;
    a->prevSword.o.x = kSentinel;
    a->curSword.o.x = kSentinel;
}

void PollToggle()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F10) & 0x8000);
    if (down && !g_f10Down)
        LOG_ENGINE("F10 : arme toujours active %s", xr::ToggleAlwaysActive() ? "ACTIVEE" : "desactivee");
    g_f10Down = down;
}

uintptr_t __fastcall HkUpdateMelee(uint8_t* agent, float dt)
{
    const uintptr_t result = g_updateMelee(agent, dt);
    if (!agent || *reinterpret_cast<int32_t*>(agent + kAgentNo) != PlayerAgentNo())
        return result;
    PollToggle();
    auto* attack = reinterpret_cast<Attack*>(agent + kAgentAttack);
    const bool gameAttacking = *reinterpret_cast<int32_t*>(agent + kAgentAttackPhase) == 2;
    ++g_playerMeleeCalls;
    const unsigned long long now = GetTickCount64();
    if (now - g_lastMeleeLog > 10000) {
        LOG_ENGINE("arme toujours active : %s, %llu mises a jour du joueur, lame %.1f m/s",
                   xr::IsAlwaysActive() ? "oui" : "non", g_playerMeleeCalls, xr::GetBladeSpeed());
        g_lastMeleeLog = now;
    }
    const Weapon weapon = ReadWeapon(agent);
    if (!xr::IsAlwaysActive() || gameAttacking || weapon.item[0] < 0 || !IsMelee(weapon.type)) {
        g_swinging = false;
        return result;
    }
    const float speed = xr::GetBladeSpeed();
    if (!g_swinging && speed >= ActiveStartSpeed()) {
        g_swinging = true;
        g_swingHit = false;
        StartSweep(attack);
    } else if (g_swinging && speed < ActiveEndSpeed()) {
        g_swinging = false;
        g_swingHit = false;
        StartSweep(attack); // prêt pour le prochain mouvement
        return result;
    }
    // Un seul coup par mouvement, comme une attaque du jeu : sinon la lame qui traverse le corps
    // touche à chaque image, en petits coups presque sans dégâts
    if (!g_swinging || g_swingHit)
        return result;
    attack->power = std::min(MaxPower(), std::max(1.0f, speed * PowerPerSpeed()));
    // Direction du coup d'après le dernier déplacement de la lame (le jeu recopie cur dans prev
    // au début de l'appel : ce sont les poses de l'image précédente)
    if (attack->prevSword.o.x > kSentinel / 2 && attack->curSword.o.x > kSentinel / 2)
        attack->type = AttackTypeFromMotion(attack->prevSword, attack->curSword, attack->type);

    alignas(16) uint8_t blow[0x200]{};
    float out = 0;
    int32_t item[2] = {weapon.item[0], weapon.item[1]};
    uint8_t* mission = g_mission ? *g_mission : nullptr;
    // Progression de l'animation d'attaque au plateau du facteur de dégâts, le temps de l'appel
    float* progress = g_attackProgressOffset ? reinterpret_cast<float*>(agent + g_attackProgressOffset) : nullptr;
    const float savedProgress = progress ? *progress : 0.0f;
    if (progress)
        *progress = kSweetSpotProgress;
    const int hit = g_checkAttack(mission, *reinterpret_cast<int32_t*>(agent + kAgentNo), item, nullptr,
                                  reinterpret_cast<Blow*>(blow), attack, &out);
    if (progress)
        *progress = savedProgress;
    if (hit != 0)
        g_swingHit = true;
    if (hit != 0 && g_loggedActive < 40) {
        ++g_loggedActive;
        static const char* const kTypes[] = {"estoc", "taille droite", "taille gauche", "par le haut"};
        const Blow* b = reinterpret_cast<const Blow*>(blow);
        LOG_ENGINE("arme active : resultat %d, agent %d os %d, %s, degats bruts %.1f infliges %.1f (lame %.1f m/s, "
                   "puissance %.1f)",
                   hit, b->agentNo, b->boneNo, attack->type >= 0 && attack->type < 4 ? kTypes[attack->type] : "?",
                   b->rawDamage, b->inflictedDamage, speed, attack->power);
    }
    return result;
}

} // namespace

bool PlayerHoldsTwoHandedWeapon()
{
    return g_twoHandedHeld;
}

bool PlayerHoldsMeleeWeapon()
{
    return g_meleeHeld;
}

bool PlayerAimingRangedWeapon()
{
    return g_rangedAiming;
}

void UpdatePlayerWeapon()
{
    const uint8_t* agent = PlayerAgent();
    const Weapon w = agent ? ReadWeapon(agent) : Weapon{};
    g_weapon = w;
    g_meleeHeld = w.item[0] >= 0 && IsMelee(w.type);
    g_twoHandedHeld = w.item[0] >= 0 && (w.type == kTypeTwoHanded || w.type == kTypePolearm);
    // Nouvel agent du joueur (début de mission) en vue 3e personne : passer en 1re personne,
    // une fois ; le joueur reste libre de rechanger (clic du stick droit)
    const ULONGLONG now = GetTickCount64();
    if (agent && agent != g_viewAgent) {
        g_viewAgent = agent;
        g_viewSinceMs = now;
        g_viewRequestMs = 0;
        g_viewRequests = 0;
        xr::TakeManualViewToggle(); // un changement d'une mission précédente ne compte pas
    } else if (!agent) {
        g_viewAgent = nullptr;
    }
    if (agent && g_viewSinceMs && now - g_viewSinceMs < kViewCheckMs) {
        const uint8_t* game = g_game ? *g_game : nullptr;
        if (xr::TakeManualViewToggle()) {
            g_viewSinceMs = 0; // le joueur choisit sa vue
        } else if (game && *reinterpret_cast<const int32_t*>(game + kGameCameraMode) == 0 &&
                   g_viewRequests < kViewMaxRequests && (!g_viewRequestMs || now - g_viewRequestMs >= kViewRetryMs)) {
            if (g_viewRequests > 0)
                LOG_ENGINE("vue toujours en 3e personne %llu ms apres l'apparition : touche R renvoyee",
                           now - g_viewSinceMs);
            g_viewRequestMs = now;
            ++g_viewRequests;
            xr::RequestFirstPersonView();
        }
    }
    xr::SetBowMode(w.type == kTypeBow);
    xr::SetTriggerAttackMode(w.type == kTypeCrossbow || w.type == kTypePistol || w.type == kTypeMusket ||
                             w.type == kTypeThrown);
    // Visée / tir : phase d'attaque du joueur non nulle (bander, viser, tirer)
    const bool gun = agent != nullptr && (w.type == kTypeCrossbow || w.type == kTypePistol || w.type == kTypeMusket ||
                                          w.type == kTypeThrown);
    const bool ranged = gun || (agent != nullptr && w.type == kTypeBow);
    // Phase d'attaque : 1 = visée, 5 = rechargement (alterne avec 0 d'une frame à l'autre), 0 = repos
    const int phase = ranged ? *reinterpret_cast<const int32_t*>(agent + kAgentAttackPhase) : -1;
    // Prise de l'animation du jeu pendant la visée, le tir, le rechargement, et encore 1 s après
    if (ranged && phase != 0)
        g_gripUntilMs = now + kGripAfterShotMs;
    g_rangedAiming = ranged && now < g_gripUntilMs;
    if (ranged && phase != g_loggedPhase && g_phaseLogs < 40) {
        LOG_ENGINE("arme a distance : phase d'attaque %d", phase);
        ++g_phaseLogs;
    }
    g_loggedPhase = phase;

    // Arbalète, armes à feu, armes de jet : réticule sur la main droite ; source d'avant
    // restaurée en changeant d'arme
    if (gun && !g_gunAim) {
        g_savedAimSource = xr::GetAimSource();
        xr::SetAimSource(xr::AimSource::RightHand);
        LOG_ENGINE("reticule : main droite (arme a feu / arbalete)");
    } else if (!gun && g_gunAim && agent) {
        xr::SetAimSource(g_savedAimSource);
        LOG_ENGINE("reticule : source precedente restauree");
    }
    if (agent)
        g_gunAim = gun;
    if (w.item[0] != g_loggedWeaponKind) {
        static const char* const kNames[] = {"aucun",   "cheval",  "une main", "deux mains", "hast",     "fleches",
                                             "carreaux", "bouclier", "arc",      "arbalete",   "jet",      "?",
                                             "?",        "?",        "?",        "?",          "pistolet", "mousquet"};
        const char* name = w.type >= 0 && w.type < 18 ? kNames[w.type] : "?";
        LOG_ENGINE("arme du joueur : sorte %d, type %d (%s)", w.item[0], w.type, agent ? name : "hors mission");
        g_loggedWeaponKind = w.item[0];
    }
}

bool InstallCombatHook()
{
    auto* updateCamera = static_cast<uint8_t*>(pattern::Find("mbMission::updateCamera", kUpdateCameraSig));
    void* checkAttack = pattern::Find("mbMission::checkAttackCollision", kCheckAttackSig);
    if (!updateCamera || !checkAttack) {
        LOG_ENGINE("combat physique desactive (signature manquante)");
        return false;
    }
    g_attackProgressOffset = pattern::FindImm32("mbAgent : progression de l'animation d'attaque",
                                                kSweetSpotProgressSig, 12);
    if (g_attackProgressOffset < 0x800 || g_attackProgressOffset > 0x1000) {
        LOG_ENGINE("progression de l'animation d'attaque introuvable (0x%X) : estocs et coups a cheval de "
                   "l'arme toujours active affaiblis", g_attackProgressOffset);
        g_attackProgressOffset = 0;
    } else {
        LOG_ENGINE("progression de l'animation d'attaque : mbAgent+0x%X", g_attackProgressOffset);
    }
    // mov rbx, [rip+disp32] : adresse = instruction suivante + disp32
    const uint8_t* load = updateCamera + kMissionLoadOffset;
    int32_t disp = 0;
    memcpy(&disp, load + 3, sizeof(disp));
    g_mission = reinterpret_cast<uint8_t**>(const_cast<uint8_t*>(load + 7 + disp));
    g_itemKinds = static_cast<uint8_t**>(pattern::FindGlobal("g_itemKinds", kItemKindsLoadSig, 3, 7));
    g_agentSize = pattern::FindImm32("taille de mbAgent", kAgentSizeSig, 13);
    g_game = static_cast<uint8_t**>(pattern::FindGlobal("g_game", kGameCameraModeSig, 3, 7));
    LOG_ENGINE("g_mission : exe+0x%llX",
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_mission) -
                                               reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    st = MH_CreateHook(checkAttack, &HkCheckAttack, reinterpret_cast<void**>(&g_checkAttack));
    if (st == MH_OK)
        st = MH_EnableHook(checkAttack);
    if (st != MH_OK) {
        LOG_ENGINE("ERREUR : hook checkAttackCollision : %s", MH_StatusToString(st));
        MH_RemoveHook(checkAttack);
        return false;
    }
    g_checkAttackTarget = checkAttack;
    LOG_ENGINE("mbMission::checkAttackCollision detourne (observation)");

    void* checkDefend = pattern::Find("mbMission::checkDefendCollision", kCheckDefendSig);
    if (checkDefend) {
        st = MH_CreateHook(checkDefend, &HkCheckDefend, reinterpret_cast<void**>(&g_checkDefend));
        if (st == MH_OK)
            st = MH_EnableHook(checkDefend);
        if (st == MH_OK) {
            g_checkDefendTarget = checkDefend;
            LOG_ENGINE("mbMission::checkDefendCollision detourne (observation)");
        } else {
            MH_RemoveHook(checkDefend);
        }
    }

    void* updateAccuracy = pattern::Find("mbAgent::updateAccuracy", kUpdateAccuracySig);
    if (updateAccuracy) {
        st = MH_CreateHook(updateAccuracy, &HkUpdateAccuracy, reinterpret_cast<void**>(&g_updateAccuracy));
        if (st == MH_OK)
            st = MH_EnableHook(updateAccuracy);
        if (st == MH_OK) {
            g_updateAccuracyTarget = updateAccuracy;
            LOG_ENGINE("mbAgent::updateAccuracy detourne (precision de l'arc selon l'ecart des mains)");
        }
    } else {
        LOG_ENGINE("precision de l'arc VR desactivee (signature manquante)");
    }

    void* updateMelee = pattern::Find("mbAgent::updateMeleeAttackAction", kUpdateMeleeSig);
    if (updateMelee) {
        st = MH_CreateHook(updateMelee, &HkUpdateMelee, reinterpret_cast<void**>(&g_updateMelee));
        if (st == MH_OK)
            st = MH_EnableHook(updateMelee);
        if (st == MH_OK) {
            g_updateMeleeTarget = updateMelee;
            LOG_ENGINE("mbAgent::updateMeleeAttackAction detourne (arme toujours active : F10)");
        } else {
            MH_RemoveHook(updateMelee);
            LOG_ENGINE("ERREUR : hook updateMeleeAttackAction : %s", MH_StatusToString(st));
        }
    }
    return true;
}

void RemoveCombatHook()
{
    if (g_updateAccuracyTarget) {
        MH_DisableHook(g_updateAccuracyTarget);
        MH_RemoveHook(g_updateAccuracyTarget);
        g_updateAccuracyTarget = nullptr;
    }
    if (g_checkDefendTarget) {
        MH_DisableHook(g_checkDefendTarget);
        MH_RemoveHook(g_checkDefendTarget);
        g_checkDefendTarget = nullptr;
    }
    if (g_updateMeleeTarget) {
        MH_DisableHook(g_updateMeleeTarget);
        MH_RemoveHook(g_updateMeleeTarget);
        g_updateMeleeTarget = nullptr;
    }
    if (!g_checkAttackTarget)
        return;
    MH_DisableHook(g_checkAttackTarget);
    MH_RemoveHook(g_checkAttackTarget);
    g_checkAttackTarget = nullptr;
}

} // namespace engine
