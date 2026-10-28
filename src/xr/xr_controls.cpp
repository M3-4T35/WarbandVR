#include "xr/xr_controls.h"
#include "engine/orders.h"
#include "input/dinput_hook.h"
#include "input/pointer.h"
#include "util/config.h"
#include "util/log.h"
#include "xr/xr_session.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#define LOG_XR(...) ::vrlog::Write("[xr]", __VA_ARGS__)

namespace xr {
namespace {

// Menus
constexpr ULONGLONG kScrollRepeatMs = 150; // un cran de molette toutes les 150 ms, stick poussé
constexpr float kScrollThreshold = 0.5f;

// Combat
constexpr float kMoveThreshold = 0.4f;    // stick -> touche de déplacement
float TurnDeadzone() { return config::Get(config::Id::TurnDeadzone); }
float TurnRate() { return config::Get(config::Id::TurnSpeed) * 3.14159265f / 180.0f; } // radians par seconde
constexpr float kActionThreshold = 0.5f;  // gâchettes / poignées analogiques
constexpr float kTriggerRelease = 0.3f;   // gâchette droite déjà tenue : relâchée en deçà (hystérésis)
float JumpThreshold() { return config::Get(config::Id::JumpThreshold); } // stick droit vers le haut -> saut

// Attaque au geste (manette droite), vitesses en m/s
float SwingStartSpeed() { return config::Get(config::Id::SwingSpeed); } // au-delà : un coup commence
float ThrustDominance() { return config::Get(config::Id::ThrustDominance); } // une taille avance aussi
constexpr ULONGLONG kSwingHoldMs = 40;      // relâché aussitôt : le coup part dès que le jeu l'a armé
constexpr ULONGLONG kSwingCooldownMs = 250;
constexpr int kSwingMouse = 30;           // mouvement de souris qui donne la direction au jeu
ULONGLONG SwingWindowMs() { return static_cast<ULONGLONG>(config::Get(config::Id::SwingWindowMs)); } // mesure de la trajectoire
float SwingMinDistance() { return config::Get(config::Id::SwingMinDistance); } // déplacement minimal (m)

// Touches par défaut du jeu (codes DIK de dinput.h)
constexpr int kKeyW = 0x11, kKeyA = 0x1E, kKeyS = 0x1F, kKeyD = 0x20;
constexpr int kKeySpace = 0x39, kKeyF = 0x21, kKeyTab = 0x0F, kKeyEscape = 0x01;
constexpr int kKeyR = 0x13;  // vue 1re / 3e personne
constexpr int kKeyE = 0x12;  // coup de pied
constexpr int kKeyX = 0x2D;  // mode de l'arme (« ToggleWeaponMode » : baïonnette, une / deux mains, lance couchée)
constexpr int kKeyCtrl = 0x1D;   // Ctrl gauche
constexpr int kKeyNum1 = 0x4F, kKeyNum3 = 0x51; // pavé numérique : rotation de la caméra de la carte du monde
// Menu radial : F1-F8 (ordres), 0 (toutes les troupes), 1-3 (infanterie, archers, cavalerie)
constexpr int kOrderKeys[8] = {0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42};
constexpr int kKeyGroupAll = 0x0B, kKeyGroup1 = 0x02, kKeyGroup2 = 0x03, kKeyGroup3 = 0x04;
constexpr float kBladeProbe = 0.4f;     // point de la lame (m depuis la poignée) dont on mesure la vitesse
constexpr float kRadialSelect = 0.6f;  // stick poussé au-delà : entrée visée
constexpr float kRadialRelease = 0.3f; // retour en deçà : entrée validée
constexpr ULONGLONG kTapMs = 80;       // durée d'appui d'une touche envoyée par le menu
constexpr int kHoldFlag = 0x10000;     // dans la file : touche à maintenir (et non à taper)
constexpr ULONGLONG kGapMs = 80;
constexpr ULONGLONG kRecenterHoldMs = 800; // bouton menu tenu : recentrage (sinon Échap)
constexpr ULONGLONG kWeaponModeHoldMs = 600; // bouton « changer d'arme » tenu : mode de l'arme
constexpr ULONGLONG kViewHoldMs = 1000;      // clic du stick droit tenu : vue 1re / 3e personne
constexpr ULONGLONG kAimHoldMs = 1000;       // X tenu : source du réticule (appui court = Échap)
constexpr float kSnapPush = 0.7f, kSnapRelease = 0.3f; // rotation par crans       // pause entre deux touches d'une séquence
constexpr ULONGLONG kSettleMs = 200;   // après la dernière touche : le menu du jeu est à jour
constexpr ULONGLONG kGiveUpMs = 800;   // sans réponse du jeu : on abandonne l'attente
constexpr int kKeyBack = 0x0E; // Retour arrière : écran d'ordres (carte de la bataille, ViewOrders)

XrPath Path(XrInstance instance, const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(instance, s, &p);
    return p;
}

// Associations d'un profil ; nullptr = pas d'association sur ce profil.
// Les chemins sont relatifs à /user/hand/left/ (L) ou /user/hand/right/ (R).
struct Profile {
    const char* name;
    // Menu
    const char* select;
    const char* secondaryL;
    const char* secondaryR;
    const char* scroll;
    const char* rotate; // stick horizontal : rotation de la caméra de la carte
    const char* ctrl;   // Ctrl maintenu
    // Combat, main gauche
    const char* moveL;
    const char* aimModeL; // source du réticule
    const char* escapeL;
    const char* tabL;
    const char* ordersL;
    const char* blockL;
    const char* useL;
    // Combat, main droite
    const char* turnR;
    const char* viewR;
    const char* weaponR;
    const char* drawR;   // arc : bander
    const char* attackR; // attaque de secours (les coups se donnent au geste)
    const char* shieldR; // mettre / retirer le bouclier
    // Échap sur un bouton de la main gauche que le runtime laisse au jeu (SteamVR garde le bouton
    // menu pour son tableau de bord) : appui court = Échap, X tenu = source du réticule en combat
    const char* escapeAltL = nullptr;
};

const Profile kProfiles[] = {
    {"/interaction_profiles/khr/simple_controller", "input/select/click", nullptr, nullptr, nullptr,
     nullptr, nullptr,
     nullptr, nullptr, "input/menu/click", nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr},
    {"/interaction_profiles/oculus/touch_controller", "input/trigger/value", "input/y/click", "input/b/click",
     "input/thumbstick/y", "input/thumbstick/x", "input/squeeze/value",
     "input/thumbstick", "input/x/click", "input/menu/click", "input/y/click", "input/thumbstick/click",
     "input/trigger/value", "input/squeeze/value",
     "input/thumbstick", "input/thumbstick/click", "input/a/click", "input/trigger/value", "input/squeeze/value",
     "input/b/click", "input/x/click"},
    {"/interaction_profiles/valve/index_controller", "input/trigger/click", "input/b/click", "input/b/click",
     "input/thumbstick/y", "input/thumbstick/x", "input/squeeze/value",
     "input/thumbstick", "input/a/click", nullptr, "input/b/click", "input/thumbstick/click", "input/trigger/click",
     "input/squeeze/value",
     "input/thumbstick", "input/thumbstick/click", "input/a/click", "input/trigger/click", "input/squeeze/value",
     "input/b/click", "input/a/click"},
    {"/interaction_profiles/htc/vive_controller", "input/trigger/click", "input/squeeze/click",
     "input/squeeze/click", "input/trackpad/y",
     "input/trackpad/x", nullptr,
     "input/trackpad", "input/trackpad/click", "input/menu/click", nullptr, nullptr, "input/trigger/click",
     "input/squeeze/click",
     "input/trackpad", "input/trackpad/click", "input/menu/click", "input/trigger/click", "input/squeeze/click"},
    {"/interaction_profiles/microsoft/motion_controller", "input/trigger/value", "input/squeeze/click",
     "input/squeeze/click", "input/thumbstick/y",
     "input/thumbstick/x", nullptr,
     "input/thumbstick", "input/thumbstick/click", "input/menu/click", "input/trackpad/click", nullptr,
     "input/trigger/value", "input/squeeze/click",
     "input/thumbstick", "input/thumbstick/click", "input/trackpad/click", "input/trigger/value",
     "input/squeeze/click"},
};

bool GetBool(XrSession session, XrAction action, XrPath hand)
{
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    gi.subactionPath = hand;
    XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &b)) && b.isActive && b.currentState;
}

float GetFloat(XrSession session, XrAction action, XrPath hand)
{
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    gi.subactionPath = hand;
    XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &f)) && f.isActive ? f.currentState : 0.0f;
}

XrVector2f GetVector(XrSession session, XrAction action)
{
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
    return XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &v)) && v.isActive ? v.currentState : XrVector2f{};
}

} // namespace

bool XrControls::Init(XrInstance instance, XrSession session)
{
    instance_ = instance;
    // Langue des libellés à nous (choix des troupes) : celle de l'outil de configuration
    french_ = config::LoadLanguage(config::DefaultPath()) == config::Lang::French;
    hands_[0] = Path(instance, "/user/hand/left");
    hands_[1] = Path(instance, "/user/hand/right");

    auto makeSet = [&](XrActionSet& set, const char* name, const char* label, uint32_t priority) {
        XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(si.actionSetName, name);
        strcpy_s(si.localizedActionSetName, label);
        si.priority = priority;
        return XR_SUCCEEDED(xrCreateActionSet(instance, &si, &set));
    };
    auto makeAction = [&](XrActionSet set, XrAction& a, XrActionType type, const char* name, const char* label,
                          bool perHand) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        ci.actionType = type;
        strcpy_s(ci.actionName, name);
        strcpy_s(ci.localizedActionName, label);
        if (perHand) {
            ci.countSubactionPaths = 2;
            ci.subactionPaths = hands_;
        }
        return XR_SUCCEEDED(xrCreateAction(set, &ci, &a));
    };
    const bool created =
        makeSet(menuSet_, "menu", "Menus", 0) && makeSet(combatSet_, "combat", "Combat", 0) &&
        makeAction(menuSet_, aim_, XR_ACTION_TYPE_POSE_INPUT, "aim", "Viser", true) &&
        makeAction(menuSet_, select_, XR_ACTION_TYPE_BOOLEAN_INPUT, "select", "Cliquer", true) &&
        makeAction(menuSet_, secondary_, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "Clic droit", true) &&
        makeAction(menuSet_, scroll_, XR_ACTION_TYPE_FLOAT_INPUT, "scroll", "Defiler", true) &&
        makeAction(menuSet_, closeOrders_, XR_ACTION_TYPE_BOOLEAN_INPUT, "close_orders", "Fermer la vue strategique",
                   false) &&
        makeAction(menuSet_, rotate_, XR_ACTION_TYPE_FLOAT_INPUT, "rotate", "Tourner la carte", true) &&
        makeAction(menuSet_, ctrl_, XR_ACTION_TYPE_BOOLEAN_INPUT, "ctrl", "Ctrl", true) &&
        makeAction(menuSet_, menuEscape_, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu_escape", "Echap (menus)", false) &&
        makeAction(combatSet_, move_, XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Se deplacer", false) &&
        makeAction(combatSet_, turn_, XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Tourner", false) &&
        makeAction(combatSet_, attack_, XR_ACTION_TYPE_BOOLEAN_INPUT, "attack", "Attaquer", false) &&
        makeAction(combatSet_, block_, XR_ACTION_TYPE_FLOAT_INPUT, "block", "Bloquer", false) &&
        makeAction(combatSet_, aimMode_, XR_ACTION_TYPE_BOOLEAN_INPUT, "aim_mode", "Source du reticule", false) &&
        makeAction(combatSet_, view_, XR_ACTION_TYPE_BOOLEAN_INPUT, "view",
                   "Coup de pied (appui) / vue 1re-3e personne (tenu)", false) &&
        makeAction(combatSet_, use_, XR_ACTION_TYPE_FLOAT_INPUT, "use", "Utiliser", false) &&
        makeAction(combatSet_, draw_, XR_ACTION_TYPE_FLOAT_INPUT, "draw", "Bander l'arc", false) &&
        makeAction(combatSet_, weapon_, XR_ACTION_TYPE_FLOAT_INPUT, "weapon", "Changer d'arme", false) &&
        makeAction(combatSet_, tabMenu_, XR_ACTION_TYPE_BOOLEAN_INPUT, "tab_menu", "Menu Tab", false) &&
        makeAction(combatSet_, orders_, XR_ACTION_TYPE_BOOLEAN_INPUT, "orders", "Vue strategique", false) &&
        makeAction(combatSet_, escape_, XR_ACTION_TYPE_BOOLEAN_INPUT, "escape", "Echap", false) &&
        makeAction(combatSet_, escapeAlt_, XR_ACTION_TYPE_BOOLEAN_INPUT, "escape_alt",
                   "Echap (appui court) / source du reticule (tenu)", false) &&
        makeAction(combatSet_, shield_, XR_ACTION_TYPE_BOOLEAN_INPUT, "shield", "Bouclier", false) &&
        makeAction(combatSet_, handPose_, XR_ACTION_TYPE_POSE_INPUT, "hand_pose", "Mains", true) &&
        makeAction(combatSet_, combatAim_, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Viser (combat)", true);
    if (!created) {
        LOG_XR("ERREUR : creation des actions, manettes desactivees");
        return false;
    }

    // Associations : une suggestion par profil, avec les actions des deux jeux
    int suggested = 0;
    for (const Profile& p : kProfiles) {
        std::vector<XrActionSuggestedBinding> b;
        auto add = [&](XrAction a, bool right, const char* rel) {
            if (rel)
                b.push_back({a, Path(instance, (std::string(right ? "/user/hand/right/" : "/user/hand/left/") + rel)
                                                   .c_str())});
        };
        for (int h = 0; h < 2; ++h) {
            add(aim_, h, "input/aim/pose");
            add(handPose_, h, "input/grip/pose"); // pose de préhension : axe d'un objet tenu
            add(combatAim_, h, "input/aim/pose");
            add(select_, h, p.select);
            add(secondary_, h, h ? p.secondaryR : p.secondaryL);
            add(scroll_, h, p.scroll);
            add(rotate_, h, p.rotate);
            add(ctrl_, h, p.ctrl);
        }
        add(move_, false, p.moveL);
        add(aimMode_, false, p.aimModeL);
        add(escape_, false, p.escapeL);
        add(menuEscape_, false, p.escapeL); // même bouton dans les menus
        add(menuEscape_, false, p.escapeAltL);
        add(escapeAlt_, false, p.escapeAltL);
        add(tabMenu_, false, p.tabL);
        add(orders_, false, p.ordersL);
        add(closeOrders_, false, p.ordersL); // même bouton dans les menus
        add(block_, false, p.blockL);
        add(use_, false, p.useL);
        add(turn_, true, p.turnR);
        add(view_, true, p.viewR);
        add(weapon_, true, p.weaponR);
        add(draw_, true, p.drawR);
        add(attack_, true, p.attackR);
        add(shield_, true, p.shieldR);

        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = Path(instance, p.name);
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        sb.suggestedBindings = b.data();
        const XrResult r = xrSuggestInteractionProfileBindings(instance, &sb);
        if (XR_SUCCEEDED(r))
            ++suggested;
        else
            LOG_XR("associations refusees pour %s (%d)", p.name, static_cast<int>(r));
    }

    XrActionSet sets[] = {menuSet_, combatSet_};
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 2;
    ai.actionSets = sets;
    if (XR_FAILED(xrAttachSessionActionSets(session, &ai))) {
        LOG_XR("ERREUR : xrAttachSessionActionSets, manettes desactivees");
        return false;
    }
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = aim_;
        ci.subactionPath = hands_[h];
        ci.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(session, &ci, &aimSpaces_[h]);
        ci.action = handPose_;
        xrCreateActionSpace(session, &ci, &combatHandSpaces_[h]);
        ci.action = combatAim_;
        xrCreateActionSpace(session, &ci, &combatAimSpaces_[h]);
    }
    ready_ = true;
    LOG_XR("manettes pretes : menus et combat (%d profils de manettes)", suggested);
    return true;
}

void XrControls::Shutdown()
{
    ReleaseAll();
    for (XrSpace* spaces : {aimSpaces_, combatHandSpaces_, combatAimSpaces_})
        for (int h = 0; h < 2; ++h) {
            if (spaces[h] != XR_NULL_HANDLE)
                xrDestroySpace(spaces[h]);
            spaces[h] = XR_NULL_HANDLE;
        }
    for (XrActionSet* set : {&menuSet_, &combatSet_}) {
        if (*set != XR_NULL_HANDLE)
            xrDestroyActionSet(*set); // détruit aussi les actions
        *set = XR_NULL_HANDLE;
    }
    ready_ = false;
}

void XrControls::ReleaseAll()
{
    const HandPose none[2]{};
    PublishHands(none);
    input::SetPointerTarget(false, 0, 0);
    input::ReleaseAllInjected();
    tapKey_ = 0;
    EndFlagPlacement();
    menuButtonDown_ = menuButtonLong_ = false;
    aimModeDown_ = aimModeLong_ = false;
    snapLatched_ = false;
    keyQueue_.clear();
    pending_ = false;
    path_.clear();
    radial_.open = false;
    radial_.highlight = -1;
    radialArmed_ = false;
    leftButtonHeld_ = rightButtonHeld_ = weaponDown_ = false;
    swing_ = Swing::Idle;
    haveLastHand_ = false;
    SetSwingBusy(false);
    SetBladeSpeed(0);
    drawing_ = false;
    triggerHeld_ = false;
    SetDrawing(false);
}

bool XrControls::Update(XrSession session, XrSpace space, XrTime time, const XrPosef* head, bool focused,
                        bool combat, bool screenActive, const Screen& screen, XrVector3f& hit)
{
    if (!ready_)
        return false;

    // Changement de mode : rien ne doit rester appuyé d'un mode à l'autre
    if (combat != combatMode_) {
        ReleaseAll();
        if (combat)
            ordersOpen_ = closeHeld_ = false; // retour au combat : la vue stratégique est fermée
        else
            closeDown_ = true; // X encore tenu à l'ouverture : il faut le relâcher avant de refermer
        combatMode_ = combat;
        LOG_XR("manettes en mode %s", combat ? "combat" : "menus");
    }
    if (!focused) {
        ReleaseAll();
        handState_[0].valid = handState_[1].valid = false;
        rayLength_ = 0;
        return false;
    }

    XrActiveActionSet active{combat ? combatSet_ : menuSet_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions(session, &sync);
    ProcessKeyQueue();

    // Profil de manette réellement utilisé par le runtime (une fois)
    if (!loggedProfile_) {
        for (int h = 0; h < 2; ++h) {
            XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
            char name[XR_MAX_PATH_LENGTH]{};
            uint32_t len = 0;
            if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, hands_[h], &ps)) &&
                ps.interactionProfile != XR_NULL_PATH &&
                XR_SUCCEEDED(xrPathToString(instance_, ps.interactionProfile, sizeof(name), &len, name))) {
                LOG_XR("manette %s : profil %s", h ? "droite" : "gauche", name);
                loggedProfile_ = true;
            }
        }
    }

    // Souris et clavier : les manettes ne commandent plus rien (ni affichées ni pointées)
    if (UpdateInputMode(space, time, combat)) {
        handState_[0].valid = handState_[1].valid = false;
        rayLength_ = 0;
        keyboardHover_ = keyboardClick_ = false;
        return false;
    }
    UpdateMenuButton(GetBool(session, combat ? escape_ : menuEscape_, XR_NULL_PATH));

    bool onScreen = false;
    if (combat) {
        handState_[0].valid = handState_[1].valid = false; // pas de manettes affichées en combat
        rayLength_ = 0;
        UpdateCombat(session, space, time, head);
    } else {
        UpdateMenu(session, space, time, focused, screenActive, screen, hit, onScreen);
    }
    return onScreen;
}

// Commandes : réglage input_mode, ou en auto le dernier périphérique utilisé. Souris : mouvement ou
// clic de la vraie souris, ou plus aucune manette suivie depuis 2 s. Manettes : l'une d'elles
// déplacée de 15 cm (prise en main ; posées, elles restent immobiles). true = souris et clavier.
bool XrControls::UpdateInputMode(XrSpace space, XrTime time, bool combat)
{
    constexpr float kPickUpDistance = 0.15f;
    constexpr unsigned long long kMouseWake = 150; // activité de la souris sur une fenêtre de 1 s
    constexpr ULONGLONG kNoHandsMs = 2000;
    const ULONGLONG now = GetTickCount64();
    const unsigned long long activity = input::RealMouseActivity();
    const int setting = config::GetInt(config::Id::InputMode);
    bool mouse = mouseMode_;
    const char* why = nullptr;
    if (setting == 1) {
        mouse = false, why = "reglage";
    } else if (setting == 2) {
        mouse = true, why = "reglage";
    } else {
        bool anyHand = false, pickedUp = false;
        const XrSpace* spaces = combat ? combatHandSpaces_ : aimSpaces_;
        const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        for (int h = 0; h < 2; ++h) {
            XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
            if (spaces[h] == XR_NULL_HANDLE || XR_FAILED(xrLocateSpace(spaces[h], space, time, &l)) ||
                (l.locationFlags & needed) != needed) {
                anchorValid_[h] = false;
                continue;
            }
            anyHand = true;
            const XrVector3f& p = l.pose.position;
            if (!anchorValid_[h]) {
                anchor_[h] = p;
                anchorValid_[h] = true;
            } else {
                const float dx = p.x - anchor_[h].x, dy = p.y - anchor_[h].y, dz = p.z - anchor_[h].z;
                pickedUp = pickedUp || dx * dx + dy * dy + dz * dz > kPickUpDistance * kPickUpDistance;
            }
        }
        if (now - mouseMarkMs_ >= 1000) {
            mouseMark_ = activity;
            mouseMarkMs_ = now;
        }
        noHandsSinceMs_ = anyHand ? 0 : noHandsSinceMs_ ? noHandsSinceMs_ : now;
        if (!mouse && activity - mouseMark_ > kMouseWake)
            mouse = true, why = "souris utilisee";
        else if (!mouse && noHandsSinceMs_ && now - noHandsSinceMs_ > kNoHandsMs)
            mouse = true, why = "aucune manette suivie";
        else if (mouse && pickedUp)
            mouse = false, why = "manette prise en main";
    }
    if (mouse != mouseMode_ || !inputModeLogged_) {
        if (mouse != mouseMode_)
            ReleaseAll();
        // Manettes posées : leur position actuelle devient la référence de la prise en main
        anchorValid_[0] = anchorValid_[1] = false;
        mouseMark_ = activity;
        mouseMarkMs_ = now;
        mouseMode_ = mouse;
        inputModeLogged_ = true;
        SetMouseMode(mouse);
        LOG_XR("commandes : %s (%s)", mouse ? "souris et clavier" : "manettes VR", why ? why : "demarrage");
    }
    return mouseMode_;
}

void XrControls::UpdateMenu(XrSession session, XrSpace space, XrTime time, bool /*focused*/, bool screenActive,
                            const Screen& screen, XrVector3f& hit, bool& onScreen)
{
    // Vue stratégique ouverte par X : le même bouton la referme (Retour arrière). Dans les
    // autres menus, X n'envoie rien (Retour arrière effacerait du texte).
    const bool close = GetBool(session, closeOrders_, XR_NULL_PATH);
    if (ordersOpen_ && close && !closeDown_) {
        LOG_XR("X : fermeture de la vue strategique");
        input::InjectKey(0x0E, true);
        closeHeld_ = true;
    } else if (closeHeld_ && !close) {
        input::InjectKey(0x0E, false);
        closeHeld_ = false;
        ordersOpen_ = false;
    }
    closeDown_ = close;

    // Échap (bouton menu), Ctrl (poignée d'une main), rotation de la carte (stick horizontal,
    // s'il domine le vertical qui fait défiler) : touches tenues tant que le bouton l'est
    float rot = 0;
    bool ctrl = false;
    for (int h = 0; h < 2; ++h) {
        const float x = GetFloat(session, rotate_, hands_[h]);
        const float y = GetFloat(session, scroll_, hands_[h]);
        if (std::fabs(x) > kScrollThreshold && std::fabs(x) > std::fabs(y))
            rot = x;
        ctrl = ctrl || GetBool(session, ctrl_, hands_[h]);
    }
    input::InjectKey(kKeyCtrl, ctrl);
    input::InjectKey(kKeyNum1, rot > 0); // sens validé au casque
    input::InjectKey(kKeyNum3, rot < 0);

    bool select[2]{}, secondary[2]{};
    float scroll[2]{};
    for (int h = 0; h < 2; ++h) {
        select[h] = GetBool(session, select_, hands_[h]);
        secondary[h] = GetBool(session, secondary_, hands_[h]);
        scroll[h] = GetFloat(session, scroll_, hands_[h]);
    }

    // La main qui clique devient la main qui pointe
    bool pressed[2]{};
    for (int h = 0; h < 2; ++h) {
        pressed[h] = select[h] && !selectDown_[h];
        if (pressed[h]) {
            activeHand_ = h;
            if (loggedPresses_ < 10) {
                LOG_XR("gachette main %s appuyee (ecran virtuel %s)", h ? "droite" : "gauche",
                       screenActive ? "affiche" : "absent");
                ++loggedPresses_;
            }
        }
        selectDown_[h] = select[h];
    }

    // Poses de visée des deux mains (pointeur et affichage des manettes)
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    for (int h = 0; h < 2; ++h) {
        XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
        handState_[h].valid = aimSpaces_[h] != XR_NULL_HANDLE &&
                              XR_SUCCEEDED(xrLocateSpace(aimSpaces_[h], space, time, &l)) &&
                              (l.locationFlags & needed) == needed;
        if (handState_[h].valid)
            handState_[h].pose = l.pose;
    }

    // Rayon de la main active -> plan de l'écran virtuel
    float u = 0, v = 0;
    rayLength_ = 0;
    keyboardHover_ = keyboardClick_ = false;
    if (screenActive && handState_[activeHand_].valid) {
        const XrQuaternionf& q = handState_[activeHand_].pose.orientation;
        const XrVector3f& p = handState_[activeHand_].pose.position;
        const float dx = -2 * (q.x * q.z + q.y * q.w);
        const float dy = -2 * (q.y * q.z - q.x * q.w);
        const float dz = -(1 - 2 * (q.x * q.x + q.y * q.y));
        if (dz < -1e-4f) {
            const float t = (-screen.distance - p.z) / dz;
            const float x = p.x + t * dx, y = p.y + t * dy;
            if (t > 0 && std::fabs(x) <= screen.width / 2 && std::fabs(y) <= screen.height / 2) {
                onScreen = true;
                u = x / screen.width + 0.5f;
                v = 0.5f - y / screen.height;
                hit = {x, y, -screen.distance};
                rayLength_ = t;
            } else if (t > 0 && screen.keyboard && std::fabs(x) <= screen.keyboardWidth / 2 &&
                       std::fabs(y - screen.keyboardY) <= screen.keyboardHeight / 2) {
                // Clavier virtuel, sous l'écran : les clics vont au clavier, pas au jeu
                keyboardHover_ = true;
                keyboardU_ = x / screen.keyboardWidth + 0.5f;
                keyboardV_ = 0.5f - (y - screen.keyboardY) / screen.keyboardHeight;
                keyboardClick_ = pressed[activeHand_];
                hit = {x, y, -screen.distance};
                rayLength_ = t;
            }
        }
    }
    input::SetPointerTarget(onScreen, u, v);
    if (onScreen && !loggedHit_) {
        LOG_XR("manette %s pointee sur l'ecran virtuel", activeHand_ ? "droite" : "gauche");
        loggedHit_ = true;
    }

    // Boutons de la main active ; un bouton tenu est toujours relâché
    const bool leftWanted = onScreen && select[activeHand_];
    if (leftWanted != leftButtonHeld_) {
        input::SetPointerButton(true, leftWanted);
        leftButtonHeld_ = leftWanted;
    }
    const bool rightWanted = onScreen && secondary[activeHand_];
    if (rightWanted != rightButtonHeld_) {
        input::SetPointerButton(false, rightWanted);
        rightButtonHeld_ = rightWanted;
    }

    // Molette : un cran à l'appui, puis à intervalle régulier tant que le stick est poussé
    const float s = scroll[activeHand_];
    if (onScreen && std::fabs(s) > kScrollThreshold && rot == 0) {
        const ULONGLONG now = GetTickCount64();
        if (now - lastScrollMs_ >= kScrollRepeatMs) {
            input::ScrollPointer(s > 0 ? 1 : -1);
            lastScrollMs_ = now;
        }
    } else {
        lastScrollMs_ = 0;
    }
}

// Attaque au geste de la manette droite. Le jeu choisit la direction d'attaque d'après le
// dernier mouvement de souris (option « direction d'attaque : mouvement de la souris ») :
// on injecte ce mouvement, puis on enfonce le bouton (le personnage arme le coup), puis on
// le relâche aussitôt (le coup part dès que le jeu l'a armé : c'est le plus rapide).
// La direction est lue sur la trajectoire de la main pendant les premières dizaines de
// millisecondes du geste (la vitesse instantanée au début du mouvement est trop imprécise).
bool XrControls::UpdateSwing(XrSpace space, XrTime time, const XrPosef* head)
{
    const ULONGLONG now = GetTickCount64();

    // Position et vitesse de la main droite (vitesse du runtime, sinon par différence)
    XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    loc.next = &vel;
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (!head || combatHandSpaces_[1] == XR_NULL_HANDLE ||
        XR_FAILED(xrLocateSpace(combatHandSpaces_[1], space, time, &loc)) || (loc.locationFlags & needed) != needed) {
        haveLastHand_ = false;
        return swing_ == Swing::Held;
    }
    const XrVector3f pos = loc.pose.position;
    XrVector3f v{};
    if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
        v = vel.linearVelocity;
    } else if (haveLastHand_ && time > lastHandTime_) {
        const float dt = (time - lastHandTime_) / 1e9f;
        v = {(pos.x - lastHandPos_.x) / dt, (pos.y - lastHandPos_.y) / dt, (pos.z - lastHandPos_.z) / dt};
    }
    const XrVector3f previous = lastHandPos_;
    const bool hadPrevious = haveLastHand_;
    lastHandPos_ = pos;
    lastHandTime_ = time;
    haveLastHand_ = true;
    const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    // Arme toujours active : vitesse d'un point de la lame (main + rotation du poignet : les
    // coups de poignet comptent). Lame selon -Z de la poignée (pose grip)
    XrVector3f blade = v;
    if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
        const XrQuaternionf q = loc.pose.orientation;
        // r = q * (0, 0, -kBladeProbe)
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        const float rx = -kBladeProbe * 2 * (x * z + w * y);
        const float ry = -kBladeProbe * 2 * (y * z - w * x);
        const float rz = -kBladeProbe * (1 - 2 * (x * x + y * y));
        const XrVector3f o = vel.angularVelocity;
        blade = {v.x + o.y * rz - o.z * ry, v.y + o.z * rx - o.x * rz, v.z + o.x * ry - o.y * rx};
    }
    SetBladeSpeed(std::sqrt(blade.x * blade.x + blade.y * blade.y + blade.z * blade.z));

    switch (swing_) {
    case Swing::Idle:
        if (speed >= SwingStartSpeed()) {
            // Début du geste : la mesure part de la position précédente
            swingStart_ = hadPrevious ? previous : pos;
            swing_ = Swing::Measuring;
            swingSinceMs_ = now;
        }
        break;

    case Swing::Measuring: {
        if (now - swingSinceMs_ < SwingWindowMs())
            break;
        // Déplacement de la main pendant la fenêtre, dans le repère horizontal du regard
        const XrVector3f d{pos.x - swingStart_.x, pos.y - swingStart_.y, pos.z - swingStart_.z};
        const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const XrQuaternionf& q = head->orientation;
        float fx = -2 * (q.x * q.z + q.y * q.w), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
        const float len = std::sqrt(fx * fx + fz * fz);
        if (dist < SwingMinDistance() || len < 1e-3f) {
            swing_ = Swing::Idle; // geste trop court : pas un coup
            break;
        }
        fx /= len, fz /= len;
        const float forward = d.x * fx + d.z * fz;
        const float right = d.x * -fz + d.z * fx;
        const float up = d.y;
        const float ar = std::fabs(right), au = std::fabs(up), af = std::max(forward, 0.0f);
        const char* kind = nullptr;
        if (up > 0 && au >= 0.5f * std::max(ar, af)) {
            // Main qui monte nettement : armé du coup, pas un coup (on attend la descente)
        } else if (up < 0 && au >= 0.7f * ar && au >= 0.5f * af) {
            // Descente marquée : coup par le haut, même s'il avance beaucoup (testé avant
            // l'estoc, qui le captait)
            kind = "coup par le haut";
            swingDx_ = 0, swingDy_ = -kSwingMouse; // souris vers le haut
        } else if (forward > 0 && forward >= ThrustDominance() * ar && forward >= ThrustDominance() * au) {
            kind = "estoc";
            swingDx_ = 0, swingDy_ = kSwingMouse; // souris vers le bas
        } else if (ar > au) {
            // Sens choisi au test casque : main vers la gauche -> taille par la gauche
            kind = right < 0 ? "taille par la gauche" : "taille par la droite";
            swingDx_ = right < 0 ? -kSwingMouse : kSwingMouse, swingDy_ = 0;
        }
        if (!kind) {
            swing_ = Swing::Idle; // main qui monte : préparation, pas un coup
            break;
        }
        input::InjectMouseMove(swingDx_, swingDy_);
        swing_ = Swing::Aiming;
        swingSinceMs_ = now;
        if (loggedSwings_ < 300) {
            LOG_XR("geste : %s (%.2f m en %llu ms ; avant %.2f, droite %.2f, haut %.2f)", kind, dist, SwingWindowMs(),
                   forward, right, up);
            ++loggedSwings_;
        }
        break;
    }

    case Swing::Aiming:
        // Frame suivante : le jeu a vu la direction ; bouton enfoncé, puis visée remise en place
        input::InjectMouseMove(-swingDx_, -swingDy_);
        swing_ = Swing::Held;
        swingSinceMs_ = now;
        break;

    case Swing::Held:
        if (now - swingSinceMs_ >= kSwingHoldMs) {
            swing_ = Swing::Cooldown;
            swingSinceMs_ = now;
        }
        break;

    case Swing::Cooldown:
        if (now - swingSinceMs_ >= kSwingCooldownMs)
            swing_ = Swing::Idle;
        break;
    }
    return swing_ == Swing::Held;
}

namespace {
std::wstring Widen(const std::string& s)
{
    for (UINT cp : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)}) {
        const DWORD flags = cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int n = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n > 0) {
            std::wstring w(n, L'\0');
            MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }
    }
    return {};
}
} // namespace

// Menu radial des ordres (X ouvre, X referme). Niveau 0 : choix des troupes (0 = toutes,
// 1-3) et carte des ordres du module (Retour arrière). Niveaux suivants : le menu d'ordres
// du moteur (F1-F8), avec ses libellés tels que le jeu les affiche (langue, module). Le stick
// gauche vise une entrée, le relâcher la valide ; clic du stick = niveau précédent.
// Le menu du jeu se refermant seul après quelques secondes, les libellés de chaque niveau
// sont mémorisés et une sélection rejoue au besoin tout le chemin (groupe, catégorie, ordre).
std::string XrControls::PathKey() const
{
    std::string k;
    for (size_t i = 1; i < path_.size(); ++i)
        k += std::to_string(path_[i]) + ",";
    return k;
}

void XrControls::SendKeys(std::initializer_list<int> keys, const std::vector<int>& prefix)
{
    for (int k : prefix)
        keyQueue_.push_back(k);
    for (int k : keys)
        keyQueue_.push_back(k);
}

void XrControls::CloseRadial(const char* why)
{
    if (radial_.open)
        LOG_XR("menu radial ferme (%s)", why);
    radial_.open = false;
    radial_.highlight = -1;
    radialArmed_ = false;
    pending_ = false;
    path_.clear();
}

// Touches envoyées par le menu radial ou le bouton menu : une à la fois, appui puis pause
void XrControls::ProcessKeyQueue()
{
    const ULONGLONG now = GetTickCount64();
    if (tapKey_ && now >= tapUntil_) {
        input::InjectKey(tapKey_, false);
        tapKey_ = 0;
        gapUntil_ = now + kGapMs;
    } else if (!tapKey_ && !keyQueue_.empty() && now >= gapUntil_) {
        const int next = keyQueue_.front();
        keyQueue_.pop_front();
        lastKeyMs_ = now;
        if (next & kHoldFlag) {
            heldKey_ = next & 0xFF;
            input::InjectKey(heldKey_, true);
            // Le drapeau suit la visée du jeu : réticule sur la main gauche pendant le placement
            flagSavedAim_ = GetAimSource();
            SetAimSource(AimSource::LeftHand);
            LOG_XR("tenez cette position : placement du drapeau a la main gauche (clic du stick gauche pour valider)");
        } else {
            tapKey_ = next;
            input::InjectKey(tapKey_, true);
            tapUntil_ = now + kTapMs;
        }
    }
}

// Bouton menu (main gauche) : appui court = Échap (au relâchement), appui long = recentrage
// de la vue (direction et hauteur de la tête, écran virtuel), utilisable sur tout casque
void XrControls::UpdateMenuButton(bool pressed)
{
    const ULONGLONG now = GetTickCount64();
    if (pressed && !menuButtonDown_) {
        menuButtonSince_ = now;
        menuButtonLong_ = false;
    } else if (pressed && !menuButtonLong_ && now - menuButtonSince_ >= kRecenterHoldMs) {
        menuButtonLong_ = true;
        recenterRequest_ = true;
        haveLastHand_ = false; // les poses vont sauter : pas de faux geste d'attaque
        LOG_XR("bouton menu tenu : recentrage");
    } else if (!pressed && menuButtonDown_ && !menuButtonLong_) {
        SendKeys({kKeyEscape});
    }
    menuButtonDown_ = pressed;
}

// Fin du placement du drapeau : réticule rendu à sa source d'avant
void XrControls::EndFlagPlacement()
{
    if (!heldKey_)
        return;
    heldKey_ = 0;
    SetAimSource(flagSavedAim_);
}

void XrControls::UpdateRadial(bool toggle, bool back, const XrVector2f& stick)
{
    const ULONGLONG now = GetTickCount64();

    // Placement du drapeau en cours : le bouton des ordres relâche F1, le jeu donne l'ordre à
    // l'endroit visé
    if (toggle && heldKey_) {
        input::InjectKey(heldKey_, false);
        LOG_XR("tenez cette position : ordre donne");
        EndFlagPlacement();
        return;
    }

    if (toggle) {
        if (radial_.open) {
            CloseRadial("bouton des ordres");
        } else {
            radial_.open = true;
            path_.clear();
            pending_ = false;
            LOG_XR("X : menu radial des ordres");
        }
    }
    if (!radial_.open)
        return;

    const engine::OrderMenu menu = engine::GetOrderMenu();
    std::vector<std::string> live;
    if (menu.open)
        live.assign(menu.labels, menu.labels + menu.count);
    if (menu.open) {
        std::string log;
        for (size_t i = 0; i < live.size(); ++i)
            log += (i ? " | " : "") + live[i];
        if (log != lastMenuLog_) {
            LOG_XR("menu d'ordres du jeu (niveau %d) : %s", menu.type, log.c_str());
            lastMenuLog_ = log;
        }
    }

    // Réponse du jeu à la dernière sélection, une fois toutes les touches envoyées
    const bool quiet = keyQueue_.empty() && !tapKey_ && now - lastKeyMs_ >= kSettleMs;
    if (pending_ && quiet) {
        const bool isGroup = path_.empty();
        if (menu.open && (isGroup || live != labelsBefore_)) {
            path_.push_back(pendingKey_); // nouveau niveau
            labelCache_[PathKey()] = live;
            pending_ = false;
        } else if (!menu.open && !isGroup) {
            pending_ = false;
            CloseRadial("ordre donne");
            return;
        } else if (now - pendingSince_ > kGiveUpMs) {
            pending_ = false;
            if (isGroup)
                path_.push_back(pendingKey_); // libellés mémorisés s'il y en a
            LOG_XR("menu radial : pas de reponse du jeu a la touche 0x%02X", pendingKey_);
        }
    }
    // Menu du jeu ouvert et à jour : rafraîchit les libellés mémorisés de ce niveau
    if (!pending_ && quiet && menu.open && !path_.empty() && labelCache_.count(PathKey()) &&
        live.size() == labelCache_[PathKey()].size())
        labelCache_[PathKey()] = live;

    // Clic du stick : niveau précédent (au niveau 0 : fermer)
    if (back && !pending_) {
        if (path_.empty()) {
            CloseRadial("retour");
            return;
        }
        path_.pop_back();
        radialArmed_ = false;
        radial_.highlight = -1;
    }

    // Entrées du niveau courant
    int n = 0;
    const std::vector<std::string>* labels = nullptr;
    if (path_.empty()) {
        radial_.groups = true;
        const wchar_t* names[] = {french_ ? L"Toutes les troupes" : L"Everyone", french_ ? L"Infanterie" : L"Infantry",
                                  L"Archers", french_ ? L"Cavalerie" : L"Cavalry"};
        const int keys[] = {kKeyGroupAll, kKeyGroup1, kKeyGroup2, kKeyGroup3};
        for (int i = 0; i < 4; ++i, ++n) {
            radial_.labels[n] = names[i];
            radialKeys_[n] = keys[i];
        }
        radial_.labels[n] = french_ ? L"Carte des ordres" : L"Order map";
        radialKeys_[n++] = kKeyBack;
    } else {
        radial_.groups = false;
        auto it = labelCache_.find(PathKey());
        if (it != labelCache_.end())
            labels = &it->second;
        if (labels)
            for (size_t i = 0; i < labels->size() && n < 8; ++i, ++n) {
                radial_.labels[n] = Widen((*labels)[i]);
                radialKeys_[n] = kOrderKeys[i];
            }
    }
    radial_.depth = static_cast<int>(path_.size());
    radial_.waiting = pending_;
    if (n != radial_.count) {
        radial_.highlight = -1;
        radialArmed_ = false;
    }
    radial_.count = n;
    if (pending_ || n == 0)
        return; // en attente du jeu : pas de sélection

    // Stick : viser une entrée, le relâcher pour la valider
    const float mag = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (mag > kRadialSelect) {
        constexpr float kTwoPi = 6.2831853f; // angle depuis le haut, sens horaire ; entrée 0 en haut
        float a = std::atan2(stick.x, stick.y);
        if (a < 0)
            a += kTwoPi;
        radial_.highlight = static_cast<int>(std::floor(a / (kTwoPi / n) + 0.5f)) % n;
        radialArmed_ = true;
        return;
    }
    if (mag >= kRadialRelease || !radialArmed_)
        return;
    const int k = radial_.highlight;
    radialArmed_ = false;
    radial_.highlight = -1;
    if (k < 0 || k >= n)
        return;
    const int key = radialKeys_[k];
    LOG_XR("menu radial : niveau %d, entree %d (touche 0x%02X)", radial_.depth, k, key);
    if (key == kKeyBack) {
        SendKeys({kKeyBack});
        ordersOpen_ = true; // la carte s'ouvre : X la refermera depuis l'écran virtuel
        CloseRadial("carte des ordres");
        return;
    }
    // Menu du jeu ouvert sur ce niveau : la touche seule ; sinon tout le chemin
    const bool inSync = menu.open && labels && live == *labels;
    // « Tenez cette position » (F1 du menu de mouvement, F1 de la racine) : le moteur affiche
    // un drapeau à l'endroit visé tant que F1 est tenu et donne l'ordre au relâchement.
    // F1 reste donc enfoncé, menu fermé, jusqu'au prochain appui sur X.
    if (path_.size() == 2 && path_[1] == kOrderKeys[0] && key == kOrderKeys[0]) {
        if (inSync)
            SendKeys({key | kHoldFlag});
        else
            SendKeys({key | kHoldFlag}, path_);
        CloseRadial("placement du drapeau");
        return;
    }
    if (path_.empty() || inSync)
        SendKeys({key});
    else
        SendKeys({key}, path_);
    pending_ = true;
    pendingKey_ = key;
    pendingSince_ = now;
    labelsBefore_ = labels ? *labels : std::vector<std::string>{};
}

void XrControls::UpdateCombat(XrSession session, XrSpace space, XrTime time, const XrPosef* head)
{
    // Poses des deux mains pour les bras du personnage
    HandPose hands[2];
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    for (int h = 0; h < 2; ++h) {
        XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
        if (combatHandSpaces_[h] != XR_NULL_HANDLE && XR_SUCCEEDED(xrLocateSpace(combatHandSpaces_[h], space, time, &l)) &&
            (l.locationFlags & needed) == needed)
            hands[h] = {true, l.pose.orientation.x, l.pose.orientation.y, l.pose.orientation.z, l.pose.orientation.w,
                        l.pose.position.x, l.pose.position.y, l.pose.position.z};
        XrSpaceLocation a{XR_TYPE_SPACE_LOCATION};
        if (combatAimSpaces_[h] != XR_NULL_HANDLE && XR_SUCCEEDED(xrLocateSpace(combatAimSpaces_[h], space, time, &a)) &&
            (a.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            hands[h].aimValid = true;
            hands[h].aqx = a.pose.orientation.x, hands[h].aqy = a.pose.orientation.y;
            hands[h].aqz = a.pose.orientation.z, hands[h].aqw = a.pose.orientation.w;
        }
    }
    PublishHands(hands);

    const ULONGLONG now = GetTickCount64();
    const float dt = lastCombatMs_ ? std::min(0.1f, (now - lastCombatMs_) / 1000.0f) : 0.0f;
    lastCombatMs_ = now;

    // Déplacement : stick gauche -> Z/Q/S/D (W/A/S/D du jeu). X maintenu : le stick gauche
    // pilote le menu radial des ordres et le personnage s'arrête.
    const XrVector2f move = GetVector(session, move_);
    const bool ordersButton = GetBool(session, orders_, XR_NULL_PATH);
    const bool aimMode = GetBool(session, aimMode_, XR_NULL_PATH);
    const bool radialWasOpen = radial_.open;
    UpdateRadial(ordersButton && !ordersButtonDown_, radialWasOpen && aimMode && !aimModeDown_, move);
    ordersButtonDown_ = ordersButton;
    const bool walk = !radial_.open;
    input::InjectKey(kKeyW, walk && move.y > kMoveThreshold);
    input::InjectKey(kKeyS, walk && move.y < -kMoveThreshold);
    input::InjectKey(kKeyD, walk && move.x > kMoveThreshold);
    input::InjectKey(kKeyA, walk && move.x < -kMoveThreshold);

    // Rotation du corps : stick droit horizontal. La visée (caméra du jeu) suit la main
    // droite et est asservie par le rendu (camera_hook) ; le stick tourne la vue.
    const XrVector2f turn = GetVector(session, turn_);
    if (config::GetInt(config::Id::TurnMode) == 1) {
        // Par crans : un cran par poussée franche, stick à relâcher avant le suivant
        if (!snapLatched_ && std::fabs(turn.x) > kSnapPush) {
            const float step = config::Get(config::Id::SnapAngle) * 3.14159265f / 180.0f;
            AddTurn(turn.x > 0 ? -step : step); // stick à droite = tourner à droite
            snapLatched_ = true;
        } else if (snapLatched_ && std::fabs(turn.x) < kSnapRelease) {
            snapLatched_ = false;
        }
    } else if (std::fabs(turn.x) > TurnDeadzone()) {
        const float sign = turn.x > 0 ? 1.0f : -1.0f;
        const float amount = (std::fabs(turn.x) - TurnDeadzone()) / (1 - TurnDeadzone());
        AddTurn(-sign * amount * TurnRate() * dt); // stick à droite = tourner à droite
    }

    // Stick droit vers le haut : saut
    input::InjectKey(kKeySpace, turn.y > JumpThreshold());

    // Arc : gâchette droite tenue = bander et viser (axe des deux mains, précision selon leur
    // écart : combat_hook), relâchée = tir. Sinon, attaque au geste.
    const bool bow = IsBowMode();
    const bool attackButton = GetBool(session, attack_, XR_NULL_PATH); // poignée droite
    // Gâchette droite, avec hystérésis (une valeur qui oscille autour du seuil relâchait l'arc
    // aussitôt bandé) ; avec l'arc, la poignée droite fait de même
    const float triggerValue = GetFloat(session, draw_, XR_NULL_PATH);
    triggerHeld_ = triggerValue > (triggerHeld_ ? kTriggerRelease : kActionThreshold);
    if (triggerHeld_ != triggerLogged_ || attackButton != gripLogged_) {
        LOG_XR("main droite : gachette %s (%.2f), poignee %s", triggerHeld_ ? "tenue" : "relachee", triggerValue,
               attackButton ? "tenue" : "relachee");
        triggerLogged_ = triggerHeld_;
        gripLogged_ = attackButton;
    }
    const bool useTrigger = triggerHeld_ || (bow && attackButton);
    bool swingHeld = false;
    if (bow) {
        SetBladeSpeed(0);
        if (swing_ != Swing::Idle) {
            swing_ = Swing::Idle;
            SetSwingBusy(false);
        }
        if (!drawing_ && useTrigger && !useTriggerDown_) {
            drawing_ = true;
            LOG_XR("arc : corde tiree");
        } else if (drawing_ && !useTrigger) {
            drawing_ = false;
            LOG_XR("arc : tir");
        }
    } else if (IsTriggerAttackMode() || !config::GetBool(config::Id::Gestures)) {
        // Arbalète, arme à feu, arme de jet : pas de geste (évite les tirs accidentels) ;
        // gâchette ou poignée droite tenue = viser, relâchée = tirer / lancer
        drawing_ = false;
        swing_ = Swing::Idle;
        SetSwingBusy(false);
        SetBladeSpeed(0);
    } else {
        drawing_ = false;
        swingHeld = UpdateSwing(space, time, head);
        SetSwingBusy(swing_ == Swing::Measuring || swing_ == Swing::Aiming || swing_ == Swing::Held);
    }
    useTriggerDown_ = useTrigger;
    // Armes de mêlée : gâchette droite tenue = arme tenue à deux mains (axe des deux mains)
    const bool twoHand = !bow && !IsTriggerAttackMode() && triggerHeld_;
    if (twoHand != IsTwoHandGrip())
        LOG_XR("gachette droite : prise a deux mains %s", twoHand ? "oui" : "non");
    SetTwoHandGrip(twoHand);
    SetDrawing(drawing_);
    const bool throwHeld = IsTriggerAttackMode() && (attackButton || triggerHeld_);
    const bool attack = swingHeld || drawing_ || throwHeld || (!bow && attackButton);
    const bool block = GetFloat(session, block_, XR_NULL_PATH) > kActionThreshold;
    input::InjectMouseButton(0, attack);
    input::InjectMouseButton(1, block);

    input::InjectKey(kKeyF, GetFloat(session, use_, XR_NULL_PATH) > kActionThreshold); // poignée gauche
    // Bouclier : « bouclier suivant » du jeu (molette vers le bas, code 0xEF) sur B
    const bool shield = GetBool(session, shield_, XR_NULL_PATH);
    if (shield && !shieldDown_) {
        input::InjectMouseWheel(-1);
        LOG_XR("B : bouclier (molette vers le bas)");
    }
    shieldDown_ = shield;

    // Source du réticule : regard -> main gauche -> main droite. Sur les profils où ce bouton
    // sert aussi d'Échap (X des Touch) : appui court = Échap, tenu 1 s = source du réticule ;
    // sinon à l'appui. Menu radial ouvert : retour, sans rien d'autre.
    static const char* const kAimNames[] = {"regard", "main gauche", "main droite"};
    const ULONGLONG aimNow = GetTickCount64();
    if (aimMode && !aimModeDown_) {
        aimModeSinceMs_ = aimNow;
        aimModeEscape_ = GetBool(session, escapeAlt_, XR_NULL_PATH);
        aimModeLong_ = radialWasOpen;
        if (!aimModeLong_ && !aimModeEscape_) {
            aimModeLong_ = true;
            LOG_XR("reticule : %s", kAimNames[static_cast<int>(CycleAimSource())]);
        }
    } else if (aimMode && !aimModeLong_ && aimNow - aimModeSinceMs_ >= kAimHoldMs) {
        aimModeLong_ = true;
        LOG_XR("X tenu : reticule : %s", kAimNames[static_cast<int>(CycleAimSource())]);
    } else if (!aimMode && aimModeDown_ && !aimModeLong_ && aimModeEscape_) {
        LOG_XR("X : Echap");
        SendKeys({kKeyEscape});
    }
    aimModeDown_ = aimMode;
    // Clic du stick droit : appui court = coup de pied (E), tenu 1 s = vue 1re / 3e personne (R)
    const bool view = GetBool(session, view_, XR_NULL_PATH);
    const ULONGLONG viewNow = GetTickCount64();
    if (view && !viewDown_) {
        viewSinceMs_ = viewNow;
        viewLong_ = false;
    }
    if (view && !viewLong_ && viewNow - viewSinceMs_ >= kViewHoldMs) {
        viewLong_ = true;
        SendKeys({kKeyR});
        NoteManualViewToggle();
        LOG_XR("stick droit tenu : vue 1re / 3e personne (touche R)");
    }
    if (!view && viewDown_ && !viewLong_) {
        SendKeys({kKeyE});
        LOG_XR("stick droit : coup de pied (touche E)");
    }
    viewDown_ = view;
    // Vue 1re personne par défaut : le jeu commence la mission en 3e personne
    if (TakeFirstPersonViewRequest()) {
        SendKeys({kKeyR});
        LOG_XR("vue 1re personne par defaut (touche R)");
    }
    input::InjectKey(kKeyTab, GetBool(session, tabMenu_, XR_NULL_PATH));

    // Bouton A : appui court = arme suivante (un cran de molette vers le haut, au relâchement) ;
    // appui long = mode de l'arme (touche X du jeu : baïonnette, une / deux mains, lance couchée)
    const bool weapon = GetFloat(session, weapon_, XR_NULL_PATH) > kActionThreshold;
    const ULONGLONG weaponNow = GetTickCount64();
    if (weapon && !weaponDown_) {
        weaponSinceMs_ = weaponNow;
        weaponLong_ = false;
    }
    if (weapon && !weaponLong_ && weaponNow - weaponSinceMs_ >= kWeaponModeHoldMs) {
        weaponLong_ = true;
        SendKeys({kKeyX});
        LOG_XR("A tenu : mode de l'arme (touche X)");
    }
    if (!weapon && weaponDown_ && !weaponLong_) {
        input::InjectMouseWheel(1);
        LOG_XR("changer d'arme (molette)");
    }
    weaponDown_ = weapon;

    if (!loggedCombat_ && (attack || block || std::fabs(move.x) > kMoveThreshold || std::fabs(move.y) > kMoveThreshold)) {
        LOG_XR("premiere commande de combat (attaque %d, blocage %d, deplacement %.1f %.1f)", attack, block, move.x,
               move.y);
        loggedCombat_ = true;
    }
}

} // namespace xr
