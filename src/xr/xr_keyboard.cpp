#include "xr/xr_keyboard.h"
#include "render/game_window.h"
#include "util/log.h"

#include <windows.h>

#include <cstring>

#define LOG_XR(...) ::vrlog::Write("[xr]", __VA_ARGS__)

namespace xr {
namespace {

// Dimensions en unités de touche, et texture
constexpr float kUnitsW = 15.6f;
constexpr float kUnitsH = 5.6f;
constexpr int kTexW = 1024;
constexpr int kTexH = static_cast<int>(kTexW * kUnitsH / kUnitsW); // 367
constexpr float kPx = kTexW / kUnitsW;                              // pixels par unité
constexpr float kGap = 0.08f;
constexpr ULONGLONG kFlashMs = 120;

// Couleurs GDI (0x00BBGGRR)
constexpr COLORREF kBackground = RGB(24, 24, 28);
constexpr COLORREF kKey = RGB(58, 60, 68);
constexpr COLORREF kKeySpecial = RGB(44, 46, 54);
constexpr COLORREF kHover = RGB(70, 110, 170);
constexpr COLORREF kFlash = RGB(120, 170, 235);
constexpr COLORREF kShiftOn = RGB(60, 130, 90);
constexpr COLORREF kText = RGB(235, 235, 235);

} // namespace

float VirtualKeyboard::HeightFor(float width)
{
    return width * kUnitsH / kUnitsW;
}

void VirtualKeyboard::Init(XrSession session, ID3D11Device* device, ID3D11DeviceContext* context, int64_t format)
{
    session_ = session;
    layer_.Init(session, device, context, format, kTexW, kTexH, "clavier virtuel");
}

void VirtualKeyboard::Shutdown()
{
    layer_.Shutdown();
    drawn_ = false;
}

// Touches d'un clavier 105 touches : libellés tirés de la disposition Windows du jeu
void VirtualKeyboard::BuildLayout()
{
    HWND wnd = render::GameWindow();
    HKL hkl = GetKeyboardLayout(wnd ? GetWindowThreadProcessId(wnd, nullptr) : 0);
    if (!keys_.empty() && hkl == layoutId_)
        return;
    layoutId_ = hkl;
    keys_.clear();

    auto charOf = [&](unsigned sc, bool shift) -> wchar_t {
        const UINT vk = MapVirtualKeyExW(sc, MAPVK_VSC_TO_VK_EX, hkl);
        if (!vk)
            return 0;
        BYTE state[256]{};
        if (shift)
            state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
        wchar_t buf[8]{};
        // 0x4 : ne pas modifier l'état clavier du noyau (touches mortes)
        const int n = ToUnicodeEx(vk, sc, state, buf, 8, 0x4, hkl);
        if (n == 0)
            return 0;
        const wchar_t c = buf[0];
        return c >= 0x20 ? c : 0;
    };
    auto addChars = [&](float x, float y, std::initializer_list<unsigned> codes) {
        for (unsigned sc : codes) {
            const wchar_t n = charOf(sc, false), s = charOf(sc, true);
            if (n || s)
                keys_.push_back({x, y, 1.0f, 1.0f, Kind::Char, sc, n ? n : s, s ? s : n});
            x += 1.0f;
        }
        return x;
    };
    // Rangée 0 : ² 1 2 ... 0 ) = et Retour arrière
    float x = addChars(0.3f, 0.3f, {0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D});
    keys_.push_back({x + 0.1f, 0.3f, kUnitsW - 0.3f - (x + 0.1f), 1.0f, Kind::Backspace, 0x0E, 0, 0});
    // Rangée 1 : A Z E R T Y ...
    addChars(0.8f, 1.3f, {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B});
    // Rangée 2 : Q S D F ... et Entrée
    x = addChars(1.05f, 2.3f, {0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2B});
    keys_.push_back({x + 0.1f, 2.3f, kUnitsW - 0.3f - (x + 0.1f), 1.0f, Kind::Enter, 0x1C, 0, 0});
    // Rangée 3 : Maj, < W X C ...
    keys_.push_back({0.3f, 3.3f, 1.3f, 1.0f, Kind::Shift, 0x2A, 0, 0});
    addChars(1.7f, 3.3f, {0x56, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35});
    // Rangée 4 : espace
    keys_.push_back({3.8f, 4.3f, 8.0f, 1.0f, Kind::Space, 0x39, L' ', L' '});

    LOG_XR("clavier virtuel : %zu touches (disposition 0x%llX)", keys_.size(),
           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hkl)));
}

int VirtualKeyboard::KeyAt(float u, float v) const
{
    const float x = u * kUnitsW, y = v * kUnitsH;
    for (size_t i = 0; i < keys_.size(); ++i) {
        const Key& k = keys_[i];
        if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h)
            return static_cast<int>(i);
    }
    return -1;
}

void VirtualKeyboard::Press(const Key& k)
{
    HWND wnd = render::GameWindow();
    wchar_t c = 0;
    switch (k.kind) {
    case Kind::Char: c = shift_ ? k.shifted : k.normal; break;
    case Kind::Space: c = L' '; break;
    case Kind::Backspace: c = L'\b'; break;
    case Kind::Enter: c = L'\r'; break;
    case Kind::Shift: shift_ = !shift_; return;
    }
    if (k.kind == Kind::Char)
        shift_ = false; // majuscule pour un seul caractère, comme sur un téléphone
    if (wnd && c) {
        // Fenêtre ANSI : Windows convertit le caractère dans sa page de code
        PostMessageW(wnd, WM_CHAR, c, 1 | (static_cast<LPARAM>(k.scancode) << 16));
        if (typed_ < 3) {
            LOG_XR("clavier virtuel : caractere U+%04X envoye", static_cast<unsigned>(c));
            ++typed_;
        }
    }
}

void VirtualKeyboard::Update(bool hover, float u, float v, bool click)
{
    BuildLayout();
    hover_ = hover ? KeyAt(u, v) : -1;
    if (click && hover_ >= 0) {
        Press(keys_[hover_]);
        flash_ = hover_;
        flashUntil_ = GetTickCount64() + kFlashMs;
    }
    if (flash_ >= 0 && GetTickCount64() > flashUntil_)
        flash_ = -1;
}

// Dessine le clavier (GDI) et le copie dans une nouvelle image de la swapchain
bool VirtualKeyboard::Redraw()
{
    HDC dc = layer_.BeginDraw();
    if (!dc)
        return false;
    HFONT font = CreateFontW(static_cast<int>(kPx * 0.55f), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);

    RECT all{0, 0, kTexW, kTexH};
    HBRUSH bg = CreateSolidBrush(kBackground);
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    SelectObject(dc, GetStockObject(NULL_PEN));
    for (size_t i = 0; i < keys_.size(); ++i) {
        const Key& k = keys_[i];
        COLORREF color = k.kind == Kind::Char || k.kind == Kind::Space ? kKey : kKeySpecial;
        if (k.kind == Kind::Shift && shift_)
            color = kShiftOn;
        if (static_cast<int>(i) == hover_)
            color = kHover;
        if (static_cast<int>(i) == flash_)
            color = kFlash;
        RECT r{static_cast<LONG>((k.x + kGap / 2) * kPx), static_cast<LONG>((k.y + kGap / 2) * kPx),
               static_cast<LONG>((k.x + k.w - kGap / 2) * kPx), static_cast<LONG>((k.y + k.h - kGap / 2) * kPx)};
        HBRUSH b = CreateSolidBrush(color);
        HGDIOBJ oldBrush = SelectObject(dc, b);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, static_cast<int>(kPx * 0.2f), static_cast<int>(kPx * 0.2f));
        SelectObject(dc, oldBrush);
        DeleteObject(b);

        const wchar_t* label = L"";
        wchar_t one[2]{};
        switch (k.kind) {
        case Kind::Char:
            one[0] = shift_ ? k.shifted : k.normal;
            label = one;
            break;
        case Kind::Backspace: label = L"←"; break; // ←
        case Kind::Enter: label = L"↵"; break;     // ↵
        case Kind::Shift: label = L"⇧"; break;     // ⇧
        case Kind::Space: break;
        }
        DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    GdiFlush();

    // GDI n'écrit pas l'alpha : clavier opaque
    uint32_t* px = layer_.Pixels();
    for (int i = 0; i < kTexW * kTexH; ++i)
        px[i] |= 0xFF000000u;
    return layer_.Commit();
}

bool VirtualKeyboard::Layer(XrSpace space, const XrVector3f& center, float width, XrCompositionLayerQuad& layer)
{
    if (!session_)
        return false;
    BuildLayout();
    if (!drawn_ || hover_ != drawnHover_ || flash_ != drawnFlash_ || shift_ != drawnShift_) {
        if (!Redraw() && !layer_.HasImage())
            return false;
        drawn_ = true;
        drawnHover_ = hover_;
        drawnFlash_ = flash_;
        drawnShift_ = shift_;
    }
    XrPosef pose{};
    pose.orientation.w = 1.0f;
    pose.position = center;
    layer_.Fill(space, pose, width, layer);
    return true;
}

} // namespace xr
