#include "xr/xr_radial.h"

#include <windows.h>

#include <cmath>

namespace xr {
namespace {

constexpr int kTexSize = 768;
constexpr float kWidth = 0.55f;    // diamètre affiché (m)
constexpr float kDistance = 1.0f;  // devant les yeux (m)
constexpr float kBelow = 0.05f;    // un peu sous l'axe du regard (m)
constexpr float kOpacity = 0.85f;
constexpr float kTwoPi = 6.2831853f;
// Bandeau de consigne : texture, largeur affichée et position sous le regard (m)
constexpr int kHintW = 1280, kHintH = 96;
constexpr float kHintWidth = 0.85f, kHintBelow = 0.22f;
constexpr int kHintMargin = 40; // pixels de marge à gauche et à droite du texte

constexpr COLORREF kDisc = RGB(28, 30, 36);
constexpr COLORREF kHighlight = RGB(70, 110, 170);
constexpr COLORREF kSeparator = RGB(90, 94, 104);
constexpr COLORREF kText = RGB(240, 240, 240);
constexpr COLORREF kTitle = RGB(170, 175, 185);

// Point de l'écran à l'angle a (depuis le haut, sens horaire) et au rayon r
POINT At(float cx, float cy, float a, float r)
{
    return {static_cast<LONG>(cx + r * std::sin(a)), static_cast<LONG>(cy - r * std::cos(a))};
}

} // namespace

void RadialMenu::Init(XrSession session, ID3D11Device* device, ID3D11DeviceContext* context, int64_t format)
{
    layer_.Init(session, device, context, format, kTexSize, kTexSize, "menu radial");
    hint_.Init(session, device, context, format, kHintW, kHintH, "consigne");
}

void RadialMenu::Shutdown()
{
    layer_.Shutdown();
    hint_.Shutdown();
    hintText_.clear();
    drawn_ = false;
}

bool RadialMenu::Redraw(const XrControls::Radial& radial)
{
    HDC dc = layer_.BeginDraw();
    if (!dc)
        return false;
    const float c = kTexSize / 2.0f;
    const float radius = kTexSize * 0.48f;
    const float inner = kTexSize * 0.13f;
    const int n = radial.count > 0 ? radial.count : 1;
    const float step = kTwoPi / n;

    RECT all{0, 0, kTexSize, kTexSize};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SelectObject(dc, GetStockObject(NULL_PEN));

    HBRUSH disc = CreateSolidBrush(kDisc);
    HGDIOBJ oldBrush = SelectObject(dc, disc);
    const int l = static_cast<int>(c - radius), r = static_cast<int>(c + radius);
    Ellipse(dc, l, l, r, r);
    // Part visée : GDI trace les parts dans le sens antihoraire, du premier rayon au second
    if (radial.highlight >= 0 && radial.highlight < radial.count) {
        HBRUSH hl = CreateSolidBrush(kHighlight);
        SelectObject(dc, hl);
        const float mid = radial.highlight * step;
        const POINT from = At(c, c, mid + step / 2, radius), to = At(c, c, mid - step / 2, radius);
        if (n == 1)
            Ellipse(dc, l, l, r, r);
        else
            Pie(dc, l, l, r, r, from.x, from.y, to.x, to.y);
        SelectObject(dc, disc);
        DeleteObject(hl);
    }
    // Séparations et centre
    HPEN sep = CreatePen(PS_SOLID, 3, kSeparator);
    HGDIOBJ oldPen = SelectObject(dc, sep);
    if (n > 1)
        for (int i = 0; i < n; ++i) {
            const POINT a = At(c, c, i * step + step / 2, inner), b = At(c, c, i * step + step / 2, radius);
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, b.x, b.y);
        }
    Ellipse(dc, static_cast<int>(c - inner), static_cast<int>(c - inner), static_cast<int>(c + inner),
            static_cast<int>(c + inner));
    SelectObject(dc, oldPen);
    DeleteObject(sep);
    SelectObject(dc, oldBrush);
    DeleteObject(disc);

    // Libellés au milieu de chaque part
    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(kTexSize / 22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             L"Segoe UI Symbol");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, kText);
    const float labelRadius = (radius + inner) / 2 + kTexSize * 0.02f;
    const int boxW = static_cast<int>(kTexSize * (n > 6 ? 0.22f : 0.28f)), boxH = kTexSize / 7;
    for (int i = 0; i < radial.count; ++i) {
        const POINT p = At(c, c, i * step, n == 1 ? 0.0f : labelRadius);
        RECT box{p.x - boxW / 2, p.y - boxH / 2, p.x + boxW / 2, p.y + boxH / 2};
        // Centrage vertical d'un texte sur plusieurs lignes : mesure, puis décalage
        RECT measure = box;
        DrawTextW(dc, radial.labels[i].c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        const int h = measure.bottom - measure.top;
        box.top = p.y - h / 2;
        box.bottom = box.top + h;
        DrawTextW(dc, radial.labels[i].c_str(), -1, &box, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    }
    // Centre : ce que propose le menu
    SetTextColor(dc, kTitle);
    RECT center{static_cast<LONG>(c - inner), static_cast<LONG>(c - inner), static_cast<LONG>(c + inner),
                static_cast<LONG>(c + inner)};
    // Centre : attente du jeu, sinon retour possible (clic du stick) ou choix des troupes
    const wchar_t* mark = radial.waiting ? L"…" : radial.depth > 0 ? L"↶" : L"⚑";
    DrawTextW(dc, mark, -1, &center, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    GdiFlush();

    // Alpha prémultiplié : disque presque opaque, transparent autour
    uint32_t* px = layer_.Pixels();
    const float edge = radius + 1.0f;
    for (int y = 0; y < kTexSize; ++y)
        for (int x = 0; x < kTexSize; ++x) {
            const float dx = x + 0.5f - c, dy = y + 0.5f - c;
            uint32_t& v = px[y * kTexSize + x];
            if (dx * dx + dy * dy > edge * edge) {
                v = 0;
                continue;
            }
            const auto scale = [](uint32_t ch) { return static_cast<uint32_t>(ch * kOpacity + 0.5f); };
            v = (static_cast<uint32_t>(255 * kOpacity + 0.5f) << 24) | (scale((v >> 16) & 0xFF) << 16) |
                (scale((v >> 8) & 0xFF) << 8) | scale(v & 0xFF);
        }
    return layer_.Commit();
}

bool RadialMenu::Layer(XrSpace space, const XrControls::Radial& radial, XrCompositionLayerQuad& layer)
{
    std::wstring key = std::to_wstring(radial.highlight) + L"/" + std::to_wstring(radial.depth) +
                       (radial.waiting ? L"w" : L"");
    for (int i = 0; i < radial.count; ++i)
        key += L"|" + radial.labels[i];
    if (!drawn_ || key != drawnKey_) {
        if (!Redraw(radial) && !layer_.HasImage())
            return false;
        drawn_ = true;
        drawnKey_ = key;
    }
    XrPosef pose{};
    pose.orientation.w = 1.0f;
    pose.position = {0.0f, -kBelow, -kDistance};
    layer_.Fill(space, pose, kWidth, layer);
    return true;
}

bool RadialMenu::HintLayer(XrSpace space, const wchar_t* text, XrCompositionLayerQuad& layer)
{
    if (text != hintText_ || !hint_.HasImage()) {
        HDC dc = hint_.BeginDraw();
        if (!dc)
            return false;
        RECT all{0, 0, kHintW, kHintH};
        HBRUSH bg = CreateSolidBrush(kDisc);
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        // Police réduite jusqu'à ce que le texte tienne dans le bandeau, marges comprises
        HFONT font = nullptr;
        HGDIOBJ oldFont = nullptr;
        for (int size = kHintH / 2; size >= kHintH / 5; size -= 2) {
            if (font) {
                SelectObject(dc, oldFont);
                DeleteObject(font);
            }
            font = CreateFontW(size, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               L"Segoe UI");
            oldFont = SelectObject(dc, font);
            RECT measure{0, 0, 0, 0};
            DrawTextW(dc, text, -1, &measure, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
            if (measure.right - measure.left <= kHintW - 2 * kHintMargin)
                break;
        }
        DrawTextW(dc, text, -1, &all, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        GdiFlush();
        // Alpha prémultiplié : bandeau presque opaque
        uint32_t* px = hint_.Pixels();
        for (int i = 0; i < kHintW * kHintH; ++i) {
            const uint32_t v = px[i];
            const auto scale = [](uint32_t ch) { return static_cast<uint32_t>(ch * kOpacity + 0.5f); };
            px[i] = (static_cast<uint32_t>(255 * kOpacity + 0.5f) << 24) | (scale((v >> 16) & 0xFF) << 16) |
                    (scale((v >> 8) & 0xFF) << 8) | scale(v & 0xFF);
        }
        if (!hint_.Commit() && !hint_.HasImage())
            return false;
        hintText_ = text;
    }
    XrPosef pose{};
    pose.orientation.w = 1.0f;
    pose.position = {0.0f, -kHintBelow, -kDistance};
    hint_.Fill(space, pose, kHintWidth, layer);
    return true;
}

} // namespace xr
