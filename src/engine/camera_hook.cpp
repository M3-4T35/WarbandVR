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

// Valeurs de retour transmises telles quelles (rax), quel que soit leur type réel
using SceneRenderFn = uintptr_t(__fastcall*)(void* scene, rgl::RenderContext* ctx, bool arg);
using RenderContextFn = uintptr_t(__fastcall*)(void* renderer, rgl::RenderContext* ctx, bool arg);
using ComputePlanesFn = void(__fastcall*)(rgl::Camera* camera);

SceneRenderFn g_sceneRender = nullptr;
RenderContextFn g_renderContext = nullptr;
ComputePlanesFn g_computePlanes = nullptr;
void* g_sceneRenderTarget = nullptr;
void* g_renderContextTarget = nullptr;

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
void ApplyHead(rgl::Camera& cam, const xr::HeadPose& h, float bx, float by)
{
    rgl::Transform& t = cam.transform;
    const rgl::Vector4 s0{-by, bx, 0, 0};
    const rgl::Vector4 f0{0, 0, 1, 0};
    const rgl::Vector4 u0{bx, by, 0, 0};

    // Matrice de rotation du quaternion : colonne j = axe j tourné
    const float x = h.qx, y = h.qy, z = h.qz, w = h.qw;
    const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    const float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);

    t.s = Combine(r00, s0, r10, f0, r20, u0, t.s.w);
    t.f = Combine(r01, s0, r11, f0, r21, u0, t.f.w);
    t.u = Combine(r02, s0, r12, f0, r22, u0, t.u.w);
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
    xr::HandPose hands[2];
    xr::GetHands(hands);
    const rgl::Vector4 fwd{-game.u.x, -game.u.y, -game.u.z, 0};
    const float gameYaw = std::atan2(fwd.y, fwd.x);
    const float gamePitch = std::asin(std::clamp(fwd.z, -1.0f, 1.0f));

    Calibrate(g_kYaw, g_lastDx, WrapAngle(gameYaw - g_lastGameYaw));
    Calibrate(g_kPitch, g_lastDy, gamePitch - g_lastGamePitch);
    g_lastDx = g_lastDy = 0;
    g_lastGameYaw = gameYaw, g_lastGamePitch = gamePitch;

    if (xr::IsSwingBusy())
        return;
    // Direction voulue (espace LOCAL) : arc bandé (et 1 s après le tir) -> axe de la flèche
    // (main droite vers main gauche) ; sinon selon la source choisie (clic du stick gauche) : regard (centre de la
    // vue), main gauche ou main droite
    float dx, dy, dz;
    const bool bow = xr::IsBowMode();
    if (bow && xr::IsBowAiming() && hands[0].valid && hands[1].valid) {
        dx = hands[0].px - hands[1].px, dy = hands[0].py - hands[1].py, dz = hands[0].pz - hands[1].pz;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 0.05f)
            return;
        dx /= len, dy /= len, dz /= len;
    } else {
        float x, y, z, w;
        const xr::AimSource source = xr::GetAimSource();
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
    if (std::fabs(errPitch) > kAimDeadzone)
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
    xr::HeadPose head;
    const bool stereoScene = ctx && IsMainScene(ctx) && xr::GetHeadPose(head) && !MenuOpen();
    if (ctx && IsMainScene(ctx) && !stereoScene) {
        g_bodyActive = false;
        g_indexInPair = 0; // la stéréo reprendra sur une paire neuve
        std::lock_guard lock(g_bodyMutex);
        g_haveBody = false;
    }
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

    // Cap du corps : initialisé sur celui du jeu, puis tourné au stick
    if (!g_bodyActive) {
        g_bodyYaw = std::atan2(-game.u.y, -game.u.x);
        g_bodyActive = true;
        g_lastDx = g_lastDy = 0;
    }
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

uintptr_t __fastcall HkRenderContext(void* renderer, rgl::RenderContext* ctx, bool arg)
{
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
        if (found) {
            // Vraie stéréo : à la seconde scène de l'image, le back buffer contient l'image finale
            // de la première, copiée pour son œil
            render::BeginMainScene(view);
            render::SetPendingView(view);
            render::MarkStereoFrame();
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
    for (void** target : {&g_sceneRenderTarget, &g_renderContextTarget}) {
        if (*target) {
            MH_DisableHook(*target);
            MH_RemoveHook(*target);
            *target = nullptr;
        }
    }
}

} // namespace engine
