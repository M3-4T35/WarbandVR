#pragma once
// Manettes OpenXR. Deux jeux d'actions, un seul actif à la fois :
//  - menu (écran virtuel) : rayon de visée vers l'écran, gâchette = clic, bouton secondaire
//    = clic droit, stick vertical = molette ;
//  - combat (stéréo), injecté comme clavier/souris dans DirectInput (touches par défaut du
//    jeu) : stick gauche = déplacement, clic = source du réticule ; menu = Échap ; Y = Tab ;
//    X = vue stratégique (écran d'ordres du jeu, carte) ; B = mettre / retirer le bouclier ; gâchette gauche = bloquer ; poignée gauche = utiliser (F) ;
//    stick droit = tourner, haut = sauter, clic = vue 1re/3e personne ; A = changer d'arme ;
//    poignée droite = attaque de secours ; gâchette droite = bander l'arc. Attaque au geste :
//    un mouvement rapide de la manette droite donne un coup dans sa direction.
// Associations suggérées pour le plus de manettes possible (profil générique, Touch, Index,
// Vive, WMR). OpenXR n'autorise qu'un attachement des jeux d'actions par session et une
// suggestion par profil : tout est donc déclaré ici, en une fois.
#include "xr/xr_session.h"

#include <openxr/openxr.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace xr {

class XrControls {
public:
    // Actions et associations (avant xrBeginSession). false : manettes indisponibles.
    bool Init(XrInstance instance, XrSession session);
    void Shutdown();

    // Écran virtuel : quad centré en (0, 0, -distance) dans l'espace de référence, face à +z
    // Clavier virtuel éventuel : même plan, centré en (0, keyboardY), keyboardWidth x keyboardHeight.
    struct Screen {
        float distance, width, height;
        bool keyboard = false;
        float keyboardY = 0, keyboardWidth = 0, keyboardHeight = 0;
    };

    // Par frame, session en cours. combat = la stéréo est affichée (sinon, menus).
    // screenActive = l'écran virtuel est affiché. Renvoie true si le rayon touche l'écran ;
    // hit = point touché dans l'espace de référence.
    // head : pose de la tête dans space (nullptr si inconnue), repère des gestes de combat.
    bool Update(XrSession session, XrSpace space, XrTime time, const XrPosef* head, bool focused, bool combat,
                bool screenActive, const Screen& screen, XrVector3f& hit);

    // Après Update : pose de visée de chaque main (0 = gauche, 1 = droite), main active et
    // longueur de son rayon (jusqu'à l'écran s'il est touché)
    struct Hand {
        bool valid = false;
        XrPosef pose{};
    };
    const Hand& GetHand(int h) const { return handState_[h]; }
    int ActiveHand() const { return activeHand_; }
    float RayLength() const { return rayLength_; }
    // Après Update : pointeur sur le clavier virtuel en (u, v) normalisés, nouvel appui dessus
    bool KeyboardHover(float& u, float& v) const
    {
        u = keyboardU_, v = keyboardV_;
        return keyboardHover_;
    }
    bool KeyboardClick() const { return keyboardClick_; }

    // Menu radial des ordres (combat, X pour ouvrir/fermer) : entrées et entrée visée au stick gauche
    struct Radial {
        bool open = false;
        bool groups = false; // choix des troupes (sinon : menu d'ordres du jeu)
        int count = 0;
        std::wstring labels[9];
        int highlight = -1;
        int depth = 0;       // 0 = choix des troupes, puis niveaux du menu du jeu
        bool waiting = false; // réponse du jeu attendue
    };
    const Radial& GetRadial() const { return radial_; }
    // Appui long sur le bouton menu : recentrage demandé (lu une fois)
    bool TakeRecenterRequest()
    {
        const bool r = recenterRequest_;
        recenterRequest_ = false;
        return r;
    }
    // Consigne à afficher devant les yeux (placement du drapeau), nullptr sinon
    const wchar_t* Hint() const
    {
        if (!heldKey_)
            return nullptr;
        return french_ ? L"Placez le drapeau avec la main gauche — clic du stick gauche pour valider"
                       : L"Place the flag with your left hand — click the left stick to confirm";
    }

private:
    void UpdateMenu(XrSession session, XrSpace space, XrTime time, bool focused, bool screenActive,
                    const Screen& screen, XrVector3f& hit, bool& onScreen);
    void UpdateCombat(XrSession session, XrSpace space, XrTime time, const XrPosef* head);
    bool UpdateSwing(XrSpace space, XrTime time, const XrPosef* head); // true : bouton d'attaque tenu
    void ReleaseAll();
    void UpdateRadial(bool toggle, bool back, const XrVector2f& stick);
    void ProcessKeyQueue();
    void UpdateMenuButton(bool pressed);
    void SendKeys(std::initializer_list<int> keys, const std::vector<int>& prefix = {});
    void CloseRadial(const char* why);
    std::string PathKey() const;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrPath hands_[2]{};
    bool ready_ = false;

    // Jeu d'actions « menu »
    XrActionSet menuSet_ = XR_NULL_HANDLE;
    XrAction aim_ = XR_NULL_HANDLE;
    XrAction select_ = XR_NULL_HANDLE;
    XrAction secondary_ = XR_NULL_HANDLE;
    XrAction scroll_ = XR_NULL_HANDLE;
    XrAction closeOrders_ = XR_NULL_HANDLE; // X dans l'écran d'ordres : le refermer
    XrAction rotate_ = XR_NULL_HANDLE;      // stick gauche/droite : tourner la caméra (carte du monde)
    XrAction ctrl_ = XR_NULL_HANDLE;        // poignée : Ctrl (Ctrl+clic dans l'inventaire)
    XrAction menuEscape_ = XR_NULL_HANDLE;  // bouton menu : Échap
    XrSpace aimSpaces_[2]{};

    // Jeu d'actions « combat »
    XrActionSet combatSet_ = XR_NULL_HANDLE;
    XrAction move_ = XR_NULL_HANDLE;
    XrAction turn_ = XR_NULL_HANDLE;
    XrAction attack_ = XR_NULL_HANDLE;
    XrAction block_ = XR_NULL_HANDLE;
    XrAction aimMode_ = XR_NULL_HANDLE;
    XrAction view_ = XR_NULL_HANDLE;
    XrAction use_ = XR_NULL_HANDLE;
    XrAction draw_ = XR_NULL_HANDLE;
    XrAction weapon_ = XR_NULL_HANDLE;
    XrAction tabMenu_ = XR_NULL_HANDLE;
    XrAction orders_ = XR_NULL_HANDLE;
    XrAction escape_ = XR_NULL_HANDLE;
    XrAction escapeAlt_ = XR_NULL_HANDLE; // X (Touch) : Échap au lieu du bouton menu (SteamVR)
    XrAction shield_ = XR_NULL_HANDLE;
    XrAction handPose_ = XR_NULL_HANDLE; // pose de préhension des mains en combat (gestes, bras)
    XrSpace combatHandSpaces_[2]{};
    XrAction combatAim_ = XR_NULL_HANDLE; // pose de visée en combat (visée du jeu)
    XrSpace combatAimSpaces_[2]{};

    bool combatMode_ = false;

    // État du menu
    Hand handState_[2];
    float rayLength_ = 0;
    int activeHand_ = 1; // main qui pointe (droite par défaut, la dernière qui clique)
    bool selectDown_[2]{};
    bool leftButtonHeld_ = false;
    bool rightButtonHeld_ = false;
    unsigned long long lastScrollMs_ = 0;
    bool keyboardHover_ = false;
    bool keyboardClick_ = false;
    float keyboardU_ = 0, keyboardV_ = 0;

    // État du combat
    enum class Swing { Idle, Measuring, Aiming, Held, Cooldown };
    Swing swing_ = Swing::Idle;
    int swingDx_ = 0, swingDy_ = 0;       // mouvement de souris de la direction choisie
    XrVector3f swingStart_{};             // position de la main au début du geste
    unsigned long long swingSinceMs_ = 0; // début de l'état courant
    XrVector3f lastHandPos_{};
    XrTime lastHandTime_ = 0;
    bool haveLastHand_ = false;
    int loggedSwings_ = 0;
    bool weaponDown_ = false;
    bool weaponLong_ = false;         // appui long déjà traité (mode de l'arme)
    bool viewDown_ = false, viewLong_ = false; // clic du stick droit : coup de pied / vue
    unsigned long long viewSinceMs_ = 0;
    unsigned long long weaponSinceMs_ = 0;
    bool drawing_ = false;       // arc bandé (gâchette droite tenue après encochage)
    bool useTriggerDown_ = false;
    bool triggerHeld_ = false;                         // gâchette droite (hystérésis)
    bool triggerLogged_ = false, gripLogged_ = false;  // derniers états journalisés
    bool aimModeDown_ = false;
    bool aimModeLong_ = false;   // X tenu : source du réticule changée (pas d'Échap au relâchement)
    bool aimModeEscape_ = false; // ce bouton sert aussi d'Échap sur ce profil
    unsigned long long aimModeSinceMs_ = 0;
    bool shieldDown_ = false;
    bool ordersOpen_ = false; // vue stratégique ouverte par X
    bool closeDown_ = false;
    bool closeHeld_ = false;
    Radial radial_;
    int radialKeys_[9]{};
    bool radialArmed_ = false; // stick poussé sur une entrée : la valider au retour au centre
    bool ordersButtonDown_ = false;
    // Chemin dans le menu d'ordres : touche de groupe puis touches F. Les libellés de chaque
    // niveau sont mémorisés (le menu du jeu se ferme seul au bout de quelques secondes) ; une
    // sélection rejoue au besoin tout le chemin.
    std::vector<int> path_;
    std::map<std::string, std::vector<std::string>> labelCache_;
    // Touches à envoyer, une à la fois (appui puis courte pause)
    std::deque<int> keyQueue_;
    int tapKey_ = 0;
    int heldKey_ = 0; // « Tenez cette position » : F1 maintenu (drapeau) jusqu'au bouton des ordres
    AimSource flagSavedAim_ = AimSource::Head; // réticule d'avant le placement du drapeau
    void EndFlagPlacement();
    unsigned long long tapUntil_ = 0, gapUntil_ = 0, lastKeyMs_ = 0;
    // Dernière sélection en attente de la réponse du jeu
    bool pending_ = false;
    int pendingKey_ = 0;
    unsigned long long pendingSince_ = 0;
    std::vector<std::string> labelsBefore_;
    std::string lastMenuLog_;
    // Bouton menu : appui court = Échap, appui long = recentrage
    bool menuButtonDown_ = false;
    bool menuButtonLong_ = false;
    unsigned long long menuButtonSince_ = 0;
    bool recenterRequest_ = false;
    bool snapLatched_ = false; // rotation par crans : stick à relâcher avant le cran suivant
    bool french_ = false;
    unsigned long long lastCombatMs_ = 0;

    bool loggedHit_ = false;
    bool loggedProfile_ = false;
    bool loggedCombat_ = false;
    int loggedPresses_ = 0;
};

} // namespace xr
