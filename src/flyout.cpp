#include "flyout.h"
#include "flyout_style.h"
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

bool Flyout::Create(HINSTANCE inst, HWND owner, const UsageState* usage, Settings* settings, const UpdateState* update,
                    const PluginState* plugin) {
    owner_ = owner;
    usage_ = usage;
    settings_ = settings;
    update_ = update;
    plugin_ = plugin;

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
#ifdef USAGEONE_DEV
    if (themeOverride) dark = themeOverride == 2;
#endif

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
    double t0 = NowMs();
    if (!visible_) upToDateStale_ = update_->status == UpdateState::UpToDate;
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
    UpdateWindow(hwnd_);
#ifdef USAGEONE_DEV
    // Development builds: how long opening took (README stats) and where the panel is (screenshots).
    RECT pr = PanelRect();
    LOG(L"flyout: shown in %.1f ms, scale %.2f, foreground %d, panel %ld,%ld,%ld,%ld", NowMs() - t0, scale_, fg,
        pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top);
#else
    (void)t0;
    LOG(L"flyout: shown, scale %.2f, foreground %d", scale_, fg);
#endif

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
#ifdef USAGEONE_DEV
    RECT settled = PanelRect();  // where it stays (tools\screenshots.ps1)
    LOG(L"flyout: panel %ld,%ld,%ld,%ld", settled.left, settled.top, settled.right - settled.left,
        settled.bottom - settled.top);
#endif
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

    // The next open starts on the main view. Draw it now, while hidden: the
    // compositor shows the last committed frame until a new one is committed,
    // and that would flash whatever was open before (e.g. Settings).
    view_ = View::Main;
    pending_ = false;
    pickerOpen_ = false;
    transition_.Set(1);
    gear_.Set(0);
    arrow_.Set(0);
    hover_ = pressed_ = Hit{};
    Render();
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

// The refresh icon turns while probing and, when the probe
// ends, finishes its turn back to rest instead of snapping there.
float Flyout::SpinAngle() {
    double now = NowMs();
    if (usage_->probing && !spinning_) {
        spinning_ = true;
        spinStart_ = now;
    }
    if (!spinning_) return 0;
    double turned = (now - spinStart_) * 0.54;  // degrees per ms: a turn every 0.67 s
    if (usage_->probing) {
        spinEnd_ = -1;
    } else {
        if (spinEnd_ < 0) spinEnd_ = std::max(360.0, std::ceil(turned / 360.0) * 360.0);
        if (turned >= spinEnd_) {
            spinning_ = false;
            spinEnd_ = -1;
            return 0;
        }
    }
    return (float)std::fmod(turned, 360.0);
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

    // Update banner, until it's installed (Settings has what's new).
    if (update_->status == UpdateState::Available || update_->status == UpdateState::InstallFailed ||
        update_->status == UpdateState::Installing) {
        bool installing = update_->status == UpdateState::Installing;
        std::wstring text = Format(S().updateAvailable, update_->version);
        const wchar_t* button = installing ? S().updating : S().updateNow;
        float top = y + 12;
        size_t hitCount = hits_.size();
        float h = CardButtonRow(top, APP_NAME, text, button, HitKind::InstallUpdate, true, !installing, false);
        hits_.resize(hitCount);
        if (draw) {
            Fill({12, top, kWidth - 12, top + h}, pal_.card, 8);
            Stroke({12, top, kWidth - 12, top + h}, pal_.cardStroke, 8);
        }
        CardButtonRow(top, APP_NAME, text, button, HitKind::InstallUpdate, true, !installing, draw);
        y = top + h - 4;
    }

    // Header: title, plan, refresh button (spins while probing).
    y += 16;
    Hit refresh{HitKind::Refresh, {x1 - 32, y - 4, x1 + 4, y + 32}};
    float deg = SpinAngle();
    if (!spinning_) Button(refresh, draw);  // not pressable until the icon is back at rest
    if (draw) {
        Text(S().title, fTitle_.Get(), {x0, y, x1 - 40, y + 24}, pal_.text);
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
    hexRect_ = {-1, -1, -1, -1};  // set by the bar colour block when it is fully shown

    auto card = [&](float top, float h) {
        if (!draw || h <= 0) return;
        Fill({x0, top, x1, top + h}, pal_.card, 8);
        Stroke({x0, top, x1, top + h}, pal_.cardStroke, 8);
    };
    auto divider = [&] {
        if (draw) Fill({x0 + 1, y, x1 - 1, y + 1}, pal_.divider);
    };

    // A collapsible section, from VolumeOne: one card with a title row and a
    // body that wipes open/closed. `body` draws at y and advances it; it is
    // measured first, then drawn (shifted up while animating, so it slides out
    // from under the header).
    auto section = [&](int idx, const wchar_t* title, auto&& body) {
        const float headerH = 48;
        float v = sectionAnim_[idx].Value();
        float headerTop = y, top = y + headerH, h = 0;
        Hit header{HitKind::Section, {x0, headerTop, x1, top}};
        header.index = idx;
        if (v > 0) {
            size_t hitCount = hits_.size();
            bool d = draw;
            draw = false;
            y = top;
            body();
            h = y - top;
            draw = d;
            hits_.resize(hitCount);
        }
        float shown = v >= 1 ? h : std::round(h * v * scale_) / scale_;  // whole pixels while moving
        card(headerTop, headerH + shown);
        Button(header, draw, 7);
        if (draw) {
            Text(title, fTitle_.Get(), {x0 + 16, headerTop, x1 - 56, top}, pal_.text);
            DrawChevron({x1 - 36, headerTop, x1 - 12, top}, pal_.textSecondary, v);
            if (shown > 0) Fill({x0 + 1, top, x1 - 1, top + 1}, pal_.divider);
        }
        if (v > 0) {
            size_t hitCount = hits_.size();
            if (draw) rt_->PushAxisAlignedClip({x0, top, x1, top + shown}, D2D1_ANTIALIAS_MODE_ALIASED);
            y = top - (h - shown);
            body();
            if (draw) rt_->PopAxisAlignedClip();
            if (sectionAnim_[idx].Running()) hits_.resize(hitCount);  // not clickable while moving
        }
        y = top + shown + 12;
    };

    struct Row { HitKind kind; std::wstring label; bool on; };
    auto toggleRows = [&](std::initializer_list<Row> rows) {
        const float rh = 52;
        bool first = true;
        for (const Row& row : rows) {
            if (!first) divider();
            first = false;
            Hit hit{row.kind, {x0, y, x1, y + rh}};
            ToggleSwitch(hit, x1 - 16 - 40, y + rh / 2, row.on, draw);
            if (draw) Text(row.label, fBody_.Get(), {x0 + 16, y, x1 - 16 - 40 - 12, y + rh}, pal_.text);
            y += rh;
        }
    };

    // General: starting with Windows and the tray icon.
    section(0, S().sectionGeneral, [&] {
        toggleRows({
            {HitKind::ToggleStartup, S().startWithWindows, startup_},
            {HitKind::ToggleStar, S().starIcon, settings_->starIcon},
        });
    });

    // Panel: what it shows, and the bar colour.
    section(1, S().sectionPanel, [&] {
        // The per-model weekly toggle is named after the rows it hides, as the
        // panel shows them ("Show Weekly · Fable").
        std::wstring modelWeeks;
        for (const Limit& l : usage_->data.limits) {
            if (!Usage::IsModelWeek(l)) continue;
            modelWeeks += modelWeeks.empty() ? LimitName(l.key) : L", " + l.key;
        }
        modelWeeks = modelWeeks.empty() ? std::wstring(S().showModelWeeks) : Format(S().showRow, modelWeeks);
        toggleRows({
            {HitKind::TogglePeak, S().showPeak, settings_->showPeak},
            {HitKind::ToggleModelWeeks, modelWeeks, settings_->showModelWeeks},
        });
        divider();
        y = LayoutBarColor(y, draw, !sectionAnim_[1].Running());
    });

    // Stream Deck: the plugin built into the exe. Installing copies it into
    // Stream Deck's plugins and restarts Stream Deck.
    section(2, S().streamDeck, [&] {
        const PluginState& p = *plugin_;
        std::wstring status;
        const wchar_t* button = S().install;
        bool enabled = p.streamDeck && !p.installing, accent = false;
        if (!p.streamDeck) {
            status = S().streamDeckMissing;
        } else if (p.installing) {
            status = S().installing;
        } else {
            if (p.installed.empty())
                status = S().pluginNotInstalled, accent = true;
            else if (p.installed != APP_VERSION)
                status = Format(S().pluginOutdated, p.installed), button = S().updateNow, accent = true;
            else
                status = Format(S().pluginInstalled, p.installed), button = S().reinstall;
            if (p.failed) status += std::wstring(L" · ") + S().installFailed;
        }
        y += CardButtonRow(y, S().pluginLabel, status, button, HitKind::InstallPlugin, accent, enabled, draw);
    });

    // Updates: always open, as in VolumeOne.
    y = LayoutUpdates(y, draw);

    // Link to the usage page on claude.ai, in the default browser.
    Hit web{HitKind::OpenWeb, {x0, y, x1, y + 48}};
    card(y, 48);
    Button(web, draw, 8);
    if (draw) {
        Text(S().openWeb, fBody_.Get(), {x0 + 16, y, x1 - 48, y + 48}, pal_.text);
        Glyph(kGlyphOpen, 14, {x1 - 40, y, x1 - 12, y + 48}, pal_.textSecondary);
    }
    y += 48 + 16;
    return y;
}

// Bar colour: four choices; "custom" adds a hex field, and its swatch opens a
// picker (colorpicker.cpp). Drawn inside the Panel section's card.
float Flyout::LayoutBarColor(float y, bool draw, bool settled) {
    const float x0 = 12, x1 = kWidth - 12, cx0 = x0 + 16, cx1 = x1 - 16;
    const float kSvH = 110, kHueH = 14;
    bool custom = settings_->barColor == Settings::BarCustom;

    y += 12;
    if (draw) {
        Text(S().barColor, fBody_.Get(), {cx0, y, cx1, y + 20}, pal_.text);
        Text(S().barColorDesc, fCaption_.Get(), {cx0, y + 20, cx1, y + 38}, pal_.textSecondary);
    }
    y += 20 + 18 + 8;

    const wchar_t* labels[] = {S().barAccent, S().barWhite, S().barGreen, S().barCustom};
    const float gap = 6, chipW = (cx1 - cx0 - gap * 3) / 4;
    for (int i = 0; i < 4; i++) {
        float cx = cx0 + i * (chipW + gap);
        Hit chip{HitKind::BarOption, {cx, y, cx + chipW, y + 32}};
        chip.index = i;
        hits_.push_back(chip);
        if (!draw) continue;
        bool selected = (int)settings_->barColor == i;
        Fill(chip.rect, hover_ == chip ? pal_.subtleHover : pal_.toggleOffFill, 4);
        Stroke(chip.rect, selected ? pal_.accent : pal_.cardStroke, 4, selected ? 2.0f : 1.0f);
        brush_->SetColor(BarColorFor(i));
        rt_->FillEllipse(D2D1::Ellipse({cx + 12, y + 16}, 4.5f, 4.5f), brush_.Get());
        Text(labels[i], fCaption_.Get(), {cx + 21, y, cx + chipW - 3, y + 32}, pal_.text);
    }
    y += 32;

    if (custom) {
        y += 10;
        D2D1_RECT_F field = {cx0, y, cx1, y + 32};
        if (settled && draw) hexRect_ = field;  // the EDIT control goes here (UpdateHexEdit)
        Hit swatch{HitKind::ColorSwatch, {cx1 - 34, y + 2, cx1 - 2, y + 30}};
        hits_.push_back(swatch);
        if (draw) {
            Fill(field, pal_.toggleOffFill, 4);
            Stroke(field, pal_.cardStroke, 4);
            Fill({field.left + 4, field.bottom - 2, field.right - 4, field.bottom - 1}, pal_.accent);
            Text(L"#", fBody_.Get(), {field.left + 10, y, field.left + 24, y + 32}, pal_.textSecondary);
            if (hover_ == swatch) Fill(swatch.rect, pal_.subtleHover, 4);
            D2D1_RECT_F sw = {field.right - 26, y + 8, field.right - 10, y + 24};
            Fill(sw, Rgba(settings_->barCustomColor), 3);
            Stroke(sw, pickerOpen_ ? pal_.accent : pal_.cardStroke, 3, pickerOpen_ ? 2.0f : 1.0f);
        }
        y += 32;
        if (pickerOpen_) {
            y += 10;
            D2D1_RECT_F sv = {cx0, y, cx1, y + kSvH};
            D2D1_RECT_F hue = {cx0, y + kSvH + 10, cx1, y + kSvH + 10 + kHueH};
            hits_.push_back(Hit{HitKind::ColorSV, sv});
            // A little extra around the hue bar, it is thin.
            hits_.push_back(Hit{HitKind::ColorHue, {hue.left - 4, hue.top - 4, hue.right + 4, hue.bottom + 4}});
            if (draw) DrawColorPicker(sv, hue);
            y += kSvH + 10 + kHueH;
        }
    }
    return y + 14;
}

// Updates, as in VolumeOne: the version, a check / update button, and what's
// new when there is an update.
float Flyout::LayoutUpdates(float y, bool draw) {
    const float x0 = 12, x1 = kWidth - 12;
    std::wstring status = Format(S().versionLine, APP_VERSION);
    const wchar_t* button = S().checkUpdates;
    HitKind kind = HitKind::CheckUpdates;
    bool enabled = true;
    switch (update_->status) {
        case UpdateState::Checking: button = S().checking; enabled = false; break;
        case UpdateState::UpToDate:
            if (!upToDateStale_) button = S().upToDate;
            break;
        case UpdateState::Failed: status += std::wstring(L" · ") + S().checkFailed; break;
        case UpdateState::Available:
        case UpdateState::InstallFailed:
            status += L" · " + Format(S().updateAvailable, update_->version);
            if (update_->status == UpdateState::InstallFailed) status += std::wstring(L" · ") + S().installFailed;
            button = S().updateNow;
            kind = HitKind::InstallUpdate;
            break;
        case UpdateState::Installing:
            status += std::wstring(L" · ") + S().updating;
            enabled = false;
            break;
        default: break;
    }
    bool hasNotes = (update_->status == UpdateState::Available || update_->status == UpdateState::InstallFailed ||
                     update_->status == UpdateState::Installing) && !update_->notes.empty();

    // Measured first (the card goes under the row), then drawn.
    size_t hitCount = hits_.size();
    float rowH = CardButtonRow(y, S().updates, status, button, kind, kind == HitKind::InstallUpdate, enabled, false);
    hits_.resize(hitCount);
    float notesH = hasNotes ? TextHeight(update_->notes, fDesc_.Get(), x1 - x0 - 32) : 0;
    float h = rowH + (hasNotes ? 1 + 40 + notesH + 12 : 0);
    if (draw) {
        Fill({x0, y, x1, y + h}, pal_.card, 8);
        Stroke({x0, y, x1, y + h}, pal_.cardStroke, 8);
    }
    CardButtonRow(y, S().updates, status, button, kind, kind == HitKind::InstallUpdate, enabled, draw);
    if (hasNotes && draw) {
        float ny = y + rowH;
        Fill({x0 + 1, ny, x1 - 1, ny + 1}, pal_.divider);
        Text(Format(S().whatsNew, update_->version), fTitle_.Get(), {x0 + 16, ny + 13, x1, ny + 33}, pal_.text);
        Text(update_->notes, fDesc_.Get(), {x0 + 16, ny + 41, x1 - 16, ny + 41 + notesH}, pal_.textSecondary);
    }
    return y + h + 12;
}

// The section header's arrow: pointing down when closed, up when open.
void Flyout::DrawChevron(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float open) {
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float s = 2.5f * (1 - 2 * std::clamp(open, 0.0f, 1.0f));  // tip offset: +down, -up
    brush_->SetColor(c);
    rt_->DrawLine({cx - 5, cy - s}, {cx, cy + s}, brush_.Get(), 1.0f, RoundStroke());
    rt_->DrawLine({cx, cy + s}, {cx + 5, cy - s}, brush_.Get(), 1.0f, RoundStroke());
}

// A row inside a card: a label with a status line under it and a button on
// the right. Returns its height. Adds the button's hit when enabled.
float Flyout::CardButtonRow(float y, const std::wstring& label, const std::wstring& status, const wchar_t* button,
                            HitKind kind, bool accent, bool enabled, bool draw) {
    const float x0 = 12, x1 = kWidth - 12, textX = x0 + 16;
    float bw = TextWidth(button, fBody_.Get()) + 28;
    float dh = TextHeight(status, fDesc_.Get(), x1 - 12 - bw - 8 - textX);
    float h = std::max(64.0f, 12 + 20 + dh + 12);
    Hit btn{kind, {x1 - 12 - bw, y + h / 2 - 16, x1 - 12, y + h / 2 + 16}};
    if (enabled) hits_.push_back(btn);
    if (draw) {
        Text(label, fBody_.Get(), {textX, y + 12, btn.rect.left - 8, y + 32}, pal_.text);
        Text(status, fDesc_.Get(), {textX, y + 32, btn.rect.left - 8, y + 32 + dh}, pal_.textSecondary);
        D2D1_COLOR_F fill = accent ? pal_.accent : pal_.toggleOffFill;
        if (!enabled)
            fill.a *= 0.5f;
        else if (hover_ == btn)
            fill = accent ? D2D1::ColorF(fill.r, fill.g, fill.b, pressed_ == btn ? 0.8f : 0.9f) : pal_.subtleHover;
        Fill(btn.rect, fill, 4);
        if (!accent) Stroke(btn.rect, pal_.cardStroke, 4);
        Text(button, fBody_.Get(), btn.rect, accent ? pal_.onAccent : pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    return h;
}

void Flyout::OnStateChanged() {
    if (update_->status != UpdateState::UpToDate) upToDateStale_ = false;
    if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
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
#ifdef USAGEONE_DEV
    if (visible_ && heightPx != loggedH_) {  // tools\screenshots.ps1 reads the latest
        loggedH_ = heightPx;
        RECT pr = PanelRect();
        LOG(L"flyout: panel %ld,%ld,%ld,%ld", pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top);
    }
#endif
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
            if (onRefresh && !usage_->probing && !spinning_) onRefresh();
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
        case HitKind::Section:
            if (h.index < 0 || h.index >= kSections) break;
            for (const Anim& a : sectionAnim_)
                if (a.Running()) return;  // one at a time: clicks meanwhile don't queue up
            sectionOpen_[h.index] = !sectionOpen_[h.index];
            sectionAnim_[h.index].Go(sectionOpen_[h.index] ? 1.0f : 0.0f, 250);
            StartFramePump();
            break;
        case HitKind::CheckUpdates:
            upToDateStale_ = false;
            if (onCheckUpdates) onCheckUpdates();
            break;
        case HitKind::InstallUpdate:
            if (onInstallUpdate) onInstallUpdate();
            break;
        case HitKind::InstallPlugin:
            if (onInstallPlugin) onInstallPlugin();
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
    if ((usage_->probing || spinning_) && view_ == View::Main) return true;
    for (const Anim& a : sectionAnim_)
        if (a.Running()) return true;
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
            } else if (wp == VK_F5 && onRefresh && !usage_->probing && !spinning_) {
                onRefresh();
            }
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
