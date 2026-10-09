#include "render/frame_profile.h"
#include "render/ui_redirect.h"
#include "util/config.h"
#include "util/log.h"

#include <d3d9.h>

#include <algorithm>
#include <mutex>

#define LOG_PERF(...) ::vrlog::Write("[perf]", __VA_ARGS__)

namespace render::profile {
namespace {

constexpr double kReportMs = 10000;

// Moyenne et maximum sur la période du résumé
struct Stat {
    double sum = 0, max = 0;
    unsigned n = 0;
    void Add(double v)
    {
        sum += v;
        max = std::max(max, v);
        ++n;
    }
    double Avg() const { return n ? sum / n : 0; }
};

// --- Thread du jeu ---
std::mutex g_gameMutex;
Stat g_gamePeriod, g_sceneFill;
double g_lastFill = 0;

// --- Thread de rendu ---
Stat g_renderPeriod, g_sceneCpu, g_uiCpu, g_otherCpu, g_capture, g_present, g_rest;
double g_frameStart = 0, g_presentStart = 0, g_reportStart = 0;
double g_ctxStart = 0;
double g_frameCtx[3]{}; // temps des contextes de l'image en cours, par sorte
double g_frameCapture = 0;
unsigned g_frames = 0;

// --- GPU : requêtes par image, lues quelques images plus tard ---
constexpr int kRing = 6;
struct GpuFrame {
    IDirect3DQuery9 *disjoint = nullptr, *freq = nullptr, *start = nullptr, *sceneBegin = nullptr,
                    *sceneEnd = nullptr, *end = nullptr;
    bool issued = false, scene = false;
};
std::mutex g_gpuMutex; // ReleaseForReset peut venir d'un autre chemin que Present
GpuFrame g_gpu[kRing];
int g_gpuCur = -1;
IDirect3DDevice9* g_gpuDevice = nullptr;
bool g_gpuFailed = false;
Stat g_gpuScene, g_gpuFrame;
unsigned g_gpuLost = 0; // résultats pas prêts à temps ou image discontinue

bool On()
{
    return config::GetBool(config::Id::DebugFrameProfile);
}

void ReleaseQueries()
{
    for (GpuFrame& f : g_gpu) {
        for (IDirect3DQuery9** q : {&f.disjoint, &f.freq, &f.start, &f.sceneBegin, &f.sceneEnd, &f.end})
            if (*q) {
                (*q)->Release();
                *q = nullptr;
            }
        f.issued = f.scene = false;
    }
    g_gpuCur = -1;
    g_gpuDevice = nullptr;
}

bool CreateQueries(IDirect3DDevice9* device)
{
    for (GpuFrame& f : g_gpu) {
        const bool ok = SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &f.disjoint)) &&
                        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &f.freq)) &&
                        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.start)) &&
                        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.sceneBegin)) &&
                        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.sceneEnd)) &&
                        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &f.end));
        if (!ok) {
            ReleaseQueries();
            return false;
        }
    }
    g_gpuDevice = device;
    return true;
}

template <typename T>
bool Read(IDirect3DQuery9* q, T& out)
{
    return q->GetData(&out, sizeof(out), 0) == S_OK;
}

// Résultats d'une image terminée (pas d'attente : sinon comptée perdue)
void Collect(GpuFrame& f)
{
    if (!f.issued)
        return;
    f.issued = false;
    BOOL disjoint = TRUE;
    UINT64 freq = 0, start = 0, end = 0, sb = 0, se = 0;
    if (!Read(f.disjoint, disjoint) || !Read(f.freq, freq) || !Read(f.start, start) || !Read(f.end, end) ||
        disjoint || !freq || (f.scene && (!Read(f.sceneBegin, sb) || !Read(f.sceneEnd, se)))) {
        ++g_gpuLost;
        return;
    }
    const double toMs = 1000.0 / static_cast<double>(freq);
    g_gpuFrame.Add(static_cast<double>(end - start) * toMs);
    if (f.scene)
        g_gpuScene.Add(static_cast<double>(se - sb) * toMs);
}

void Timestamp(IDirect3DQuery9* q)
{
    if (q)
        q->Issue(D3DISSUE_END);
}

void Report(double now)
{
    Stat gamePeriod, fill;
    {
        std::lock_guard lock(g_gameMutex);
        gamePeriod = g_gamePeriod;
        fill = g_sceneFill;
        g_gamePeriod = g_sceneFill = Stat{};
    }
    const double seconds = (now - g_reportStart) / 1000.0;
    LOG_PERF("%.0f s, %u images (%.1f/s) | jeu : periode %.1f ms (max %.1f), scene remplie en %.1f (max %.1f)",
             seconds, g_frames, g_frames / seconds, gamePeriod.Avg(), gamePeriod.max, fill.Avg(), fill.max);
    LOG_PERF("  rendu (CPU) : periode %.1f ms | scene %.1f (max %.1f), interface %.1f, autres %.1f, capture VR %.1f, "
             "Present %.1f (max %.1f), reste %.1f",
             g_renderPeriod.Avg(), g_sceneCpu.Avg(), g_sceneCpu.max, g_uiCpu.Avg(), g_otherCpu.Avg(), g_capture.Avg(),
             g_present.Avg(), g_present.max, g_rest.Avg());
    {
        std::lock_guard lock(g_gpuMutex);
        if (g_gpuFailed)
            LOG_PERF("  GPU : requetes timestamp indisponibles");
        else
            LOG_PERF("  GPU : scene %.1f ms (max %.1f), image entiere %.1f ms (max %.1f), %u images mesurees, %u perdues",
                     g_gpuScene.Avg(), g_gpuScene.max, g_gpuFrame.Avg(), g_gpuFrame.max, g_gpuFrame.n, g_gpuLost);
        g_gpuScene = g_gpuFrame = Stat{};
        g_gpuLost = 0;
    }
    g_renderPeriod = g_sceneCpu = g_uiCpu = g_otherCpu = g_capture = g_present = g_rest = Stat{};
    g_frames = 0;
    g_reportStart = now;
}

} // namespace

double NowMs()
{
    static const double toMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * toMs;
}

void SceneFill(double ms)
{
    if (!On())
        return;
    const double now = NowMs();
    std::lock_guard lock(g_gameMutex);
    g_sceneFill.Add(ms);
    if (g_lastFill > 0 && now - g_lastFill < 1000)
        g_gamePeriod.Add(now - g_lastFill);
    g_lastFill = now;
}

void BeginContext(Ctx kind)
{
    if (!On())
        return;
    g_ctxStart = NowMs();
    if (kind == Ctx::Scene) {
        std::lock_guard lock(g_gpuMutex);
        if (g_gpuCur >= 0 && !g_gpu[g_gpuCur].scene)
            Timestamp(g_gpu[g_gpuCur].sceneBegin);
    }
}

void EndContext(Ctx kind)
{
    if (!On() || g_ctxStart == 0)
        return;
    g_frameCtx[static_cast<int>(kind)] += NowMs() - g_ctxStart;
    g_ctxStart = 0;
    if (kind == Ctx::Scene) {
        std::lock_guard lock(g_gpuMutex);
        if (g_gpuCur >= 0 && !g_gpu[g_gpuCur].scene) {
            Timestamp(g_gpu[g_gpuCur].sceneEnd);
            g_gpu[g_gpuCur].scene = true; // première scène principale de l'image
        }
    }
}

void Capture(double ms)
{
    if (On())
        g_frameCapture += ms;
}

void BeforePresent(IDirect3DDevice9* device)
{
    if (!On())
        return;
    g_presentStart = NowMs();
    std::lock_guard lock(g_gpuMutex);
    if (g_gpuCur >= 0 && g_gpu[g_gpuCur].start && device == g_gpuDevice) {
        GpuFrame& f = g_gpu[g_gpuCur];
        Timestamp(f.end);
        f.freq->Issue(D3DISSUE_END);
        f.disjoint->Issue(D3DISSUE_END);
        f.issued = true;
    }
    if (device && device != g_gpuDevice && !g_gpuFailed) {
        ReleaseQueries();
        if (!CreateQueries(device)) {
            g_gpuFailed = true;
            LOG_PERF("requetes timestamp D3D9 indisponibles : pas de mesure GPU");
        }
    }
}

void AfterPresent()
{
    if (!On()) {
        g_frameStart = g_reportStart = 0;
        g_frameCtx[0] = g_frameCtx[1] = g_frameCtx[2] = g_frameCapture = 0;
        return;
    }
    const double now = NowMs();
    if (g_reportStart == 0) {
        g_reportStart = now;
        LOG_PERF("mesure des temps par image active (resume toutes les %.0f s)", kReportMs / 1000);
    }
    if (g_frameStart > 0 && g_presentStart > 0) {
        const double period = now - g_frameStart;
        const double present = now - g_presentStart;
        const double busy = g_frameCtx[0] + g_frameCtx[1] + g_frameCtx[2] + g_frameCapture;
        g_renderPeriod.Add(period);
        g_sceneCpu.Add(g_frameCtx[0]);
        g_uiCpu.Add(g_frameCtx[1]);
        g_otherCpu.Add(g_frameCtx[2]);
        g_capture.Add(g_frameCapture);
        g_present.Add(present);
        g_rest.Add(std::max(0.0, period - busy - present));
        ++g_frames;
    }
    g_frameCtx[0] = g_frameCtx[1] = g_frameCtx[2] = g_frameCapture = 0;
    g_frameStart = now;
    g_presentStart = 0;

    {
        // Image suivante : ses requêtes GPU (l'emplacement, vieux de kRing images, est relu avant)
        std::lock_guard lock(g_gpuMutex);
        if (g_gpuDevice) {
            g_gpuCur = (g_gpuCur + 1) % kRing;
            GpuFrame& f = g_gpu[g_gpuCur];
            Collect(f);
            f.scene = false;
            f.disjoint->Issue(D3DISSUE_BEGIN);
            Timestamp(f.start);
        }
    }
    if (now - g_reportStart >= kReportMs)
        Report(now);
}

void ReleaseForReset()
{
    std::lock_guard lock(g_gpuMutex);
    ReleaseQueries();
}

} // namespace render::profile
