// Caméra : rendu stéréo alterné de la scène principale.
//
// rglScene::render(ctx) remplit le contexte de rendu et fait le culling avec ctx->camera ;
// on y place la caméra sur l'œil rendu à cette frame (gauche et droit en alternance) :
// orientation du casque (on garde le cap du jeu), origine décalée de la position de l'œil,
// champ de vue asymétrique de l'œil, puis on recalcule le frustum. Seul le contexte de rendu est modifié, jamais
// l'état du jeu : pas de dérive, et la logique du jeu (visée, IA) n'est pas touchée.
// rglRenderer::renderContext rend ensuite une copie de ce contexte (pool du moteur, champ
// source) : on y retrouve la vue préparée par l'orientation exacte de la caméra et on la
// publie pour que le thread XR affiche l'image avec la bonne orientation.
#include "engine/camera_hook.h"
#include "engine/combat_hook.h"
#include "engine/rgl.h"
#include "engine/true_stereo.h"
#include "input/dinput_hook.h"
#include "render/d3d9_hook.h"
#include "render/frame_profile.h"
#include "render/frame_share.h"
#include "render/ui_redirect.h"
#include "util/config.h"
#include "util/log.h"
#include "util/pattern.h"
#include "xr/xr_session.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)

namespace engine {
namespace {

// rglScene::render : prologue différent selon la version de WSE2 (registres sauvegardés)
constexpr char kSceneRenderSigJul2026[] =
    "44 88 44 24 18 55 53 56 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? 00 00";
constexpr char kSceneRenderSigSep2026[] =
    "44 88 44 24 18 55 53 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? 00 00 8B 05";
constexpr char kRenderContextSig[] = "48 8B C4 44 88 40 18 55 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
                                     "48 89 58 E8 48 89 70 E0 48 89 78 D8 4C 89 60 D0 4C 89 68 C8 4C 89 70 C0";
constexpr char kComputePlanesSig[] =
    "40 55 48 8B EC 48 83 EC 50 80 39 00 0F 29 7C 24 30 4C 8B D1 C7 41 04 06 00 00 00 0F 85";

// mbMapWindow::updateCamera(float) : caméra de la carte du monde, à chaque image où la carte est active
constexpr char kMapUpdateCameraSig[] = "48 8B C4 55 48 8D 68 D8 48 81 EC 20 01 00 00 F3 0F 10 1D";

// Valeurs de retour transmises telles quelles (rax), quel que soit leur type réel
using SceneRenderFn = uintptr_t(__fastcall*)(void* scene, rgl::RenderContext* ctx, bool arg);
using RenderContextFn = uintptr_t(__fastcall*)(void* renderer, rgl::RenderContext* ctx, bool arg);
using ComputePlanesFn = void(__fastcall*)(rgl::Camera* camera);

SceneRenderFn g_sceneRender = nullptr;
RenderContextFn g_renderContext = nullptr;
ComputePlanesFn g_computePlanes = nullptr;
void* g_sceneRenderTarget = nullptr;
void* g_renderContextTarget = nullptr;

// --- Carte du monde immersive ------------------------------------------------------------
// mbMapWindow::updateCamera est appelé à chaque image où la carte est affichée : on y relève la
// fenêtre. Sa scène est rendue par son widget de scène (m_sceneWidget, +0x70), dont le contexte de
// rendu est rglSceneWidget::m_renderContext (+0x280).
using MapUpdateCameraFn = void(__fastcall*)(void* window, float dt);
MapUpdateCameraFn g_mapUpdateCamera = nullptr;
void* g_mapUpdateCameraTarget = nullptr;
constexpr size_t kMapSceneWidget = 0x70;     // mbMapWindow::m_sceneWidget
constexpr size_t kMapCameraZoom = 0x2D0;     // mbMapWindow::m_cameraZoom (distance au point visé)
constexpr size_t kWidgetRenderContext = 0x280; // rglSceneWidget::m_renderContext
constexpr ULONGLONG kMapActiveMs = 250;
std::atomic<char*> g_mapWindow{nullptr};
std::atomic<ULONGLONG> g_mapFrameMs{0};
bool g_loggedMap = false;

void __fastcall HkMapUpdateCamera(void* window, float dt)
{
    g_mapUpdateCamera(window, dt);
    g_mapWindow = static_cast<char*>(window);
    g_mapFrameMs = GetTickCount64();
}

// Contexte de la scène de la carte, carte affichée et réglage actif
bool IsMapContext(const rgl::RenderContext* ctx)
{
    char* window = g_mapWindow;
    if (!window || GetTickCount64() - g_mapFrameMs > kMapActiveMs || !config::GetBool(config::Id::ImmersiveMap))
        return false;
    char* widget = *reinterpret_cast<char**>(window + kMapSceneWidget);
    return widget && reinterpret_cast<const char*>(ctx) == widget + kWidgetRenderContext;
}

// Le jeu lit la caméra du contexte de son widget pour viser au curseur (getCursorRay : clic sur la
// carte, infobulles) et pour placer ses libellés à l'écran (getScreenProjection). Ce contexte garde
// la caméra du dernier œil rendu : le temps de ces appels, on lui remet celle du jeu.
using CursorRayFn = uintptr_t(__fastcall*)(void* widget, void* nearPoint, void* farPoint);
using ScreenProjectionFn = uintptr_t(__fastcall*)(void* widget, void* out, void* point);
constexpr char kCursorRaySig[] = "48 8B C4 48 83 EC 58 80 B9 08 03 00 00 00 F3 0F 10 0D";
constexpr char kScreenProjectionSig[] = "48 83 EC 18 80 B9 08 03 00 00 00 F3 41 0F 10 18";
CursorRayFn g_cursorRay = nullptr;
ScreenProjectionFn g_screenProjection = nullptr;
void* g_cursorRayTarget = nullptr;
void* g_screenProjectionTarget = nullptr;
std::mutex g_mapCameraMutex;
rgl::Camera g_mapGameCamera{}; // caméra du jeu de la dernière image stéréo de la carte
bool g_haveMapCamera = false;
int g_loggedMapSwaps = 0;

class MapCameraSwap {
public:
    explicit MapCameraSwap(void* widget, const char* what)
    {
        char* window = g_mapWindow;
        if (!widget || !window || *reinterpret_cast<void**>(window + kMapSceneWidget) != widget)
            return;
        std::lock_guard lock(g_mapCameraMutex);
        if (!g_haveMapCamera)
            return;
        cam_ = &reinterpret_cast<rgl::RenderContext*>(static_cast<char*>(widget) + kWidgetRenderContext)->camera;
        saved_ = *cam_;
        *cam_ = g_mapGameCamera;
        if (g_loggedMapSwaps < 2) {
            ++g_loggedMapSwaps;
            LOG_ENGINE("carte immersive : %s calcule avec la camera du jeu", what);
        }
    }
    ~MapCameraSwap()
    {
        if (cam_)
            *cam_ = saved_;
    }
    MapCameraSwap(const MapCameraSwap&) = delete;
    MapCameraSwap& operator=(const MapCameraSwap&) = delete;

private:
    rgl::Camera* cam_ = nullptr;
    rgl::Camera saved_{};
};

uintptr_t __fastcall HkCursorRay(void* widget, void* nearPoint, void* farPoint)
{
    MapCameraSwap swap(widget, "rayon du curseur");
    return g_cursorRay(widget, nearPoint, farPoint);
}

uintptr_t __fastcall HkScreenProjection(void* widget, void* out, void* point)
{
    MapCameraSwap swap(widget, "projection a l'ecran");
    return g_screenProjection(widget, out, point);
}

float MapZoom()
{
    char* window = g_mapWindow;
    const float zoom = window ? *reinterpret_cast<float*>(window + kMapCameraZoom) : 0.0f;
    return zoom > 0.1f && zoom < 10000.0f ? zoom : 30.0f;
}

// Dernières vues préparées par rglScene::render, retrouvées au rendu par l'orientation de
// la caméra (le thread de jeu peut avoir une frame d'avance sur le thread de rendu)
struct Prepared {
    rgl::Vector4 s{}, f{}, o{};
    render::RenderView view;
};
constexpr int kPrepared = 8; // deux images par image du jeu en vraie stéréo, une image d'avance
std::mutex g_mutex;
Prepared g_prepared[kPrepared];
int g_preparedNext = 0;

// Rendu alterné, décidé ici seulement (thread du jeu) : chaque paire = deux images
// consécutives de la scène, un œil chacune, avec la même pose de tête. Le numéro de paire
// suit l'image jusqu'au casque, qui n'affiche que des paires complètes.
int g_indexInPair = 0;    // rang de la prochaine image dans sa paire (0 ou 1)
uint64_t g_pairId = 0;    // paire en cours
xr::HeadPose g_pairHead;  // pose de tête de la paire en cours

bool SameAxis(const rgl::Vector4& a, const rgl::Vector4& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool g_loggedApply = false; // stéréo appliquée au moins une fois
bool g_loggedOverwrite = false;
bool g_loggedPublish = false;

// --- Gestion du roulis (touche F9) -------------------------------------------------
// L'orientation du casque est décomposée en cap/tangage et roulis (autour de l'axe de
// visée). Le cap et le tangage sont toujours rendus par le jeu ; pour le roulis :
//  - compositeur inversé (défaut, choisi au test casque) : image rendue sans roulis,
//    pose déclarée avec le roulis inversé ;
//  - compositeur : image rendue sans roulis, pose déclarée sans roulis, le casque tourne
//    l'image pour compenser ;
//  - jeu : roulis rendu par le jeu et pose réelle déclarée.
enum RollMode { kRollCompositorInverted, kRollCompositor, kRollGame, kRollModes };
const char* const kRollModeName[] = {"compositeur inverse", "compositeur", "jeu"};
std::atomic<int> g_rollMode{kRollCompositorInverted};
bool g_f9Down = false;

bool g_f11Down = false;

void PollDebugKeys()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool focused = pid == GetCurrentProcessId();
    const bool down = focused && (GetAsyncKeyState(VK_F9) & 0x8000);
    if (down && !g_f9Down) {
        const int mode = (g_rollMode + 1) % kRollModes;
        g_rollMode = mode;
        LOG_ENGINE("F9 : roulis en mode %s", kRollModeName[mode]);
    }
    g_f9Down = down;
    const bool down11 = focused && (GetAsyncKeyState(VK_F11) & 0x8000);
    if (down11 && !g_f11Down) {
        LOG_ENGINE("F11 : capture PNG demandee");
        render::RequestDump();
    }
    g_f11Down = down11;
}

struct Quat {
    float x, y, z, w;
};

Quat Mul(const Quat& a, const Quat& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat Conj(const Quat& q)
{
    return {-q.x, -q.y, -q.z, q.w};
}

// Décompose q = cap/tangage * roulis (roulis autour de l'axe de visée)
void SplitRoll(const Quat& q, Quat& yawPitch, Quat& roll)
{
    // Direction de visée = q * (0, 0, -1)
    const float fx = -2 * (q.x * q.z + q.y * q.w);
    const float fy = -2 * (q.y * q.z - q.x * q.w);
    const float fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz);
    const float pitch = std::asin(fy < -1 ? -1 : fy > 1 ? 1 : fy);
    const Quat qy{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    const Quat qx{std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
    yawPitch = Mul(qy, qx);
    roll = Mul(Conj(yawPitch), q);
}

bool IsMainScene(const rgl::RenderContext* ctx)
{
    return ctx->camera.isPerspective && ctx->isFinalRender && ctx->scene;
}

// Interface (HUD, menus) : contextes du gestionnaire de fenêtres, sans scène, drapeau
// 0x1000. La plupart sont orthographiques, mais celui des éléments 3D de l'interface
// (cadres d'infobulle, comme le nom de la cible) est en perspective.
constexpr uint32_t kUiContextFlag = 0x1000;

bool IsUiContext(const rgl::RenderContext* ctx)
{
    return !ctx->scene && (ctx->flags & kUiContextFlag);
}

rgl::Vector4 Combine(float a, const rgl::Vector4& x, float b, const rgl::Vector4& y, float c, const rgl::Vector4& z,
                     float w)
{
    return {a * x.x + b * y.x + c * z.x, a * x.y + b * y.y + c * z.y, a * x.z + b * y.z + c * z.z, w};
}

// Remplace l'orientation de la caméra : cap du corps + orientation complète du casque.
// Repère caméra (s droite, f haut, u arrière) = repère OpenXR (x, y, z). (bx, by) : arrière
// horizontal unitaire du corps.
// Orientation q (espace LOCAL) appliquée sur un repère de base (s0, f0, u0) = axes LOCAL x, y, z
// dans le monde
void ApplyOrientation(rgl::Camera& cam, float x, float y, float z, float w, const rgl::Vector4& s0,
                      const rgl::Vector4& f0, const rgl::Vector4& u0)
{
    rgl::Transform& t = cam.transform;
    // Matrice de rotation du quaternion : colonne j = axe j tourné
    const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    const float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);

    t.s = Combine(r00, s0, r10, f0, r20, u0, t.s.w);
    t.f = Combine(r01, s0, r11, f0, r21, u0, t.f.w);
    t.u = Combine(r02, s0, r12, f0, r22, u0, t.u.w);
}

void ApplyHead(rgl::Camera& cam, const xr::HeadPose& h, float bx, float by)
{
    ApplyOrientation(cam, h.qx, h.qy, h.qz, h.qw, {-by, bx, 0, 0}, {0, 0, 1, 0}, {bx, by, 0, 0});
}

// Un menu attend la souris (Tab, inventaire, dialogue...) : le jeu affiche son curseur.
// On suit ce qu'il demande à Direct3D (ShowCursor) ; le curseur Windows n'est fiable que si
// le vrai curseur est sur la fenêtre, ce qui n'est plus le cas quand elle dépasse l'écran.
bool g_cursorWasShown = false;

bool MenuOpen()
{
    bool shown;
    const int game = render::GameCursorState();
    if (game >= 0) {
        shown = game == 1;
    } else {
        CURSORINFO ci{sizeof(ci)};
        shown = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
    }
    if (shown != g_cursorWasShown) {
        LOG_ENGINE("curseur %s : %s", shown ? "visible" : "masque", shown ? "ecran virtuel" : "stereo");
        g_cursorWasShown = shown;
    }
    return shown;
}

// Caméra du jeu avant nos modifications (pour situer le joueur, bras VR)
std::mutex g_gameCameraMutex;
rgl::Transform g_gameCamera{};
bool g_haveGameCamera = false;

// --- Cap du corps et visée à la main gauche ----------------------------------------------
// En stéréo, la vue VR suit un cap du corps qui nous appartient (tourné au stick) ; la
// caméra du jeu, qui porte la visée (coups, arc) et la direction de marche, est asservie
// à la direction de la manette gauche par des mouvements de souris injectés. Le rapport
// souris -> angle dépend de la sensibilité du jeu : il est mesuré en continu.
constexpr float kPi = 3.14159265f;
float AimGain() { return config::Get(config::Id::AimGain); } // part de l'écart corrigée par frame
constexpr int kAimMaxStep = 300;        // mouvement de souris maximal par frame
constexpr float kAimDeadzone = 0.004f;  // radians : écart ignoré (pas de tremblement)

bool g_bodyActive = false;
float g_bodyYaw = 0;                    // cap du corps (angle du vecteur avant, monde Z haut)
float g_kYaw = -0.003f, g_kPitch = -0.003f; // radians par unité de souris (mesurés)
int g_lastDx = 0, g_lastDy = 0;         // mouvement injecté à la frame précédente
// Mode souris : mouvement de la vraie souris depuis la dernière correction de visée (le jeu l'a déjà
// appliqué à sa caméra), et dernier mouvement vertical : la hauteur n'est ramenée sur le regard
// qu'après une pause, pour que le jeu lise bien le geste de la souris comme direction du coup
int g_realSinceAimDx = 0, g_realSinceAimDy = 0;
ULONGLONG g_lastRealDyMs = 0;
constexpr ULONGLONG kMousePitchHoldMs = 300;
float g_lastGameYaw = 0, g_lastGamePitch = 0;
bool g_loggedAim = false;
std::mutex g_bodyMutex;
float g_bodyBackX = 0, g_bodyBackY = 0;
bool g_haveBody = false;

float WrapAngle(float a)
{
    while (a > kPi)
        a -= 2 * kPi;
    while (a < -kPi)
        a += 2 * kPi;
    return a;
}

// Mesure du rapport souris -> angle d'après la réponse au mouvement précédent
void Calibrate(float& k, int injected, float observed)
{
    if (std::abs(injected) < 4)
        return;
    const float measured = observed / injected;
    // Valeurs aberrantes ignorées (autre cause de rotation, saut d'angle)
    if (measured * k <= 0 || std::fabs(measured) > 0.05f || std::fabs(measured) < 1e-5f)
        return;
    k = 0.8f * k + 0.2f * measured;
}

// Asservit la caméra du jeu sur la manette droite. forward : direction voulue (monde).
void AimAtHand(const rgl::Transform& game, float bx, float by)
{
    const int realDx = g_realSinceAimDx, realDy = g_realSinceAimDy;
    g_realSinceAimDx = g_realSinceAimDy = 0;
    xr::HandPose hands[2];
    xr::GetHands(hands);
    const rgl::Vector4 fwd{-game.u.x, -game.u.y, -game.u.z, 0};
    const float gameYaw = std::atan2(fwd.y, fwd.x);
    const float gamePitch = std::asin(std::clamp(fwd.z, -1.0f, 1.0f));

    // Le jeu a reçu notre correction précédente et la vraie souris
    Calibrate(g_kYaw, g_lastDx + realDx, WrapAngle(gameYaw - g_lastGameYaw));
    if (realDy == 0) // la vraie souris peut buter sur la limite de hauteur du jeu : mesure fausse
        Calibrate(g_kPitch, g_lastDy, gamePitch - g_lastGamePitch);
    g_lastDx = g_lastDy = 0;
    g_lastGameYaw = gameYaw, g_lastGamePitch = gamePitch;

    if (xr::IsSwingBusy())
        return;
    // Mode souris : visée au regard. Coup de mêlée tenu (bouton de la vraie souris) : la direction
    // est choisie, on ne touche plus à la caméra jusqu'au relâchement
    const bool mouse = xr::IsMouseMode();
    const bool ranged = xr::IsBowMode() || xr::IsTriggerAttackMode();
    if (mouse && !ranged && input::RealMouseButtonHeld())
        return;
    const bool holdPitch = mouse && GetTickCount64() - g_lastRealDyMs < kMousePitchHoldMs;
    // Direction voulue (espace LOCAL) : arc bandé (et 1 s après le tir) -> axe de la flèche
    // (main droite vers main gauche) ; sinon selon la source choisie (clic du stick gauche) : regard (centre de la
    // vue), main gauche ou main droite
    float dx, dy, dz;
    const bool bow = xr::IsBowMode();
    if (!mouse && bow && xr::IsBowAiming() && hands[0].valid && hands[1].valid) {
        dx = hands[0].px - hands[1].px, dy = hands[0].py - hands[1].py, dz = hands[0].pz - hands[1].pz;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 0.05f)
            return;
        dx /= len, dy /= len, dz /= len;
    } else {
        float x, y, z, w;
        const xr::AimSource source = mouse ? xr::AimSource::Head : xr::GetAimSource();
        if (source == xr::AimSource::Head) {
            xr::HeadPose head;
            if (!xr::GetHeadPose(head))
                return;
            x = head.qx, y = head.qy, z = head.qz, w = head.qw;
        } else {
            const xr::HandPose& aim = hands[source == xr::AimSource::LeftHand ? 0 : 1];
            if (!aim.aimValid)
                return;
            x = aim.aqx, y = aim.aqy, z = aim.aqz, w = aim.aqw;
        }
        dx = -2 * (x * z + y * w), dy = -2 * (y * z - x * w), dz = -(1 - 2 * (x * x + y * y));
    }
    // Axes monde : droite (-by, bx, 0), haut (0, 0, 1), arrière (bx, by, 0)
    const float wx = dx * -by + dz * bx, wy = dx * bx + dz * by, wz = dy;
    const float wantYaw = std::atan2(wy, wx);
    const float wantPitch = std::asin(std::clamp(wz, -1.0f, 1.0f));

    const float errYaw = WrapAngle(wantYaw - gameYaw), errPitch = wantPitch - gamePitch;
    int mx = 0, my = 0;
    if (std::fabs(errYaw) > kAimDeadzone)
        mx = std::clamp(static_cast<int>(std::lround(AimGain() * errYaw / g_kYaw)), -kAimMaxStep, kAimMaxStep);
    if (std::fabs(errPitch) > kAimDeadzone && !holdPitch)
        my = std::clamp(static_cast<int>(std::lround(AimGain() * errPitch / g_kPitch)), -kAimMaxStep, kAimMaxStep);
    if (mx || my)
        input::InjectMouseMove(mx, my);
    g_lastDx = mx, g_lastDy = my;
    static unsigned long long lastLog = 0;
    const unsigned long long now = GetTickCount64();
    if (now - lastLog > 10000) {
        LOG_ENGINE("visee : ecart %.1f / %.1f degres (cap / hauteur), souris %.5f / %.5f rad par unite",
                   errYaw * 180 / kPi, errPitch * 180 / kPi, g_kYaw, g_kPitch);
        lastLog = now;
    }
    if (!g_loggedAim) {
        LOG_ENGINE("visee a la main gauche active (souris : %.5f rad/unite en cap, %.5f en hauteur)", g_kYaw,
                   g_kPitch);
        g_loggedAim = true;
    }
}

// Place la caméra cam sur un œil : orientation du casque sur le cap du corps (bx, by), origine
// de la caméra du jeu (game) décalée de la position de l'œil, champ de vue asymétrique de
// l'œil, frustum recalculé. La vue est enregistrée pour être retrouvée au rendu.
void PrepareEye(rgl::Camera& cam, const rgl::Transform& game, const xr::HeadPose& head, int eyeIndex, float bx,
                float by, bool both, const rgl::RenderContext* ctx)
{
    const xr::EyeView& eye = head.eyes[eyeIndex];

    // renderHead : orientation appliquée à la caméra ; declaredQ : pose déclarée au casque,
    // selon le mode de roulis
    const Quat eyeQ{eye.qx, eye.qy, eye.qz, eye.qw};
    Quat renderQ = eyeQ, declaredQ = eyeQ;
    const int mode = g_rollMode;
    if (mode != kRollGame) {
        Quat yawPitch, roll;
        SplitRoll(eyeQ, yawPitch, roll);
        renderQ = yawPitch;
        declaredQ = mode == kRollCompositor ? yawPitch : Mul(yawPitch, Conj(roll));
    }
    xr::HeadPose renderHead = head;
    renderHead.qx = renderQ.x, renderHead.qy = renderQ.y, renderHead.qz = renderQ.z, renderHead.qw = renderQ.w;

    cam.transform = game;
    ApplyHead(cam, renderHead, bx, by);

    // Position de l'œil par rapport au centre de la tête, dans le repère de la tête,
    // reportée sur les axes de la caméra (1 unité du jeu = 1 m)
    const Quat headQ{head.qx, head.qy, head.qz, head.qw};
    const Quat d{eye.px - head.px, eye.py - head.py, eye.pz - head.pz, 0};
    const Quat local = Mul(Mul(Conj(headQ), d), headQ);
    rgl::Transform& t = cam.transform;
    t.o.x += local.x * t.s.x + local.y * t.f.x + local.z * t.u.x;
    t.o.y += local.x * t.s.y + local.y * t.f.y + local.z * t.u.y;
    t.o.z += local.x * t.s.z + local.y * t.f.z + local.z * t.u.z;

    // Champ de vue asymétrique de l'œil (le jeu construit sa projection avec
    // D3DXMatrixPerspectiveOffCenterRH à partir de ces valeurs). Un champ de vue
    // symétrique englobant a été essayé pour le nom de la cible : vision double au casque.
    if (cam.znear > 0) {
        cam.left = cam.znear * std::tan(eye.angleLeft);
        cam.right = cam.znear * std::tan(eye.angleRight);
        cam.top = cam.znear * std::tan(eye.angleUp);
        cam.bottom = cam.znear * std::tan(eye.angleDown);
    }
    g_computePlanes(&cam);

    render::RenderView view;
    view.valid = cam.znear > 0;
    view.eye = eyeIndex;
    view.pair = g_pairId;
    view.both = both;
    view.qx = declaredQ.x, view.qy = declaredQ.y, view.qz = declaredQ.z, view.qw = declaredQ.w;
    view.px = eye.px, view.py = eye.py, view.pz = eye.pz;
    view.angleLeft = eye.angleLeft;
    view.angleRight = eye.angleRight;
    view.angleUp = eye.angleUp;
    view.angleDown = eye.angleDown;
    {
        std::lock_guard lock(g_mutex);
        g_prepared[g_preparedNext] = {t.s, t.f, t.o, view};
        g_preparedNext = (g_preparedNext + 1) % kPrepared;
    }
    g_loggedApply = true;
    static int loggedMode = -1;
    if (loggedMode != (both ? 1 : 0)) {
        LOG_ENGINE("stereo (%s) appliquee a la scene (contexte %p, viewport %dx%d, near %.3f, decalage oeil %.1f mm, "
                   "thread %lu)",
                   both ? "vraie stereo : deux yeux par image" : "rendu alterne", ctx, ctx->viewport.width,
                   ctx->viewport.height, cam.znear,
                   std::sqrt(local.x * local.x + local.y * local.y + local.z * local.z) * 1000.0f,
                   GetCurrentThreadId());
        loggedMode = both ? 1 : 0;
    }
}

// Carte du monde : œil placé à la caméra du jeu, orientation du casque autour de celle de la caméra
// (son cap ET son inclinaison : regarder droit devant = la vue du jeu), position de l'œil dans
// l'espace LOCAL multipliée par l'échelle de la carte (unités par mètre) : relief de maquette, et se
// pencher rapproche. Roulis rendu tel quel (repère rigide, pas de compensation).
void PrepareMapEye(rgl::Camera& cam, const rgl::Camera& game, const xr::HeadPose& head, int eyeIndex, float scale,
                   bool both)
{
    const xr::EyeView& eye = head.eyes[eyeIndex];
    const rgl::Transform& g = game.transform;
    cam.transform = g;
    ApplyOrientation(cam, eye.qx, eye.qy, eye.qz, eye.qw, g.s, g.f, g.u);
    rgl::Transform& t = cam.transform;
    t.o.x = g.o.x + scale * (eye.px * g.s.x + eye.py * g.f.x + eye.pz * g.u.x);
    t.o.y = g.o.y + scale * (eye.px * g.s.y + eye.py * g.f.y + eye.pz * g.u.y);
    t.o.z = g.o.z + scale * (eye.px * g.s.z + eye.py * g.f.z + eye.pz * g.u.z);
    if (cam.znear > 0) {
        cam.left = cam.znear * std::tan(eye.angleLeft);
        cam.right = cam.znear * std::tan(eye.angleRight);
        cam.top = cam.znear * std::tan(eye.angleUp);
        cam.bottom = cam.znear * std::tan(eye.angleDown);
    }
    g_computePlanes(&cam);

    render::RenderView view;
    view.valid = cam.znear > 0 && game.znear > 0;
    view.eye = eyeIndex;
    view.pair = g_pairId;
    view.both = both;
    view.qx = eye.qx, view.qy = eye.qy, view.qz = eye.qz, view.qw = eye.qw;
    view.px = eye.px, view.py = eye.py, view.pz = eye.pz;
    view.angleLeft = eye.angleLeft;
    view.angleRight = eye.angleRight;
    view.angleUp = eye.angleUp;
    view.angleDown = eye.angleDown;
    view.map = true;
    if (game.znear > 0) {
        view.mapTanLeft = game.left / game.znear;
        view.mapTanRight = game.right / game.znear;
        view.mapTanUp = game.top / game.znear;
        view.mapTanDown = game.bottom / game.znear;
    }
    std::lock_guard lock(g_mutex);
    g_prepared[g_preparedNext] = {t.s, t.f, t.o, view};
    g_preparedNext = (g_preparedNext + 1) % kPrepared;
}

uintptr_t RenderMapStereo(void* scene, rgl::RenderContext* ctx, bool arg, xr::HeadPose head)
{
    PollDebugKeys();
    const double start = render::profile::NowMs();
    rgl::Camera& cam = ctx->camera;
    // Champ de vue du jeu élargi (réglage) : l'interface, posée sur ce champ de vue, couvre une plus grande
    // part de la vue ; le jeu vise avec cette même caméra (getCursorRay), donc pointer reste juste
    rgl::Camera game = cam;
    const float uiScale = config::Get(config::Id::MapUiScale);
    if (uiScale != 1.0f) {
        game.left *= uiScale;
        game.right *= uiScale;
        game.top *= uiScale;
        game.bottom *= uiScale;
        g_computePlanes(&game);
    }
    {
        std::lock_guard lock(g_mapCameraMutex);
        g_mapGameCamera = game;
        g_haveMapCamera = true;
    }
    const float scale = MapZoom() / config::Get(config::Id::MapDistance);
    if (!g_loggedMap) {
        LOG_ENGINE("carte du monde immersive (contexte %p) : distance de la camera %.1f, echelle %.1f unites par "
                   "metre, champ de vue %.0f degres (jeu x %.2f)",
                   ctx, MapZoom(), scale,
                   game.znear > 0 ? 2 * std::atan(game.right / game.znear) * 180 / kPi : 0.0f, uiScale);
        g_loggedMap = true;
    }
    uintptr_t result;
    rgl::RenderContext* second = stereo::Enabled() ? stereo::PrepareSecond(ctx) : nullptr;
    if (second) {
        g_indexInPair = 0;
        ++g_pairId;
        PrepareMapEye(cam, game, head, 0, scale, true);
        result = g_sceneRender(scene, ctx, arg);
        const stereo::SceneShadowState shadows = stereo::SaveSceneShadows(scene);
        PrepareMapEye(second->camera, game, head, 1, scale, true);
        g_sceneRender(scene, second, arg);
        stereo::RestoreSceneShadows(scene, shadows);
    } else {
        const int inPair = g_indexInPair;
        g_indexInPair ^= 1;
        const int eyeIndex = inPair ^ (config::GetBool(config::Id::EyeOrderSwap) ? 1 : 0);
        if (inPair == 0) {
            ++g_pairId;
            g_pairHead = head;
        } else {
            head = g_pairHead;
        }
        PrepareMapEye(cam, game, head, eyeIndex, scale, false);
        result = g_sceneRender(scene, ctx, arg);
    }
    g_loggedApply = true;
    render::profile::SceneFill(render::profile::NowMs() - start);
    return result;
}

uintptr_t __fastcall HkSceneRender(void* scene, rgl::RenderContext* ctx, bool arg)
{
    if (ctx && IsMainScene(ctx)) {
        {
            std::lock_guard lock(g_gameCameraMutex);
            g_gameCamera = ctx->camera.transform;
            g_haveGameCamera = true;
        }
        UpdatePlayerWeapon();
    }
    // Mode de roulis : réglage (F9 le change le temps de la session)
    static int configRoll = -1;
    if (config::GetInt(config::Id::RollMode) != configRoll) {
        configRoll = config::GetInt(config::Id::RollMode);
        g_rollMode = configRoll;
    }
    // Vraie souris lue par le jeu pour cette image (déjà appliquée à sa caméra)
    int realDx = 0, realDy = 0;
    if (ctx && IsMainScene(ctx))
        input::TakeRealMouseMotion(realDx, realDy);
    xr::HeadPose head;
    // Carte du monde immersive : en stéréo bien que le curseur soit affiché
    const bool map = ctx && IsMainScene(ctx) && IsMapContext(ctx);
    const bool stereoScene = ctx && IsMainScene(ctx) && xr::GetHeadPose(head) && (map || !MenuOpen());
    if (ctx && IsMainScene(ctx) && (!stereoScene || map)) {
        g_bodyActive = false;
        if (!stereoScene)
            g_indexInPair = 0; // la stéréo reprendra sur une paire neuve
        std::lock_guard lock(g_bodyMutex);
        g_haveBody = false;
    }
    if (ctx && IsMainScene(ctx) && !(stereoScene && map)) {
        std::lock_guard lock(g_mapCameraMutex);
        g_haveMapCamera = false;
    }
    if (stereoScene && map)
        return RenderMapStereo(scene, ctx, arg, head);
    if (!stereoScene) {
        if (ctx && IsMainScene(ctx)) {
            const double start = render::profile::NowMs();
            const uintptr_t result = g_sceneRender(scene, ctx, arg);
            render::profile::SceneFill(render::profile::NowMs() - start);
            return result;
        }
        return g_sceneRender(scene, ctx, arg);
    }

    PollDebugKeys();
    const double start = render::profile::NowMs();
    rgl::Camera& cam = ctx->camera;
    const rgl::Transform game = cam.transform;

    // Cap du corps : initialisé sur celui du jeu, puis tourné au stick (ou à la souris)
    if (!g_bodyActive) {
        g_bodyYaw = std::atan2(-game.u.y, -game.u.x);
        g_bodyActive = true;
        g_lastDx = g_lastDy = 0;
        g_realSinceAimDx = g_realSinceAimDy = 0;
        realDx = realDy = 0;
    }
    // Mode souris : le corps tourne avec la caméra du jeu, de ce que la souris lui a fait tourner
    g_realSinceAimDx += realDx, g_realSinceAimDy += realDy;
    if (realDy)
        g_lastRealDyMs = GetTickCount64();
    if (xr::IsMouseMode())
        g_bodyYaw += g_kYaw * realDx;
    g_bodyYaw = WrapAngle(g_bodyYaw + xr::TakeTurn());
    const float bx = -std::cos(g_bodyYaw), by = -std::sin(g_bodyYaw);
    {
        std::lock_guard lock(g_bodyMutex);
        g_bodyBackX = bx, g_bodyBackY = by;
        g_haveBody = true;
    }

    // Vraie stéréo : les deux yeux à chaque image, même pose de tête, une paire par image.
    // Second contexte reconfiguré avant le remplissage de celui du widget (voir true_stereo).
    rgl::RenderContext* second = stereo::Enabled() ? stereo::PrepareSecond(ctx) : nullptr;
    if (second) {
        g_indexInPair = 0;
        ++g_pairId;
        AimAtHand(game, bx, by); // appliquée par le jeu à l'image suivante, pour les deux yeux
        PrepareEye(cam, game, head, 0, bx, by, true, ctx);
        const uintptr_t result = g_sceneRender(scene, ctx, arg);
        const stereo::SceneShadowState shadows = stereo::SaveSceneShadows(scene);
        PrepareEye(second->camera, game, head, 1, bx, by, true, second);
        g_sceneRender(scene, second, arg);
        stereo::RestoreSceneShadows(scene, shadows);
        render::profile::SceneFill(render::profile::NowMs() - start);
        return result;
    }

    // Rendu alterné : un œil par image, ordre fixe dans la paire (réglage eye_order_swap).
    // Pose de tête figée pour la paire : le second œil réutilise celle du premier, les deux
    // images déclarent ainsi le même instant au compositeur.
    const int inPair = g_indexInPair;
    g_indexInPair ^= 1;
    const bool swap = config::GetBool(config::Id::EyeOrderSwap);
    const int eyeIndex = inPair ^ (swap ? 1 : 0);
    if (inPair == 0) {
        ++g_pairId;
        g_pairHead = head;
    } else {
        head = g_pairHead;
    }
    static int loggedOrder = -1;
    if ((swap ? 1 : 0) != loggedOrder) {
        loggedOrder = swap ? 1 : 0;
        LOG_ENGINE("ordre des yeux dans la paire : %s d'abord", swap ? "droit" : "gauche");
    }
    // Une correction de visée par paire, envoyée sur la DERNIÈRE image de la paire : le jeu
    // l'applique à l'image suivante, donc au début de la paire suivante, pour les deux yeux
    // à la fois (sinon petit saut de la caméra du jeu entre les deux yeux d'une paire).
    if (inPair == 1)
        AimAtHand(game, bx, by);
    PrepareEye(cam, game, head, eyeIndex, bx, by, false, ctx);
    const uintptr_t result = g_sceneRender(scene, ctx, arg);
    render::profile::SceneFill(render::profile::NowMs() - start);
    return result;
}

// --- Recensement des contextes rendus (diagnostic) ------------------------------------
// Thread de rendu : sorte de chaque contexte de l'image en cours. Écrit dans le log quand la
// composition change (au plus toutes les 2 s) et à chaque capture de diagnostic : on y voit ce
// que les réglages graphiques ajoutent (reflets, ombres, post-effets) et l'ordre des yeux.
std::string g_frameContexts, g_lastContexts;
std::string g_lastToken;
int g_tokenRepeat = 0;
ULONGLONG g_lastContextsLogMs = 0;

void FlushToken()
{
    if (g_lastToken.empty())
        return;
    if (!g_frameContexts.empty())
        g_frameContexts += ' ';
    g_frameContexts += g_lastToken;
    if (g_tokenRepeat > 1)
        g_frameContexts += " x" + std::to_string(g_tokenRepeat);
    g_lastToken.clear();
    g_tokenRepeat = 0;
}

void NoteContext(const rgl::RenderContext* ctx, const render::RenderView* view)
{
    if (!ctx)
        return;
    char token[96];
    const bool scene = ctx->scene != nullptr, persp = ctx->camera.isPerspective;
    if (view)
        snprintf(token, sizeof(token), "scene-%s(paire %llu, %dx%d, pfx %s/%zu)", view->eye ? "D" : "G",
                 static_cast<unsigned long long>(view->pair), ctx->viewport.width, ctx->viewport.height,
                 ctx->postFxEnabled ? "oui" : "non",
                 (static_cast<char*>(ctx->postFxNodes[1]) - static_cast<char*>(ctx->postFxNodes[0])) / sizeof(void*));
    else if (IsUiContext(ctx))
        snprintf(token, sizeof(token), "ui");
    else if (scene && persp && ctx->isFinalRender)
        snprintf(token, sizeof(token), "scene-sans-vue(%dx%d)", ctx->viewport.width, ctx->viewport.height);
    else if (scene && persp)
        snprintf(token, sizeof(token), "perspective(%dx%d, drapeaux 0x%X)", ctx->viewport.width,
                 ctx->viewport.height, ctx->flags);
    else if (scene)
        snprintf(token, sizeof(token), "ortho(%dx%d)", ctx->viewport.width, ctx->viewport.height);
    else
        snprintf(token, sizeof(token), "%s(%dx%d)", persp ? "autre-persp" : "autre-ortho", ctx->viewport.width,
                 ctx->viewport.height);
    // Paire et taille changent à chaque image : la comparaison ne garde que la structure
    if (token == g_lastToken) {
        ++g_tokenRepeat;
        return;
    }
    FlushToken();
    g_lastToken = token;
    g_tokenRepeat = 1;
}

uintptr_t __fastcall HkRenderContext(void* renderer, rgl::RenderContext* ctx, bool arg)
{
    bool noted = false;
    if (ctx && IsMainScene(ctx)) {
        bool found = false;
        render::RenderView view;
        {
            std::lock_guard lock(g_mutex);
            for (const Prepared& p : g_prepared) {
                if (p.view.valid && SameAxis(p.s, ctx->camera.transform.s) && SameAxis(p.f, ctx->camera.transform.f) &&
                    SameAxis(p.o, ctx->camera.transform.o)) {
                    view = p.view;
                    found = true;
                    break;
                }
            }
        }
        NoteContext(ctx, found ? &view : nullptr);
        noted = true;
        if (found) {
            // Vraie stéréo : à la seconde scène de l'image, le back buffer contient l'image finale
            // de la première, copiée pour son œil
            render::BeginMainScene(view);
            render::SetPendingView(view);
            render::MarkStereoFrame();
            if (view.both)
                stereo::BeforeEyeScene(renderer);
        }
        if (found && !g_loggedPublish) {
            LOG_ENGINE("vue du casque retrouvee au rendu (contexte %p, thread %lu)", ctx,
                       GetCurrentThreadId());
            g_loggedPublish = true;
        } else if (!found && g_loggedApply && !g_loggedOverwrite && !g_cursorWasShown) {
            LOG_ENGINE("ALERTE : scene rendue avec une camera sans vue du casque correspondante");
            g_loggedOverwrite = true;
        }
    }
    if (!noted)
        NoteContext(ctx, nullptr);
    using render::profile::Ctx;
    const Ctx kind = ctx && IsMainScene(ctx) ? Ctx::Scene : ctx && IsUiContext(ctx) ? Ctx::Ui : Ctx::Other;
    render::profile::BeginContext(kind);
    uintptr_t result;
    if (ctx && IsUiContext(ctx) && render::BeginUiRedirect()) {
        result = g_renderContext(renderer, ctx, arg);
        render::EndUiRedirect();
    } else {
        result = g_renderContext(renderer, ctx, arg);
    }
    render::profile::EndContext(kind);
    return result;
}

bool Hook(void* target, void* detour, void** original, const char* name)
{
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st == MH_OK)
        st = MH_EnableHook(target);
    if (st != MH_OK) {
        LOG_ENGINE("ERREUR : hook %s : %s", name, MH_StatusToString(st));
        MH_RemoveHook(target);
        return false;
    }
    LOG_ENGINE("%s detourne", name);
    return true;
}

} // namespace

void EndContextFrame(bool force)
{
    FlushToken();
    // Structure de l'image sans les numéros de paire (qui changent à chaque image)
    std::string structure;
    for (size_t i = 0; i < g_frameContexts.size(); ++i) {
        if (g_frameContexts.compare(i, 7, "(paire ") == 0) {
            const size_t comma = g_frameContexts.find(',', i);
            if (comma != std::string::npos) {
                structure += '(';
                i = comma;
                continue;
            }
        }
        structure += g_frameContexts[i];
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_frameContexts.empty() &&
        (force || (structure != g_lastContexts && now - g_lastContextsLogMs >= 2000))) {
        LOG_ENGINE("contextes de l'image : %s", g_frameContexts.c_str());
        g_lastContexts = structure;
        g_lastContextsLogMs = now;
    }
    g_frameContexts.clear();
}

bool InstallCameraHook()
{
    stereo::Init();
    void* sceneRender = pattern::FindFirst("rglScene::render", {kSceneRenderSigJul2026, kSceneRenderSigSep2026});
    void* renderContext = pattern::Find("rglRenderer::renderContext", kRenderContextSig);
    g_computePlanes = reinterpret_cast<ComputePlanesFn>(pattern::Find("rglCamera::computePlanes", kComputePlanesSig));
    if (!sceneRender || !renderContext || !g_computePlanes) {
        LOG_ENGINE("suivi de la tete desactive (signature manquante)");
        return false;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ENGINE("ERREUR : MH_Initialize : %s", MH_StatusToString(st));
        return false;
    }
    // renderContext d'abord : il ne fait rien tant que rglScene::render n'a rien préparé
    if (!Hook(renderContext, &HkRenderContext, reinterpret_cast<void**>(&g_renderContext),
              "rglRenderer::renderContext"))
        return false;
    g_renderContextTarget = renderContext;
    if (!Hook(sceneRender, &HkSceneRender, reinterpret_cast<void**>(&g_sceneRender), "rglScene::render")) {
        RemoveCameraHook();
        return false;
    }
    g_sceneRenderTarget = sceneRender;

    // Carte du monde immersive (facultative : sans elle, la carte reste sur l'écran virtuel)
    void* mapCamera = pattern::Find("mbMapWindow::updateCamera", kMapUpdateCameraSig);
    void* cursorRay = pattern::Find("rglSceneWidget::getCursorRay", kCursorRaySig);
    void* screenProjection = pattern::Find("rglSceneWidget::getScreenProjection", kScreenProjectionSig);
    if (!mapCamera || !cursorRay || !screenProjection) {
        LOG_ENGINE("carte du monde immersive desactivee (signature manquante)");
        return true;
    }
    if (Hook(cursorRay, &HkCursorRay, reinterpret_cast<void**>(&g_cursorRay), "rglSceneWidget::getCursorRay"))
        g_cursorRayTarget = cursorRay;
    if (Hook(screenProjection, &HkScreenProjection, reinterpret_cast<void**>(&g_screenProjection),
             "rglSceneWidget::getScreenProjection"))
        g_screenProjectionTarget = screenProjection;
    // La carte ne passe en stéréo que si le jeu peut viser et placer ses libellés avec sa caméra
    if (g_cursorRayTarget && g_screenProjectionTarget &&
        Hook(mapCamera, &HkMapUpdateCamera, reinterpret_cast<void**>(&g_mapUpdateCamera), "mbMapWindow::updateCamera"))
        g_mapUpdateCameraTarget = mapCamera;
    return true;
}

bool GetBodyBack(rgl::Vector4& back)
{
    std::lock_guard lock(g_bodyMutex);
    if (g_haveBody)
        back = {g_bodyBackX, g_bodyBackY, 0, 0};
    return g_haveBody;
}

bool GetGameCamera(rgl::Transform& out)
{
    std::lock_guard lock(g_gameCameraMutex);
    if (g_haveGameCamera)
        out = g_gameCamera;
    return g_haveGameCamera;
}

void RemoveCameraHook()
{
    for (void** target : {&g_mapUpdateCameraTarget, &g_cursorRayTarget, &g_screenProjectionTarget,
                          &g_sceneRenderTarget, &g_renderContextTarget}) {
        if (*target) {
            MH_DisableHook(*target);
            MH_RemoveHook(*target);
            *target = nullptr;
        }
    }
}

} // namespace engine
