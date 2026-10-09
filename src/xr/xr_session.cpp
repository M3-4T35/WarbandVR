// Session OpenXR (runtime Meta via Quest Link) sur un device D3D11 dédié.
//
// Hors mission, l'image du jeu est affichée sur un quad (écran virtuel) fixe dans l'espace
// LOCAL, à 2 m devant la position de la tête au démarrage. En mission, le jeu rend
// alternativement l'œil gauche et l'œil droit (rendu alterné) : chaque œil a sa swapchain
// et est soumis dans un calque de projection avec la pose et le champ de vue de son rendu.
// Tout tourne dans un thread à part : xrWaitFrame cadence ce thread au rythme du casque
// sans ralentir le jeu. Il publie aussi la pose de la tête pour le rendu du jeu.
// Avant tout en-tête OpenXR (xr_hands.h l'inclut aussi) : API D3D11 et extensions Windows
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include "xr/xr_session.h"
#include "render/d3d9_hook.h"
#include "render/frame_share.h"
#include "render/game_window.h"
#include "util/config.h"
#include "util/log.h"
#include "xr/xr_hands.h"
#include "xr/xr_controls.h"
#include "xr/xr_keyboard.h"
#include "xr/xr_radial.h"
#include "engine/text_input.h"
#include "engine/audio_route.h"
#include "engine/frame_pacing.h"
#include "engine/true_stereo.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define LOG_XR(...) ::vrlog::Write("[xr]", __VA_ARGS__)

namespace xr {

// Applique les réglages à l'état partagé (au démarrage : aussi la source du réticule)
void ApplyConfigToState(bool startup);

namespace {

// Écran virtuel : largeur en mètres et distance devant l'utilisateur
float ScreenWidth() { return config::Get(config::Id::ScreenWidth); }
float ScreenDistance() { return config::Get(config::Id::ScreenDistance); }
// Interface (HUD) pendant la stéréo : écran lié à la tête, largeur et distance en mètres.
// Assez loin pour que les éléments placés sur les soldats (noms) restent proches de leur
// profondeur : un HUD proche paraît dédoublé quand on fixe la scène au loin. Même taille
// apparente qu'un écran de 1,3 m à 1,5 m (~47 degrés).
float HudDistance() { return config::Get(config::Id::HudDistance); }
float HudWidth() { return 2 * HudDistance() * std::tan(config::Get(config::Id::HudAngle) * 3.14159265f / 360.0f); }
constexpr ULONGLONG kStatsIntervalMs = 10000;
// Point du curseur des menus : taille en pixels de sa texture et en mètres sur l'écran
constexpr UINT kCursorPixels = 64;
constexpr float kCursorSize = 0.03f;

std::atomic<bool> g_stop{false};

// Pose de la tête publiée par le thread XR, lue par le thread de rendu du jeu
std::mutex g_headMutex;
HeadPose g_head;
bool g_headValid = false;

void PublishHead(const XrPosef* head, const XrView* views)
{
    std::lock_guard lock(g_headMutex);
    g_headValid = head != nullptr;
    if (!head)
        return;
    g_head.qx = head->orientation.x, g_head.qy = head->orientation.y, g_head.qz = head->orientation.z,
    g_head.qw = head->orientation.w;
    g_head.px = head->position.x, g_head.py = head->position.y, g_head.pz = head->position.z;
    for (int e = 0; e < 2; ++e) {
        const XrView& v = views[e];
        g_head.eyes[e] = {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w,
                          v.pose.position.x,    v.pose.position.y,    v.pose.position.z,
                          v.fov.angleLeft,      v.fov.angleRight,     v.fov.angleUp,
                          v.fov.angleDown};
    }
}

// --- Chien de garde : signale un appel bloquant qui ne rend pas la main -------------

constexpr ULONGLONG kStuckMs = 3000;
std::atomic<const char*> g_phase{nullptr}; // appel en cours dans le thread XR
std::atomic<ULONGLONG> g_phaseSince{0};

struct Phase {
    explicit Phase(const char* name)
    {
        g_phaseSince = GetTickCount64();
        g_phase = name;
    }
    ~Phase() { g_phase = nullptr; }
};

void Watchdog()
{
    const char* stuckIn = nullptr;
    while (!g_stop) {
        Sleep(1000);
        const char* phase = g_phase;
        const ULONGLONG since = g_phaseSince;
        if (phase && GetTickCount64() - since > kStuckMs) {
            if (stuckIn != phase)
                LOG_XR("ALERTE : thread XR bloque dans %s depuis %llu ms", phase, GetTickCount64() - since);
            stuckIn = phase;
        } else if (stuckIn) {
            LOG_XR("thread XR debloque (etait dans %s)", stuckIn);
            stuckIn = nullptr;
        }
    }
}

const char* StateName(XrSessionState s)
{
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

class XrApp {
public:
    ~XrApp() { Shutdown(); }
    void Run();

private:
    bool Check(XrResult r, const char* what);
    bool CreateInstance();
    bool WaitForSystem();
    bool CreateDevice();
    bool CreateSession();
    bool EnsureSwapchains(UINT w, UINT h);
    void DestroySwapchains();
    void PollEvents();
    void RenderFrame();
    bool CopyGameFrame(int eye, int target);
    bool EnsureCursor();
    void Shutdown();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace baseSpace_ = XR_NULL_HANDLE;  // LOCAL du runtime
    XrSpace localSpace_ = XR_NULL_HANDLE; // LOCAL recentré (référence de tout le reste)
    void Recenter(XrTime time);
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    bool exitRequested_ = false;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Query* copyDone_ = nullptr;

    // Une swapchain par œil (celle de l'œil gauche sert aussi à l'image plate), plus une
    // pour l'interface
    struct EyeChain {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageD3D11KHR> images;
        bool hasContent = false;
        render::RenderView view; // vue de l'image actuellement dans la swapchain
        uint64_t copiedSeq = 0;  // numéro de la dernière image copiée
    };
    static constexpr int kUiChain = 2;
    EyeChain chains_[3];
    bool hudLogged_ = false;
    int64_t format_ = 0;
    UINT scWidth_ = 0;
    UINT scHeight_ = 0;
    bool copyWarned_ = false;
    bool stereoLogged_ = false;
    // Résolution recommandée par œil (réglages Quest Link) : taille donnée à la fenêtre du jeu
    uint32_t recommendedWidth_ = 0;
    uint32_t recommendedHeight_ = 0;
    bool windowResized_ = false;
    bool windowWaitLogged_ = false;
    float windowScale_ = 0; // échelle de rendu appliquée à la fenêtre
    // Une fois par partie : la fenêtre du jeu prend la résolution d'un œil. Retenté à chaque
    // frame tant que le jeu n'a pas encore présenté d'image (fenêtre inconnue) : la session
    // peut démarrer avant lui.
    void ResizeWindowOnce();

    // Pointeur des menus (manettes) et son point de curseur
    XrControls controls_;
    HandRenderer hands_;
    VirtualKeyboard keyboard_;
    RadialMenu radial_;
    bool keyboardLogged_ = false;
    XrSwapchain cursorSwapchain_ = XR_NULL_HANDLE;
    bool cursorReady_ = false;
    bool cursorFailed_ = false;
    bool eyesLogged_ = false;
    bool hasAudioGuid_ = false;
    void UpdateAudio();
    // Cadence du jeu calée sur le casque (rendu alterné : 2 images par rafraîchissement)
    void UpdatePacing(XrDuration period, bool stereo);
    double refreshHz_ = 0;  // fréquence d'affichage du casque (la plus haute annoncée)
    int pacingLevel_ = 1;   // une paire d'images toutes les pacingLevel_ frames du casque
    int pacingPerPair_ = 2; // images du jeu par paire (2 rendu alterné, 1 vraie stéréo)
    float pacingScale_ = 0; // échelle de rendu pour laquelle le niveau a été choisi
    int pacingLow_ = 0;
    // Retour au niveau plus rapide après une baisse : essai de quelques secondes, espacé de plus
    // en plus s'il échoue (une scène trop lourde ne provoque pas un à-coup régulier)
    static constexpr ULONGLONG kProbeFirstMs = 20000, kProbeMaxMs = 300000, kProbeWindowMs = 4000;
    bool probing_ = false;
    ULONGLONG probeAtMs_ = 0;
    ULONGLONG probeBackoffMs_ = kProbeFirstMs;
    ULONGLONG pacingCheckMs_ = 0;
    unsigned long long pacingFrames_ = 0;
    float pacingManual_ = -1; // limite manuelle (réglage), 0 = automatique
    // Slots du jeu copiés vers le casque : rendus au jeu quand la copie GPU est finie
    void SubmitCopies();
    void FinishCopies(bool wait);
    std::vector<int> copySlots_;    // copiés à cette frame
    std::vector<int> pendingSlots_; // copies soumises, en attente du GPU
    int pendingFrames_ = 0;
    uint64_t copyPair_ = UINT64_MAX; // paire stéréo copiée à cette frame
    uint64_t shownPair_ = 0;         // dernière paire affichée
    // Statistiques : paires nouvelles, sautées (jamais affichées), frames sans nouvelle paire
    unsigned long long newPairs_ = 0, skippedPairs_ = 0, repeatFrames_ = 0;

    // Textures partagées du jeu ouvertes côté D3D11, par slot
    ID3D11Texture2D* shared_[render::kSharedSlots]{};
    uint64_t sharedGeneration_ = 0;

    unsigned long long framesSubmitted_ = 0;
    unsigned long long framesCopied_ = 0;
    ULONGLONG lastStats_ = 0;
};

bool XrApp::Check(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r))
        return true;
    char name[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance_ == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance_, r, name)))
        snprintf(name, sizeof(name), "%d", static_cast<int>(r));
    LOG_XR("ERREUR : %s -> %s", what, name);
    return false;
}

bool XrApp::CreateInstance()
{
    uint32_t count = 0;
    if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "enumeration des extensions"))
        return false;
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
    bool hasD3D11 = false;
    for (const auto& e : exts) {
        hasD3D11 |= strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
        hasAudioGuid_ |= strcmp(e.extensionName, XR_OCULUS_AUDIO_DEVICE_GUID_EXTENSION_NAME) == 0;
    }
    if (!hasD3D11) {
        LOG_XR("ERREUR : le runtime ne propose pas %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return false;
    }

    // Sortie audio du casque : extension optionnelle (runtime Meta) ; sinon sortie par défaut
    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_OCULUS_AUDIO_DEVICE_GUID_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "WarbandVR");
    strcpy_s(ci.applicationInfo.engineName, "WSE2");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = hasAudioGuid_ ? 2 : 1;
    ci.enabledExtensionNames = enabled;
    if (!Check(xrCreateInstance(&ci, &instance_), "xrCreateInstance"))
        return false;

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance_, &props);
    LOG_XR("runtime : %s %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
           XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
    LOG_XR("sortie audio du casque : %s", hasAudioGuid_ ? "donnee par le runtime (XR_OCULUS_audio_device_guid)"
                                                         : "sortie Windows par defaut");
    return true;
}

// Le casque peut ne pas être encore branché / Link inactif : on réessaie
bool XrApp::WaitForSystem()
{
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    bool warned = false;
    while (!g_stop) {
        XrResult r = xrGetSystem(instance_, &gi, &system_);
        if (XR_SUCCEEDED(r))
            break;
        if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE)
            return Check(r, "xrGetSystem");
        if (!warned) {
            LOG_XR("casque non disponible (Quest Link actif ?), nouvel essai toutes les 2 s");
            warned = true;
        }
        Sleep(2000);
    }
    if (g_stop)
        return false;

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance_, system_, &sp);
    LOG_XR("casque : %s (vendor 0x%X), %u calques max", sp.systemName, sp.vendorId,
           sp.graphicsProperties.maxLayerCount);
    return true;
}

bool XrApp::CreateDevice()
{
    auto getReqs = PFN_xrGetD3D11GraphicsRequirementsKHR{};
    if (!Check(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)),
               "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
        return false;
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(getReqs(instance_, system_, &reqs), "xrGetD3D11GraphicsRequirementsKHR"))
        return false;

    // Le device D3D11 doit être sur l'adaptateur imposé par le runtime
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        LOG_XR("ERREUR : CreateDXGIFactory1");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (memcmp(&d.AdapterLuid, &reqs.adapterLuid, sizeof(LUID)) == 0) {
            char name[128]{};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
            LOG_XR("adaptateur du casque : %s", name);
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    if (!adapter) {
        LOG_XR("ERREUR : adaptateur demande par le runtime introuvable");
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_,
                                   &got, &context_);
    adapter->Release();
    if (FAILED(hr) || got < reqs.minFeatureLevel) {
        LOG_XR("ERREUR : D3D11CreateDevice 0x%08lX (niveau 0x%X, minimum 0x%X)", static_cast<unsigned long>(hr),
               got, reqs.minFeatureLevel);
        return false;
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    device_->CreateQuery(&qd, &copyDone_);
    LOG_XR("device D3D11 cree (niveau 0x%X)", got);
    return true;
}

bool XrApp::CreateSession()
{
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device_;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = system_;
    if (!Check(xrCreateSession(instance_, &ci, &session_), "xrCreateSession"))
        return false;

    XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    si.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(session_, &si, &baseSpace_), "xrCreateReferenceSpace(LOCAL)") ||
        !Check(xrCreateReferenceSpace(session_, &si, &localSpace_), "xrCreateReferenceSpace(LOCAL)"))
        return false;
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(session_, &si, &viewSpace_), "xrCreateReferenceSpace(VIEW)"))
        return false;

    // Format : sRGB en priorité, car le jeu écrit des valeurs déjà encodées en sRGB
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(session_, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(session_, count, &count, formats.data());
    for (int64_t wanted : {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM}) {
        for (int64_t f : formats)
            if (f == wanted && !format_)
                format_ = f;
    }
    if (!format_) {
        LOG_XR("ERREUR : aucun format BGRA propose par le runtime (%u formats)", count);
        return false;
    }
    // Résolution recommandée par œil
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    std::vector<XrViewConfigurationView> configViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount && XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance_, system_,
                                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                    viewCount, &viewCount, configViews.data()))) {
        recommendedWidth_ = configViews[0].recommendedImageRectWidth;
        recommendedHeight_ = configViews[0].recommendedImageRectHeight;
        LOG_XR("resolution par oeil : recommandee %ux%u, maximale %ux%u", recommendedWidth_, recommendedHeight_,
               configViews[0].maxImageRectWidth, configViews[0].maxImageRectHeight);
    }
    LOG_XR("session creee, format de swapchain %lld%s", format_,
           format_ == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? " (BGRA sRGB)" : " (BGRA, couleurs possiblement fausses)");
    return true;
}

// Recentrage : l'espace de référence prend la position actuelle de la tête et sa direction
// horizontale (le haut reste vertical). Écran virtuel, corps en combat et hauteur suivent.
void XrApp::Recenter(XrTime time)
{
    XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (XR_FAILED(xrLocateSpace(viewSpace_, baseSpace_, time, &l)) || (l.locationFlags & needed) != needed) {
        LOG_XR("recentrage impossible : pose de la tete inconnue");
        return;
    }
    const XrQuaternionf& q = l.pose.orientation;
    const float fx = -2 * (q.x * q.z + q.y * q.w); // direction du regard (-Z de la tête)
    const float fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz);
    XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    si.poseInReferenceSpace.orientation = {0.0f, std::sin(yaw / 2), 0.0f, std::cos(yaw / 2)};
    si.poseInReferenceSpace.position = l.pose.position;
    XrSpace space = XR_NULL_HANDLE;
    if (!Check(xrCreateReferenceSpace(session_, &si, &space), "xrCreateReferenceSpace (recentrage)"))
        return;
    if (localSpace_ != XR_NULL_HANDLE)
        xrDestroySpace(localSpace_);
    localSpace_ = space;
    LOG_XR("recentrage : direction %.0f degres, tete a (%.2f, %.2f, %.2f) m", yaw * 57.29578f, l.pose.position.x,
           l.pose.position.y, l.pose.position.z);
}

// Cadence : en rendu alterné, chaque image du jeu ne sert qu'un œil ; le casque affiche des
// paires (gauche + droite). La cadence doit être RÉGULIÈRE : le limiteur du jeu vise une paire
// toutes les 1, 2 ou 3 frames du casque (niveau), le plus rapide que la machine tient en
// permanence. Une cadence qui varie avec la scène (jeu non limité) fait saccader l'image.
// Fréquence de référence : la plus haute annoncée par le runtime.
void XrApp::UpdatePacing(XrDuration period, bool stereo)
{
    if (period <= 0)
        return;
    engine::EnforceGameMaxFps(); // options du jeu : sa limite d'images ne doit pas écraser la nôtre
    const ULONGLONG now = GetTickCount64();
    const double hz = 1e9 / static_cast<double>(period);
    const float manual = config::Get(config::Id::GameFpsLimit);
    // Images du jeu par paire : 2 en rendu alterné, 1 en vraie stéréo
    const int perPair = engine::stereo::Enabled() ? 1 : 2;
    // Nouvelle échelle de rendu : la machine tient peut-être une cadence plus haute, on repart du haut
    if (manual != pacingManual_ || hz > refreshHz_ + 1.0 || perPair != pacingPerPair_ || windowScale_ != pacingScale_) {
        pacingManual_ = manual;
        pacingPerPair_ = perPair;
        pacingScale_ = windowScale_;
        refreshHz_ = std::max(refreshHz_, hz);
        pacingLevel_ = 1;
        pacingLow_ = 0;
        probing_ = false;
        probeBackoffMs_ = kProbeFirstMs;
        pacingCheckMs_ = now;
        pacingFrames_ = render::PresentCount();
        if (manual > 0) {
            LOG_XR("casque a %.1f Hz, limite manuelle du jeu : %.0f images/s", refreshHz_, manual);
            engine::SetGameMaxFps(manual);
        } else {
            LOG_XR("casque a %.1f Hz : une paire d'images (gauche + droite) par frame du casque (%s)", refreshHz_,
                   perPair == 1 ? "vraie stereo" : "rendu alterne");
            engine::SetGameMaxFps(perPair * refreshHz_);
        }
        return;
    }
    if (manual > 0 || !stereo) {
        pacingCheckMs_ = now;
        pacingFrames_ = render::PresentCount();
        return;
    }
    if (now - pacingCheckMs_ < (probing_ ? kProbeWindowMs : 5000))
        return;
    const unsigned long long frames = render::PresentCount();
    const double fps = (frames - pacingFrames_) * 1000.0 / static_cast<double>(now - pacingCheckMs_);
    pacingCheckMs_ = now;
    pacingFrames_ = frames;
    const double target = perPair * refreshHz_ / pacingLevel_;
    // 90 % : quelques images de moins valent mieux que la moitié de la cadence (68 au lieu de 72
    // ne doit pas faire tomber à 36)
    const bool holds = fps >= 0.9 * target;

    // Essai du niveau plus rapide : gardé s'il tient, sinon retour aussitôt et essai suivant plus tard
    if (probing_) {
        probing_ = false;
        if (holds) {
            LOG_XR("le jeu tient de nouveau %.0f images/s (%.0f mesurees) : cadence remontee", target, fps);
            probeBackoffMs_ = kProbeFirstMs;
            probeAtMs_ = now + probeBackoffMs_;
            return;
        }
        ++pacingLevel_;
        probeBackoffMs_ = std::min(probeBackoffMs_ * 2, kProbeMaxMs);
        probeAtMs_ = now + probeBackoffMs_;
        engine::SetGameMaxFps(perPair * refreshHz_ / pacingLevel_);
        LOG_XR("essai a %.0f images/s echoue (%.0f mesurees) : retour a %.0f, prochain essai dans %llu s", target, fps,
               perPair * refreshHz_ / pacingLevel_, probeBackoffMs_ / 1000);
        return;
    }
    if (holds || pacingLevel_ >= 3) {
        pacingLow_ = 0;
        // Cadence réduite et tenue : on retente la plus rapide de temps en temps
        if (holds && pacingLevel_ > 1 && now >= probeAtMs_) {
            --pacingLevel_;
            probing_ = true;
            engine::SetGameMaxFps(perPair * refreshHz_ / pacingLevel_);
            LOG_XR("essai de la cadence superieure : %.0f images/s", perPair * refreshHz_ / pacingLevel_);
        }
        return;
    }
    if (++pacingLow_ < 2)
        return;
    // La machine ne tient pas ce rythme : niveau suivant (une paire toutes les n frames)
    ++pacingLevel_;
    pacingLow_ = 0;
    probeAtMs_ = now + probeBackoffMs_;
    const double next = perPair * refreshHz_ / pacingLevel_;
    LOG_XR("le jeu ne tient pas %.0f images/s (%.0f mesurees) : une paire toutes les %d frames du casque "
           "(%.0f images/s), nouvel essai dans %llu s", target, fps, pacingLevel_, next, probeBackoffMs_ / 1000);
    engine::SetGameMaxFps(next);
}

// Copies vers le casque soumises au GPU : un seul point de synchronisation par frame
void XrApp::SubmitCopies()
{
    if (copySlots_.empty())
        return;
    if (!pendingSlots_.empty())
        FinishCopies(true); // la précédente doit être finie (une seule requête GPU)
    context_->End(copyDone_);
    context_->Flush();
    pendingSlots_.swap(copySlots_);
    copySlots_.clear();
    pendingFrames_ = 0;
}

// Rend au jeu les textures dont la copie GPU est finie. wait : attendre (200 ms au plus)
void XrApp::FinishCopies(bool wait)
{
    if (pendingSlots_.empty())
        return;
    HRESULT hr = context_->GetData(copyDone_, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    // Au-delà de quelques frames, on attend pour ne pas priver le jeu de textures
    if (hr == S_FALSE && (wait || ++pendingFrames_ > 3)) {
        Phase p("attente fin de copie D3D11");
        const ULONGLONG deadline = GetTickCount64() + 200;
        while ((hr = context_->GetData(copyDone_, nullptr, 0, 0)) == S_FALSE && GetTickCount64() < deadline)
            YieldProcessor();
        if (hr != S_OK && !copyWarned_) {
            LOG_XR("ALERTE : copie D3D11 non terminee (0x%08lX)", static_cast<unsigned long>(hr));
            copyWarned_ = true;
        }
        hr = S_OK; // textures rendues quand même : le jeu ne doit pas manquer de slots
    }
    if (hr != S_OK)
        return;
    for (int slot : pendingSlots_)
        render::ReleaseFrame(slot);
    pendingSlots_.clear();
}

// Son : FMOD (chargé par le jeu) basculé sur la sortie audio du casque
void XrApp::UpdateAudio()
{
    if (!engine::InstallAudioRoute())
        return; // FMOD pas encore chargé
    wchar_t id[XR_MAX_AUDIO_DEVICE_STR_SIZE_OCULUS]{};
    if (hasAudioGuid_ && instance_ != XR_NULL_HANDLE) {
        PFN_xrGetAudioOutputDeviceGuidOculus get = nullptr;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(instance_, "xrGetAudioOutputDeviceGuidOculus",
                                               reinterpret_cast<PFN_xrVoidFunction*>(&get))) &&
            get && XR_FAILED(get(instance_, id)))
            id[0] = 0;
    }
    engine::SetHeadsetAudioDevice(id);
}

bool XrApp::EnsureSwapchains(UINT w, UINT h)
{
    if (chains_[0].swapchain != XR_NULL_HANDLE && w == scWidth_ && h == scHeight_)
        return true;
    DestroySwapchains();

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format_;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    for (EyeChain& c : chains_) {
        if (!Check(xrCreateSwapchain(session_, &ci, &c.swapchain), "xrCreateSwapchain")) {
            DestroySwapchains();
            return false;
        }
        uint32_t count = 0;
        xrEnumerateSwapchainImages(c.swapchain, 0, &count, nullptr);
        c.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(c.swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(c.images.data()));
    }
    scWidth_ = w;
    scHeight_ = h;
    LOG_XR("3 swapchains %ux%u creees (%zu images chacune)", w, h, chains_[0].images.size());
    return true;
}

void XrApp::DestroySwapchains()
{
    for (EyeChain& c : chains_) {
        if (c.swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(c.swapchain);
        c = EyeChain{};
    }
    scWidth_ = scHeight_ = 0;
}

// Swapchain du point de curseur : disque blanc cerclé de sombre, alpha prémultiplié,
// rempli une fois (le calque réutilise la dernière image libérée)
bool XrApp::EnsureCursor()
{
    if (cursorReady_ || cursorFailed_)
        return cursorReady_;
    cursorFailed_ = true; // en cas d'échec, on n'insiste pas

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format_;
    ci.sampleCount = 1;
    ci.width = kCursorPixels;
    ci.height = kCursorPixels;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!Check(xrCreateSwapchain(session_, &ci, &cursorSwapchain_), "xrCreateSwapchain (curseur)"))
        return false;
    uint32_t count = 0;
    xrEnumerateSwapchainImages(cursorSwapchain_, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(cursorSwapchain_, count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    // Pixels BGRA prémultipliés
    std::vector<uint32_t> pixels(kCursorPixels * kCursorPixels);
    const float c = (kCursorPixels - 1) / 2.0f;
    for (UINT y = 0; y < kCursorPixels; ++y)
        for (UINT x = 0; x < kCursorPixels; ++x) {
            const float d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c)) / c;
            uint32_t px = 0;
            if (d <= 0.7f)
                px = 0xFFFFFFFF; // blanc opaque
            else if (d <= 1.0f)
                px = 0xE0000000; // contour sombre
            pixels[y * kCursorPixels + x] = px;
        }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kCursorPixels;
    td.MipLevels = td.ArraySize = 1;
    td.Format = static_cast<DXGI_FORMAT>(format_);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{pixels.data(), kCursorPixels * 4, 0};
    ID3D11Texture2D* src = nullptr;
    if (FAILED(device_->CreateTexture2D(&td, &init, &src))) {
        LOG_XR("ERREUR : texture du curseur non creee");
        return false;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const bool ok = Check(xrAcquireSwapchainImage(cursorSwapchain_, &ai, &index), "xrAcquireSwapchainImage (curseur)") &&
                    Check(xrWaitSwapchainImage(cursorSwapchain_, &wi), "xrWaitSwapchainImage (curseur)");
    if (ok) {
        context_->CopyResource(images[index].texture, src);
        xrReleaseSwapchainImage(cursorSwapchain_, &ri);
    }
    src->Release();
    cursorReady_ = ok;
    cursorFailed_ = !ok;
    return ok;
}

void XrApp::PollEvents()
{
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& e = reinterpret_cast<const XrEventDataSessionStateChanged&>(ev);
            state_ = e.state;
            LOG_XR("etat de session : %s", StateName(state_));
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running_ = Check(xrBeginSession(session_, &bi), "xrBeginSession");
                if (running_)
                    LOG_XR("session demarree");
                ResizeWindowOnce();
                // Casque mis : le jeu doit avoir le focus pour recevoir la souris (pointeur)
                if (running_)
                    render::BringGameToFront();
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session_);
                running_ = false;
                PublishHead(nullptr, nullptr);
                LOG_XR("session arretee");
            } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
                exitRequested_ = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LOG_XR("perte de l'instance OpenXR annoncee");
            exitRequested_ = true;
            break;
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Copie la dernière image de la catégorie eye (-1 plate, 0/1 œil) dans la swapchain target,
// si elle est plus récente que ce qui y est déjà. Renvoie true si une image a été copiée.
bool XrApp::CopyGameFrame(int eye, int target)
{
    EyeChain& chain = chains_[target];
    render::SharedFrame frame;
    const uint64_t newerThan = chain.view.eye == eye ? chain.copiedSeq : 0;
    if (!render::AcquireLatestFrame(eye, newerThan, frame, copyPair_))
        return false;

    bool copied = false;
    if (frame.generation != sharedGeneration_) {
        for (auto*& t : shared_)
            if (t) {
                t->Release();
                t = nullptr;
            }
        sharedGeneration_ = frame.generation;
    }
    ID3D11Texture2D*& src = shared_[frame.slot];
    if (!src) {
        HRESULT hr = device_->OpenSharedResource(frame.handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
        if (FAILED(hr)) {
            LOG_XR("ERREUR : OpenSharedResource slot %d : 0x%08lX", frame.slot, static_cast<unsigned long>(hr));
            src = nullptr;
        }
    }

    // Recréer les swapchains remet chain à zéro : on prend la référence ensuite
    if (src && EnsureSwapchains(frame.width, frame.height)) {
        XrSwapchain swapchain = chain.swapchain;
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        bool acquired = false;
        {
            Phase p("xrAcquireSwapchainImage");
            acquired = Check(xrAcquireSwapchainImage(swapchain, &ai, &index), "xrAcquireSwapchainImage");
        }
        if (acquired) {
            {
                Phase p("xrWaitSwapchainImage");
                acquired = Check(xrWaitSwapchainImage(swapchain, &wi), "xrWaitSwapchainImage");
            }
            if (acquired) {
                context_->CopySubresourceRegion(chain.images[index].texture, 0, 0, 0, 0, src, 0, nullptr);
                // La texture du jeu ne lui est rendue qu'une fois la copie GPU finie
                // (FinishCopies) : pas d'attente ici, qui retarderait la frame du casque
                copied = true;
                chain.hasContent = true;
                chain.view = frame.view;
                chain.view.eye = eye;
                chain.copiedSeq = frame.seq;
            }
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            Phase p("xrReleaseSwapchainImage");
            xrReleaseSwapchainImage(swapchain, &ri);
        }
    }
    if (copied)
        copySlots_.push_back(frame.slot);
    else
        render::ReleaseFrame(frame.slot);
    return copied;
}

void XrApp::ResizeWindowOnce()
{
    // Échelle de rendu modifiée en cours de partie : nouveau redimensionnement
    const float scale = config::Get(config::Id::RenderScale);
    if (windowResized_ && scale != windowScale_)
        windowResized_ = false;
    if (!running_ || windowResized_ || !recommendedWidth_ || !recommendedHeight_ ||
        !config::GetBool(config::Id::ResizeWindow))
        return;
    // Dimensions paires, au moins 256
    const auto scaled = [scale](uint32_t v) {
        return std::max<UINT>(256, static_cast<UINT>(v * scale / 2.0f + 0.5f) * 2);
    };
    const UINT w = scaled(recommendedWidth_), h = scaled(recommendedHeight_);
    if (render::ResizeGameWindow(w, h)) {
        windowResized_ = true;
        windowScale_ = scale;
        LOG_XR("resolution de rendu par oeil : %ux%u (echelle %.2f de la resolution recommandee)", w, h, scale);
    } else if (!windowWaitLogged_) {
        LOG_XR("fenetre du jeu pas encore connue : redimensionnement au premier Present");
        windowWaitLogged_ = true;
    }
}

void XrApp::RenderFrame()
{
    ResizeWindowOnce();
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    {
        Phase p("xrWaitFrame");
        if (!Check(xrWaitFrame(session_, &wi, &fs), "xrWaitFrame"))
            return;
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    {
        Phase p("xrBeginFrame");
        if (!Check(xrBeginFrame(session_, &bi), "xrBeginFrame"))
            return;
    }

    // Poses de la tête et des yeux prédites pour cet affichage : le jeu les utilisera pour
    // ses prochaines images
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = localSpace_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView eyeViews[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t viewCount = 0;
    const bool eyesValid = XR_SUCCEEDED(xrLocateSpace(viewSpace_, localSpace_, fs.predictedDisplayTime, &loc)) &&
                           (loc.locationFlags & needed) == needed &&
                           XR_SUCCEEDED(xrLocateViews(session_, &vli, &vs, 2, &viewCount, eyeViews)) &&
                           viewCount == 2 && (vs.viewStateFlags & needed) == needed;
    if (eyesValid) {
        PublishHead(&loc.pose, eyeViews);
        if (!eyesLogged_) {
            constexpr float kDeg = 57.29578f;
            const float dx = eyeViews[1].pose.position.x - eyeViews[0].pose.position.x;
            const float dy = eyeViews[1].pose.position.y - eyeViews[0].pose.position.y;
            const float dz = eyeViews[1].pose.position.z - eyeViews[0].pose.position.z;
            LOG_XR("yeux : ecart %.1f mm", std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f);
            for (int e = 0; e < 2; ++e)
                LOG_XR("  oeil %s : champ de vue g=%.1f d=%.1f h=%.1f b=%.1f degres", e ? "droit" : "gauche",
                       eyeViews[e].fov.angleLeft * kDeg, eyeViews[e].fov.angleRight * kDeg,
                       eyeViews[e].fov.angleUp * kDeg, eyeViews[e].fov.angleDown * kDeg);
            eyesLogged_ = true;
        }
    }

    // Mode plat (quad) si l'image plate est la plus récente, sinon stéréo (projection)
    uint64_t seqs[render::kCategories]{};
    render::LatestSeqs(seqs);
    const bool stereo = std::max(seqs[1], seqs[2]) > seqs[0];
    FinishCopies(false); // copies de la frame précédente : textures rendues au jeu si finies
    // Stéréo : les deux yeux viennent toujours de la même paire (même pose de tête, images
    // consécutives du jeu), la plus récente dont les DEUX images sont finies. Prendre la
    // dernière image de chaque œil séparément mélangeait des paires : l'œil droit montrait
    // un autre instant que le gauche et tremblait.
    copyPair_ = UINT64_MAX;
    if (stereo) {
        copyPair_ = render::LatestCompletePair();
        if (copyPair_ > shownPair_) {
            ++newPairs_;
            if (shownPair_ > 0)
                skippedPairs_ += copyPair_ - shownPair_ - 1;
            shownPair_ = copyPair_;
        } else {
            ++repeatFrames_;
        }
    } else {
        shownPair_ = 0; // pas de paires sautées comptées au retour en stéréo
    }
    if (fs.shouldRender) {
        if (stereo) {
            for (int e = 0; e < 2; ++e)
                framesCopied_ += CopyGameFrame(e, e);
            CopyGameFrame(render::kEyeUi, kUiChain);
        } else {
            framesCopied_ += CopyGameFrame(render::kEyeFlat, 0);
        }
    }
    SubmitCopies();

    UpdatePacing(fs.predictedDisplayPeriod, stereo);

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2]{};
    XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad cursor{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection handsLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView handViews[2]{};
    XrCompositionLayerQuad keyboardLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad radialLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[6]{};
    uint32_t layerCount = 0;
    const XrRect2Di fullRect{{0, 0}, {static_cast<int32_t>(scWidth_), static_cast<int32_t>(scHeight_)}};

    const bool eyesReady = chains_[0].hasContent && chains_[1].hasContent && chains_[0].view.eye == 0 &&
                           chains_[1].view.eye == 1 && chains_[0].view.valid && chains_[1].view.valid;
    if (fs.shouldRender && stereo && eyesReady) {
        // Chaque œil : sa dernière image, avec la pose et le champ de vue de son rendu (le
        // runtime corrige l'écart avec la pose réelle au moment de l'affichage)
        // Diagnostic : l'image (et la vue) de l'œil gauche pour les deux yeux
        const bool leftOnly = config::GetBool(config::Id::DebugLeftImageBothEyes);
        for (int e = 0; e < 2; ++e) {
            const int src = leftOnly ? 0 : e;
            const render::RenderView& v = chains_[src].view;
            views[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[e].pose.orientation = {v.qx, v.qy, v.qz, v.qw};
            views[e].pose.position = {v.px, v.py, v.pz};
            views[e].fov = {v.angleLeft, v.angleRight, v.angleUp, v.angleDown};
            views[e].subImage = {chains_[src].swapchain, fullRect, 0};
        }
        static bool loggedLeftOnly = false;
        if (leftOnly != loggedLeftOnly) {
            LOG_XR("diagnostic : image gauche dans les deux yeux %s", leftOnly ? "ACTIVE" : "desactive");
            loggedLeftOnly = leftOnly;
        }
        proj.space = localSpace_;
        proj.viewCount = 2;
        proj.views = views;
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);

        // Interface par-dessus, sur un écran lié à la tête. Elle a été dessinée sur un fond
        // transparent noir : couleurs prémultipliées par l'alpha.
        const EyeChain& ui = chains_[kUiChain];
        if (ui.hasContent && ui.view.eye == render::kEyeUi) {
            hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            hud.space = viewSpace_;
            hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hud.subImage = {ui.swapchain, fullRect, 0};
            hud.pose.orientation.w = 1.0f;
            hud.pose.position = {0.0f, 0.0f, -HudDistance()};
            hud.size = {HudWidth(), HudWidth() * scHeight_ / scWidth_};
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud);
            if (!hudLogged_) {
                LOG_XR("interface affichee dans un calque separe (%.1f m a %.1f m)", HudWidth(), HudDistance());
                hudLogged_ = true;
            }
        }
        if (!stereoLogged_) {
            LOG_XR("premiere image stereo soumise (rendu alterne)");
            stereoLogged_ = true;
        }
    }

    // Écran virtuel (menus) : l'image plate, pilotée par le pointeur des manettes
    const bool screenActive =
        fs.shouldRender && !stereo && chains_[0].hasContent && chains_[0].view.eye == render::kEyeFlat;
    XrControls::Screen screen{ScreenDistance(), ScreenWidth(),
                              scWidth_ ? ScreenWidth() * scHeight_ / scWidth_ : 0.0f};
    // Clavier virtuel sous l'écran quand un champ de texte du jeu a le focus
    screen.keyboard = screenActive && engine::TextInputActive();
    if (screen.keyboard) {
        screen.keyboardWidth = screen.width * 0.8f;
        screen.keyboardHeight = VirtualKeyboard::HeightFor(screen.keyboardWidth);
        screen.keyboardY = -screen.height / 2 - screen.keyboardHeight / 2 - 0.03f;
    }
    if (screen.keyboard != keyboardLogged_) {
        LOG_XR("champ de texte %s : clavier virtuel %s", screen.keyboard ? "actif" : "inactif",
               screen.keyboard ? "affiche" : "masque");
        keyboardLogged_ = screen.keyboard;
    }
    XrVector3f hit{};
    const bool pointing = controls_.Update(session_, localSpace_, fs.predictedDisplayTime,
                                           eyesValid ? &loc.pose : nullptr, state_ == XR_SESSION_STATE_FOCUSED,
                                           stereo && eyesReady, screenActive, screen, hit);
    // Menu radial des ordres (combat), devant les yeux, par-dessus l'interface
    if (fs.shouldRender && stereo && eyesReady && controls_.GetRadial().open &&
        radial_.Layer(viewSpace_, controls_.GetRadial(), radialLayer))
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&radialLayer);
    else if (fs.shouldRender && stereo && eyesReady && controls_.Hint() &&
             radial_.HintLayer(viewSpace_, controls_.Hint(), radialLayer))
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&radialLayer);
    if (controls_.TakeRecenterRequest())
        Recenter(fs.predictedDisplayTime);
    float kbU = 0, kbV = 0;
    const bool onKeyboard = screen.keyboard && controls_.KeyboardHover(kbU, kbV);
    if (screen.keyboard)
        keyboard_.Update(onKeyboard, kbU, kbV, onKeyboard && controls_.KeyboardClick());
    if (screenActive) {
        quad.space = localSpace_;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage = {chains_[0].swapchain, fullRect, 0};
        quad.pose.orientation.w = 1.0f;
        quad.pose.position = {0.0f, 0.0f, -ScreenDistance()};
        quad.size = {screen.width, screen.height};
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        if (screen.keyboard &&
            keyboard_.Layer(localSpace_, {0.0f, screen.keyboardY, -screen.distance}, screen.keyboardWidth, keyboardLayer))
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&keyboardLayer);
        // Manettes 3D et rayon, puis le point du curseur par-dessus
        if (eyesValid && hands_.Render(eyeViews, controls_, localSpace_, handsLayer, handViews))
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&handsLayer);
        if ((pointing || onKeyboard) && EnsureCursor()) {
            cursor.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            cursor.space = localSpace_;
            cursor.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cursor.subImage = {cursorSwapchain_, {{0, 0}, {kCursorPixels, kCursorPixels}}, 0};
            cursor.pose.orientation.w = 1.0f;
            cursor.pose.position = {hit.x, hit.y, hit.z + 0.005f};
            cursor.size = {kCursorSize, kCursorSize};
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cursor);
        }
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    {
        Phase p("xrEndFrame");
        if (!Check(xrEndFrame(session_, &ei), "xrEndFrame"))
            return;
    }
    // Hors affichage (SYNCHRONIZED), certains runtimes ne cadencent pas xrWaitFrame :
    // on évite de tourner à vide
    if (!fs.shouldRender)
        Sleep(5);

    ++framesSubmitted_;
    const ULONGLONG now = GetTickCount64();
    if (framesSubmitted_ == 1) {
        LOG_XR("premiere frame soumise au casque");
        lastStats_ = now;
    } else if (now - lastStats_ >= kStatsIntervalMs) {
        LOG_XR("frames casque : %llu soumises, %llu images du jeu copiees ; paires stereo : %llu nouvelles, "
               "%llu sautees, %llu frames sans nouvelle paire",
               framesSubmitted_, framesCopied_, newPairs_, skippedPairs_, repeatFrames_);
        newPairs_ = skippedPairs_ = repeatFrames_ = 0;
        lastStats_ = now;
    }
}

void XrApp::Shutdown()
{
    DestroySwapchains();
    if (cursorSwapchain_ != XR_NULL_HANDLE)
        xrDestroySwapchain(cursorSwapchain_);
    cursorSwapchain_ = XR_NULL_HANDLE;
    cursorReady_ = false;
    hands_.Shutdown();
    keyboard_.Shutdown();
    radial_.Shutdown();
    controls_.Shutdown();
    for (auto*& t : shared_)
        if (t) {
            t->Release();
            t = nullptr;
        }
    PublishHead(nullptr, nullptr);
    if (viewSpace_ != XR_NULL_HANDLE)
        xrDestroySpace(viewSpace_);
    viewSpace_ = XR_NULL_HANDLE;
    if (localSpace_ != XR_NULL_HANDLE)
        xrDestroySpace(localSpace_);
    if (baseSpace_ != XR_NULL_HANDLE)
        xrDestroySpace(baseSpace_);
    baseSpace_ = XR_NULL_HANDLE;
    if (session_ != XR_NULL_HANDLE)
        xrDestroySession(session_);
    if (instance_ != XR_NULL_HANDLE)
        xrDestroyInstance(instance_);
    localSpace_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
    if (copyDone_)
        copyDone_->Release();
    if (context_)
        context_->Release();
    if (device_)
        device_->Release();
    copyDone_ = nullptr;
    context_ = nullptr;
    device_ = nullptr;
}

void XrApp::Run()
{
    // Runtime pas encore prêt (casque sans fil pas encore connecté, SteamVR qui démarre...) :
    // on réessaie au lieu d'abandonner la VR pour toute la partie
    bool warned = false;
    while (!g_stop && !CreateInstance()) {
        if (!warned) {
            LOG_XR("runtime OpenXR pas pret (casque connecte ?), nouvel essai toutes les 3 s");
            warned = true;
        }
        Sleep(3000);
    }
    if (g_stop || !WaitForSystem() || !CreateDevice() || !CreateSession()) {
        LOG_XR("OpenXR indisponible : le jeu continue sans VR");
        return;
    }
    controls_.Init(instance_, session_);
    // Manettes : géométrie simple, une résolution modérée suffit
    const uint32_t handSize =
        std::clamp<uint32_t>(recommendedWidth_ ? recommendedWidth_ * 6 / 10 : 1024, 512, 1536);
    hands_.Init(session_, device_, context_, format_, handSize);
    keyboard_.Init(session_, device_, context_, format_);
    radial_.Init(session_, device_, context_, format_);
    LOG_XR("en attente de l'etat READY (mettre le casque)");
    ULONGLONG lastConfigCheck = 0;
    while (!g_stop && !exitRequested_) {
        {
            Phase p("xrPollEvent");
            PollEvents();
        }
        // Réglages modifiés (outil de configuration ou à la main) : relus à chaud ; sortie
        // audio du casque suivie
        if (GetTickCount64() - lastConfigCheck > 1000) {
            lastConfigCheck = GetTickCount64();
            UpdateAudio();
            if (config::ReloadIfChanged()) {
                ApplyConfigToState(false);
                LOG_XR("reglages recharges (warband_vr.ini)");
            }
        }
        if (running_)
            RenderFrame();
        else
            Sleep(50);
    }
    if (running_)
        xrEndSession(session_);
    LOG_XR("fin de la session OpenXR");
}

} // namespace

void Start()
{
    ApplyConfigToState(true);
    std::thread(Watchdog).detach();
    std::thread([] {
        XrApp app;
        app.Run();
    }).detach();
}

namespace {
std::mutex g_handsMutex;
HandPose g_hands[2];
float g_turn = 0;
std::atomic<bool> g_swingBusy{false};
std::atomic<bool> g_bowMode{false};
std::atomic<bool> g_triggerAttackMode{false};
std::atomic<bool> g_firstPersonRequest{false};
std::atomic<bool> g_drawing{false};
std::atomic<bool> g_twoHandGrip{false};
std::atomic<ULONGLONG> g_drawReleasedMs{0};
constexpr ULONGLONG kBowAimAfterShotMs = 1000;
std::atomic<int> g_aimSource{0};
std::atomic<float> g_bladeSpeed{0};
std::atomic<bool> g_alwaysActive{true}; // réglages : ApplyConfigToState
} // namespace

void AddTurn(float radians)
{
    std::lock_guard lock(g_handsMutex);
    g_turn += radians;
}

float TakeTurn()
{
    std::lock_guard lock(g_handsMutex);
    const float t = g_turn;
    g_turn = 0;
    return t;
}

void SetSwingBusy(bool busy)
{
    g_swingBusy = busy;
}

bool IsSwingBusy()
{
    return g_swingBusy;
}

void SetBowMode(bool bow)
{
    g_bowMode = bow;
}

void RequestFirstPersonView()
{
    g_firstPersonRequest = true;
}

bool TakeFirstPersonViewRequest()
{
    return g_firstPersonRequest.exchange(false);
}

void SetTriggerAttackMode(bool on)
{
    g_triggerAttackMode = on;
}

bool IsTriggerAttackMode()
{
    return g_triggerAttackMode;
}

bool IsBowMode()
{
    return g_bowMode;
}

void SetDrawing(bool drawing)
{
    if (g_drawing && !drawing)
        g_drawReleasedMs = GetTickCount64();
    g_drawing = drawing;
}

bool IsDrawing()
{
    return g_drawing;
}

void SetTwoHandGrip(bool held)
{
    g_twoHandGrip = held;
}

bool IsTwoHandGrip()
{
    return g_twoHandGrip;
}

bool IsBowAiming()
{
    return g_drawing || GetTickCount64() - g_drawReleasedMs < kBowAimAfterShotMs;
}

void ApplyConfigToState(bool startup)
{
    g_alwaysActive = config::GetBool(config::Id::AlwaysActive);
    if (startup)
        g_aimSource = config::GetInt(config::Id::AimSource);
}

AimSource CycleAimSource()
{
    const int next = (g_aimSource + 1) % 3;
    g_aimSource = next;
    return static_cast<AimSource>(next);
}

bool ToggleAlwaysActive()
{
    const bool on = !g_alwaysActive;
    g_alwaysActive = on;
    return on;
}

bool IsAlwaysActive()
{
    return g_alwaysActive;
}

void SetBladeSpeed(float speed)
{
    g_bladeSpeed = speed;
}

float GetBladeSpeed()
{
    return g_bladeSpeed;
}

AimSource GetAimSource()
{
    return static_cast<AimSource>(g_aimSource.load());
}

void SetAimSource(AimSource source)
{
    g_aimSource = static_cast<int>(source);
}

void PublishHands(const HandPose hands[2])
{
    std::lock_guard lock(g_handsMutex);
    g_hands[0] = hands[0];
    g_hands[1] = hands[1];
}

void GetHands(HandPose out[2])
{
    std::lock_guard lock(g_handsMutex);
    out[0] = g_hands[0];
    out[1] = g_hands[1];
}

bool GetHeadPose(HeadPose& out)
{
    std::lock_guard lock(g_headMutex);
    if (g_headValid)
        out = g_head;
    return g_headValid;
}

void RequestStop()
{
    g_stop = true;
}

} // namespace xr
