#pragma once
// Structures du moteur (rgl = moteur graphique de Warband) relevées dans
// mb_warband_wse2_x64.pdb. Seuls les champs utiles sont décrits ; les offsets sont
// vérifiés à la compilation. À revérifier (tools/, cdb "dt") à chaque mise à jour de WSE2.
#include <cstddef>
#include <cstdint>

namespace rgl {

struct Vector4 {
    float x, y, z, w;
};

// Repère : s = côté (droite), f = avant, u = haut, o = origine. Monde Z-haut.
struct Transform {
    Vector4 s, f, u, o;
};
static_assert(sizeof(Transform) == 0x40);

struct Camera {
    bool isPerspective;          // +0x000
    uint8_t frustum[0xC4];       // +0x004 rglCameraFrustum
    Transform transform;         // +0x0C8
    float left, right, top, bottom, znear, zfar; // +0x108 .. +0x11C
    float aperture;              // +0x120
};
static_assert(offsetof(Camera, transform) == 0xC8);
static_assert(offsetof(Camera, left) == 0x108);
static_assert(offsetof(Camera, aperture) == 0x120);
static_assert(sizeof(Camera) == 0x124);

struct Viewport {
    int32_t x, y, width, height;
    float zMin, zMax;
};

// rglRenderContext : début commun aux versions de WSE2 (la fin varie : 0x310 octets en
// juillet 2026, 0x350 en septembre ; m_source à 0x308 / 0x348, offset lu dans le code).
// Les rglVector du moteur font 3 pointeurs (début, fin, capacité) : ne jamais les modifier
// nous-mêmes (allocateur du jeu), passer par ses fonctions. Jamais alloué ni copié par nous.
struct RenderContext {
    void* renderTarget;             // +0x000 rglTexture* (nul : back buffer)
    void* depthOfFieldRenderTarget; // +0x008
    void* postFxMsRenderTarget;     // +0x010
    void* postFxMsDepthStencil;     // +0x018
    void* postFxNodes[3];           // +0x020 rglVector<rglPostFxNode*>, lus sur le contexte source
    void* activeRenderTarget;       // +0x038 cible de la scène (HDR), lue sur le contexte source
    void* waterReflectionRenderTarget; // +0x040
    void* waterReflectionDepthStencil; // +0x048
    void* activeDepthStencil;       // +0x050
    int32_t cubeRenderTargetFace;   // +0x058
    bool postFxEnabled;             // +0x05C
    RenderContext* waterReflection; // +0x060
    uint8_t pad1[0x70 - 0x68];
    Viewport viewport;              // +0x070
    Camera camera;                  // +0x088
    uint8_t pad2[0x1B2 - 0x1AC];
    bool isFinalRender;             // +0x1B2
    uint8_t pad3[0x1E4 - 0x1B3];
    uint32_t flags;                 // +0x1E4
    uint8_t pad4[0x218 - 0x1E8];
    bool queuedForRendering;        // +0x218 posé par rglRenderer::addContext
    uint8_t pad5[0x221 - 0x219];
    bool entityDegradingEnabled;    // +0x221
    uint8_t pad6[0x230 - 0x222];
    void* renderMeshes[3];          // +0x230 rglVector<rglRenderMesh*> rempli par rglScene::render
    void* scene;                    // +0x248 rglScene*
};
static_assert(offsetof(RenderContext, postFxNodes) == 0x20);
static_assert(offsetof(RenderContext, activeRenderTarget) == 0x38);
static_assert(offsetof(RenderContext, activeDepthStencil) == 0x50);
static_assert(offsetof(RenderContext, postFxEnabled) == 0x5C);
static_assert(offsetof(RenderContext, waterReflection) == 0x60);
static_assert(offsetof(RenderContext, viewport) == 0x70);
static_assert(offsetof(RenderContext, camera) == 0x88);
static_assert(offsetof(RenderContext, isFinalRender) == 0x1B2);
static_assert(offsetof(RenderContext, flags) == 0x1E4);
static_assert(offsetof(RenderContext, queuedForRendering) == 0x218);
static_assert(offsetof(RenderContext, entityDegradingEnabled) == 0x221);
static_assert(offsetof(RenderContext, renderMeshes) == 0x230);
static_assert(offsetof(RenderContext, scene) == 0x248);

// rglScene : adaptation des ombres factices des personnages. rglScene::render ajuste la portée
// d'après le nombre compté au rendu précédent, puis remet le compteur à zéro.
constexpr size_t kSceneNumCharacterShadows = 0x180;   // int
constexpr size_t kSceneCharacterShadowsRange = 0x184; // float

// Chaîne du moteur (rglString)
struct String {
    int32_t cursor;      // +0x00
    char* buffer;        // +0x08
    int32_t bufferLength; // +0x10
    int32_t length;      // +0x14
    char staticBuffer[48]; // +0x18
};
static_assert(sizeof(String) == 0x48);

// Maillage accroché à un os (rglMetaMesh)
struct MetaMesh {
    uint8_t pad0[0xC8];
    Transform localTransform;     // +0x0C8 relatif à l'os
    uint32_t visibilityFlags;     // +0x108
    uint8_t pad1[0x110 - 0x10C];
    String name;                  // +0x110
};
static_assert(offsetof(MetaMesh, localTransform) == 0xC8);
static_assert(offsetof(MetaMesh, name) == 0x110);

// Os d'un squelette animé (rglBone). m_transform est dans le repère du personnage.
struct Bone {
    Transform startingTransform;  // +0x000
    Transform localTransform;     // +0x040 relatif au parent
    Transform transform;          // +0x080 repère du personnage
    Transform skinningTransform;  // +0x0C0
    bool skinningNeedsUpdate;     // +0x100
    uint8_t pad0[0x228 - 0x101];
    void** metaMeshesBegin;       // +0x228 rglVector<rglMetaMesh*> : maillages accrochés
    void** metaMeshesEnd;         // +0x230
    void** metaMeshesCapacity;    // +0x238
    int32_t parentBoneNo;         // +0x240
    uint8_t pad1[0x268 - 0x244];
};
static_assert(offsetof(Bone, transform) == 0x80);
static_assert(offsetof(Bone, skinningNeedsUpdate) == 0x100);
static_assert(offsetof(Bone, parentBoneNo) == 0x240);
static_assert(offsetof(Bone, metaMeshesBegin) == 0x228);
static_assert(sizeof(Bone) == 0x268);

// Os du modèle de squelette (rglSkeletonModelBone) : nom et hiérarchie
struct ModelBone {
    int32_t parentBoneNo;         // +0x000
    uint32_t flags;               // +0x004
    Transform transform;          // +0x008
    Transform parentTransform;    // +0x048
    String name;                  // +0x088
    uint8_t pad[0x168 - 0xD0];
};
static_assert(offsetof(ModelBone, name) == 0x88);
static_assert(sizeof(ModelBone) == 0x168);

struct SkeletonModel {
    void* vtable;                 // +0x000
    String name;                  // +0x008
    uint8_t pad[0x60 - 0x50];
    int32_t numBones;             // +0x060
    ModelBone* bones;             // +0x068
};
static_assert(offsetof(SkeletonModel, numBones) == 0x60);
static_assert(offsetof(SkeletonModel, bones) == 0x68);

// Squelette animé (rglSkeleton)
struct Skeleton {
    int32_t numBones;             // +0x000
    Bone* bones;                  // +0x008
    SkeletonModel* model;         // +0x010
};

// Indices des os du squelette humain (constantes hb_* du module system)
constexpr int kBoneHead = 9;
constexpr int kBoneUpperArmL = 11, kBoneForearmL = 12, kBoneHandL = 13, kBoneItemL = 14;
constexpr int kBoneUpperArmR = 16, kBoneForearmR = 17, kBoneHandR = 18, kBoneItemR = 19;

// Point du repère t vers le repère parent
inline Vector4 ToParent(const Transform& t, const Vector4& p)
{
    return {t.o.x + p.x * t.s.x + p.y * t.f.x + p.z * t.u.x, t.o.y + p.x * t.s.y + p.y * t.f.y + p.z * t.u.y,
            t.o.z + p.x * t.s.z + p.y * t.f.z + p.z * t.u.z, 1};
}

} // namespace rgl
