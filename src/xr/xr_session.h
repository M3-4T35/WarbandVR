#pragma once

namespace xr {

// Pose et champ de vue d'un œil dans l'espace LOCAL d'OpenXR
struct EyeView {
    float qx = 0, qy = 0, qz = 0, qw = 1;
    float px = 0, py = 0, pz = 0;
    float angleLeft = 0, angleRight = 0, angleUp = 0, angleDown = 0; // radians
};

// Pose de la tête dans l'espace LOCAL d'OpenXR (x droite, y haut, z vers l'arrière),
// et vues des deux yeux (0 = gauche, 1 = droite)
struct HeadPose {
    float qx = 0, qy = 0, qz = 0, qw = 1;
    float px = 0, py = 0, pz = 0;
    EyeView eyes[2];
};

// Dernière pose connue, prédite pour l'affichage. false si la session ne tourne pas.
bool GetHeadPose(HeadPose& out);

// Pose de préhension d'une manette (grip : origine dans la paume, -Z le long d'un objet tenu)
// dans l'espace LOCAL (combat uniquement)
struct HandPose {
    bool valid = false;
    float qx = 0, qy = 0, qz = 0, qw = 1;
    float px = 0, py = 0, pz = 0;
    // Pose de visée (aim) : direction où pointe la manette, pour la visée du jeu
    bool aimValid = false;
    float aqx = 0, aqy = 0, aqz = 0, aqw = 1;
};
// Publiées par le thread XR en combat (0 = gauche, 1 = droite) ; invalides hors combat
void PublishHands(const HandPose hands[2]);
void GetHands(HandPose out[2]);

// Rotation du corps demandée au stick (radians, positif = vers la gauche), cumulée par le
// thread XR et consommée par le rendu du jeu
void AddTurn(float radians);
float TakeTurn();

// Un coup au geste est en cours : la visée ne doit pas injecter de mouvement de souris
void SetSwingBusy(bool busy);
bool IsSwingBusy();

// Le joueur tient un arc (objet dans la main gauche) : détecté par le moteur
void SetBowMode(bool bow);
bool IsBowMode();
// Arbalète, arme à feu ou de jet en main : pas d'attaque au geste (tir ou lancer à la
// gâchette / poignée droite)
// Début de mission en vue 3e personne : la manette envoie une fois la touche de vue (R)
void RequestFirstPersonView();
bool TakeFirstPersonViewRequest();
void SetTriggerAttackMode(bool on);
bool IsTriggerAttackMode();

// La corde est tirée (gâchette droite tenue après encochage) : la visée suit l'axe de la
// flèche, de la main droite vers la main gauche
void SetDrawing(bool drawing);
bool IsDrawing();
// Visée de l'arc à deux mains : corde tirée, ou moins d'une seconde après le tir
bool IsBowAiming();

// Source du réticule (visée du jeu) : regard (centre de la vue, défaut), main gauche, main
// droite. Changée au clic du stick gauche.
enum class AimSource { Head, LeftHand, RightHand };

// Arme toujours active (combat physique, mêlée uniquement) : active par défaut, F10 (clavier)
// pour la basculer au diagnostic
bool ToggleAlwaysActive();
bool IsAlwaysActive();

// Vitesse de la main droite (m/s), pour l'arme toujours active du combat physique
void SetBladeSpeed(float speed);
float GetBladeSpeed();
AimSource CycleAimSource();
AimSource GetAimSource();
void SetAimSource(AimSource source);

// Démarre le thread OpenXR : instance, session D3D11, puis affichage de l'image du jeu
// sur un écran virtuel. En cas d'échec (pas de runtime, casque absent...) on loggue et
// le jeu continue normalement.
void Start();

// Demande l'arrêt du thread (non bloquant : utilisable depuis DllMain)
void RequestStop();

} // namespace xr
