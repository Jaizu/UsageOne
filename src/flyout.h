#pragma once

#include "common.h"
#include "settings.h"
#include "usage.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <functional>
#include <map>

// The panel above the tray icon. Its look and mechanics come from VolumeOne:
// a Windows.UI.Composition visual tree with the acrylic backdrop, a drop
// shadow and Direct2D content, sliding in like the Windows flyouts.

struct Palette {
    bool dark = true;
    D2D1_COLOR_F text, textSecondary, textTertiary;
    D2D1_COLOR_F subtleHover, subtlePressed;
    D2D1_COLOR_F card, cardStroke, divider, footer;
    D2D1_COLOR_F track;
    D2D1_COLOR_F accent, onAccent, warn, danger, ok;
    D2D1_COLOR_F toggleOffFill;
    D2D1_COLOR_F tint;    // painted over the acrylic, like WinUI's acrylic tint layer
    D2D1_COLOR_F border;  // panel outline
};

// Milliseconds from the high-resolution counter.
inline double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / freq;
}

// A value eased from one number to another over time (ease-out cubic).
struct Anim {
    float from = 0, to = 0, ms = 1;
    double start = 0;
    float Progress() const { return std::min(1.0f, (float)((NowMs() - start) / ms)); }
    bool Running() const { return Progress() < 1.0f; }
    float Value() const {
        float t = Progress();
        return from + (to - from) * (1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t));
    }
    void Go(float target, float duration) {
        from = Value();
        to = target;
        ms = duration;
        start = NowMs();
    }
    void Set(float v) { from = to = v; start = 0; }
};

class Flyout {
public:
    std::function<void()> onRefresh;
    std::function<void()> onTrayIconChanged;

    bool Create(HINSTANCE inst, HWND owner, const UsageState* usage, Settings* settings);
    void Toggle();
    void Show(bool settingsView = false);
    void Hide();
    bool IsVisible() const { return visible_; }
    HWND Hwnd() const { return hwnd_; }
    RECT PanelRect() const;  // the visible panel, screen pixels (the window is bigger)

    void OnUsageChanged();   // new data, or a probe started/ended
    void OnThemeChanged();

private:
    enum class View { Main, Settings };
    enum class HitKind {
        None, Refresh, Gear, Back, ToggleStartup, TogglePeak, ToggleStar, ToggleModelWeeks, OpenWeb,
        BarOption, ColorSwatch, ColorSV, ColorHue,
    };
    struct Hit {
        HitKind kind = HitKind::None;
        D2D1_RECT_F rect{};
        bool footer = false;  // in the fixed footer
        int index = 0;        // which bar colour option
        bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT, WPARAM, LPARAM);

    void ApplyWindowStyle();
    void LoadPalette();
    bool EnsureResources();
    void Render();
    float Layout(bool draw);
    float LayoutMain(float y, bool draw);
    float LayoutSettings(float y, bool draw);
    void LayoutFooter(float top, bool draw);
    void Reposition();

    // Windows.UI.Composition visual tree (see flyout.cpp).
    struct Composition;
    Composition* comp_ = nullptr;
    bool CreateComposition();
    bool EnsureDevice();
    void UpdateBackdrop();
    void UpdateShadowMask();
    void PlacePanel(int heightPx);
    void UpdateRegion(bool settleLater);
    D2D1_POINT_2F ToPanel(int clientX, int clientY) const;
    int panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;  // client pixels
    int panelEdge_ = 0;       // y of the edge that stays put (next to the taskbar)
    bool anchorTop_ = false;  // taskbar at the top: the panel grows down
    RECT region_{};
    D2D1::Matrix3x2F drawBase_ = D2D1::Matrix3x2F::Identity();

    // Drawing helpers (DIPs).
    void Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius, float width = 1);
    void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float TextHeight(const std::wstring& s, IDWriteTextFormat* f, float width);
    float TextWidth(const std::wstring& s, IDWriteTextFormat* f);
    void Glyph(wchar_t g, float size, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float degrees = 0);
    void Button(const Hit& h, bool draw, float radius = 4);
    void ToggleSwitch(const Hit& h, float x, float cy, bool on, bool draw);
    void Bar(float x0, float x1, float cy, float h, float value, const D2D1_COLOR_F& c);
    D2D1_COLOR_F LevelColor(int percent) const;
    D2D1_COLOR_F BarColorFor(DWORD option) const;  // Settings::BarColor

    // Bar colour "custom": a hex field (a real EDIT control laid over the
    // drawn one) and a hue/saturation/value picker, as in VolumeOne.
    static LRESULT CALLBACK HexEditProc(HWND, UINT, WPARAM, LPARAM);
    void UpdateHexEdit();
    void SyncPickerFromColor();
    void SetPickerValue(const Hit& h, float x, float y);
    void DrawColorPicker(const D2D1_RECT_F& sv, const D2D1_RECT_F& hue);
    HWND hexEdit_ = nullptr;
    HFONT hexFont_ = nullptr;
    HBRUSH hexBrush_ = nullptr;
    D2D1_RECT_F hexRect_{-1, -1, -1, -1};
    bool pickerOpen_ = false;
    bool pickerSyncing_ = false;  // setting the field's text from the picker
    float pickerH_ = 0, pickerS_ = 1, pickerV_ = 1;
    void DrawGear(const D2D1_RECT_F& r, const D2D1_COLOR_F& c);
    ID2D1StrokeStyle* RoundStroke();

    Hit HitTest(float x, float y) const;
    void Activate(const Hit& h);

    // View changes and animations, as in VolumeOne.
    void GoToView(View v);
    void SwitchView(View v);
    bool Animating() const;
    void StartFramePump();
    void QueueFrame();
    void AnimFrame();
    void StartBars(bool instant);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    int slideDx_ = 0, slideDy_ = 1;
    const UsageState* usage_ = nullptr;
    Settings* settings_ = nullptr;
    bool visible_ = false;
    ULONGLONG lastHideTick_ = 0;
    View view_ = View::Main;
    bool startup_ = false;

    float scale_ = 1.0f;
    float winH_ = 100000.0f;
    float maxH_ = 100000.0f;
    float contentBottom_ = 0;

    std::vector<Hit> hits_;
    Hit hover_, pressed_;
    bool tracking_ = false;

    std::vector<Anim> bars_;      // one per limit, 0..1
    std::vector<int> barTargets_; // the percent each bar is heading to

    Palette pal_;
    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1RenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1PathGeometry> gearGeo_;
    ComPtr<ID2D1StrokeStyle> roundStroke_;
    Anim gear_, arrow_, transition_;
    int transitionDir_ = 1;
    bool pending_ = false;
    View pendingView_ = View::Main;
    double pendingAt_ = 0;
    bool pumping_ = false;
    ComPtr<IDWriteTextFormat> fTitle_, fBody_, fCaption_, fDesc_, fBig_, fBigUnit_, fPercent_;
    std::map<int, ComPtr<IDWriteTextFormat>> iconFormats_;
};
