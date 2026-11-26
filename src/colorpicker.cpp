// The panel's custom bar colour: a hex field (a real EDIT control laid over
// the field the settings view draws) and a hue/saturation/value picker.
// Ported from VolumeOne's meter colour.

#include "flyout.h"
#include "flyout_style.h"

#include <cmath>

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
