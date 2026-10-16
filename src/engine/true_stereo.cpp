#include "engine/true_stereo.h"
#include "util/config.h"
#include "util/log.h"
#include "util/pattern.h"

#include <algorithm>
#include <cstring>

#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)

namespace engine::stereo {
namespace {

// rglResourceManager::getRenderContext() : contexte du pool, initialisé.
// Juillet 2026 : lea rcx, [section critique] ; xor edi, edi ; mov ebx, edi ; call [EnterCriticalSection]
constexpr char kGetRenderContextSigJul2026[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8D 0D ?? ?? ?? ?? 33 FF 8B DF FF 15 ?? ?? ?? ?? 48 8B 15";
// Septembre 2026 : cadre de gestion d'exceptions, puis xor edi, edi ; mov ebx, edi ; lea rcx, [...]
constexpr char kGetRenderContextSigSep2026[] =
    "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 33 FF 8B DF 48 8D 0D";
// rglRenderContext::setSourceContext(this, source) : copie de la configuration (caméra, viewport,
// lumières, paramètres...) et échange des listes de maillages. Commence par
// mov [rcx + m_source], rdx : l'offset de m_source (variable selon la version) est lu à +16.
constexpr char kSetSourceContextSig[] = "48 89 5C 24 08 57 48 81 EC 80 00 00 00 48 89 91 ?? ?? 00 00 0F B6 42 5C";
constexpr int kSourceOffsetImm = 16;
// Dans setSourceContext : copie de m_flags (+0x1E8) puis call std::vector<T*>::operator= (lumières)
constexpr char kPtrVectorAssignCallSig[] = "8B 87 E8 01 00 00 89 83 E8 01 00 00 E8";
// rglRenderer::renderMesh, premier maillage d'eau de l'image : copie de la scène pour la réfraction
// mov eax, [rcx+numéro d'image] ; cmp eax, [rcx+image de la copie] ; je ; mov [rcx+image de la
// copie], eax ; mov rcx, [rcx+screen_texture]
constexpr char kRefractionCopySig[] =
    "8B 81 ?? ?? 00 00 3B 81 ?? ?? 00 00 0F 84 ?? ?? ?? ?? 89 81 ?? ?? 00 00 48 8B 89 ?? ?? 00 00";

// this inutilisé : la fonction ne lit que les membres du gestionnaire par adresse globale
using GetRenderContextFn = rgl::RenderContext*(__fastcall*)(void* resourceManager);
using SetSourceContextFn = void(__fastcall*)(rgl::RenderContext* self, rgl::RenderContext* source);
using PtrVectorAssignFn = void*(__fastcall*)(void* self, const void* other);

GetRenderContextFn g_getRenderContext = nullptr;
SetSourceContextFn g_setSourceContext = nullptr;
PtrVectorAssignFn g_ptrVectorAssign = nullptr;
bool g_available = false;
uint32_t g_sourceOffset = 0; // rglRenderContext::m_source
// rglRenderer : numéro de l'image et image de la dernière copie de réfraction (0 : introuvables)
uint32_t g_frameNoOffset = 0;
uint32_t g_refractionFrameOffset = 0;

rgl::RenderContext* g_second = nullptr;
bool g_loggedSetup = false;
bool g_loggedBusy = false;
bool g_loggedNotEmpty = false;

size_t VectorSize(void* const v[3])
{
    return (static_cast<char*>(v[1]) - static_cast<char*>(v[0])) / sizeof(void*);
}

} // namespace

bool Init()
{
    g_getRenderContext = reinterpret_cast<GetRenderContextFn>(pattern::FindFirst(
        "rglResourceManager::getRenderContext", {kGetRenderContextSigJul2026, kGetRenderContextSigSep2026}));
    g_setSourceContext = reinterpret_cast<SetSourceContextFn>(
        pattern::Find("rglRenderContext::setSourceContext", kSetSourceContextSig));
    if (g_setSourceContext) {
        memcpy(&g_sourceOffset, reinterpret_cast<const char*>(g_setSourceContext) + kSourceOffsetImm,
               sizeof(g_sourceOffset));
        // Après la scène (+0x248), aligné sur 8 : sinon le code a changé, on ne s'y fie pas
        if (g_sourceOffset < 0x250 || g_sourceOffset > 0x800 || (g_sourceOffset & 7)) {
            LOG_ENGINE("ERREUR : offset de rglRenderContext::m_source invraisemblable (0x%X)", g_sourceOffset);
            g_setSourceContext = nullptr;
        } else {
            LOG_ENGINE("rglRenderContext::m_source a +0x%X", g_sourceOffset);
        }
    }
    g_ptrVectorAssign = reinterpret_cast<PtrVectorAssignFn>(
        pattern::FindGlobal("std::vector<T*>::operator=", kPtrVectorAssignCallSig, 13, 17));
    g_available = g_getRenderContext && g_setSourceContext && g_ptrVectorAssign;
    if (!g_available)
        LOG_ENGINE("vraie stereo indisponible (signature manquante) : rendu alterne seulement");

    if (const auto* p = static_cast<const char*>(pattern::Find("copie de refraction de l'eau", kRefractionCopySig))) {
        uint32_t frameNo = 0, copied = 0, copiedStore = 0;
        memcpy(&frameNo, p + 2, sizeof(frameNo));
        memcpy(&copied, p + 8, sizeof(copied));
        memcpy(&copiedStore, p + 20, sizeof(copiedStore));
        if (copied == copiedStore && frameNo != copied && frameNo < 0x10000 && copied < 0x10000) {
            g_frameNoOffset = frameNo;
            g_refractionFrameOffset = copied;
            LOG_ENGINE("refraction de l'eau : numero d'image a rglRenderer+0x%X, copie a +0x%X", frameNo, copied);
        } else {
            LOG_ENGINE("ERREUR : copie de refraction : offsets invraisemblables (0x%X 0x%X 0x%X)", frameNo, copied,
                       copiedStore);
        }
    }
    if (!g_refractionFrameOffset)
        LOG_ENGINE("vraie stereo : refraction de l'eau non corrigee (le second oeil peut montrer sous l'eau "
                   "l'image du premier)");
    return g_available;
}

bool Enabled()
{
    return g_available && config::GetBool(config::Id::TrueStereo);
}

rgl::RenderContext* PrepareSecond(rgl::RenderContext* main)
{
    if (!g_available || !main)
        return nullptr;
    if (!g_second) {
        g_second = g_getRenderContext(nullptr);
        if (!g_second) {
            LOG_ENGINE("ERREUR : vraie stereo : pas de contexte de rendu disponible");
            g_available = false;
            return nullptr;
        }
        LOG_ENGINE("vraie stereo : second contexte de rendu %p (contexte du widget %p)", g_second, main);
    }
    rgl::RenderContext* second = g_second;
    // Encore dans la file du renderer (image précédente pas encore consommée) : on n'y touche pas
    if (second->queuedForRendering) {
        if (!g_loggedBusy) {
            LOG_ENGINE("vraie stereo : second contexte encore en file, image en rendu alterne");
            g_loggedBusy = true;
        }
        return nullptr;
    }
    // setSourceContext échange les listes de maillages : elles doivent être vides des deux côtés
    // (elles le sont avant le remplissage : le renderer les a reprises et vidées)
    if (VectorSize(main->renderMeshes) || VectorSize(second->renderMeshes)) {
        if (!g_loggedNotEmpty) {
            LOG_ENGINE("vraie stereo : liste de maillages non vide avant remplissage (%zu / %zu), image ignoree",
                       VectorSize(main->renderMeshes), VectorSize(second->renderMeshes));
            g_loggedNotEmpty = true;
        }
        return nullptr;
    }
    // Reflets de l'eau : rglScene::render garde son contexte de reflet dans m_waterReflectionRenderContext
    // (créé à la première image, réutilisé ensuite). setSourceContext recopie celui du widget : les deux
    // yeux partageraient un seul contexte de reflet, vidé et reconfiguré par le second remplissage (le
    // renderer le rendait une fois plein avec la caméra de l'œil droit, une fois vide : pas de reflet
    // dans l'œil gauche). Le second contexte garde le sien (nul la première fois : le moteur le crée).
    rgl::RenderContext* ownReflection = second->waterReflection;
    if (ownReflection == main->waterReflection)
        ownReflection = nullptr;
    g_setSourceContext(second, main);
    second->waterReflection = ownReflection;
    // Contexte mis en file lui-même, pas une copie du renderer : m_source nul
    *reinterpret_cast<rgl::RenderContext**>(reinterpret_cast<char*>(second) + g_sourceOffset) = nullptr;
    // Ce que setSourceContext ne recopie pas : cibles de rendu (scène HDR, profondeur...) et
    // nœuds de post-effets, lus sur le contexte source au rendu. Ressources du renderer,
    // partagées : les deux contextes sont rendus l'un après l'autre.
    second->renderTarget = main->renderTarget;
    second->depthOfFieldRenderTarget = main->depthOfFieldRenderTarget;
    second->postFxMsRenderTarget = main->postFxMsRenderTarget;
    second->postFxMsDepthStencil = main->postFxMsDepthStencil;
    second->activeRenderTarget = main->activeRenderTarget;
    second->activeDepthStencil = main->activeDepthStencil;
    second->waterReflectionRenderTarget = main->waterReflectionRenderTarget;
    second->waterReflectionDepthStencil = main->waterReflectionDepthStencil;
    second->cubeRenderTargetFace = main->cubeRenderTargetFace;
    second->entityDegradingEnabled = main->entityDegradingEnabled;
    g_ptrVectorAssign(second->postFxNodes, main->postFxNodes);

    if (!g_loggedSetup) {
        LOG_ENGINE("vraie stereo : contexte du widget : cible %p, scene HDR %p, profondeur %p, post-effets %s "
                   "(%zu noeuds), reflet %p, drapeaux 0x%X, viewport %dx%d",
                   main->renderTarget, main->activeRenderTarget, main->activeDepthStencil,
                   main->postFxEnabled ? "actifs" : "inactifs", VectorSize(main->postFxNodes), main->waterReflection,
                   main->flags, main->viewport.width, main->viewport.height);
        LOG_ENGINE("vraie stereo : second contexte : post-effets %s (%zu noeuds), drapeaux 0x%X, viewport %dx%d",
                   second->postFxEnabled ? "actifs" : "inactifs", VectorSize(second->postFxNodes), second->flags,
                   second->viewport.width, second->viewport.height);
        g_loggedSetup = true;
    }
    // Contexte de reflet propre au second œil : une ligne quand il apparaît (reflets activés)
    static rgl::RenderContext* loggedReflection = nullptr;
    if (ownReflection && ownReflection != loggedReflection) {
        LOG_ENGINE("vraie stereo : reflets de l'eau : contexte %p pour l'oeil gauche, %p pour l'oeil droit",
                   main->waterReflection, ownReflection);
        loggedReflection = ownReflection;
    }
    return second;
}

void BeforeEyeScene(void* renderer)
{
    if (!renderer || !g_refractionFrameOffset)
        return;
    // Image de la dernière copie différente de l'image en cours : la copie est refaite au premier
    // maillage d'eau de cette scène, avec l'image de cet œil
    char* r = static_cast<char*>(renderer);
    int32_t frameNo = 0;
    memcpy(&frameNo, r + g_frameNoOffset, sizeof(frameNo));
    const int32_t stale = frameNo - 1;
    memcpy(r + g_refractionFrameOffset, &stale, sizeof(stale));
}

SceneShadowState SaveSceneShadows(void* scene)
{
    SceneShadowState s;
    if (scene) {
        const char* p = static_cast<const char*>(scene);
        memcpy(&s.count, p + rgl::kSceneNumCharacterShadows, sizeof(s.count));
        memcpy(&s.range, p + rgl::kSceneCharacterShadowsRange, sizeof(s.range));
    }
    return s;
}

void RestoreSceneShadows(void* scene, const SceneShadowState& afterFirst)
{
    if (!scene)
        return;
    char* p = static_cast<char*>(scene);
    // Portée : celle décidée par le premier rendu ; compteur : le plus grand des deux yeux, pour
    // que l'ajustement de l'image suivante voie la charge réelle
    int count = 0;
    memcpy(&count, p + rgl::kSceneNumCharacterShadows, sizeof(count));
    count = std::max(count, afterFirst.count);
    memcpy(p + rgl::kSceneNumCharacterShadows, &count, sizeof(count));
    memcpy(p + rgl::kSceneCharacterShadowsRange, &afterFirst.range, sizeof(afterFirst.range));
}

} // namespace engine::stereo
