#include "flyout.h"
#include "log.h"
#include "strings.h"

#include <shellscalingapi.h>
#include <cmath>
#include <d3d11.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>

#undef GetCurrentTime  // clashes with C++/WinRT
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

namespace wuc = winrt::Windows::UI::Composition;
namespace abi = ABI::Windows::UI::Composition;

static const float kWidth = 340.0f;
static const float kFooterHeight = 48.0f;  // same as the Quick Settings footer
static const UINT WM_OUTSIDE_CLICK = WM_APP + 100;
static const UINT WM_ANIM_FRAME = WM_APP + 101;
static const UINT_PTR kRegionTimer = 2;  // shrink the input region once the panel settled
static const UINT_PTR kTickTimer = 3;    // countdowns, once a second while open
static const float kShadowMargin = 32.0f;
static const float kCornerRadius = 8.0f;
static const wchar_t kTextFont[] = L"Segoe UI Variable Text";
static const wchar_t kDisplayFont[] = L"Segoe UI Variable Display";
static const wchar_t kIconFont[] = L"Segoe Fluent Icons";
static const wchar_t kUsageUrl[] = L"https://claude.ai/settings/usage";

enum : wchar_t { kGlyphRefresh = 0xE72C, kGlyphBack = 0xE72B, kGlyphOpen = 0xE8A7 };

// While the flyout is open, a low-level mouse hook closes it on a click
// anywhere else (losing activation alone misses some cases).
static Flyout* g_hookOwner = nullptr;
static HHOOK g_mouseHook = nullptr;

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_hookOwner &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN)) {
        auto info = (MSLLHOOKSTRUCT*)lp;
        RECT rc = g_hookOwner->PanelRect();
        if (!PtInRect(&rc, info->pt)) PostMessageW(g_hookOwner->Hwnd(), WM_OUTSIDE_CLICK, 0, 0);
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

static D2D1_COLOR_F Rgba(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// Peak hours: Monday to Friday, 13:00-19:00 UTC. Returns whether it's peak now
// and when that changes (UTC ticks).
static bool PeakNow(ULONGLONG* change) {
    SYSTEMTIME u;
    GetSystemTime(&u);
    ULONGLONG now = Usage::NowTicks();
    ULONGLONG midnight = now - ((u.wHour * 3600ULL + u.wMinute * 60ULL + u.wSecond) * Usage::kSecond) -
                         u.wMilliseconds * 10000ULL;
    int dow = u.wDayOfWeek;
    if (dow >= 1 && dow <= 5 && u.wHour >= 13 && u.wHour < 19) {
        *change = midnight + 19 * Usage::kHour;
        return true;
    }
    *change = 0;
    for (int d = 0; d < 8; d++) {
        int w = (dow + d) % 7;
        if (w < 1 || w > 5) continue;
        ULONGLONG s = midnight + d * Usage::kDay + 13 * Usage::kHour;
        if (s > now) {
            *change = s;
            break;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------

bool Flyout::Create(HINSTANCE inst, HWND owner, const UsageState* usage, Settings* settings) {
    owner_ = owner;
    usage_ = usage;
    settings_ = settings;

    // Multi-threaded: the compositor uses the Direct2D device from its own thread.
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, d2d_.GetAddressOf());
    if (FAILED(hr)) {
        LOG(L"flyout: D2D1CreateFactory failed %08X", hr);
        return false;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dwrite_.GetAddressOf());
    if (FAILED(hr)) {
        LOG(L"flyout: DWriteCreateFactory failed %08X", hr);
        return false;
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"UsageOne.Flyout";
    RegisterClassExW(&wc);

    // No redirection bitmap: everything shown is the composition visual tree.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, APP_NAME,
                            WS_POPUP, 0, 0, 10, 10, owner, nullptr, inst, this);
    if (!hwnd_) {
        LOG(L"flyout: CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }

    LoadPalette();
    if (!CreateComposition()) {
        LOG(L"flyout: composition setup failed");
        return false;
    }
    ApplyWindowStyle();
    return true;
}

// ---------------------------------------------------------------------------
// Composition. The window covers the tallest the panel can be and never
// changes size while open; the panel is a visual tree: a drop shadow, then a
// container clipped to a rounded rectangle with the blurred desktop (acrylic)
// and the content Direct2D draws into a composition surface.

struct Flyout::Composition {
    winrt::Windows::System::DispatcherQueueController queue{nullptr};
    wuc::Compositor compositor{nullptr};
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr};
    wuc::SpriteVisual shadow{nullptr};
    wuc::DropShadow dropShadow{nullptr};
    wuc::ContainerVisual panel{nullptr};
    wuc::CompositionRoundedRectangleGeometry clipShape{nullptr};
    wuc::SpriteVisual backdrop{nullptr};
    wuc::SpriteVisual content{nullptr};
    wuc::CompositionGraphicsDevice graphics{nullptr};
    wuc::CompositionDrawingSurface surface{nullptr};
    wuc::CompositionDrawingSurface mask{nullptr};
    wuc::CompositionNineGridBrush maskBrush{nullptr};
    ComPtr<ID2D1Device> device;
    ComPtr<ID3D11DeviceContext> d3dContext;
    int surfaceW = 0, surfaceH = 0;
    bool surfaceFresh = true;
    int maskPx = 0;
};

bool Flyout::CreateComposition() {
    try {
        auto c = new Composition();
        DispatcherQueueOptions options = {sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        winrt::check_hresult(CreateDispatcherQueueController(
            options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(c->queue))));
        c->compositor = wuc::Compositor();
        auto desktop = c->compositor.as<abi::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(desktop->CreateDesktopWindowTarget(
            hwnd_, FALSE, reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(c->target))));

        auto& k = c->compositor;
        c->root = k.CreateContainerVisual();
        c->target.Root(c->root);
        c->shadow = k.CreateSpriteVisual();
        c->dropShadow = k.CreateDropShadow();
        c->shadow.Shadow(c->dropShadow);
        c->panel = k.CreateContainerVisual();
        c->clipShape = k.CreateRoundedRectangleGeometry();
        c->panel.Clip(k.CreateGeometricClip(c->clipShape));
        c->backdrop = k.CreateSpriteVisual();
        c->content = k.CreateSpriteVisual();
        c->panel.Children().InsertAtTop(c->backdrop);
        c->panel.Children().InsertAtTop(c->content);
        c->root.Children().InsertAtTop(c->shadow);
        c->root.Children().InsertAtTop(c->panel);
        comp_ = c;
    } catch (const winrt::hresult_error& e) {
        LOG(L"flyout: composition error %08X %s", (unsigned)e.code(), e.message().c_str());
        return false;
    } catch (...) {
        LOG(L"flyout: composition error (unknown)");
        return false;
    }
    return EnsureDevice();
}

bool Flyout::EnsureDevice() {
    Composition* c = comp_;
    if (c->device) return true;
    ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) {
        LOG(L"flyout: hardware D3D11 device failed %08X, trying WARP", hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    if (FAILED(hr) || FAILED(hr = d3d.As(&dxgi)) || FAILED(hr = d2d_->CreateDevice(dxgi.Get(), &device))) {
        LOG(L"flyout: Direct3D/Direct2D device failed %08X", hr);
        return false;
    }
    try {
        if (!c->graphics) {
            auto interop = c->compositor.as<abi::ICompositorInterop>();
            winrt::check_hresult(interop->CreateGraphicsDevice(
                device.Get(), reinterpret_cast<abi::ICompositionGraphicsDevice**>(winrt::put_abi(c->graphics))));
        } else {
            winrt::check_hresult(c->graphics.as<abi::ICompositionGraphicsDeviceInterop>()->SetRenderingDevice(device.Get()));
        }
    } catch (...) {
        LOG(L"flyout: composition graphics device failed");
        return false;
    }
    c->device = device;
    c->d3dContext.Reset();
    d3d->GetImmediateContext(&c->d3dContext);
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
    brush_.Reset();
    dc->CreateSolidColorBrush(pal_.text, &brush_);
    c->maskPx = 0;
    c->surfaceFresh = true;
    return true;
}

static ComPtr<ID2D1DeviceContext> BeginSurfaceDraw(const wuc::CompositionDrawingSurface& surface, const RECT& update,
                                                   float scale, D2D1::Matrix3x2F* base, HRESULT* result) {
    ComPtr<ID2D1DeviceContext> dc;
    POINT offset{};
    auto interop = surface.as<abi::ICompositionDrawingSurfaceInterop>();
    *result = interop->BeginDraw(&update, __uuidof(ID2D1DeviceContext), (void**)dc.GetAddressOf(), &offset);
    if (FAILED(*result)) return nullptr;
    dc->SetDpi(96.0f * scale, 96.0f * scale);
    *base = D2D1::Matrix3x2F::Translation(offset.x / scale, offset.y / scale);
    dc->SetTransform(*base);
    dc->PushAxisAlignedClip({0, 0, (update.right - update.left) / scale, (update.bottom - update.top) / scale},
                            D2D1_ANTIALIAS_MODE_ALIASED);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    return dc;
}

// Submits the drawing to the GPU right away, or the compositor keeps showing
// the previous content.
static void EndSurfaceDraw(const wuc::CompositionDrawingSurface& surface, ID2D1DeviceContext* dc,
                           ID3D11DeviceContext* d3d) {
    dc->PopAxisAlignedClip();
    surface.as<abi::ICompositionDrawingSurfaceInterop>()->EndDraw();
    if (d3d) d3d->Flush();
}

static bool DeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET;
}

// Acrylic: the blurred desktop behind the window. With transparency effects
// off, a solid colour.
void Flyout::UpdateBackdrop() {
    Composition* c = comp_;
    DWORD transparency = 1, size = sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
    try {
        if (transparency) {
            c->backdrop.Brush(c->compositor.CreateHostBackdropBrush());
        } else {
            D2D1_COLOR_F t = pal_.tint;
            c->backdrop.Brush(c->compositor.CreateColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, (BYTE)(t.r * 255), (BYTE)(t.g * 255), (BYTE)(t.b * 255))));
        }
    } catch (...) {
    }
    c->dropShadow.Color(winrt::Windows::UI::ColorHelper::FromArgb(255, 0, 0, 0));
    c->dropShadow.Opacity(pal_.dark ? 0.45f : 0.22f);
}

void Flyout::PlacePanel(int heightPx) {
    Composition* c = comp_;
    float w = (float)panelW_, h = (float)heightPx;
    panelH_ = heightPx;
    panelY_ = anchorTop_ ? panelEdge_ : panelEdge_ - heightPx;
    winrt::Windows::Foundation::Numerics::float3 offset{(float)panelX_, (float)panelY_, 0};
    winrt::Windows::Foundation::Numerics::float2 size{w, h};
    c->panel.Offset(offset);
    c->panel.Size(size);
    c->clipShape.Size(size);
    float r = kCornerRadius * scale_;
    c->clipShape.CornerRadius({r, r});
    c->backdrop.Size(size);
    c->content.Size(size);
    c->shadow.Offset(offset);
    c->shadow.Size(size);
    c->dropShadow.BlurRadius(32 * scale_);
    c->dropShadow.Offset({0, 8 * scale_, 0});
}

void Flyout::UpdateShadowMask() {
    Composition* c = comp_;
    int r = (int)std::ceil(kCornerRadius * scale_);
    int px = r * 2 + 2;
    if (c->maskPx == px && c->maskBrush) return;
    try {
        if (!c->mask) {
            c->mask = c->graphics.CreateDrawingSurface({(float)px, (float)px},
                                                       winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                       winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            c->maskBrush = c->compositor.CreateNineGridBrush();
            c->maskBrush.Source(c->compositor.CreateSurfaceBrush(c->mask));
        } else {
            winrt::check_hresult(c->mask.as<abi::ICompositionDrawingSurfaceInterop>()->Resize({px, px}));
        }
    } catch (...) {
        return;
    }
    D2D1::Matrix3x2F base;
    HRESULT hr;
    auto dc = BeginSurfaceDraw(c->mask, {0, 0, px, px}, 1.0f, &base, &hr);
    if (!dc) {
        if (DeviceLost(hr)) c->device.Reset();
        return;
    }
    ComPtr<ID2D1SolidColorBrush> white;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    dc->FillRoundedRectangle(D2D1::RoundedRect({0, 0, (float)px, (float)px}, (float)r, (float)r), white.Get());
    EndSurfaceDraw(c->mask, dc.Get(), c->d3dContext.Get());
    c->maskBrush.SetInsets((float)r);
    c->shadow.Brush(c->maskBrush);
    c->dropShadow.Mask(c->maskBrush);
    c->maskPx = px;
}

// Clicks only reach the window over the panel (and its shadow).
void Flyout::UpdateRegion(bool settleLater) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int m = (int)std::lround(kShadowMargin * scale_);
    RECT want = {panelX_ - m, panelY_ - m, panelX_ + panelW_ + m, panelY_ + panelH_ + m};
    IntersectRect(&want, &want, &rc);
    bool grows = want.left < region_.left || want.top < region_.top || want.right > region_.right ||
                 want.bottom > region_.bottom;
    if (EqualRect(&want, &region_)) return;
    if (grows || !settleLater) {
        region_ = want;
        SetWindowRgn(hwnd_, CreateRectRgnIndirect(&want), FALSE);
    } else {
        SetTimer(hwnd_, kRegionTimer, 150, nullptr);
    }
}

RECT Flyout::PanelRect() const {
    POINT origin = {0, 0};
    ClientToScreen(hwnd_, &origin);
    return {origin.x + panelX_, origin.y + panelY_, origin.x + panelX_ + panelW_, origin.y + panelY_ + panelH_};
}

void Flyout::LoadPalette() {
    DWORD light = 0, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    bool dark = light == 0;

    // The accent palette's shades: WinUI uses Light2 on dark and Dark1 on light.
    D2D1_COLOR_F accent = Rgba(dark ? 0x99EBFF : 0x005FB8);
    BYTE shades[32];
    size = sizeof(shades);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, shades, &size) == ERROR_SUCCESS &&
        size >= 28) {
        int i = (dark ? 1 : 4) * 4;
        accent = D2D1::ColorF(shades[i] / 255.0f, shades[i + 1] / 255.0f, shades[i + 2] / 255.0f, 1.0f);
    }

    Palette p;
    p.dark = dark;
    p.accent = accent;
    if (dark) {
        p.text = Rgba(0xFFFFFF);
        p.textSecondary = Rgba(0xFFFFFF, 0.786f);
        p.textTertiary = Rgba(0xFFFFFF, 0.545f);
        p.subtleHover = Rgba(0xFFFFFF, 0.0605f);
        p.subtlePressed = Rgba(0xFFFFFF, 0.0419f);
        p.card = Rgba(0xFFFFFF, 0.0512f);
        p.cardStroke = Rgba(0x000000, 0.10f);
        p.divider = Rgba(0xFFFFFF, 0.0837f);
        p.footer = Rgba(0x000000, 0.2f);
        p.track = Rgba(0xFFFFFF, 0.10f);
        p.onAccent = Rgba(0x000000);
        p.toggleOffFill = Rgba(0x000000, 0.10f);
        p.tint = Rgba(0x202020, 0.82f);
        p.border = Rgba(0xFFFFFF, 0.10f);
        p.ok = Rgba(0x6CCB5F);
        p.warn = Rgba(0xFCE100);
        p.danger = Rgba(0xFF99A4);
    } else {
        p.text = Rgba(0x000000, 0.894f);
        p.textSecondary = Rgba(0x000000, 0.606f);
        p.textTertiary = Rgba(0x000000, 0.447f);
        p.subtleHover = Rgba(0x000000, 0.0373f);
        p.subtlePressed = Rgba(0x000000, 0.0241f);
        p.card = Rgba(0xFFFFFF, 0.70f);
        p.cardStroke = Rgba(0x000000, 0.0578f);
        p.divider = Rgba(0x000000, 0.0803f);
        p.footer = Rgba(0x000000, 0.03f);
        p.track = Rgba(0x000000, 0.08f);
        p.onAccent = Rgba(0xFFFFFF);
        p.toggleOffFill = Rgba(0x000000, 0.0241f);
        p.tint = Rgba(0xF3F3F3, 0.82f);
        p.border = Rgba(0x000000, 0.10f);
        p.ok = Rgba(0x0F7B0F);
        p.warn = Rgba(0x9D5D00);
        p.danger = Rgba(0xC42B1C);
    }
    pal_ = p;
}

void Flyout::ApplyWindowStyle() {
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof(on));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd_, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    UpdateBackdrop();
}

static ComPtr<IDWriteTextFormat> MakeFormat(IDWriteFactory* f, const wchar_t* family, float size,
                                            DWRITE_FONT_WEIGHT weight, bool wrap) {
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(f->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                   L"", &fmt)))
        return nullptr;
    if (wrap) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    } else {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        f->CreateEllipsisTrimmingSign(fmt.Get(), &sign);
        fmt->SetTrimming(&trimming, sign.Get());
    }
    return fmt;
}

bool Flyout::EnsureResources() {
    if (!fTitle_) {
        IDWriteFactory* f = dwrite_.Get();
        fTitle_ = MakeFormat(f, kTextFont, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
        fBody_ = MakeFormat(f, kTextFont, 14, DWRITE_FONT_WEIGHT_NORMAL, false);
        fCaption_ = MakeFormat(f, kTextFont, 12, DWRITE_FONT_WEIGHT_NORMAL, false);
        fDesc_ = MakeFormat(f, kTextFont, 13, DWRITE_FONT_WEIGHT_NORMAL, true);
        fBig_ = MakeFormat(f, kDisplayFont, 40, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
        fBigUnit_ = MakeFormat(f, kDisplayFont, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
        fPercent_ = MakeFormat(f, kTextFont, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
        if (!fTitle_ || !fBody_ || !fCaption_ || !fDesc_ || !fBig_ || !fBigUnit_ || !fPercent_) return false;
    }
    return EnsureDevice();
}

// ---------------------------------------------------------------------------
// Showing and positioning

void Flyout::Toggle() {
    if (visible_)
        Hide();
    else if (GetTickCount64() - lastHideTick_ > 300)  // the click that closed us
        Show();
}

void Flyout::Show(bool settingsView) {
    if (visible_) {
        SetForegroundWindow(hwnd_);
        GoToView(settingsView ? View::Settings : View::Main);
        return;
    }
    view_ = settingsView ? View::Settings : View::Main;
    pending_ = false;
    gear_.Set(0);
    arrow_.Set(0);
    transition_.Set(1);
    hover_ = pressed_ = Hit{};
    startup_ = GetStartWithWindows();
    // Bars open at their value; they only move when new numbers arrive.
    bars_.assign(usage_->data.limits.size(), Anim{});
    barTargets_.assign(usage_->data.limits.size(), -1);
    StartBars(true);

    Reposition();
    DwmFlush();
    RECT final;
    GetWindowRect(hwnd_, &final);
    int dist = (int)std::lround(24 * scale_);
    SetWindowPos(hwnd_, HWND_TOPMOST, final.left + slideDx_ * dist, final.top + slideDy_ * dist, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);

    visible_ = true;
    ShowWindow(hwnd_, SW_SHOW);
    BOOL fg = SetForegroundWindow(hwnd_);
    LOG(L"flyout: shown, scale %.2f, foreground %d", scale_, fg);
    UpdateWindow(hwnd_);

    // Slide in with an ease-out curve, one step per compositor frame.
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const double duration = 0.20;
    for (;;) {
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart / duration;
        if (t >= 1.0 || !visible_) break;
        double e = 1.0 - std::pow(1.0 - t, 3.0);
        int off = (int)std::lround(dist * (1.0 - e));
        SetWindowPos(hwnd_, nullptr, final.left + slideDx_ * off, final.top + slideDy_ * off, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        DwmFlush();
    }
    if (!visible_) return;
    SetWindowPos(hwnd_, nullptr, final.left, final.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetTimer(hwnd_, kTickTimer, 1000, nullptr);
    if (!g_mouseHook) {
        g_hookOwner = this;
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    }
}

void Flyout::Hide() {
    if (!visible_) return;
    LOG(L"flyout: hidden");
    visible_ = false;
    KillTimer(hwnd_, kTickTimer);
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
        g_hookOwner = nullptr;
    }
    lastHideTick_ = GetTickCount64();
    if (hexEdit_) ShowWindow(hexEdit_, SW_HIDE);
    ShowWindow(hwnd_, SW_HIDE);
    if (GetCapture() == hwnd_) ReleaseCapture();
}

void Flyout::Reposition() {
    RECT anchor{};
    NOTIFYICONIDENTIFIER nii = {sizeof(nii)};
    nii.hWnd = owner_;
    nii.uID = 1;
    if (FAILED(Shell_NotifyIconGetRect(&nii, &anchor))) {
        POINT pt;
        GetCursorPos(&pt);
        anchor = {pt.x, pt.y, pt.x + 1, pt.y + 1};
    }
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    scale_ = dpiX / 96.0f;
    if (!EnsureResources()) return;

    const RECT& work = mi.rcWork;
    const RECT& full = mi.rcMonitor;
    maxH_ = (work.bottom - work.top) / scale_ - 24.0f;

    // Fixed edges 12 DIPs from the work area, like Quick Settings.
    int w = (int)std::lround(kWidth * scale_);
    int m = (int)std::lround(12 * scale_);
    int shadow = (int)std::lround(kShadowMargin * scale_);
    int left = work.right - m - w;
    anchorTop_ = false;
    slideDx_ = 0;
    slideDy_ = 1;
    if (work.top > full.top) {  // taskbar at the top
        anchorTop_ = true;
        slideDy_ = -1;
    } else if (work.left > full.left) {  // taskbar on the left
        left = work.left + m;
        slideDx_ = -1;
        slideDy_ = 0;
    } else if (work.right < full.right) {  // taskbar on the right
        slideDx_ = 1;
        slideDy_ = 0;
    }
    RECT win = {std::max<LONG>(work.left, left - shadow), work.top, std::min<LONG>(work.right, left + w + shadow), work.bottom};
    panelX_ = left - win.left;
    panelW_ = w;
    panelEdge_ = anchorTop_ ? m : (win.bottom - win.top) - m;
    SetWindowPos(hwnd_, HWND_TOPMOST, win.left, win.top, win.right - win.left, win.bottom - win.top, SWP_NOACTIVATE);
    Render();
    UpdateRegion(false);
}

void Flyout::OnUsageChanged() {
    if (!visible_) return;
    StartBars(false);
    StartFramePump();  // the refresh icon spins while probing
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Flyout::OnThemeChanged() {
    LoadPalette();
    ApplyWindowStyle();
    if (brush_) brush_->SetColor(pal_.text);
    if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// Bars jump to their value when the panel opens (`instant`) and ease from
// where they are when new numbers arrive while it's open.
void Flyout::StartBars(bool instant) {
    const auto& limits = usage_->data.limits;
    if (bars_.size() != limits.size()) {
        bars_.resize(limits.size());
        barTargets_.resize(limits.size(), -1);
    }
    bool any = false;
    for (size_t i = 0; i < limits.size(); i++) {
        if (barTargets_[i] == limits[i].percent) continue;
        float v = limits[i].percent / 100.0f;
        if (instant || barTargets_[i] < 0) {
            bars_[i].Set(v);
        } else {
            bars_[i].Go(v, 450.0f);
            any = true;
        }
        barTargets_[i] = limits[i].percent;
    }
    if (any) StartFramePump();
}

// ---------------------------------------------------------------------------
// Drawing helpers

void Flyout::Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius) {
    brush_->SetColor(c);
    if (radius > 0)
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
    else
        rt_->FillRectangle(r, brush_.Get());
}

void Flyout::Stroke(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius, float width) {
    brush_->SetColor(c);
    float i = width / 2;
    D2D1_RECT_F rr = {r.left + i, r.top + i, r.right - i, r.bottom - i};
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(rr, radius, radius), brush_.Get(), width);
}

void Flyout::Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c,
                  DWRITE_TEXT_ALIGNMENT align) {
    brush_->SetColor(c);
    f->SetTextAlignment(align);
    rt_->DrawText(s.c_str(), (UINT32)s.size(), f, r, brush_.Get());
}

float Flyout::TextHeight(const std::wstring& s, IDWriteTextFormat* f, float width) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, width, 10000, &layout))) return 16;
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return std::ceil(m.height);
}

float Flyout::TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 100, &layout))) return 60;
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return std::ceil(m.widthIncludingTrailingWhitespace);
}

void Flyout::Glyph(wchar_t g, float size, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float degrees) {
    auto& fmt = iconFormats_[(int)size];
    if (!fmt) {
        dwrite_->CreateTextFormat(kIconFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
        if (!fmt) return;
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    D2D1_MATRIX_3X2_F old;
    if (degrees != 0) {
        rt_->GetTransform(&old);
        D2D1_POINT_2F center = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        rt_->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, center) * old);
    }
    brush_->SetColor(c);
    rt_->DrawText(&g, 1, fmt.Get(), r, brush_.Get());
    if (degrees != 0) rt_->SetTransform(old);
}

void Flyout::Button(const Hit& h, bool draw, float radius) {
    hits_.push_back(h);
    if (draw && hover_ == h) Fill(h.rect, pressed_ == h ? pal_.subtlePressed : pal_.subtleHover, radius);
}

void Flyout::ToggleSwitch(const Hit& h, float x, float cy, bool on, bool draw) {
    hits_.push_back(h);
    if (!draw) return;
    if (hover_ == h) Fill({h.rect.left + 2, h.rect.top + 2, h.rect.right - 2, h.rect.bottom - 2},
                          pressed_ == h ? pal_.subtlePressed : pal_.subtleHover, 6);
    D2D1_RECT_F r = {x, cy - 10, x + 40, cy + 10};
    float kr = hover_ == h ? 7.0f : 6.0f;
    if (on) {
        Fill(r, pal_.accent, 10);
        brush_->SetColor(pal_.onAccent);
        rt_->FillEllipse(D2D1::Ellipse({x + 30, cy}, kr, kr), brush_.Get());
    } else {
        Fill(r, pal_.toggleOffFill, 10);
        Stroke(r, pal_.textSecondary, 10);
        brush_->SetColor(pal_.textSecondary);
        rt_->FillEllipse(D2D1::Ellipse({x + 10, cy}, kr - 1, kr - 1), brush_.Get());
    }
}

// A progress bar like the WinUI ProgressBar: a thin track, the value filled.
void Flyout::Bar(float x0, float x1, float cy, float h, float value, const D2D1_COLOR_F& c) {
    float r = h / 2;
    Fill({x0, cy - r, x1, cy + r}, pal_.track, r);
    float x = x0 + std::clamp(value, 0.0f, 1.0f) * (x1 - x0);
    if (x > x0 + 0.5f) Fill({x0, cy - r, std::max(x, x0 + h), cy + r}, c, r);
}

D2D1_COLOR_F Flyout::LevelColor(int percent) const {
    if (percent >= 85) return pal_.danger;
    if (percent >= 60) return pal_.warn;
    return BarColorFor(settings_->barColor);
}

D2D1_COLOR_F Flyout::BarColorFor(DWORD option) const {
    switch (option) {
        case Settings::BarWhite:
            return pal_.dark ? Rgba(0xFFFFFF) : Rgba(0x3A3A3A);  // "white" is the text colour on light
        case Settings::BarGreen:
            return pal_.dark ? Rgba(0x3CC83C) : Rgba(0x0F7B0F);  // the Windows 7 meter green
        case Settings::BarCustom:
            return Rgba(settings_->barCustomColor);
        default:
            return pal_.accent;
    }
}

ID2D1StrokeStyle* Flyout::RoundStroke() {
    if (!roundStroke_) {
        auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                 D2D1_LINE_JOIN_ROUND);
        d2d_->CreateStrokeStyle(props, nullptr, 0, &roundStroke_);
    }
    return roundStroke_.Get();
}

static ComPtr<ID2D1PathGeometry> MakeGearGeometry(ID2D1Factory* f) {
    const int teeth = 6;
    const float outer = 7.2f, root = 4.9f;
    const float tipHalf = 10.0f, rootHalf = 21.0f;
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
    auto at = [](float r, float deg) {
        float a = deg * 3.14159265f / 180.0f;
        return D2D1::Point2F(r * std::cos(a), r * std::sin(a));
    };
    for (int i = 0; i < teeth; i++) {
        float c = -90.0f + i * 360.0f / teeth;
        D2D1_POINT_2F pts[] = {at(root, c - rootHalf), at(outer, c - tipHalf), at(outer, c + tipHalf), at(root, c + rootHalf)};
        if (i == 0)
            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
        else
            sink->AddLine(pts[0]);
        sink->AddLines(pts + 1, 3);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return geo;
}

void Flyout::DrawGear(const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    if (!gearGeo_) gearGeo_ = MakeGearGeometry(d2d_.Get());
    RoundStroke();
    if (!gearGeo_) return;
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    D2D1_POINT_2F center = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    rt_->SetTransform(D2D1::Matrix3x2F::Rotation(gear_.Value()) * D2D1::Matrix3x2F::Translation(center.x, center.y) * old);
    brush_->SetColor(c);
    rt_->DrawGeometry(gearGeo_.Get(), brush_.Get(), 1.0f, roundStroke_.Get());
    rt_->DrawEllipse(D2D1::Ellipse({0, 0}, 1.9f, 1.9f), brush_.Get(), 1.0f);
    rt_->SetTransform(old);
}

// ---------------------------------------------------------------------------
// Layout. Each function both measures (draw = false) and draws, so the hit
// areas and the panel height always match what's on screen.

float Flyout::Layout(bool draw) {
    hits_.clear();
    contentBottom_ = winH_ - kFooterHeight;
    if (draw) rt_->PushAxisAlignedClip({0, 0, kWidth, contentBottom_}, D2D1_ANTIALIAS_MODE_ALIASED);
    bool entering = draw && transition_.Running();
    if (entering) {
        float v = transition_.Value();
        rt_->SetTransform(D2D1::Matrix3x2F::Translation(transitionDir_ * 24.0f * (1 - v), 0) * drawBase_);
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), v),
                       nullptr);
    }
    float end = view_ == View::Main ? LayoutMain(0, draw) : LayoutSettings(0, draw);
    if (entering) {
        rt_->PopLayer();
        rt_->SetTransform(drawBase_);
    }
    if (draw) rt_->PopAxisAlignedClip();
    LayoutFooter(contentBottom_, draw);
    return end + kFooterHeight;
}

float Flyout::LayoutMain(float y, bool draw) {
    const float x0 = 20, x1 = kWidth - 20;
    const UsageData& d = usage_->data;

    // Header: title, plan, refresh button (spins while probing).
    y += 16;
    Hit refresh{HitKind::Refresh, {x1 - 32, y - 4, x1 + 4, y + 32}};
    Button(refresh, draw);
    if (draw) {
        Text(S().title, fTitle_.Get(), {x0, y, x1 - 40, y + 24}, pal_.text);
        float deg = usage_->probing ? (float)std::fmod(NowMs() * 0.36, 360.0) : 0.0f;
        Glyph(kGlyphRefresh, 16, refresh.rect, pal_.text, deg);
    }
    y += 24;
    if (!d.plan.name.empty()) {
        // Only the plan: the billing date isn't available locally, and estimating it
        // from the subscription start was wrong (an upgrade moves the billing day).
        std::wstring plan = Format(S().planOnly, d.plan.name);
        if (draw) Text(plan, fCaption_.Get(), {x0, y + 2, x1, y + 20}, pal_.textSecondary);
        y += 20;
    }
    y += 4;  // the big number's line box already leaves room above it

    if (d.limits.empty()) {
        const wchar_t* msg = S().loading;
        if (!usage_->probing) {
            switch (usage_->last) {
                case UsageData::NoClaude: msg = S().noClaude; break;
                case UsageData::NotLoggedIn: msg = S().notLoggedIn; break;
                case UsageData::NoLimits: msg = S().noLimits; break;
                case UsageData::Failed: msg = S().failed; break;
                default: break;
            }
        }
        float h = TextHeight(msg, fDesc_.Get(), x1 - x0);
        if (draw) Text(msg, fDesc_.Get(), {x0, y, x1, y + h}, pal_.textSecondary);
        return y + h + 20;
    }

    auto anim = [&](size_t i) { return i < bars_.size() ? bars_[i].Value() : d.limits[i].percent / 100.0f; };
    auto shown = [&](size_t i) { return settings_->showModelWeeks || !Usage::IsModelWeek(d.limits[i]); };
    bool first = true;
    for (size_t i = 0; i < d.limits.size(); i++) {
        if (!shown(i)) continue;
        const Limit& l = d.limits[i];
        D2D1_COLOR_F color = LevelColor(l.percent);
        std::wstring num = std::to_wstring(l.percent);
        if (l.key == L"session") {
            // The session leads, with a big number like the Windows battery flyout.
            float wNum = TextWidth(num, fBig_.Get()), wUnit = TextWidth(L"%", fBigUnit_.Get());
            if (draw) {
                Text(LimitName(l.key), fBody_.Get(), {x0, y + 18, x1 - wNum - wUnit - 8, y + 44}, pal_.text);
                Text(num, fBig_.Get(), {x1 - wUnit - wNum - 2, y, x1 - wUnit, y + 50}, color, DWRITE_TEXT_ALIGNMENT_TRAILING);
                Text(L"%", fBigUnit_.Get(), {x1 - wUnit, y + 18, x1, y + 46}, color, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
            y += 54;
            if (draw) Bar(x0, x1, y + 4, 8, anim(i), color);
            y += 16;
        } else {
            if (!first) y += 2;
            if (draw) {
                Text(LimitName(l.key), fBody_.Get(), {x0, y, x1 - 56, y + 22}, pal_.text);
                Text(num + L"%", fPercent_.Get(), {x1 - 56, y, x1, y + 22}, color, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
            y += 30;
            if (draw) Bar(x0, x1, y + 3, 6, anim(i), color);
            y += 14;
        }
        if (draw) Text(ResetLine(l), fCaption_.Get(), {x0, y, x1, y + 18}, pal_.textSecondary);
        y += 18;
        // A divider after the session; the weeks follow each other.
        bool more = false;
        for (size_t j = i + 1; j < d.limits.size(); j++) more = more || shown(j);
        bool sessionDone = l.key == L"session" && more;
        if (sessionDone) {
            y += 16;
            if (draw) Fill({x0, y, x1, y + 1}, pal_.divider);
            y += 17;
        } else {
            y += 14;
        }
        first = false;
    }

    if (settings_->showPeak) {
        y += 2;
        if (draw) Fill({x0, y, x1, y + 1}, pal_.divider);
        y += 15;
        ULONGLONG change = 0;
        bool peak = PeakNow(&change);
        ULONGLONG now = Usage::NowTicks();
        std::wstring right = change ? Format(peak ? S().peakEndsIn : S().peakStartsIn, Duration(change > now ? change - now : 0))
                                    : L"";
        if (draw) {
            brush_->SetColor(peak ? pal_.warn : pal_.ok);
            rt_->FillEllipse(D2D1::Ellipse({x0 + 4, y + 11}, 4, 4), brush_.Get());
            Text(peak ? S().peakOn : S().peakOff, fBody_.Get(), {x0 + 16, y, x1 - 150, y + 22}, pal_.text);
            Text(right, fCaption_.Get(), {x1 - 170, y, x1, y + 22}, pal_.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        y += 22 + 16;
    }
    return y;
}

float Flyout::LayoutSettings(float y, bool draw) {
    const float x0 = 12, x1 = kWidth - 12;
    y += 16;
    if (draw) Text(S().settings, fTitle_.Get(), {x0 + 8, y, x1, y + 24}, pal_.text);
    y += 24 + 16;

    struct Row { HitKind kind; const wchar_t* label; bool on; };
    const Row rows[] = {
        {HitKind::ToggleStartup, S().startWithWindows, startup_},
        {HitKind::TogglePeak, S().showPeak, settings_->showPeak},
        {HitKind::ToggleModelWeeks, S().showModelWeeks, settings_->showModelWeeks},
        {HitKind::ToggleStar, S().starIcon, settings_->starIcon},
    };
    const float rh = 52;
    float top = y, h = rh * ARRAYSIZE(rows);
    if (draw) {
        Fill({x0, top, x1, top + h}, pal_.card, 8);
        Stroke({x0, top, x1, top + h}, pal_.cardStroke, 8);
    }
    for (int i = 0; i < (int)ARRAYSIZE(rows); i++) {
        float ry = top + i * rh;
        if (i > 0 && draw) Fill({x0 + 1, ry, x1 - 1, ry + 1}, pal_.divider);
        Hit hit{rows[i].kind, {x0, ry, x1, ry + rh}};
        ToggleSwitch(hit, x1 - 16 - 40, ry + rh / 2, rows[i].on, draw);
        if (draw) Text(rows[i].label, fBody_.Get(), {x0 + 16, ry, x1 - 16 - 40 - 12, ry + rh}, pal_.text);
    }
    y = top + h + 12;

    // Bar colour: four choices; "custom" adds a hex field, and its swatch
    // opens a picker.
    {
        bool custom = settings_->barColor == Settings::BarCustom;
        bool picker = custom && pickerOpen_;
        const float kSvH = 110, kHueH = 14;
        float ch = 12 + 20 + 18 + 8 + 32 + (custom ? 10 + 32 : 0) + (picker ? 10 + kSvH + 10 + kHueH : 0) + 14;
        float cx0 = x0 + 16, cx1 = x1 - 16;
        if (draw) {
            Fill({x0, y, x1, y + ch}, pal_.card, 8);
            Stroke({x0, y, x1, y + ch}, pal_.cardStroke, 8);
            Text(S().barColor, fBody_.Get(), {cx0, y + 12, cx1, y + 32}, pal_.text);
            Text(S().barColorDesc, fCaption_.Get(), {cx0, y + 32, cx1, y + 50}, pal_.textSecondary);
        }
        const wchar_t* labels[] = {S().barAccent, S().barWhite, S().barGreen, S().barCustom};
        float cy = y + 12 + 20 + 18 + 8;
        float gap = 6, cw = (cx1 - cx0 - gap * 3) / 4;
        for (int i = 0; i < 4; i++) {
            float cx = cx0 + i * (cw + gap);
            Hit chip{HitKind::BarOption, {cx, cy, cx + cw, cy + 32}};
            chip.index = i;
            hits_.push_back(chip);
            if (!draw) continue;
            bool selected = (int)settings_->barColor == i;
            Fill(chip.rect, hover_ == chip ? pal_.subtleHover : pal_.toggleOffFill, 4);
            Stroke(chip.rect, selected ? pal_.accent : pal_.cardStroke, 4, selected ? 2.0f : 1.0f);
            brush_->SetColor(BarColorFor(i));
            rt_->FillEllipse(D2D1::Ellipse({cx + 12, cy + 16}, 4.5f, 4.5f), brush_.Get());
            Text(labels[i], fCaption_.Get(), {cx + 21, cy, cx + cw - 3, cy + 32}, pal_.text);
        }
        float py = cy + 32;
        hexRect_ = {-1, -1, -1, -1};
        if (custom) {
            py += 10;
            hexRect_ = {cx0, py, cx1, py + 32};
            Hit swatch{HitKind::ColorSwatch, {cx1 - 34, py + 2, cx1 - 2, py + 30}};
            hits_.push_back(swatch);
            if (draw) {
                const D2D1_RECT_F& r = hexRect_;
                Fill(r, pal_.toggleOffFill, 4);
                Stroke(r, pal_.cardStroke, 4);
                Fill({r.left + 4, r.bottom - 2, r.right - 4, r.bottom - 1}, pal_.accent);
                Text(L"#", fBody_.Get(), {r.left + 10, py, r.left + 24, py + 32}, pal_.textSecondary);
                if (hover_ == swatch) Fill(swatch.rect, pal_.subtleHover, 4);
                D2D1_RECT_F sw = {r.right - 26, py + 8, r.right - 10, py + 24};
                Fill(sw, Rgba(settings_->barCustomColor), 3);
                Stroke(sw, pickerOpen_ ? pal_.accent : pal_.cardStroke, 3, pickerOpen_ ? 2.0f : 1.0f);
            }
            py += 32;
            if (picker) {
                py += 10;
                D2D1_RECT_F sv = {cx0, py, cx1, py + kSvH};
                D2D1_RECT_F hue = {cx0, py + kSvH + 10, cx1, py + kSvH + 10 + kHueH};
                hits_.push_back(Hit{HitKind::ColorSV, sv});
                // A little extra around the hue bar, it is thin.
                hits_.push_back(Hit{HitKind::ColorHue, {hue.left - 4, hue.top - 4, hue.right + 4, hue.bottom + 4}});
                if (draw) DrawColorPicker(sv, hue);
            }
        }
        y += ch + 12;
    }

    // Link to the usage page on claude.ai, in the default browser.
    Hit web{HitKind::OpenWeb, {x0, y, x1, y + 48}};
    if (draw) {
        Fill(web.rect, pal_.card, 8);
        Stroke(web.rect, pal_.cardStroke, 8);
    }
    Button(web, draw, 8);
    if (draw) {
        Text(S().openWeb, fBody_.Get(), {x0 + 16, y, x1 - 48, y + 48}, pal_.text);
        Glyph(kGlyphOpen, 14, {x1 - 40, y, x1 - 12, y + 48}, pal_.textSecondary);
    }
    y += 48 + 16;
    return y;
}

void Flyout::LayoutFooter(float top, bool draw) {
    // The Quick Settings footer: 48 tall, darker band, the icon button 32 px
    // from the right edge (the gear here, the back arrow in Settings).
    if (draw) Fill({0, top, kWidth, top + kFooterHeight}, pal_.footer);
    Hit corner{view_ == View::Main ? HitKind::Gear : HitKind::Back, {kWidth - 50, top + 6, kWidth - 14, top + 42}};
    corner.footer = true;
    Button(corner, draw);
    if (!draw) return;
    if (view_ == View::Main)
        DrawGear(corner.rect, pal_.text);
    else
        Glyph(kGlyphBack, 16, corner.rect, pal_.text, arrow_.Value());

    std::wstring status;
    if (view_ == View::Settings) {
        status = APP_NAME L" " APP_VERSION;
    } else if (usage_->probing) {
        status = S().updating;
    } else if (usage_->data.fetchedAt) {
        ULONGLONG now = Usage::NowTicks();
        ULONGLONG age = now > usage_->data.fetchedAt ? now - usage_->data.fetchedAt : 0;
        if (usage_->last != UsageData::Ok)
            status = Format(S().updateFailed, Duration(age));
        else
            status = age < Usage::kMinute ? S().updatedNow : Format(S().updatedAgo, Duration(age));
        ULONGLONG tick = GetTickCount64();
        if (usage_->nextRefreshTick > tick) {
            ULONGLONG s = (usage_->nextRefreshTick - tick + 999) / 1000;
            wchar_t ms[16];
            swprintf_s(ms, L"%llu:%02llu", s / 60, s % 60);
            status += L"  ·  " + Format(S().nextIn, ms);
        }
    }
    Text(status, fCaption_.Get(), {20, top, kWidth - 56, top + kFooterHeight}, pal_.textTertiary);
}

void Flyout::Render() {
    if (!EnsureResources() || panelW_ <= 0) return;
    Composition* c = comp_;
    UpdateShadowMask();

    winH_ = 100000.0f;
    float natural = Layout(false);
    int heightPx = std::max(1, (int)std::lround(std::min(natural, maxH_) * scale_));
    winH_ = heightPx / scale_;

    int surfaceH = (int)std::lround(maxH_ * scale_) + 1;
    try {
        if (!c->surface) {
            c->surface = c->graphics.CreateDrawingSurface({(float)panelW_, (float)surfaceH},
                                                          winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                          winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            auto brush = c->compositor.CreateSurfaceBrush(c->surface);
            brush.Stretch(wuc::CompositionStretch::None);
            brush.HorizontalAlignmentRatio(0);
            brush.VerticalAlignmentRatio(0);
            c->content.Brush(brush);
        } else if (c->surfaceW != panelW_ || c->surfaceH != surfaceH) {
            winrt::check_hresult(c->surface.as<abi::ICompositionDrawingSurfaceInterop>()->Resize({panelW_, surfaceH}));
        }
        if (c->surfaceW != panelW_ || c->surfaceH != surfaceH) c->surfaceFresh = true;
        c->surfaceW = panelW_;
        c->surfaceH = surfaceH;
    } catch (...) {
        return;
    }

    panelH_ = heightPx;
    panelY_ = anchorTop_ ? panelEdge_ : panelEdge_ - heightPx;
    UpdateRegion(true);

    HRESULT hr;
    RECT update = {0, 0, panelW_, c->surfaceFresh ? surfaceH : heightPx};
    auto dc = BeginSurfaceDraw(c->surface, update, scale_, &drawBase_, &hr);
    if (!dc) {
        if (DeviceLost(hr)) {
            c->device.Reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    rt_ = dc;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // transparent target: no ClearType
    Fill({0, 0, kWidth, winH_}, pal_.tint);
    Layout(true);
    Stroke({0, 0, kWidth, winH_}, pal_.border, kCornerRadius);
    rt_.Reset();
    EndSurfaceDraw(c->surface, dc.Get(), c->d3dContext.Get());
    c->surfaceFresh = false;
    PlacePanel(heightPx);
}

D2D1_POINT_2F Flyout::ToPanel(int x, int y) const { return {(x - panelX_) / scale_, (y - panelY_) / scale_}; }

// ---------------------------------------------------------------------------
// Input

Flyout::Hit Flyout::HitTest(float x, float y) const {
    for (const Hit& h : hits_) {
        if (!h.footer && y >= contentBottom_) continue;
        if (x >= h.rect.left && x < h.rect.right && y >= h.rect.top && y < h.rect.bottom) return h;
    }
    return Hit{};
}

void Flyout::Activate(const Hit& h) {
    switch (h.kind) {
        case HitKind::Refresh:
            if (onRefresh) onRefresh();
            break;
        case HitKind::Gear:
            GoToView(View::Settings);
            break;
        case HitKind::Back:
            GoToView(View::Main);
            break;
        case HitKind::ToggleStartup:
            SetStartWithWindows(!startup_);
            startup_ = GetStartWithWindows();
            break;
        case HitKind::TogglePeak:
            settings_->showPeak = !settings_->showPeak;
            settings_->Save();
            break;
        case HitKind::ToggleModelWeeks:
            settings_->showModelWeeks = !settings_->showModelWeeks;
            settings_->Save();
            if (onTrayIconChanged) onTrayIconChanged();  // the tooltip lists them too
            break;
        case HitKind::BarOption:
            settings_->barColor = (DWORD)h.index;
            settings_->Save();
            if (h.index != Settings::BarCustom) pickerOpen_ = false;
            break;
        case HitKind::ColorSwatch:
            pickerOpen_ = !pickerOpen_;
            if (pickerOpen_) SyncPickerFromColor();
            break;
        case HitKind::ToggleStar:
            settings_->starIcon = !settings_->starIcon;
            settings_->Save();
            if (onTrayIconChanged) onTrayIconChanged();
            break;
        case HitKind::OpenWeb:
            Hide();
            ShellExecuteW(nullptr, L"open", kUsageUrl, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        default:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// View changes: the footer icon turns first, then the new view slides in.

static const float kIconTurnMs = 260, kSwitchDelayMs = 90, kTransitionMs = 200;

void Flyout::GoToView(View v) {
    if (pending_ || v == view_) return;
    if (v == View::Settings)
        gear_.Go(180, kIconTurnMs);
    else
        arrow_.Go(-90, kIconTurnMs);
    pending_ = true;
    pendingView_ = v;
    pendingAt_ = NowMs() + kSwitchDelayMs;
    StartFramePump();
}

void Flyout::StartFramePump() {
    if (pumping_ || !visible_) return;
    pumping_ = true;
    QueueFrame();
}

// Frames go through the composition DispatcherQueue at low priority, so the
// compositor gets to commit each one.
void Flyout::QueueFrame() {
    try {
        if (comp_->queue.DispatcherQueue().TryEnqueue(winrt::Windows::System::DispatcherQueuePriority::Low,
                                                      [this] { AnimFrame(); }))
            return;
    } catch (...) {
    }
    PostMessageW(hwnd_, WM_ANIM_FRAME, 0, 0);
}

void Flyout::AnimFrame() {
    if (!visible_) {
        pumping_ = false;
        return;
    }
    DwmFlush();  // one frame per composition
    if (pending_ && NowMs() >= pendingAt_) SwitchView(pendingView_);
    Render();
    UpdateHexEdit();
    ValidateRect(hwnd_, nullptr);
    if (Animating())
        QueueFrame();
    else
        pumping_ = false;
}

void Flyout::SwitchView(View v) {
    pending_ = false;
    view_ = v;
    hover_ = pressed_ = Hit{};
    if (v == View::Settings) startup_ = GetStartWithWindows();
    gear_.Set(0);
    arrow_.Set(0);
    transitionDir_ = v == View::Settings ? 1 : -1;
    transition_.Set(0);
    transition_.Go(1, kTransitionMs);
    Reposition();
    StartFramePump();
}

bool Flyout::Animating() const {
    if (pending_ || gear_.Running() || arrow_.Running() || transition_.Running()) return true;
    if (usage_->probing && view_ == View::Main) return true;
    for (const Anim& a : bars_)
        if (a.Running()) return true;
    return false;
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK Flyout::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto self = (Flyout*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    auto self = (Flyout*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return self ? self->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Flyout::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) Hide();
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Render();
            EndPaint(hwnd_, &ps);
            UpdateHexEdit();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_COMMAND:  // custom colour typed in the hex field
            if (HIWORD(wp) == EN_CHANGE && lp && (HWND)lp == hexEdit_ && !pickerSyncing_) {
                wchar_t text[16] = {};
                GetWindowTextW(hexEdit_, text, ARRAYSIZE(text));
                wchar_t* end = nullptr;
                unsigned long v = wcstoul(text, &end, 16);
                if (wcslen(text) == 6 && end && *end == 0) {
                    settings_->barCustomColor = v;
                    settings_->Save();
                    SyncPickerFromColor();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return 0;

        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            COLORREF bg = pal_.dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xF9, 0xF9, 0xF9);
            SetTextColor(dc, pal_.dark ? RGB(255, 255, 255) : RGB(0x1A, 0x1A, 0x1A));
            SetBkColor(dc, bg);
            if (hexBrush_) DeleteObject(hexBrush_);
            hexBrush_ = CreateSolidBrush(bg);
            return (LRESULT)hexBrush_;
        }

        case WM_ANIM_FRAME:
            AnimFrame();
            return 0;

        case WM_OUTSIDE_CLICK:
            Hide();
            return 0;

        case WM_TIMER:
            if (wp == kRegionTimer) {
                KillTimer(hwnd_, kRegionTimer);
                UpdateRegion(false);
            } else if (wp == kTickTimer && visible_ && !pumping_) {
                InvalidateRect(hwnd_, nullptr, FALSE);  // countdowns
            }
            return 0;

        case WM_DPICHANGED:
            scale_ = HIWORD(wp) / 96.0f;
            if (visible_) Reposition();
            return 0;

        case WM_MOUSEMOVE: {
            auto [x, y] = ToPanel(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (!tracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            if (GetCapture() == hwnd_ && (pressed_.kind == HitKind::ColorSV || pressed_.kind == HitKind::ColorHue)) {
                SetPickerValue(pressed_, x, y);
                return 0;
            }
            Hit h = HitTest(x, y);
            if (!(h == hover_)) {
                hover_ = h;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            hover_ = Hit{};
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            auto [x, y] = ToPanel(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            SetCapture(hwnd_);
            pressed_ = HitTest(x, y);
            if (pressed_.kind == HitKind::ColorSV || pressed_.kind == HitKind::ColorHue) {
                if (hexEdit_ && GetFocus() == hexEdit_) SetFocus(hwnd_);
                SetPickerValue(pressed_, x, y);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            auto [x, y] = ToPanel(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            Hit h = HitTest(x, y);
            Hit p = pressed_;
            pressed_ = Hit{};
            if (GetCapture() == hwnd_) ReleaseCapture();
            if (p.kind == HitKind::ColorSV || p.kind == HitKind::ColorHue) {
                settings_->Save();  // the drag ended
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            hover_ = h;
            if (p.kind != HitKind::None && h == p) Activate(h);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                if (view_ == View::Settings)
                    GoToView(View::Main);
                else
                    Hide();
            } else if (wp == VK_F5 && onRefresh) {
                onRefresh();
            }
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Bar colour "custom": hex field and picker, from VolumeOne.

// The hex field is a real EDIT control laid over the field the settings view
// draws. Called after each paint.
void Flyout::UpdateHexEdit() {
    bool show = visible_ && view_ == View::Settings && !transition_.Running() && !pending_ && hexRect_.top >= 0 &&
                hexRect_.bottom <= contentBottom_;
    if (!show) {
        if (hexEdit_ && IsWindowVisible(hexEdit_)) ShowWindow(hexEdit_, SW_HIDE);
        return;
    }
    int fontPx = (int)std::lround(14 * scale_);
    static int lastFontPx = 0;
    if (!hexFont_ || lastFontPx != fontPx) {
        if (hexFont_) DeleteObject(hexFont_);
        hexFont_ = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, kTextFont);
        if (hexEdit_) SendMessageW(hexEdit_, WM_SETFONT, (WPARAM)hexFont_, TRUE);
        lastFontPx = fontPx;
    }
    if (!hexEdit_) {
        // Layered: the panel has no redirection bitmap for a plain child to
        // draw into; a layered child gets its own surface.
        hexEdit_ = CreateWindowExW(WS_EX_LAYERED, L"EDIT", nullptr, WS_CHILD | ES_AUTOHSCROLL | ES_UPPERCASE, 0, 0, 10,
                                   10, hwnd_, (HMENU)1, GetModuleHandleW(nullptr), nullptr);
        if (!hexEdit_) return;
        SetLayeredWindowAttributes(hexEdit_, 0, 255, LWA_ALPHA);
        SendMessageW(hexEdit_, EM_LIMITTEXT, 6, 0);
        SendMessageW(hexEdit_, WM_SETFONT, (WPARAM)hexFont_, FALSE);
        SetWindowLongPtrW(hexEdit_, GWLP_USERDATA, SetWindowLongPtrW(hexEdit_, GWLP_WNDPROC, (LONG_PTR)HexEditProc));
    }
    if (!IsWindowVisible(hexEdit_)) {
        wchar_t text[8];
        swprintf_s(text, L"%06X", settings_->barCustomColor & 0xFFFFFF);
        pickerSyncing_ = true;
        SetWindowTextW(hexEdit_, text);
        pickerSyncing_ = false;
    }
    // Inside the drawn field, between the "#" and the swatch.
    const D2D1_RECT_F& r = hexRect_;
    int x = panelX_ + (int)std::lround((r.left + 24) * scale_);
    int h = (int)std::lround(fontPx * 1.4f);
    int y = panelY_ + (int)std::lround(((r.top + r.bottom) / 2) * scale_) - h / 2;
    int w = panelX_ + (int)std::lround((r.right - 40) * scale_) - x;
    SetWindowPos(hexEdit_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Only hex digits get in. A pasted colour drops its "#" (or "0x") and, when
// it is a whole colour, replaces the field.
LRESULT CALLBACK Flyout::HexEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto base = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CHAR && wp >= 0x20 && !iswxdigit((wint_t)wp)) return 0;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {  // Esc works as elsewhere in the panel
        SendMessageW(GetParent(hwnd), WM_KEYDOWN, wp, lp);
        return 0;
    }
    if (msg == WM_PASTE) {
        std::wstring text;
        if (OpenClipboard(hwnd)) {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                if (auto p = (const wchar_t*)GlobalLock(data)) {
                    text = p;
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        size_t i = text.find_first_not_of(L" \t\r\n");
        if (i == std::wstring::npos) return 0;
        if (text[i] == L'#')
            i++;
        else if (text.compare(i, 2, L"0x") == 0 || text.compare(i, 2, L"0X") == 0)
            i += 2;
        std::wstring hex;
        for (; i < text.size() && iswxdigit(text[i]) && hex.size() < 6; i++) hex += text[i];
        if (hex.empty()) return 0;
        if (hex.size() == 6) SendMessageW(hwnd, EM_SETSEL, 0, -1);
        SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)hex.c_str());
        return 0;
    }
    return CallWindowProcW(base, hwnd, msg, wp, lp);
}

static DWORD HsvToRgb(float h, float s, float v) {
    float c = v * s, hp = std::fmod(h, 360.0f) / 60.0f, x = c * (1 - std::fabs(std::fmod(hp, 2.0f) - 1));
    float r = 0, g = 0, b = 0;
    if (hp < 1) r = c, g = x;
    else if (hp < 2) r = x, g = c;
    else if (hp < 3) g = c, b = x;
    else if (hp < 4) g = x, b = c;
    else if (hp < 5) r = x, b = c;
    else r = c, b = x;
    float m = v - c;
    auto byte = [&](float f) { return (DWORD)std::lround(std::clamp(f + m, 0.0f, 1.0f) * 255); };
    return (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

// The picker's position from the colour. Hue (and saturation, for black) stay
// where they were when the colour does not define them.
void Flyout::SyncPickerFromColor() {
    DWORD c = settings_->barCustomColor;
    float r = ((c >> 16) & 0xFF) / 255.0f, g = ((c >> 8) & 0xFF) / 255.0f, b = (c & 0xFF) / 255.0f;
    float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    pickerV_ = mx;
    if (mx > 0) pickerS_ = d / mx;
    if (d > 0) {
        float h = mx == r ? std::fmod((g - b) / d, 6.0f) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        pickerH_ = h * 60 < 0 ? h * 60 + 360 : h * 60;
    }
}

// Dragging in the square or along the hue bar. Saved when the drag ends.
void Flyout::SetPickerValue(const Hit& h, float x, float y) {
    const D2D1_RECT_F& r = h.rect;
    if (h.kind == HitKind::ColorSV) {
        pickerS_ = std::clamp((x - r.left) / (r.right - r.left), 0.0f, 1.0f);
        pickerV_ = 1 - std::clamp((y - r.top) / (r.bottom - r.top), 0.0f, 1.0f);
    } else {
        // The hit is 4 DIPs wider than the bar on each side.
        pickerH_ = std::clamp((x - r.left - 4) / (r.right - r.left - 8), 0.0f, 1.0f) * 359.9f;
    }
    DWORD c = HsvToRgb(pickerH_, pickerS_, pickerV_);
    settings_->barCustomColor = c;
    if (hexEdit_) {
        wchar_t text[8];
        swprintf_s(text, L"%06X", c);
        pickerSyncing_ = true;
        SetWindowTextW(hexEdit_, text);
        pickerSyncing_ = false;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Flyout::DrawColorPicker(const D2D1_RECT_F& sv, const D2D1_RECT_F& hue) {
    auto gradient = [&](const D2D1_GRADIENT_STOP* stops, UINT n, D2D1_POINT_2F a, D2D1_POINT_2F b) {
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> brush;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, n, &coll)))
            rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll.Get(), &brush);
        return brush;
    };

    // Square: the hue, white towards the left, black towards the bottom.
    D2D1_ROUNDED_RECT svr = {sv, 4, 4};
    brush_->SetColor(Rgba(HsvToRgb(pickerH_, 1, 1)));
    rt_->FillRoundedRectangle(svr, brush_.Get());
    D2D1_GRADIENT_STOP white[] = {{0, D2D1::ColorF(1, 1, 1, 1)}, {1, D2D1::ColorF(1, 1, 1, 0)}};
    if (auto b = gradient(white, 2, {sv.left, sv.top}, {sv.right, sv.top})) rt_->FillRoundedRectangle(svr, b.Get());
    D2D1_GRADIENT_STOP black[] = {{0, D2D1::ColorF(0, 0, 0, 0)}, {1, D2D1::ColorF(0, 0, 0, 1)}};
    if (auto b = gradient(black, 2, {sv.left, sv.top}, {sv.left, sv.bottom})) rt_->FillRoundedRectangle(svr, b.Get());
    Stroke(sv, pal_.cardStroke, 4);

    // Hue bar: red, yellow, green, cyan, blue, magenta, red.
    D2D1_GRADIENT_STOP hues[7];
    for (int i = 0; i < 7; i++) hues[i] = {i / 6.0f, Rgba(HsvToRgb(i * 60.0f, 1, 1))};
    float rad = (hue.bottom - hue.top) / 2;
    if (auto b = gradient(hues, 7, {hue.left, hue.top}, {hue.right, hue.top}))
        rt_->FillRoundedRectangle({hue, rad, rad}, b.Get());
    Stroke(hue, pal_.cardStroke, rad);

    // Handles: white rings with a thin dark edge, filled with their colour.
    auto handle = [&](D2D1_POINT_2F p, float r, DWORD color) {
        brush_->SetColor(Rgba(color));
        rt_->FillEllipse(D2D1::Ellipse(p, r, r), brush_.Get());
        brush_->SetColor(Rgba(0xFFFFFF));
        rt_->DrawEllipse(D2D1::Ellipse(p, r, r), brush_.Get(), 2.0f);
        brush_->SetColor(Rgba(0x000000, 0.35f));
        rt_->DrawEllipse(D2D1::Ellipse(p, r + 1.5f, r + 1.5f), brush_.Get(), 1.0f);
    };
    handle({sv.left + pickerS_ * (sv.right - sv.left), sv.top + (1 - pickerV_) * (sv.bottom - sv.top)}, 6,
           HsvToRgb(pickerH_, pickerS_, pickerV_));
    handle({hue.left + pickerH_ / 360.0f * (hue.right - hue.left), (hue.top + hue.bottom) / 2}, rad - 1,
           HsvToRgb(pickerH_, 1, 1));
}
