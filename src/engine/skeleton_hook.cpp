// Squelettes animés.
//
// rglSkeleton::frameMove(dt, repère monde, ...) avance l'animation ; updateBonePositions
// calcule ensuite la pose de chaque os dans le repère du personnage (appelé à la demande,
// y compris par la logique de jeu : collisions des armes, projectiles, et parfois sur des
// threads de travail). Le squelette du joueur est celui dont la tête est à la caméra du jeu
// (vue à la première personne).
//
// Bras VR : après le calcul des os du joueur, ses bras sont replacés sur les manettes par
// cinématique inverse (épaule fixe, coude calculé, main et objet tenu orientés comme la
// manette). Conventions relevées sur le squelette humain : repère du personnage x à droite,
// y devant, z en haut, en mètres ; pour chaque os du bras, l'axe f pointe vers l'os enfant.
// Bouclier : accroché à l'avant-bras gauche ; l'avant-bras prend la torsion de la main, le
// bouclier suit donc l'orientation de la manette gauche (sans toucher aux maillages).
// Effet voulu : la logique de jeu (collisions des armes) utilise ces os, les coups partent
// donc de la vraie position de l'arme.
#include "engine/skeleton_hook.h"
#include "engine/camera_hook.h"
#include "engine/combat_hook.h"
#include "engine/rgl.h"
#include "util/config.h"
#include "util/log.h"
#include "util/pattern.h"
#include "xr/xr_session.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)

namespace engine {
namespace {

constexpr char kFrameMoveSig[] =
    "48 89 5C 24 18 4C 89 74 24 20 41 57 48 83 EC 30 80 79 70 00 0F 29 74 24 20 4D 8B F9 0F 28 F1 4D 8B F0 48 8B D9";
constexpr char kUpdateBonesSig[] = "40 55 57 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 8B 41 18 33 D2 0F 29 BC "
                                   "24 ?? ?? ?? ?? F3 0F 10 3D ?? ?? ?? ?? 89 41 28 8B 41 1C 89 41 2C";

// La tête du joueur est à quelques centimètres de la caméra en vue à la première personne
constexpr float kPlayerHeadMaxDistance = 0.4f;

using FrameMoveFn = uintptr_t(__fastcall*)(rgl::Skeleton* self, float dt, rgl::Transform* frame, void* offset,
                                           bool flag);
using UpdateBonesFn = void(__fastcall*)(rgl::Skeleton* self);
FrameMoveFn g_frameMove = nullptr;
UpdateBonesFn g_updateBones = nullptr;
void* g_frameMoveTarget = nullptr;
void* g_updateBonesTarget = nullptr;

// Repère monde de chaque squelette (frameMove : thread de jeu ; updateBonePositions :
// n'importe quel thread)
std::mutex g_mutex;
std::unordered_map<rgl::Skeleton*, rgl::Transform> g_frames;
bool g_loggedPlayer = false;
long long g_armMeshSignature = -1; // nombre de maillages sur les os des bras, pour relog


const char* BoneName(const rgl::Skeleton* sk, int i)
{
    if (!sk->model || i >= sk->model->numBones || !sk->model->bones)
        return "?";
    const rgl::String& n = sk->model->bones[i].name;
    return n.buffer ? n.buffer : "?";
}

int MeshCount(const rgl::Bone& b)
{
    return b.metaMeshesBegin && b.metaMeshesEnd ? static_cast<int>(b.metaMeshesEnd - b.metaMeshesBegin) : 0;
}

// Maillages accrochés aux os des bras (armes, bouclier, gants), à chaque changement
void LogArmMeshes(const rgl::Skeleton* sk)
{
    long long signature = 0;
    for (int i = rgl::kBoneUpperArmL; i <= rgl::kBoneItemR; ++i)
        signature = signature * 8 + MeshCount(sk->bones[i]);
    if (signature == g_armMeshSignature)
        return;
    g_armMeshSignature = signature;
    LOG_ENGINE("maillages accroches aux bras du joueur :");
    for (int i = rgl::kBoneUpperArmL; i <= rgl::kBoneItemR; ++i) {
        const rgl::Bone& b = sk->bones[i];
        for (int m = 0; m < MeshCount(b); ++m) {
            const auto* mesh = static_cast<const rgl::MetaMesh*>(b.metaMeshesBegin[m]);
            if (!mesh)
                continue;
            const rgl::Transform& t = mesh->localTransform;
            LOG_ENGINE("  os %d %s : \"%s\" local o=(%.3f %.3f %.3f) s=(%.2f %.2f %.2f) f=(%.2f %.2f %.2f)", i,
                       BoneName(sk, i), mesh->name.buffer ? mesh->name.buffer : "?", t.o.x, t.o.y, t.o.z, t.s.x,
                       t.s.y, t.s.z, t.f.x, t.f.y, t.f.z);
        }
    }
}

void LogTransform(const char* label, const rgl::Transform& t)
{
    LOG_ENGINE("  %-12s o=(%6.3f %6.3f %6.3f) s=(%5.2f %5.2f %5.2f) f=(%5.2f %5.2f %5.2f) u=(%5.2f %5.2f %5.2f)",
               label, t.o.x, t.o.y, t.o.z, t.s.x, t.s.y, t.s.z, t.f.x, t.f.y, t.f.z, t.u.x, t.u.y, t.u.z);
}

// Découverte : pose des os du joueur, une fois
void LogPlayerSkeleton(rgl::Skeleton* sk, const rgl::Transform& frame, const rgl::Transform& camera, float dist)
{
    LOG_ENGINE("squelette du joueur : %p, %d os, tete a %.2f m de la camera (thread %lu)", sk, sk->numBones, dist,
               GetCurrentThreadId());
    LogTransform("repere monde", frame);
    LogTransform("camera jeu", camera);
    for (int i = 0; i < sk->numBones; ++i) {
        const rgl::Bone& b = sk->bones[i];
        char label[64];
        const long long meshes = b.metaMeshesBegin && b.metaMeshesEnd ? b.metaMeshesEnd - b.metaMeshesBegin : 0;
        snprintf(label, sizeof(label), "%d %s p%d m%lld", i, BoneName(sk, i), b.parentBoneNo, meshes);
        LogTransform(label, b.transform);
    }
}

// --- Petite algèbre dans le repère du personnage -----------------------------------------
using V = rgl::Vector4;
V Add(const V& a, const V& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, 0}; }
V Sub(const V& a, const V& b) { return {a.x - b.x, a.y - b.y, a.z - b.z, 0}; }
V Mul(const V& a, float k) { return {a.x * k, a.y * k, a.z * k, 0}; }
float Dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V Cross(const V& a, const V& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0}; }
float Len(const V& a) { return std::sqrt(Dot(a, a)); }
V Norm(const V& a)
{
    const float l = Len(a);
    return l > 1e-6f ? Mul(a, 1.0f / l) : V{0, 0, 0, 0};
}

// Tourne v autour de l'axe unitaire k (Rodrigues)
V Rotate(const V& v, const V& k, float c, float sn)
{
    return Add(Add(Mul(v, c), Mul(Cross(k, v), sn)), Mul(k, Dot(k, v) * (1 - c)));
}

// Réoriente le repère t par la plus petite rotation qui amène from sur to (unitaires)
void RotateFrame(rgl::Transform& t, const V& from, const V& to)
{
    V axis = Cross(from, to);
    const float sn = Len(axis), c = Dot(from, to);
    if (sn < 1e-6f)
        return;
    axis = Mul(axis, 1.0f / sn);
    const float ws = t.s.w, wf = t.f.w, wu = t.u.w;
    t.s = Rotate(t.s, axis, c, sn), t.s.w = ws;
    t.f = Rotate(t.f, axis, c, sn), t.f.w = wf;
    t.u = Rotate(t.u, axis, c, sn), t.u.w = wu;
}

// a exprimé dans le repère de b (b orthonormé) : inverse(b) * a
rgl::Transform Relative(const rgl::Transform& b, const rgl::Transform& a)
{
    auto local = [&](const V& v) { return V{Dot(v, b.s), Dot(v, b.f), Dot(v, b.u), 0}; };
    rgl::Transform r{};
    r.s = local(a.s), r.s.w = a.s.w;
    r.f = local(a.f), r.f.w = a.f.w;
    r.u = local(a.u), r.u.w = a.u.w;
    r.o = local(Sub(a.o, b.o)), r.o.w = a.o.w;
    return r;
}

// Repère local r replacé dans le repère parent p : p * r
rgl::Transform Compose(const rgl::Transform& p, const rgl::Transform& r)
{
    auto parentDir = [&](const V& v) { return Add(Add(Mul(p.s, v.x), Mul(p.f, v.y)), Mul(p.u, v.z)); };
    rgl::Transform t{};
    t.s = parentDir(r.s), t.s.w = r.s.w;
    t.f = parentDir(r.f), t.f.w = r.f.w;
    t.u = parentDir(r.u), t.u.w = r.u.w;
    t.o = Add(p.o, parentDir(r.o)), t.o.w = r.o.w;
    return t;
}

// --- Manettes -> repère du personnage --------------------------------------------------------
// Même correspondance que la caméra (camera_hook) : le repère LOCAL d'OpenXR (x droite,
// y haut, z arrière), centré sur la tête, est posé sur la caméra du jeu avec son cap.
struct XrToSkeleton {
    V camO, s0, f0, u0; // caméra du jeu et axes monde du repère OpenXR à plat
    V head;             // position de la tête (espace LOCAL)
    rgl::Transform frame;
    V Point(float x, float y, float z) const
    {
        const V w = Add(camO, Add(Add(Mul(s0, x - head.x), Mul(f0, y - head.y)), Mul(u0, z - head.z)));
        const V d = Sub(w, frame.o);
        return {Dot(d, frame.s), Dot(d, frame.f), Dot(d, frame.u), 1};
    }
    V Dir(float x, float y, float z) const
    {
        const V w = Add(Add(Mul(s0, x), Mul(f0, y)), Mul(u0, z));
        return {Dot(w, frame.s), Dot(w, frame.f), Dot(w, frame.u), 0};
    }
};

bool MakeXrToSkeleton(const rgl::Transform& camera, const rgl::Transform& frame, const xr::HeadPose& head,
                      XrToSkeleton& out)
{
    // Arrière horizontal du repère de la vue (cap du corps) ; à défaut, celui de la caméra
    rgl::Vector4 back;
    const bool body = GetBodyBack(back);
    float bx = body ? back.x : camera.u.x, by = body ? back.y : camera.u.y;
    float len = std::sqrt(bx * bx + by * by);
    if (len < 0.01f) {
        const float sign = camera.u.z > 0 ? -1.0f : 1.0f;
        bx = sign * camera.f.x, by = sign * camera.f.y;
        len = std::sqrt(bx * bx + by * by);
        if (len < 1e-6f)
            return false;
    }
    bx /= len, by /= len;
    out.camO = camera.o;
    out.s0 = {-by, bx, 0, 0};
    out.f0 = {0, 0, 1, 0};
    out.u0 = {bx, by, 0, 0};
    out.head = {head.px, head.py, head.pz, 1};
    out.frame = frame;
    return true;
}

// Axe du quaternion q (x, y, z, w) : q * (vx, vy, vz)
void QuatAxis(const xr::HandPose& h, float vx, float vy, float vz, float& ox, float& oy, float& oz)
{
    const float x = h.qx, y = h.qy, z = h.qz, w = h.qw;
    // v' = v + 2w (q x v) + 2 q x (q x v)
    const float cx = y * vz - z * vy, cy = z * vx - x * vz, cz = x * vy - y * vx;
    ox = vx + 2 * (w * cx + y * cz - z * cy);
    oy = vy + 2 * (w * cy + z * cx - x * cz);
    oz = vz + 2 * (w * cz + x * cy - y * cx);
}

// --- Cinématique inverse d'un bras -----------------------------------------------------------
bool g_loggedArms = false;
std::mutex g_shieldMutex;
rgl::Vector4 g_shieldCenter{};
unsigned long long g_shieldTimeMs = 0;

// animatedGrip : la main et l'objet gardent l'orientation de l'animation du jeu (armes à
// distance) ; seule leur position suit la manette.
void SolveArm(rgl::Skeleton* sk, int upper, const xr::HandPose& hand, const XrToSkeleton& map, float side,
              bool rightHand, bool animatedGrip)
{
    rgl::Bone* b = sk->bones;
    rgl::Bone& up = b[upper];
    rgl::Bone& fore = b[upper + 1];
    rgl::Bone& hnd = b[upper + 2];
    rgl::Bone& item = b[upper + 3];

    // Longueurs et liaison main/objet mesurées sur l'animation en cours
    const float l1 = Len(Sub(fore.transform.o, up.transform.o));
    const float l2 = Len(Sub(hnd.transform.o, fore.transform.o));
    if (l1 < 0.05f || l2 < 0.05f)
        return;
    const rgl::Transform itemInHand = Relative(hnd.transform, item.transform);
    // Avant-bras d'origine vu depuis la main (pour les maillages accrochés à l'avant-bras)
    const rgl::Transform foreInHand = Relative(hnd.transform, fore.transform);

    // Objet tenu : dans la paume, orienté comme la poignée de la manette (pose grip
    // d'OpenXR : -Z le long de l'objet tenu, +X perpendiculaire à la paume). La lame (f)
    // suit -Z ; la garde est tournée de 90° autour de la lame pour être dans le plan de la
    // paume (réglé au test casque) : s = +Y, u = -X.
    float fx, fy, fz, ux, uy, uz, sx, sy, sz;
    // Main droite : objet retourné de 180° autour de la lame (réglé au test casque)
    const float flip = rightHand ? -1.0f : 1.0f;
    QuatAxis(hand, 0, 0, -1, fx, fy, fz);
    QuatAxis(hand, -flip, 0, 0, ux, uy, uz);
    QuatAxis(hand, 0, flip, 0, sx, sy, sz);
    rgl::Transform itemT = item.transform;
    if (!animatedGrip) {
        itemT.f = map.Dir(fx, fy, fz), itemT.f.w = item.transform.f.w;
        itemT.u = map.Dir(ux, uy, uz), itemT.u.w = item.transform.u.w;
        itemT.s = map.Dir(sx, sy, sz), itemT.s.w = item.transform.s.w;
    }
    itemT.o = map.Point(hand.px, hand.py, hand.pz);
    itemT.o.w = item.transform.o.w;

    // Main : l'objet, moins leur liaison fixe (main = objet * inverse(liaison))
    const rgl::Transform identity{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    rgl::Transform handT = Compose(itemT, Relative(itemInHand, identity));

    // Poignet hors de portée : on le ramène à la longueur du bras
    const V shoulder = up.transform.o;
    V toWrist = Sub(handT.o, shoulder);
    float d = Len(toWrist);
    const float maxReach = (l1 + l2) * 0.999f, minReach = std::fabs(l1 - l2) + 0.01f;
    if (d < 1e-4f)
        return;
    V dir = Mul(toWrist, 1.0f / d);
    if (d > maxReach || d < minReach) {
        d = d > maxReach ? maxReach : minReach;
        const V shift = Sub(Add(shoulder, Mul(dir, d)), handT.o);
        handT.o = Add(handT.o, shift);
        itemT.o = Add(itemT.o, shift);
    }

    // Coude : plié vers l'extérieur (x), vers le bas et un peu vers l'arrière (y)
    const float a = (l1 * l1 - l2 * l2 + d * d) / (2 * d);
    const float h = std::sqrt(std::max(0.0f, l1 * l1 - a * a));
    V pole = Norm(V{side * 0.8f, -0.3f, -1.0f, 0});
    pole = Norm(Sub(pole, Mul(dir, Dot(pole, dir))));
    const V elbow = Add(Add(shoulder, Mul(dir, a)), Mul(pole, h));

    // Bras et avant-bras : rotation minimale de leur axe f vers la nouvelle direction
    rgl::Transform upT = up.transform;
    RotateFrame(upT, Norm(Sub(fore.transform.o, up.transform.o)), Norm(Sub(elbow, shoulder)));
    // Avant-bras : il prend la rotation de la main (torsion du poignet), garde sa relation
    // d'origine avec elle, puis est aligné du coude au poignet. Le bouclier, accroché à
    // l'avant-bras gauche, suit ainsi l'orientation de la main.
    rgl::Transform foreT = Compose(handT, foreInHand);
    RotateFrame(foreT, Norm(V{foreT.f.x, foreT.f.y, foreT.f.z, 0}), Norm(Sub(handT.o, elbow)));
    foreT.o = elbow, foreT.o.w = fore.transform.o.w;

    up.transform = upT;
    fore.transform = foreT;
    hnd.transform = handT;
    item.transform = itemT;
    up.skinningNeedsUpdate = fore.skinningNeedsUpdate = hnd.skinningNeedsUpdate = item.skinningNeedsUpdate = true;
}

// Haut du corps de face : les postures du jeu (garde, repos, arc...) tournent le torse de
// profil. On fait pivoter la colonne et tout ce qui en dépend (thorax, tête, bras) autour de la
// verticale de la colonne, pour que la ligne des épaules soit perpendiculaire à l'avant du
// personnage (+y ; droite = +x, haut = +z). Avant la cinématique inverse des bras.
constexpr int kBoneSpine = 7;
bool g_loggedTorso = false;

void StraightenTorso(rgl::Skeleton* sk)
{
    rgl::Bone* b = sk->bones;
    const V across = Sub(b[rgl::kBoneUpperArmR].transform.o, b[rgl::kBoneUpperArmL].transform.o);
    if (across.x * across.x + across.y * across.y < 0.01f)
        return;
    const float yaw = std::atan2(across.y, across.x); // 0 : épaules de face
    if (std::fabs(yaw) < 0.01f || std::fabs(yaw) > 1.5f)
        return; // déjà de face, ou squelette inattendu
    const V axis{0, 0, 1, 0};
    const float c = std::cos(-yaw), sn = std::sin(-yaw);
    const V pivot = b[kBoneSpine].transform.o;
    for (int i = kBoneSpine; i < sk->numBones; ++i) {
        // Descendant de la colonne ?
        int p = i, guard = 0;
        while (p > kBoneSpine && p < sk->numBones && guard++ < 32)
            p = b[p].parentBoneNo;
        if (p != kBoneSpine)
            continue;
        rgl::Transform& t = b[i].transform;
        const float ws = t.s.w, wf = t.f.w, wu = t.u.w, wo = t.o.w;
        t.s = Rotate(t.s, axis, c, sn), t.s.w = ws;
        t.f = Rotate(t.f, axis, c, sn), t.f.w = wf;
        t.u = Rotate(t.u, axis, c, sn), t.u.w = wu;
        t.o = Add(pivot, Rotate(Sub(t.o, pivot), axis, c, sn)), t.o.w = wo;
        b[i].skinningNeedsUpdate = true;
    }
    if (!g_loggedTorso) {
        LOG_ENGINE("torse de face : rotation de %.0f degres du haut du corps", -yaw * 57.2958f);
        g_loggedTorso = true;
    }
}

// --- Prise à deux mains ----------------------------------------------------------------------
// Arme à deux mains ou d'hast, gâchette droite tenue : l'arme suit l'axe des deux mains (espace
// LOCAL d'OpenXR). La lame part de la main droite, du côté où pointe la manette droite ; le fil
// garde la rotation de la main droite autour de l'axe. La main gauche est fixée sur l'arme : à
// la distance de la main droite mesurée au moment de la prise (elle ne glisse plus le long du
// manche), orientée le long de la poignée (rotation minimale de sa pose).
struct Q {
    float x, y, z, w;
};
Q QMul(const Q& a, const Q& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
// Plus petite rotation amenant le vecteur unitaire a sur b
Q QFromTo(float ax, float ay, float az, float bx, float by, float bz)
{
    const float d = ax * bx + ay * by + az * bz;
    if (d < -0.9999f) { // opposés : demi-tour autour d'un axe perpendiculaire
        float px = 0, py = -az, pz = ay;
        if (py * py + pz * pz < 1e-6f)
            px = az, py = 0, pz = -ax;
        const float l = std::sqrt(px * px + py * py + pz * pz);
        return {px / l, py / l, pz / l, 0};
    }
    const float cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    Q q{cx, cy, cz, 1 + d};
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
bool g_loggedTwoHand = false;
std::atomic<bool> g_twoHandActive{false};
std::atomic<float> g_twoHandOffset{0}; // position de la main gauche sur l'axe, depuis la main droite (m)

// Fin de la prise à deux mains : la prochaine prise mesure de nouveau la place de la main gauche
void ReleaseTwoHandGrip()
{
    g_twoHandActive = false;
}

bool ApplyTwoHandGrip(xr::HandPose hands[2])
{
    xr::HandPose& l = hands[0];
    xr::HandPose& r = hands[1];
    float dx = r.px - l.px, dy = r.py - l.py, dz = r.pz - l.pz;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 0.05f || len > 1.2f) {
        g_twoHandActive = false;
        return false; // mains confondues ou trop écartées : prise normale
    }
    dx /= len, dy /= len, dz /= len;
    // Lame de la manette droite : -Z de sa pose grip ; l'axe prend son sens
    float bx, by, bz;
    QuatAxis(r, 0, 0, -1, bx, by, bz);
    if (dx * bx + dy * by + dz * bz < 0)
        dx = -dx, dy = -dy, dz = -dz;
    // Main gauche fixée sur le manche : distance (signée, le long de l'axe) mesurée à la prise
    const float along = (l.px - r.px) * dx + (l.py - r.py) * dy + (l.pz - r.pz) * dz;
    if (!g_twoHandActive.exchange(true))
        g_twoHandOffset = std::fabs(along);
    // Distance fixe ; côté de la main droite où se trouve la main gauche (si l'axe s'inverse)
    const float offset = along < 0 ? -g_twoHandOffset.load() : g_twoHandOffset.load();
    l.px = r.px + dx * offset, l.py = r.py + dy * offset, l.pz = r.pz + dz * offset;
    for (xr::HandPose* h : {&r, &l}) {
        float hx, hy, hz;
        QuatAxis(*h, 0, 0, -1, hx, hy, hz);
        const Q q = QMul(QFromTo(hx, hy, hz, dx, dy, dz), Q{h->qx, h->qy, h->qz, h->qw});
        h->qx = q.x, h->qy = q.y, h->qz = q.z, h->qw = q.w;
    }
    if (!g_loggedTwoHand) {
        LOG_ENGINE("prise a deux mains : arme sur l'axe des mains (%.2f m entre les mains)", len);
        g_loggedTwoHand = true;
    }
    return true;
}

void ApplyArms(rgl::Skeleton* sk, const rgl::Transform& frame, const rgl::Transform& camera)
{
    xr::HandPose hands[2];
    xr::GetHands(hands);
    xr::HeadPose head;
    if ((!hands[0].valid && !hands[1].valid) || !xr::GetHeadPose(head))
        return;
    XrToSkeleton map;
    if (!MakeXrToSkeleton(camera, frame, head, map))
        return;
    // Arc, arbalète, arme à feu en visée ou en tir : les mains tiennent l'arme comme dans
    // l'animation du jeu (orientation), les bras suivent toujours les manettes
    const bool animatedGrip = PlayerAimingRangedWeapon();
    // Arme à deux mains, gâchette droite tenue : l'arme suit l'axe des deux mains
    if (hands[0].valid && hands[1].valid && xr::IsTwoHandGrip() && PlayerHoldsTwoHandedWeapon())
        ApplyTwoHandGrip(hands);
    else
        ReleaseTwoHandGrip();
    // Torse de face : armes de mêlée seulement (les postures de tir restent celles du jeu)
    if (config::GetBool(config::Id::TorsoForward) && PlayerHoldsMeleeWeapon() && sk->numBones > rgl::kBoneItemR)
        StraightenTorso(sk);
    // Côté extérieur de chaque bras dans le repère du personnage : -x à gauche, +x à droite
    if (hands[0].valid)
        SolveArm(sk, rgl::kBoneUpperArmL, hands[0], map, -1.0f, false, animatedGrip);
    if (hands[1].valid)
        SolveArm(sk, rgl::kBoneUpperArmR, hands[1], map, 1.0f, true, animatedGrip);
    // Centre du bouclier : milieu de l'avant-bras gauche (le bouclier y est accroché)
    if (hands[0].valid) {
        const rgl::Bone& fore = sk->bones[rgl::kBoneForearmL];
        const rgl::Bone& hand = sk->bones[rgl::kBoneHandL];
        const rgl::Vector4 mid{(fore.transform.o.x + hand.transform.o.x) / 2, (fore.transform.o.y + hand.transform.o.y) / 2,
                               (fore.transform.o.z + hand.transform.o.z) / 2, 1};
        std::lock_guard lock(g_shieldMutex);
        g_shieldCenter = rgl::ToParent(frame, mid);
        g_shieldTimeMs = GetTickCount64();
    }
    if (!g_loggedArms) {
        LOG_ENGINE("bras VR appliques au squelette du joueur %p (thread %lu)", sk, GetCurrentThreadId());
        g_loggedArms = true;
    }
}

uintptr_t __fastcall HkFrameMove(rgl::Skeleton* self, float dt, rgl::Transform* frame, void* offset, bool flag)
{
    if (self && frame) {
        std::lock_guard lock(g_mutex);
        if (g_frames.size() > 4096)
            g_frames.clear(); // squelettes détruits : on repart de zéro
        g_frames[self] = *frame;
    }
    return g_frameMove(self, dt, frame, offset, flag);
}

void __fastcall HkUpdateBones(rgl::Skeleton* self)
{
    g_updateBones(self);
    if (!self || !self->bones || self->numBones <= rgl::kBoneItemR)
        return;

    rgl::Transform frame, camera;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_frames.find(self);
        if (it == g_frames.end())
            return;
        frame = it->second;
    }
    if (!GetGameCamera(camera))
        return;
    // Joueur en vue à la première personne : sa tête est à la caméra
    const rgl::Vector4 headPos = rgl::ToParent(frame, self->bones[rgl::kBoneHead].transform.o);
    const float dx = headPos.x - camera.o.x, dy = headPos.y - camera.o.y, dz = headPos.z - camera.o.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist > kPlayerHeadMaxDistance)
        return;
    {
        std::lock_guard lock(g_mutex);
        if (!g_loggedPlayer) {
            g_loggedPlayer = true;
            LogPlayerSkeleton(self, frame, camera, dist);
        }
        LogArmMeshes(self);
    }

    ApplyArms(self, frame, camera);
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

bool InstallSkeletonHook()
{
    void* frameMove = pattern::Find("rglSkeleton::frameMove", kFrameMoveSig);
    void* updateBones = pattern::Find("rglSkeleton::updateBonePositions", kUpdateBonesSig);
    if (!frameMove || !updateBones) {
        LOG_ENGINE("bras VR desactives (signature manquante)");
        return false;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (Hook(frameMove, &HkFrameMove, reinterpret_cast<void**>(&g_frameMove), "rglSkeleton::frameMove"))
        g_frameMoveTarget = frameMove;
    if (Hook(updateBones, &HkUpdateBones, reinterpret_cast<void**>(&g_updateBones), "rglSkeleton::updateBonePositions"))
        g_updateBonesTarget = updateBones;
    return g_frameMoveTarget && g_updateBonesTarget;
}

bool GetPlayerShieldCenter(rgl::Vector4& out)
{
    std::lock_guard lock(g_shieldMutex);
    if (GetTickCount64() - g_shieldTimeMs > 200)
        return false;
    out = g_shieldCenter;
    return true;
}

void RemoveSkeletonHook()
{
    for (void** target : {&g_updateBonesTarget, &g_frameMoveTarget}) {
        if (*target) {
            MH_DisableHook(*target);
            MH_RemoveHook(*target);
            *target = nullptr;
        }
    }
}

} // namespace engine
